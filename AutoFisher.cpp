//
// AutoFisher - 自动钓鱼
// ClassicEFMod 重写版，适配 Terraria 1.4.5.6.4 (手机端 / PE)
//
// 功能:
//   1. 自动收杆: 鱼上钩(浮标 ai[1] 变为负数)后自动收线, 无需点击。
//   2. 自动甩杆: 手持鱼竿且没有浮标在场上时自动抛竿(进入钓鱼状态后生效)。
//   3. 循环钓鱼: 两者结合实现全自动挂机钓鱼。
//
// 实现要点(结合 PE 1.4.5.6.4 dump.cs 与 PC 1.4.5.6 源码):
//   1. 上钩判定: 浮标在等待计时结束后调用 Projectile.FishingCheck() 掷判定。
//      成功上钩时(掉鱼/宝箱/杂物/血月怪物/松露虫)会把 ai[1] 置为负数,
//      localAI[1] 存捕获内容(物品 ID 为正、NPC 为负) (Projectile.cs:19333/19354)。
//      因此 Hook 这个公开方法: 原版运行后若该浮标 ai[1] < 0 且 localAI[1] != 0,
//      即判定"鱼已上钩"。
//   2. 收杆: 玩家收线时 ItemCheck_PullFishingBobbers 会把 ai[1] 转正、
//      浮标飞回玩家身边并在 Kill 时把鱼交给玩家 (Projectile.cs:68736)。
//      因此本 Mod 在检测到上钩后, 模拟按一次使用键(controlUseItem=true +
//      重新调用原版 ItemCheck), 让原版逻辑完整走一遍收线 + 交鱼。
//   3. 抛竿: 手持鱼竿且场上没有浮标时, 同样的"模拟按键"会让原版抛出新浮标。
//   4. 钓鱼状态: 玩家手动抛过一次竿(场上出现自己的浮标)后进入自动循环,
//      切走鱼竿则退出。
//
// 注意:
//   - 只作用于本地玩家 (通过 Main.get_myPlayer 与 Player.whoAmI 判定)。
//   - 浮标 ai/localAI 是内联 struct, 必须按对象内偏移直接读写, 不能用 Field<void*>。
//   - 所有解析器/字段/方法指针均做了空指针保护, 解析失败时对应功能自动降级,
//     不会崩溃。请查看日志确认各解析是否成功。

#include "efmod_core.hpp"
#include "TEFMod.hpp"
#include "BaseType.hpp"
#include "Logger.hpp"

#include <cstdint>

// ============ 全局组件 ============
TEFMod::Logger* g_log = nullptr;
TEFMod::TEFModAPI* g_api = nullptr;

// ============ 解析器 ============
static TEFMod::Field<int>*    (*g_parseIntField)(void*);
static TEFMod::Field<bool>*   (*g_parseBoolField)(void*);
static TEFMod::Field<void*>*  (*g_parseObjField)(void*);
static TEFMod::Array<void*>*  (*g_parseObjArray)(void*);
static TEFMod::Method<int>*   (*g_parseIntMethod)(void*);

// ============ 字段 ============
static TEFMod::Field<int>*    g_fWhoAmI;          // Entity.whoAmI        (int)
static TEFMod::Field<void*>*  g_fMainProjectile;  // Main.projectile      (Projectile[])
static TEFMod::Field<bool>*   g_fProjActive;      // Projectile.active    (bool)
static TEFMod::Field<int>*    g_fProjOwner;       // Projectile.owner     (int)
static TEFMod::Field<int>*    g_fProjAiStyle;     // Projectile.aiStyle   (int)
static TEFMod::Field<void*>*  g_fPlayerInventory; // Player.inventory     (Item[])
static TEFMod::Field<bool>*   g_fControlUseItem;  // Player.controlUseItem(bool)
static TEFMod::Field<int>*    g_fItemAnimation;   // Player.itemAnimation (int)
static TEFMod::Field<int>*    g_fItemFishingPole; // Item.fishingPole     (int)

