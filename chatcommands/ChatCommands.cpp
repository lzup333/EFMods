//
// ChatCommands - 聊天指令助手
// ClassicEFMod 重写版，适配 Terraria 1.4.5.6.4 (手机端 / PE)
//
// 功能(在游戏聊天框输入指令):
//   1. /get <物品ID> [数量]     生成对应物品并给予玩家(默认 1 个, 数量上限 9999, /give 同义)
//   2. /sp  <生物ID> [数量]     在玩家脚下生成对应生物(默认 1 只, 数量上限 100)
//   3. /god [on|off]            开关无敌模式(置 creativeGodMode, 免疫一切伤害)
//
// 实现要点(结合 PE 1.4.5.6.4 dump.cs 与 PC 1.4.5.6 源码):
//   1. 聊天拦截: 单机(netMode==0)下玩家回车发送聊天时,
//      Main.DoUpdate_HandleChat -> ChatManager.Commands.ProcessIncomingMessage(msg, myPlayer)
//      会被调用 (Main.cs:18214), 因此 Hook 这个公开方法即可拿到 ChatMessage 对象,
//      其 Text(偏移 0x18, string) 就是玩家输入的原始文本。
//      多人客户端(netMode==1)走 ChatHelper.SendChatMessageFromClient(msg) (dump.cs:120113),
//      同样 Hook 以便指令在多人客户端也生效。
//   2. 读取 C# 字符串: il2cpp 的 System.String 对象布局为
//      [klass 0x00][monitor 0x08][length 0x10(int)][chars 0x14(char16[])]
//      因此直接按偏移读长度与 UTF-16 字符即可(与 AutoFisher 内联 struct 同思路)。
//      不需要依赖加载器的字符串解析通道。
//   3. 生成物品: 使用 PE 独有的旧重载 Item.NewItem(IEntitySource, int,int,int,int,int,int,bool,int,bool)
//      (dump.cs:48809, 10 参数唯一无歧义)。源码中该重载不会解引用 source, 传 NULL 安全
//      (Item.cs:49246)。
//   4. 生成生物: NPC.NewNPC(IEntitySource, int,int,int,int,float,float,float,float,int)
//      (dump.cs:57418, 10 参数)。其方法体同样不读取 source (NPC.cs:81524), 传 NULL 安全。
//      注意: 通过 Method<Int>::Call 的 varargs 传参时, 4 个 float 参数(ai0-3)必须传
//      double 字面量(0.0) —— C varargs 中 float 提升为 double。若传 int 0 会被部分
//      实现按 4 字节读取, 后续参数(Target 等)错位成野值, 生成的史莱姆 AI 运行到
//      野指针时崩溃(实测 /sp 1 生成后约 0.7 秒崩溃)。
//   5. 无敌模式: 直接置 Player.creativeGodMode(旅途模式无敌字段)为 true, 与
//      旅途模式 GodmodePower 完全一致 —— Player.Hurt 返回 0(Player.cs:37595)、
//      Player.KillMe 不执行(Player.cs:38199)、NPC 接触免伤(Player.cs:30863)。
//      由于 ResetEffects 每帧会把它重置为 false(Player.cs:18607), 因此 Hook
//      ResetEffects 每帧重新置位; PlayerFrame 锁血保留作备份保险。
//   6. 聊天反馈: 通过 ResolveGameSymbol 解析 il2cpp_string_new 创建 C# 字符串,
//      调用 Main.NewText 显示(libil2cpp 为 RTLD_LOCAL 加载, 需按 SONAME 重新
//      dlopen 拿句柄); 若符号不可用则仅写日志, 不影响指令执行。
//
// 注意:
//   - 只处理本地玩家的消息(ProcessIncomingMessage 的 clientId 即本地玩家)。
//   - 所有解析器/字段/方法指针均做了空指针保护, 解析失败时对应功能自动降级, 不会崩溃。
//   - 指令消息会由原版默认命令先回显到聊天框, 这是 post-hook 的固有限制(仅外观问题)。

#include "efmod_core.hpp"
#include "TEFMod.hpp"
#include "BaseType.hpp"
#include "Logger.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <dlfcn.h>

// ============ 全局组件 ============
TEFMod::Logger* g_log = nullptr;
TEFMod::TEFModAPI* g_api = nullptr;

