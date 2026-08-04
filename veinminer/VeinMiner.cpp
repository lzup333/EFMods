//
// VeinMiner - 连锁挖矿
// ClassicEFMod 重写版，适配 Terraria 1.4.5.6.4 (手机端 / PE)
//
// 功能: 破坏矿石/宝石方块时, 自动连锁破坏与其同类型、且相连的所有矿石/宝石。
//       例如挖掉一块铁矿, 会把连在一起的整条铁矿脉全部挖掉; 挖石头/泥土不会连锁。
//
// 连锁类型列表参考 Terraria Wiki "Ores"/"Gems" 页:
//   矿石 Tile ID: 6-9, 22, 37, 56, 58, 107-108, 111, 166-169, 204, 211, 221-223, 408
//   宝石 Tile ID: 63-68 (Sapphire/Ruby/Emerald/Topaz/Amethyst/Diamond),
//                 178 (ExposedGems) 与 566 (AmberStoneBlock, 沙漠琥珀)
// 并与 PC 端源码 Source/Terraria/ID/TileID.cs 逐一核对一致(手机端 1.4.5.6.4 同款)。
//
// 实现要点(结合 PE/PC 源码):
//   1. 挖矿路径: PC 端 Player.PickTile -> WorldGen.KillTile (Source/Terraria/Player.cs:52839)
//      PE 端同样由 WorldGen.KillTile(int i, int j, bool fail, bool effectOnly, bool noItem)
//      实际执行方块破坏 (pe/dump.cs:75602), 因此 Hook 此处。
//   2. 方块类型读取: PE 端(1.4.5.6.4) Tile 是 struct 而非 PC 的 class,
//      方块数据被拆成若干原生数组 (TileData 类, pe/dump.cs:70223):
//        Main.maxTilesX/maxTilesY -> int 世界尺寸(坐标查找的宽高)
//        TileData.TileLookup      -> uint*  坐标->tile索引 (0xFFFFFFFF 表示空)
//        TileData.TileType        -> ushort* tile索引->方块类型
//      因此 GetTileType(x,y) = TileType[TileLookup[y*maxTilesX+x]]。
//      注意: 不能用 Field<void*> 读静态 struct 字段 TileData.Data,
//            其 Get() 只返回结构体前 8 字节, 会得到野指针导致崩溃。
//   3. 只有最外层 KillTile(玩家挖的那一下)才触发连锁, 使用深度计数器防重入。
//

#include "efmod_core.hpp"
#include "TEFMod.hpp"
#include "BaseType.hpp"
#include "Logger.hpp"

#include <cstdint>
#include <cstring>

// 全局组件
TEFMod::Logger* g_log;
TEFMod::TEFModAPI* g_api;

// 字段解析器
static TEFMod::Field<void*>* (*ParseObjField)(void*);
static TEFMod::Field<int>* (*ParseIntField)(void*);

/*
 * PE 端方块数据(由 dump.cs 推断, 均为静态字段)
 * 注意: 不能用 Field<void*> 读取静态 struct 字段(如 TileData.Data),
 *       加载器的 GetStaticValue 只取前 8 字节, 会得到野指针导致崩溃,
 *       因此世界尺寸改用 Main.maxTilesX/maxTilesY(int 静态字段, 取值正确)。
 */
static TEFMod::Field<int>*    g_pMaxTilesX;   // Main.maxTilesX       (int)
static TEFMod::Field<int>*    g_pMaxTilesY;   // Main.maxTilesY       (int)
static TEFMod::Field<void*>*  g_pTileLookup;  // TileData.TileLookup  (uint*)
static TEFMod::Field<void*>*  g_pTileType;    // TileData.TileType    (ushort*)

/*
 * 矿石方块类型表
 * 参考 Terraria Wiki "Ores" 页 Tile ID, 与 PC 源码 ID/TileID.cs 核对:
 *   Copper=7 Iron=6 Silver=9 Gold=8 Tin=166 Lead=167 Tungsten=168 Platinum=169
 *   Demonite=22 Crimtane=204 Meteorite=37 Obsidian=56 Hellstone=58
 *   Cobalt=107 Palladium=221 Mythril=108 Orichalcum=222 Adamantite=111 Titanium=223
 *   Chlorophyte=211 Luminite=408
 */
static constexpr uint16_t kOreTypes[] = {
        6, 7, 8, 9, 22, 37, 56, 58,
        107, 108, 111,
        166, 167, 168, 169,
        204, 211, 221, 222, 223,
        408
};
static constexpr int kOreCount = static_cast<int>(sizeof(kOreTypes) / sizeof(kOreTypes[0]));

/*
 * 宝石方块类型表
 * 参考 Terraria Wiki "Gems" 页, 与 PC 源码 ID/TileID.cs 核对:
 *   63=Sapphire 64=Ruby 65=Emerald 66=Topaz 67=Amethyst 68=Diamond (洞窟中开采的宝石)
 *   178=ExposedGems (露出的放置宝石, 即 Wiki Gems 页标注的 Tile ID 178)
 *   566=AmberStoneBlock (琥珀石, 地下沙漠生成的琥珀, 掉落 item 999; PC 掉落 case 566)
 */