// ============ 方法 ============
static TEFMod::Method<int>*   g_mGetMyPlayer;     // Main.get_myPlayer    (static int)
static TEFMod::Method<int>*   g_mGetSelectedItem; // Player.get_selectedItem (int)

// ============ 原版函数 ============
static void (*g_original_ItemCheck)(TEFMod::TerrariaInstance);
static void (*g_original_FishingCheck)(TEFMod::TerrariaInstance);

// ============ Hook 模板 ============
void ItemCheck_T(TEFMod::TerrariaInstance i);
void FishingCheck_T(TEFMod::TerrariaInstance i);

inline TEFMod::HookTemplate g_hookItemCheck {
        reinterpret_cast<void*>(ItemCheck_T),
        {}
};

inline TEFMod::HookTemplate g_hookFishingCheck {
        reinterpret_cast<void*>(FishingCheck_T),
        {}
};

// ============ 状态 ============
static bool g_fishing      = false;  // 自动钓鱼会话是否激活
static bool g_bitePending  = false;  // 有鱼上钩待收线
static bool g_inItemCheck  = false;  // ItemCheck 重入保护
static int  g_castCooldown = 0;      // 抛竿冷却帧数

// 已解析数组的缓存(避免每帧重新 Parse 造成泄漏)
static void*          g_projRaw    = nullptr;  // Main.projectile 原始指针
static TEFMod::Array<void*>* g_projArray = nullptr;
static void*          g_invPlayer  = nullptr;  // 缓存的玩家实例
static void*          g_invRaw     = nullptr;  // Player.inventory 原始指针
static TEFMod::Array<void*>* g_invArray = nullptr;

/*
 * 浮标数据说明(PE 1.4.5.6.4, dump.cs):
 *   Projectile.ai / localAI 的类型是 Float_FixedArray_3,
 *   一个内联 struct(3 个 float 直接嵌在对象里, ai 位于偏移 0x74, 占 12 字节)。
 *   注意: 不能用 Field<void*> 读取它 —— Get() 会把前 8 字节(ai[0]、ai[1])当作指针,
 *   得到野指针导致崩溃(与 VeinMiner 中 TileData.Data 是同类坑)。
 *   因此这里直接按 dump.cs 给出的对象内偏移访问浮标 ai/localAI 数组。
 *
 *   上钩语义(与 PC 1.4.5.6 Projectile.FishingCheck / SetFishingCheckResults 一致):
 *     成功上钩时(掉鱼/宝箱/杂物/血月怪物/松露虫)会把 ai[1] 置为负数,
 *     localAI[1] 存捕获内容(物品 ID 为正, NPC 为负), 然后等待玩家收线。
 *     因此"鱼已上钩"的判定是 ai[1] < 0(而非正数)。
 */
static constexpr std::size_t kProjAiOffset     = 0x74;  // Projectile.ai     (Float_FixedArray_3)
static constexpr std::size_t kProjLocalAiOffset = 0x80; // Projectile.localAI (Float_FixedArray_3)

// ============ 转发函数 ============
void ItemCheck_T(TEFMod::TerrariaInstance i) {
    if (g_original_ItemCheck) g_original_ItemCheck(i);
    for (const auto fun : g_hookItemCheck.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(TEFMod::TerrariaInstance)>(fun)(i);
    }
}

void FishingCheck_T(TEFMod::TerrariaInstance i) {
    if (g_original_FishingCheck) g_original_FishingCheck(i);
    for (const auto fun : g_hookFishingCheck.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(TEFMod::TerrariaInstance)>(fun)(i);
    }
}

// ============ 工具函数 ============

/** 获取本地玩家编号; 失败返回 -1 */
static int LocalPlayer() {
    if (!g_mGetMyPlayer) return -1;
    return g_mGetMyPlayer->Call(nullptr, 0);
}

/** 获取 Main.projectile 数组(静态字段, 按原始指针缓存) */
static TEFMod::Array<void*>* MainProjectileArray() {
    if (!g_fMainProjectile || !g_parseObjArray) return nullptr;
    void* raw = g_fMainProjectile->Get();
    if (raw != g_projRaw) {
        g_projRaw = raw;
        g_projArray = g_parseObjArray(raw);
    }
    return g_projArray;
}