// ============ 解析器 ============
static TEFMod::Field<int>*    (*g_parseIntField)(void*);
static TEFMod::Field<bool>*   (*g_parseBoolField)(void*);
static TEFMod::Field<void*>*  (*g_parseObjField)(void*);
static TEFMod::Array<void*>*  (*g_parseObjArray)(void*);
static TEFMod::Method<int>*   (*g_parseIntMethod)(void*);
static TEFMod::Method<void>*  (*g_parseVoidMethod)(void*);

// ============ 字段 ============
static TEFMod::Field<int>*    g_fStatLife;      // Player.statLife       (int)
static TEFMod::Field<int>*    g_fStatLifeMax;   // Player.statLifeMax    (int)
static TEFMod::Field<void*>*  g_fMainPlayer;    // Main.player           (Player[], 静态)
static TEFMod::Field<int>*    g_fWhoAmI;        // Entity.whoAmI         (int)
static TEFMod::Field<bool>*   g_fCreativeGod;   // Player.creativeGodMode(bool, 旅途模式无敌)

// ============ 方法 ============
static TEFMod::Method<int>*   g_mGetMyPlayer;   // Main.get_myPlayer     (static int)
static TEFMod::Method<int>*   g_mNewItem;       // Item.NewItem          (static int, 10 参)
static TEFMod::Method<int>*   g_mNewNPC;        // NPC.NewNPC            (static int, 10 参)
static TEFMod::Method<void>*  g_mNewText;       // Main.NewText          (static void, 4 参)

// ============ 原版函数 ============
static void (*g_original_ProcessIncomingMessage)(TEFMod::TerrariaInstance, TEFMod::TerrariaInstance, int);
static void (*g_original_SendChatMessageFromClient)(TEFMod::TerrariaInstance);
static void (*g_original_PlayerFrame)(TEFMod::TerrariaInstance);
static void (*g_original_ResetEffects)(TEFMod::TerrariaInstance);

// ============ Hook 模板 ============
void ProcessIncomingMessage_T(TEFMod::TerrariaInstance processor, TEFMod::TerrariaInstance message, int clientId);
void SendChatMessageFromClient_T(TEFMod::TerrariaInstance message);
void PlayerFrame_T(TEFMod::TerrariaInstance player);
void ResetEffects_T(TEFMod::TerrariaInstance player);

inline TEFMod::HookTemplate g_hookProcessIncoming {
        reinterpret_cast<void*>(ProcessIncomingMessage_T),
        {}
};

inline TEFMod::HookTemplate g_hookSendChatFromClient {
        reinterpret_cast<void*>(SendChatMessageFromClient_T),
        {}
};

inline TEFMod::HookTemplate g_hookPlayerFrame {
        reinterpret_cast<void*>(PlayerFrame_T),
        {}
};

inline TEFMod::HookTemplate g_hookResetEffects {
        reinterpret_cast<void*>(ResetEffects_T),
        {}
};

// ============ 状态 ============
static bool g_godMode = false;      // 无敌模式开关

// ============ 对象内偏移 (PE 1.4.5.6.4 dump.cs) ============
// Entity:        whoAmI 0x10 | position(Vector2=2float) 0x14 | width 0x3C | height 0x40
static constexpr std::size_t kWhoAmIOffset   = 0x10;
static constexpr std::size_t kPosOffset      = 0x14;
static constexpr std::size_t kWidthOffset    = 0x3C;
static constexpr std::size_t kHeightOffset   = 0x40;
// ChatMessage:   <Text>k__BackingField 0x18 (string) | <IsConsumed> 0x20 (bool)
static constexpr std::size_t kMsgTextOffset  = 0x18;
static constexpr std::size_t kMsgConsumed    = 0x20;
// System.String: length 0x10 (int) | chars 0x14 (char16[])
static constexpr std::size_t kStrLenOffset   = 0x10;
static constexpr std::size_t kStrCharOffset  = 0x14;

// ============ 转发函数 ============
void ProcessIncomingMessage_T(TEFMod::TerrariaInstance processor, TEFMod::TerrariaInstance message, int clientId) {
    if (g_original_ProcessIncomingMessage) g_original_ProcessIncomingMessage(processor, message, clientId);
    for (const auto fun : g_hookProcessIncoming.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(TEFMod::TerrariaInstance, TEFMod::TerrariaInstance, int)>(fun)(processor, message, clientId);
    }
}