static constexpr uint16_t kGemTypes[] = {
        63, 64, 65, 66, 67, 68, 178, 566
};
static constexpr int kGemCount = static_cast<int>(sizeof(kGemTypes) / sizeof(kGemTypes[0]));

/** 判断方块类型是否为矿石或宝石 */
static bool IsVeinType(uint16_t type) {
    for (int i = 0; i < kOreCount; ++i) {
        if (kOreTypes[i] == type) return true;
    }
    for (int i = 0; i < kGemCount; ++i) {
        if (kGemTypes[i] == type) return true;
    }
    return false;
}

/*
 * 原版 KillTile
 */
static void (*g_original_KillTile)(int i, int j, bool fail, bool effectOnly, bool noItem);

// 连锁上限与搜索半径
static constexpr int kMaxChainTiles = 256;
static constexpr int kChainRadius    = 32;
static constexpr uint32_t kNoTile    = 0xFFFFFFFFu;   // TileData.TileBufferNoEntry

// 重入深度计数: 0 = 无调用, 1 = 最外层 KillTile
static int g_killDepth = 0;
// 最外层 KillTile 所破坏方块的类型(由转发函数在调用原版前捕获)
static uint16_t g_lastKilledType = 0;

// Hook 转发函数声明
void KillTile_T(int i, int j, bool fail, bool effectOnly, bool noItem);

// Hook 模板
inline TEFMod::HookTemplate HookTemplate_KillTile {
        reinterpret_cast<void*>(KillTile_T),
        {}
};

// 四个方向的偏移
static const int kDx[4] = { 0, 0, -1, 1 };
static const int kDy[4] = { -1, 1, 0, 0 };

/**
 * 读取指定坐标的方块类型(PE 端 TileData 原生数组方案)
 * @param x 世界坐标 X
 * @param y 世界坐标 Y
 * @return 方块类型(0 表示空/越界/读取失败)
 */
static uint16_t GetTileType(int x, int y) {
    if (!g_pMaxTilesX || !g_pMaxTilesY || !g_pTileLookup || !g_pTileType) return 0;

    const int maxX = g_pMaxTilesX->Get();
    const int maxY = g_pMaxTilesY->Get();
    if (maxX <= 0 || maxY <= 0) return 0;
    if (x < 0 || y < 0 || x >= maxX || y >= maxY) return 0;

    const uint32_t* lookup = reinterpret_cast<const uint32_t*>(g_pTileLookup->Get());
    const uint16_t* types  = reinterpret_cast<const uint16_t*>(g_pTileType->Get());
    if (!lookup || !types) return 0;

    const uint32_t tileIndex = lookup[y * maxX + x];
    if (tileIndex == kNoTile) return 0;

    return types[tileIndex];
}

/**
 * 转发函数: 先捕获被挖方块的类型, 再调用原版, 最后执行所有注册的钩子
 */
void KillTile_T(int i, int j, bool fail, bool effectOnly, bool noItem) {
    const int depth = g_killDepth++;

    // 仅在最外层调用时捕获类型(此时方块还没被销毁)
    if (depth == 0) {
        g_lastKilledType = GetTileType(i, j);
    }

    if (g_original_KillTile) g_original_KillTile(i, j, fail, effectOnly, noItem);

    for (const auto fun : HookTemplate_KillTile.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(int, int, bool, bool, bool)>(fun)(i, j, fail, effectOnly, noItem);
    }

    --g_killDepth;
}

/**
 * 实际连锁逻辑: 以 (ox, oy) 为中心做广度优先搜索,
 * 将与 origin 同类型的相连矿石/宝石用原版 KillTile 全部破坏
 */
static void ChainMine(int ox, int oy, uint16_t type, bool noItem) {
    if (!g_original_KillTile) return;

    struct Pos { int x; int y; };
    Pos queue[kMaxChainTiles];
    int count = 0;

    queue[count++] = { ox, oy };

    for (int visited = 0; visited < count; ++visited) {
        const Pos cur = queue[visited];

        for (int dir = 0; dir < 4; ++dir) {
            const int nx = cur.x + kDx[dir];
            const int ny = cur.y + kDy[dir];

            // 限制搜索半径, 防止误挖远处方块
            if (nx < ox - kChainRadius || nx > ox + kChainRadius) continue;
            if (ny < oy - kChainRadius || ny > oy + kChainRadius) continue;

            // 已访问检查(防止死循环)
            bool seen = false;
            for (int v = 0; v < count; ++v) {
                if (queue[v].x == nx && queue[v].y == ny) { seen = true; break; }
            }
            if (seen) continue;

            // 必须与目标类型一致
            if (GetTileType(nx, ny) != type) continue;

            if (count >= kMaxChainTiles) return;   // 达到连锁上限

            queue[count++] = { nx, ny };
            g_original_KillTile(nx, ny, false, false, noItem);
        }
    }

    if (g_log) g_log->d("VeinMiner", "chain done, total=", count);
}