/** 获取指定玩家的物品栏数组(按玩家实例缓存) */
static TEFMod::Array<void*>* PlayerInventoryArray(TEFMod::TerrariaInstance player) {
    if (!g_fPlayerInventory || !g_parseObjArray) return nullptr;
    if (player != g_invPlayer) {
        g_invPlayer = player;
        g_invRaw = g_fPlayerInventory->Get(player);
        g_invArray = g_parseObjArray(g_invRaw);
    }
    return g_invArray;
}

/** 获取浮标的 ai 数组首元素地址(直接按对象内偏移访问内联 struct) */
static float* BobberAi(TEFMod::TerrariaInstance proj) {
    if (!proj) return nullptr;
    return reinterpret_cast<float*>(reinterpret_cast<char*>(proj) + kProjAiOffset);
}

/** 获取浮标的 localAI 数组首元素地址 */
static float* BobberLocalAi(TEFMod::TerrariaInstance proj) {
    if (!proj) return nullptr;
    return reinterpret_cast<float*>(reinterpret_cast<char*>(proj) + kProjLocalAiOffset);
}

/** 判断某个弹幕是否为本地玩家的浮标 */
static bool IsLocalBobber(void* p, int myPlayer) {
    if (!p) return false;
    if (g_fProjAiStyle && g_fProjAiStyle->Get(p) != 61) return false;
    if (g_fProjOwner && g_fProjOwner->Get(p) != myPlayer) return false;
    if (g_fProjActive && !g_fProjActive->Get(p)) return false;
    return true;
}

/** 统计本地玩家在场上的浮标数量 */
static int CountLocalBobbers(int myPlayer) {
    auto projs = MainProjectileArray();
    if (!projs) return 0;
    const int n = static_cast<int>(projs->Size());
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (IsLocalBobber(projs->at(i), myPlayer)) ++count;
    }
    return count;
}

// ============ Hook: 自动收杆 ============
// ============ Hook: 检测上钩 ============
// Projectile.FishingCheck 在每次钓鱼判定时调用(仅本地玩家浮标)。
// 原版运行后若成功上钩, 该浮标 ai[1] 会被置为负数、localAI[1] 存捕获内容。
void Hook_FishingCheck(TEFMod::TerrariaInstance proj) {
    // 只处理本地玩家的浮标
    const int myPlayer = LocalPlayer();
    if (myPlayer < 0) return;
    if (g_fProjOwner && g_fProjOwner->Get(proj) != myPlayer) return;

    // 直接按对象内偏移读取 ai/localAI(不能用 Field<void*> 读 struct 字段)
    float* ai = BobberAi(proj);
    float* lai = BobberLocalAi(proj);
    if (!ai || !lai) return;

    // 鱼已上钩: ai[1] 为负数且 localAI[1] 非空
    if (ai[1] < 0.0f && lai[1] != 0.0f) {
        g_bitePending = true;
        g_fishing = true;
        if (g_log) g_log->d("AutoFisher", "bite! item=", (int)lai[1]);
    }
}

// ============ 模拟按下使用键 ============
// 置 controlUseItem=true 并重新调用原版 ItemCheck, 等价于真实按一次使用键。
// 场上无浮标时会抛竿; 场上有浮标且鱼已上钩时会自动收线并交鱼(原版逻辑)。
static void PressUse(TEFMod::TerrariaInstance player) {
    g_fControlUseItem->Set(true, player);
    g_inItemCheck = true;
    g_original_ItemCheck(player);
    g_inItemCheck = false;
    g_fControlUseItem->Set(false, player);
}