void SendChatMessageFromClient_T(TEFMod::TerrariaInstance message) {
    if (g_original_SendChatMessageFromClient) g_original_SendChatMessageFromClient(message);
    for (const auto fun : g_hookSendChatFromClient.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(TEFMod::TerrariaInstance)>(fun)(message);
    }
}

void PlayerFrame_T(TEFMod::TerrariaInstance player) {
    if (g_original_PlayerFrame) g_original_PlayerFrame(player);
    for (const auto fun : g_hookPlayerFrame.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(TEFMod::TerrariaInstance)>(fun)(player);
    }
}

void ResetEffects_T(TEFMod::TerrariaInstance player) {
    if (g_original_ResetEffects) g_original_ResetEffects(player);
    for (const auto fun : g_hookResetEffects.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(TEFMod::TerrariaInstance)>(fun)(player);
    }
}

// ============ 工具函数 ============

/**
 * 读取 C# 托管字符串(il2cpp System.String 布局), 返回 ASCII 内容
 * 非 ASCII 字符(中文/emoji)一律替换为 '?' —— 指令本身就是 ASCII, 足够用
 */
static std::string ReadCSharpString(void* str) {
    if (!str) return "";
    const std::int32_t len = *reinterpret_cast<const std::int32_t*>(
            reinterpret_cast<const char*>(str) + kStrLenOffset);
    if (len <= 0 || len > 256) return "";
    const char16_t* chars = reinterpret_cast<const char16_t*>(
            reinterpret_cast<const char*>(str) + kStrCharOffset);
    std::string out;
    out.reserve(static_cast<std::size_t>(len));
    for (std::int32_t i = 0; i < len; ++i) {
        const char16_t c = chars[i];
        if (c == 0) break;
        if (c >= 0x20 && c < 0x7F) out.push_back(static_cast<char>(c));
        else out.push_back('?');
    }
    return out;
}

/**
 * 从游戏进程内解析 il2cpp 导出符号
 * libil2cpp 通常以 RTLD_LOCAL 加载, 直接 dlsym(RTLD_DEFAULT) 找不到,
 * 需按 SONAME(libil2cpp.so) 重新 dlopen 拿句柄; 再不行就从 /proc/self/maps
 * 定位实际路径后打开
 */
static void* ResolveGameSymbol(const char* sym) {
    void* p = dlsym(RTLD_DEFAULT, sym);
    if (p) return p;

    void* h = dlopen("libil2cpp.so", RTLD_NOW);
    if (h) p = dlsym(h, sym);
    if (p) return p;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libil2cpp.so")) continue;
        char* path = strchr(line, '/');
        if (!path) continue;
        char* nl = strchr(path, '\n');
        if (nl) *nl = '\0';
        h = dlopen(path, RTLD_NOW);
        if (h) { p = dlsym(h, sym); break; }
    }
    fclose(f);
    return p;
}

/**
 * 创建 C# 托管字符串(用于给 Main.NewText 传参)
 * 通过 il2cpp_string_new 导出函数创建; 拿不到则返回 nullptr(仅无聊天反馈)
 */
static void* CreateGameString(const char* utf8) {
    if (!utf8) return nullptr;
    static void* (*il2cpp_string_new_fn)(const char*) = nullptr;
    if (!il2cpp_string_new_fn) {
        il2cpp_string_new_fn = reinterpret_cast<void*(*)(const char*)>(ResolveGameSymbol("il2cpp_string_new"));
        if (g_log) g_log->i("ChatCommands", "il2cpp_string_new=", (void*)il2cpp_string_new_fn);
    }
    return il2cpp_string_new_fn ? il2cpp_string_new_fn(utf8) : nullptr;
}

/** 在聊天框显示一行文字(尽力而为, 失败仅降级为日志) */
static void ShowChat(const char* text) {
    if (!g_mNewText) return;
    void* s = CreateGameString(text);
    if (!s) return;
    g_mNewText->Call(nullptr, 4, s, 255, 255, 255);
    if (g_log) g_log->i("ChatCommands", text);
}