/**
 * Hook 逻辑: 仅处理最外层成功的方块破坏, 并过滤非实心方块
 */
void Hook_KillTile(int i, int j, bool fail, bool effectOnly, bool noItem) {
    // 仅最外层触发, 嵌套/重入调用直接跳过
    if (g_killDepth != 1) return;
    // 没有真正破坏方块(fail / 纯特效)则跳过
    if (fail || effectOnly) return;

    const uint16_t type = g_lastKilledType;
    // 类型 0 表示空气/读取失败
    if (type == 0 || type >= 4096) return;

    // 诊断日志: 记录被挖方块的类型
    if (g_log) g_log->d("VeinMiner", "kill (", i, ",", j, ") type=", (int)type);

    // 只连锁矿石与宝石(Wiki 类型表), 挖石头/泥土/家具等不会连锁
    if (!IsVeinType(type)) return;

    if (g_log) g_log->d("VeinMiner", "start chain at (", i, ",", j, ") type=", (int)type);

    ChainMine(i, j, type, noItem);
}

/**
 * 连锁挖矿核心实现
 */
class VeinMiner final : public EFMod {
public:
    int Initialize(const std::string &path, MultiChannel *multiChannel) override {
        return 0;
    }

    int UnLoad(const std::string &path, MultiChannel *multiChannel) override {
        return 0;
    }

    int Load(const std::string &path, MultiChannel* channel) override {
        g_log = channel->receive<TEFMod::Logger*(*)(const std::string&, const std::string&, const std::size_t)>(
                "TEFMod::CreateLogger")("VeinMiner", "", 0);
        g_api = channel->receive<TEFMod::TEFModAPI*>("TEFMod::TEFModAPI");
        if (!g_api) return 1;
        g_log->init();
        g_log->i("VeinMiner", "mod loaded");
        return 0;
    }

    void Send(const std::string &path, MultiChannel* channel) override {
        // Hook WorldGen.KillTile(静态方法, 5 个参数)
        g_api->registerFunctionDescriptor({
                "Terraria",
                "WorldGen",
                "KillTile",
                "hook>>void",
                5,
                &HookTemplate_KillTile,
                { reinterpret_cast<void*>(Hook_KillTile) }
        });

        // PE 端方块数据字段(1.4.5.6.4, dump.cs)
        // 世界尺寸: Main.maxTilesX/maxTilesY (int 静态字段)
        g_api->registerApiDescriptor({"Terraria", "Main", "maxTilesX", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Main", "maxTilesY", "Field"});
        // 方块数据指针: TileData.TileLookup / TileType (dump.cs:70236/70244)
        g_api->registerApiDescriptor({"Terraria", "TileData", "TileLookup", "Field"});
        g_api->registerApiDescriptor({"Terraria", "TileData", "TileType", "Field"});
    }

    void Receive(const std::string &path, MultiChannel* channel) override {
        // 获取字段解析器
        ParseObjField = channel->receive<TEFMod::Field<void*>*(*)(void*)>(
                "TEFMod::Field<Other>::ParseFromPointer");
        ParseIntField = channel->receive<TEFMod::Field<int>*(*)(void*)>(
                "TEFMod::Field<Int>::ParseFromPointer");

        // 获取原版 KillTile
        g_original_KillTile = g_api->GetAPI<void(*)(int, int, bool, bool, bool)>({
                "Terraria", "WorldGen", "KillTile", "old_fun", 5
        });

        // 解析世界尺寸
        g_pMaxTilesX = ParseIntField(g_api->GetAPI<void*>({
                "Terraria", "Main", "maxTilesX", "Field"
        }));
        g_pMaxTilesY = ParseIntField(g_api->GetAPI<void*>({
                "Terraria", "Main", "maxTilesY", "Field"
        }));

        // 解析 PE 端方块数据指针字段
        g_pTileLookup = ParseObjField(g_api->GetAPI<void*>({
                "Terraria", "TileData", "TileLookup", "Field"
        }));
        g_pTileType = ParseObjField(g_api->GetAPI<void*>({
                "Terraria", "TileData", "TileType", "Field"
        }));

        if (g_log) {
            g_log->i("VeinMiner", "origKillTile=", (void*)g_original_KillTile);
            g_log->i("VeinMiner", "maxTilesX=", (void*)g_pMaxTilesX,
                     " maxTilesY=", (void*)g_pMaxTilesY);
            g_log->i("VeinMiner", "lookup=", (void*)g_pTileLookup,
                     " type=", (void*)g_pTileType);
        }
    }

    Metadata GetMetadata() override {
        return {
                "VeinMiner",  // Mod名称(英文标识)
                "lzup",       // 作者
                "1.0.0",      // 版本
                20250711,     // 标准规范(与经典EFMod API一致)
                ModuleType::Game,
                { false }
        };
    }
};

EFMod* CreateMod() {
    static VeinMiner instance;
    return &instance;
}