// ============ Hook: 自动甩杆 / 自动收杆 ============
// Player.ItemCheck 每帧对每个玩家调用; 仅处理本地玩家。
void Hook_ItemCheck(TEFMod::TerrariaInstance player) {
    if (g_inItemCheck) return;
    if (!g_original_ItemCheck) return;

    const int myPlayer = LocalPlayer();
    if (myPlayer < 0) return;
    if (g_fWhoAmI && g_fWhoAmI->Get(player) != myPlayer) return;

    // 读取手持物品, 判断是否为鱼竿
    if (!g_mGetSelectedItem || !g_fItemFishingPole || !g_fControlUseItem) return;
    const int sel = g_mGetSelectedItem->Call(player, 0);
    auto inv = PlayerInventoryArray(player);
    if (!inv || sel < 0 || sel >= static_cast<int>(inv->Size())) return;
    void* item = inv->at(sel);
    if (!item) return;

    const int pole = g_fItemFishingPole->Get(item);
    if (pole <= 0) {
        // 手上没有鱼竿: 结束自动钓鱼会话
        g_fishing = false;
        g_bitePending = false;
        return;
    }

    // 场上还有浮标
    if (CountLocalBobbers(myPlayer) > 0) {
        g_fishing = true;  // 玩家开始钓鱼了

        // 鱼已上钩(Hook_FishingCheck 置位): 模拟按一次使用键, 原版会自动收线并交鱼
        if (!g_bitePending) return;
        g_bitePending = false;
        if (g_fItemAnimation && g_fItemAnimation->Get(player) > 0) return;

        if (g_log) g_log->d("AutoFisher", "reel in!");
        PressUse(player);
        return;
    }

    // 没有浮标。只有进入钓鱼状态后才自动抛竿(避免走路时乱甩)
    if (!g_fishing) return;

    // 冷却 + 使用动画保护, 避免重复抛竿
    if (g_castCooldown > 0) { --g_castCooldown; return; }
    g_castCooldown = 15;
    if (g_fItemAnimation && g_fItemAnimation->Get(player) > 0) return;

    if (g_log) g_log->d("AutoFisher", "auto cast");
    PressUse(player);
}

// ============ Mod 主体 ============
class AutoFisher final : public EFMod {
public:
    int Initialize(const std::string &path, MultiChannel *multiChannel) override {
        return 0;
    }

    int UnLoad(const std::string &path, MultiChannel *multiChannel) override {
        return 0;
    }

    int Load(const std::string &path, MultiChannel* channel) override {
        g_log = channel->receive<TEFMod::Logger*(*)(const std::string&, const std::string&, const std::size_t)>(
                "TEFMod::CreateLogger")("AutoFisher", "", 0);
        g_api = channel->receive<TEFMod::TEFModAPI*>("TEFMod::TEFModAPI");
        if (!g_api) return 1;
        g_log->init();
        g_log->i("AutoFisher", "mod loaded");
        return 0;
    }