/** 获取本地玩家实例; 失败返回 nullptr */
static TEFMod::TerrariaInstance LocalPlayer() {
    if (!g_mGetMyPlayer) return nullptr;
    const int my = g_mGetMyPlayer->Call(nullptr, 0);
    if (my < 0) return nullptr;
    if (!g_fMainPlayer || !g_parseObjArray) return nullptr;
    void* raw = g_fMainPlayer->Get();
    if (!raw) return nullptr;
    TEFMod::Array<void*>* arr = g_parseObjArray(raw);
    if (!arr || my >= static_cast<int>(arr->Size())) return nullptr;
    return arr->at(my);
}

/** 玩家几何信息(位置 + 尺寸) */
struct PlayerGeo { int x, y, w, h; };

/** 按对象内偏移读取玩家位置与尺寸(Entity 字段) */
static bool PlayerGeometry(TEFMod::TerrariaInstance p, PlayerGeo& g) {
    if (!p) return false;
    const char* base = reinterpret_cast<const char*>(p);
    const float* pos = reinterpret_cast<const float*>(base + kPosOffset);
    const int* wh = reinterpret_cast<const int*>(base + kWidthOffset);
    g.x = static_cast<int>(pos[0]);
    g.y = static_cast<int>(pos[1]);
    g.w = wh[0];
    g.h = wh[1];
    if (g.w < 0) g.w = 0;
    if (g.h < 0) g.h = 0;
    return true;
}

/** 去掉行首空白 */
static std::string TrimLeft(const std::string& s) {
    const std::size_t p = s.find_first_not_of(" \t\r\n");
    return p == std::string::npos ? std::string() : s.substr(p);
}

/**
 * 处理一条本地玩家的聊天指令
 * @param raw 玩家输入的原始文本(如 "/get 5 99")
 */
static void HandleCommand(const std::string& raw) {
    if (raw.rfind("/get", 0) == 0 || raw.rfind("/give", 0) == 0) {
        const std::string rest = TrimLeft(raw.rfind("/get", 0) == 0 ? raw.substr(4) : raw.substr(5));
        if (rest.empty()) {
            ShowChat("[指令助手] 用法: /get 或 /give <物品ID> [数量]");
            return;
        }
        char* end = nullptr;
        const long id = std::strtol(rest.c_str(), &end, 10);
        if (end == rest.c_str() || id <= 0 || id >= 100000) {
            ShowChat("[指令助手] 无效的物品ID");
            return;
        }
        int stack = 1;
        if (end && *end != '\0') {
            long st = std::strtol(end, nullptr, 10);
            if (st < 1) st = 1;
            if (st > 9999) st = 9999;
            stack = static_cast<int>(st);
        }
        if (!g_mNewItem) {
            ShowChat("[指令助手] Item.NewItem 解析失败");
            return;
        }
        PlayerGeo geo;
        if (!PlayerGeometry(LocalPlayer(), geo)) return;
        // NewItem(source=null, X, Y, Width, Height, Type, Stack, noBroadcast, pfix, noGrabDelay)
        const int idx = g_mNewItem->Call(nullptr, 10, 0, geo.x, geo.y, geo.w, geo.h,
                                         static_cast<int>(id), stack, 0, 0, 0);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "[指令助手] 已给予物品 %ld x %d (slot=%d)", id, stack, idx);
        ShowChat(buf);
        return;
    }

    if (raw.rfind("/sp", 0) == 0) {
        const std::string rest = TrimLeft(raw.substr(3));
        if (rest.empty()) {
            ShowChat("[指令助手] 用法: /sp <生物ID> [数量]");
            return;
        }
        char* end = nullptr;
        const long id = std::strtol(rest.c_str(), &end, 10);
        if (end == rest.c_str() || id < 0 || id >= 100000) {
            ShowChat("[指令助手] 无效的生物ID");
            return;
        }
        int count = 1;
        if (end && *end != '\0') {
            long c = std::strtol(end, nullptr, 10);
            if (c < 1) c = 1;
            if (c > 100) c = 100;
            count = static_cast<int>(c);
        }
        if (!g_mNewNPC) {
            ShowChat("[指令助手] NPC.NewNPC 解析失败");
            return;
        }
        PlayerGeo geo;
        if (!PlayerGeometry(LocalPlayer(), geo)) return;

        const int bx = geo.x + geo.w / 2;
        const int by = geo.y + geo.h;
        if (g_log) g_log->i("ChatCommands", "spawn: id=", (int)id, " count=", count,
                            " bx=", bx, " by=", by);

        // NewNPC(source=null, X, Y, Type, Start, ai0..3, Target)
        // 注意: ai0-3 是 float, C varargs 中 float 提升为 double, 必须传 0.0(此前传 int 0
        //       会被部分实现按 4 字节读取, 导致后续参数(尤其 Target)错位成野值,
        //       生成的史莱姆 AI 运行时读到野指针而崩溃)
        // 每次生成偏移位置, 避免全部叠在同一格(否则看起来像只生成了一只)
        for (int i = 0; i < count; ++i) {
            const int sx = bx + (i % 8) * 24 - 84;
            const int sy = by + (i / 8) * 24;
            const int slot = g_mNewNPC->Call(nullptr, 10, 0, sx, sy, static_cast<int>(id), 0,
                                              0.0, 0.0, 0.0, 0.0, 255);
            if (g_log) g_log->i("ChatCommands", "  NewNPC[", i, "] -> slot=", slot);
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "[指令助手] 已生成生物 %ld x %d", id, count);
        ShowChat(buf);
        return;
    }

    if (raw.rfind("/god", 0) == 0) {
        const std::string rest = TrimLeft(raw.substr(4));
        if (rest == "on" || rest == "1" || rest == "true") {
            g_godMode = true;
        } else if (rest == "off" || rest == "0" || rest == "false") {
            g_godMode = false;
        } else {
            g_godMode = !g_godMode;   // 无参数则切换
        }
        ShowChat(g_godMode ? "[指令助手] 无敌模式已开启" : "[指令助手] 无敌模式已关闭");
        return;
    }
}

/** Hook: 单机聊天消息处理 */
void Hook_ProcessIncomingMessage(TEFMod::TerrariaInstance processor,
                                 TEFMod::TerrariaInstance message, int clientId) {
    if (!message) return;
    const int myPlayer = g_mGetMyPlayer ? g_mGetMyPlayer->Call(nullptr, 0) : -1;
    if (myPlayer < 0 || clientId != myPlayer) return;   // 只处理本地玩家

    const std::string text = ReadCSharpString(
            *reinterpret_cast<void**>(reinterpret_cast<char*>(message) + kMsgTextOffset));
    if (text.empty()) return;

    if (g_log) g_log->i("ChatCommands", "msg[", clientId, "]=", text);

    if (text.rfind("/get", 0) == 0 || text.rfind("/give", 0) == 0 ||
        text.rfind("/sp", 0) == 0 || text.rfind("/god", 0) == 0) {
        HandleCommand(text);
        // post-hook 无法阻止原版默认命令先回显, 但置位无副作用
        bool* consumed = reinterpret_cast<bool*>(reinterpret_cast<char*>(message) + kMsgConsumed);
        *consumed = true;
    }
}

/** Hook: 多人客户端聊天消息处理 */
void Hook_SendChatMessageFromClient(TEFMod::TerrariaInstance message) {
    if (!message) return;
    const std::string text = ReadCSharpString(
            *reinterpret_cast<void**>(reinterpret_cast<char*>(message) + kMsgTextOffset));
    if (text.empty()) return;
    if (text.rfind("/get", 0) == 0 || text.rfind("/give", 0) == 0 ||
        text.rfind("/sp", 0) == 0 || text.rfind("/god", 0) == 0) {
        HandleCommand(text);
    }
}

/** Hook: 每帧锁定血量(备份保险, 正常情况下 creativeGodMode 已免疫所有伤害) */
void Hook_PlayerFrame(TEFMod::TerrariaInstance player) {
    if (!g_godMode) return;
    if (!g_fStatLife || !g_fStatLifeMax) return;
    if (g_fWhoAmI) {
        const int my = g_mGetMyPlayer ? g_mGetMyPlayer->Call(nullptr, 0) : -1;
        if (my >= 0 && g_fWhoAmI->Get(player) != my) return;   // 只锁本地玩家
    }
    g_fStatLife->Set(g_fStatLifeMax->Get(player), player);
}