    void Send(const std::string &path, MultiChannel* channel) override {
        // Hook: Player.ItemCheck(自动甩杆/自动收杆) 与 Projectile.FishingCheck(检测上钩)
        g_api->registerFunctionDescriptor({
                "Terraria", "Player", "ItemCheck", "hook>>void", 0,
                &g_hookItemCheck, { reinterpret_cast<void*>(Hook_ItemCheck) }
        });
        g_api->registerFunctionDescriptor({
                "Terraria", "Projectile", "FishingCheck", "hook>>void", 0,
                &g_hookFishingCheck, { reinterpret_cast<void*>(Hook_FishingCheck) }
        });

        // 字段
        g_api->registerApiDescriptor({"Terraria", "Entity", "whoAmI", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Main", "projectile", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Projectile", "active", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Projectile", "owner", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Projectile", "aiStyle", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Player", "inventory", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Player", "controlUseItem", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Player", "itemAnimation", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Item", "fishingPole", "Field"});

        // 方法
        g_api->registerApiDescriptor({"Terraria", "Main", "get_myPlayer", "Method", 0});
        g_api->registerApiDescriptor({"Terraria", "Player", "get_selectedItem", "Method", 0});
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

        // 原版函数
        g_original_ItemCheck = g_api->GetAPI<void(*)(TEFMod::TerrariaInstance)>({
                "Terraria", "Player", "ItemCheck", "old_fun", 0
        });
        g_original_FishingCheck = g_api->GetAPI<void(*)(TEFMod::TerrariaInstance)>({
                "Terraria", "Projectile", "FishingCheck", "old_fun", 0
        });

        // 字段(解析器可能为空, 逐一判空, 失败仅降级对应功能)
        g_fWhoAmI = g_parseIntField  ? g_parseIntField(g_api->GetAPI<void*>({"Terraria", "Entity", "whoAmI", "Field"})) : nullptr;
        g_fMainProjectile = g_parseObjField ? g_parseObjField(g_api->GetAPI<void*>({"Terraria", "Main", "projectile", "Field"})) : nullptr;
        g_fProjActive = g_parseBoolField ? g_parseBoolField(g_api->GetAPI<void*>({"Terraria", "Projectile", "active", "Field"})) : nullptr;
        g_fProjOwner = g_parseIntField  ? g_parseIntField(g_api->GetAPI<void*>({"Terraria", "Projectile", "owner", "Field"})) : nullptr;
        g_fProjAiStyle = g_parseIntField ? g_parseIntField(g_api->GetAPI<void*>({"Terraria", "Projectile", "aiStyle", "Field"})) : nullptr;
        g_fPlayerInventory = g_parseObjField ? g_parseObjField(g_api->GetAPI<void*>({"Terraria", "Player", "inventory", "Field"})) : nullptr;
        g_fControlUseItem = g_parseBoolField ? g_parseBoolField(g_api->GetAPI<void*>({"Terraria", "Player", "controlUseItem", "Field"})) : nullptr;
        g_fItemAnimation = g_parseIntField ? g_parseIntField(g_api->GetAPI<void*>({"Terraria", "Player", "itemAnimation", "Field"})) : nullptr;
        g_fItemFishingPole = g_parseIntField ? g_parseIntField(g_api->GetAPI<void*>({"Terraria", "Item", "fishingPole", "Field"})) : nullptr;

        // 方法
        g_mGetMyPlayer = g_parseIntMethod ? g_parseIntMethod(g_api->GetAPI<void*>({"Terraria", "Main", "get_myPlayer", "Method", 0})) : nullptr;
        g_mGetSelectedItem = g_parseIntMethod ? g_parseIntMethod(g_api->GetAPI<void*>({"Terraria", "Player", "get_selectedItem", "Method", 0})) : nullptr;

        if (g_log) {
            g_log->i("AutoFisher", "parsers: int=", (void*)g_parseIntField,
                     " bool=", (void*)g_parseBoolField,
                     " obj=", (void*)g_parseObjField,
                     " oarr=", (void*)g_parseObjArray,
                     " imeth=", (void*)g_parseIntMethod);
            g_log->i("AutoFisher", "hooks: itemCheck=", (void*)g_original_ItemCheck,
                     " fishingCheck=", (void*)g_original_FishingCheck);
            g_log->i("AutoFisher", "fields: whoAmI=", (void*)g_fWhoAmI,
                     " projectile=", (void*)g_fMainProjectile,
                     " aiStyle=", (void*)g_fProjAiStyle,
                     " owner=", (void*)g_fProjOwner,
                     " inventory=", (void*)g_fPlayerInventory,
                     " controlUseItem=", (void*)g_fControlUseItem,
                     " fishingPole=", (void*)g_fItemFishingPole);
            g_log->i("AutoFisher", "methods: myPlayer=", (void*)g_mGetMyPlayer,
                     " selectedItem=", (void*)g_mGetSelectedItem);
        }
    }

    Metadata GetMetadata() override {
        return {
                "AutoFisher",  // Mod名称(英文标识)
                "lzup",        // 作者
                "1.0.0",       // 版本
                20250517,      // 标准规范(与经典EFMod API一致)
                ModuleType::Game,
                { false }
        };
    }
};

EFMod* CreateMod() {
    static AutoFisher instance;
    return &instance;
}