/**
 * Hook: 每帧维持 creativeGodMode(旅途模式无敌)
 * ResetEffects 每帧会把 creativeGodMode 重置为 false (Player.cs:18607),
 * 因此在这里(原版之后)重新置为 true。置位后:
 *   - Player.Hurt 直接返回 0.0 (Player.cs:37595) —— 免疫一切伤害
 *   - Player.KillMe 直接返回 (Player.cs:38199) —— 永不死亡
 *   - Update_NPCCollision 跳过 (Player.cs:30863) —— 免疫 NPC 接触伤害
 *   与旅途模式 GodmodePower 效果完全一致, 且无需 Hook 带返回值的 Hurt。
 */
void Hook_ResetEffects(TEFMod::TerrariaInstance player) {
    if (!g_godMode) return;
    if (!g_fCreativeGod) return;
    g_fCreativeGod->Set(true, player);
}

// ============ Mod 主体 ============
class ChatCommands final : public EFMod {
public:
    int Initialize(const std::string &path, MultiChannel *multiChannel) override {
        return 0;
    }

    int UnLoad(const std::string &path, MultiChannel *multiChannel) override {
        g_godMode = false;
        return 0;
    }

    int Load(const std::string &path, MultiChannel* channel) override {
        g_log = channel->receive<TEFMod::Logger*(*)(const std::string&, const std::string&, const std::size_t)>(
                "TEFMod::CreateLogger")("ChatCommands", "", 0);
        g_api = channel->receive<TEFMod::TEFModAPI*>("TEFMod::TEFModAPI");
        if (!g_api) return 1;
        g_log->init();
        g_log->i("ChatCommands", "mod loaded");
        return 0;
    }

    void Send(const std::string &path, MultiChannel* channel) override {
        // Hook: 聊天消息(单机 / 多人客户端)与玩家每帧逻辑(无敌模式)
        // 注意: ChatCommandProcessor / ChatHelper 的命名空间是 Terraria.Chat(非 Terraria)
        g_api->registerFunctionDescriptor({
                "Terraria.Chat", "ChatCommandProcessor", "ProcessIncomingMessage", "hook>>void", 2,
                &g_hookProcessIncoming, { reinterpret_cast<void*>(Hook_ProcessIncomingMessage) }
        });
        g_api->registerFunctionDescriptor({
                "Terraria.Chat", "ChatHelper", "SendChatMessageFromClient", "hook>>void", 1,
                &g_hookSendChatFromClient, { reinterpret_cast<void*>(Hook_SendChatMessageFromClient) }
        });
        g_api->registerFunctionDescriptor({
                "Terraria", "Player", "PlayerFrame", "hook>>void", 0,
                &g_hookPlayerFrame, { reinterpret_cast<void*>(Hook_PlayerFrame) }
        });
        g_api->registerFunctionDescriptor({
                "Terraria", "Player", "ResetEffects", "hook>>void", 0,
                &g_hookResetEffects, { reinterpret_cast<void*>(Hook_ResetEffects) }
        });

        // 字段
        g_api->registerApiDescriptor({"Terraria", "Player", "statLife", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Player", "statLifeMax", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Main", "player", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Entity", "whoAmI", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Player", "creativeGodMode", "Field"});

        // 方法
        g_api->registerApiDescriptor({"Terraria", "Main", "get_myPlayer", "Method", 0});
        g_api->registerApiDescriptor({"Terraria", "Item", "NewItem", "Method", 10});
        g_api->registerApiDescriptor({"Terraria", "NPC", "NewNPC", "Method", 10});
        g_api->registerApiDescriptor({"Terraria", "Main", "NewText", "Method", 4});
    }

    void Receive(const std::string &path, MultiChannel* channel) override {
        // 解析器
        g_parseIntField = channel->receive<TEFMod::Field<int>*(*)(void*)>(
                "TEFMod::Field<Int>::ParseFromPointer");
        g_parseBoolField = channel->receive<TEFMod::Field<bool>*(*)(void*)>(
                "TEFMod::Field<Bool>::ParseFromPointer");
        g_parseObjField = channel->receive<TEFMod::Field<void*>*(*)(void*)>(
                "TEFMod::Field<Other>::ParseFromPointer");
        g_parseObjArray = channel->receive<TEFMod::Array<void*>*(*)(void*)>(
                "TEFMod::Array<Other>::ParseFromPointer");
        g_parseIntMethod = channel->receive<TEFMod::Method<int>*(*)(void*)>(
                "TEFMod::Method<Int>::ParseFromPointer");
        g_parseVoidMethod = channel->receive<TEFMod::Method<void>*(*)(void*)>(
                "TEFMod::Method<Void>::ParseFromPointer");

        // 原版函数
        g_original_ProcessIncomingMessage = g_api->GetAPI<void(*)(TEFMod::TerrariaInstance, TEFMod::TerrariaInstance, int)>({
                "Terraria.Chat", "ChatCommandProcessor", "ProcessIncomingMessage", "old_fun", 2
        });
        g_original_SendChatMessageFromClient = g_api->GetAPI<void(*)(TEFMod::TerrariaInstance)>({
                "Terraria.Chat", "ChatHelper", "SendChatMessageFromClient", "old_fun", 1
        });
        g_original_PlayerFrame = g_api->GetAPI<void(*)(TEFMod::TerrariaInstance)>({
                "Terraria", "Player", "PlayerFrame", "old_fun", 0
        });
        g_original_ResetEffects = g_api->GetAPI<void(*)(TEFMod::TerrariaInstance)>({
                "Terraria", "Player", "ResetEffects", "old_fun", 0
        });

        // 字段
        g_fStatLife = g_parseIntField ? g_parseIntField(g_api->GetAPI<void*>({"Terraria", "Player", "statLife", "Field"})) : nullptr;
        g_fStatLifeMax = g_parseIntField ? g_parseIntField(g_api->GetAPI<void*>({"Terraria", "Player", "statLifeMax", "Field"})) : nullptr;
        g_fMainPlayer = g_parseObjField ? g_parseObjField(g_api->GetAPI<void*>({"Terraria", "Main", "player", "Field"})) : nullptr;
        g_fWhoAmI = g_parseIntField ? g_parseIntField(g_api->GetAPI<void*>({"Terraria", "Entity", "whoAmI", "Field"})) : nullptr;
        g_fCreativeGod = g_parseBoolField ? g_parseBoolField(g_api->GetAPI<void*>({"Terraria", "Player", "creativeGodMode", "Field"})) : nullptr;

        // 方法
        g_mGetMyPlayer = g_parseIntMethod ? g_parseIntMethod(g_api->GetAPI<void*>({"Terraria", "Main", "get_myPlayer", "Method", 0})) : nullptr;
        g_mNewItem = g_parseIntMethod ? g_parseIntMethod(g_api->GetAPI<void*>({"Terraria", "Item", "NewItem", "Method", 10})) : nullptr;
        g_mNewNPC = g_parseIntMethod ? g_parseIntMethod(g_api->GetAPI<void*>({"Terraria", "NPC", "NewNPC", "Method", 10})) : nullptr;
        g_mNewText = g_parseVoidMethod ? g_parseVoidMethod(g_api->GetAPI<void*>({"Terraria", "Main", "NewText", "Method", 4})) : nullptr;

        if (g_log) {
            g_log->i("ChatCommands", "hooks: processIn=", (void*)g_original_ProcessIncomingMessage,
                     " sendChat=", (void*)g_original_SendChatMessageFromClient,
                     " frame=", (void*)g_original_PlayerFrame,
                     " resetFx=", (void*)g_original_ResetEffects);
            g_log->i("ChatCommands", "fields: statLife=", (void*)g_fStatLife,
                     " statLifeMax=", (void*)g_fStatLifeMax,
                     " mainPlayer=", (void*)g_fMainPlayer,
                     " whoAmI=", (void*)g_fWhoAmI,
                     " creativeGod=", (void*)g_fCreativeGod);
            g_log->i("ChatCommands", "methods: myPlayer=", (void*)g_mGetMyPlayer,
                     " newItem=", (void*)g_mNewItem,
                     " newNPC=", (void*)g_mNewNPC,
                     " newText=", (void*)g_mNewText);
        }
    }

    Metadata GetMetadata() override {
        return {
                "ChatCommands",  // Mod名称(英文标识)
                "lzup",          // 作者
                "1.0.0",         // 版本
                20250517,        // 标准规范(与经典EFMod API一致)
                ModuleType::Game,
                { false }
        };
    }
};

EFMod* CreateMod() {
    static ChatCommands instance;
    return &instance;
}
