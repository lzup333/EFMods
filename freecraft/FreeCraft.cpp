//
// FreeCraft - 自由合成 (无需对应材料 / 无需工作站)
// ClassicEFMod 重写版，适配 Terraria 1.4.5.6.4 (手机端 / PE)
//
// 功能:
//   1. 无需工作站: 所有配方的 requiredTile/needWater/needHoney/needLava/
//      生物群系等前置条件全部被清除, 不再需要任何工作台/熔炉/砧台/液体/环境;
//      同时 Hook Recipe.SetCraftingFilter 为空操作, 让手机端合成界面的
//      工作站过滤器(GUICrafting.UpdateFilter)始终处于"全部"状态。
//   2. 无需对应材料: 所有配方的 requiredItemQuickLookup 被清空,
//      材料检查恒为通过, 合成列表显示全部配方。
//   3. 合成不消耗材料: Hook Recipe.GetIngredientsForOneCraft 并跳过原版,
//      让本次要消耗的材料列表保持为空 -> 合成直接发放产物。
//
// 实现要点(结合 pe/dump.cs 反汇编):
//   1. PE 端每次刷新合成界面都会调用 Recipe.FindRecipes(bool) 重建配方,
//      部分情况推迟到下一帧由 Recipe.GetThroughDelayedFindRecipes() 执行。
//      Hook 这两个入口: 在原始逻辑执行后, 把 Main.recipe 里所有配方
//      的"工作站/液体/环境/材料"数据全部抹掉。原版 FindRecipes 依次检查
//      PlayerMeetsTileRequirements / PlayerMeetsEnvironmentConditions /
//      CollectedEnoughItemsToCraft, 数据抹掉后三项恒为通过,
//      原版会自然地(按配方下标顺序)把所有配方加入列表并正确维护 focusRecipe,
//      因此【不可】再手动覆盖 Main.availableRecipe, 否则合成后选中项会跳变。
//   2. 手机端合成界面 (GUICrafting) 的 UpdateFilter 按 requiredTile 是否等于
//      当前工作站(TileFilter = AllowedRequiredTileIdsForFilter[0])过滤配方;
//      SetCraftingFilter 空操作后过滤器始终为空 -> get_TileFilter 返回 -1 ->
//      所有配方都被显示。
//   3. 字段均做空指针保护, 解析失败时对应功能自动降级, 不会崩溃。
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

// 解析器
static TEFMod::Field<void*>*  (*ParseObjField)(void*);
static TEFMod::Field<int>*    (*ParseIntField)(void*);
static TEFMod::Field<bool>*   (*ParseBoolField)(void*);
static TEFMod::Array<void*>*  (*ParseObjArray)(void*);

// 字段
static TEFMod::Field<void*>* g_fMainRecipe;        // Main.recipe            (Recipe[], 静态字段)
static TEFMod::Field<void*>* g_fRecipeCreateItem;  // Recipe.createItem      (Item)
static TEFMod::Field<int>*   g_fItemType;          // Item.type              (int)
static TEFMod::Field<int>*   g_fRecipeRequiredTile;// Recipe.requiredTile    (int)
static TEFMod::Field<bool>*  g_fRecipeNeedWater;   // Recipe.needWater       (bool)
static TEFMod::Field<bool>*  g_fRecipeNeedHoney;   // Recipe.needHoney       (bool)
static TEFMod::Field<bool>*  g_fRecipeNeedLava;    // Recipe.needLava        (bool)
static TEFMod::Field<bool>*  g_fRecipeNeedTorch;   // Recipe.needTorchGodsFavor (bool)
static TEFMod::Field<bool>*  g_fRecipeNeedSnow;    // Recipe.needSnowBiome   (bool)
static TEFMod::Field<bool>*  g_fRecipeNeedGrave;   // Recipe.needGraveyardBiome (bool)
static TEFMod::Field<bool>*  g_fRecipeNeedMech;    // Recipe.needMechdusa    (bool)
static TEFMod::Field<void*>* g_fRecipeQuickLookup; // Recipe.requiredItemQuickLookup (RequiredItemEntry[])

// 原始函数
static void (*g_original_FindRecipes)(bool canDelayCheck);
static void (*g_original_GetThroughDelayedFindRecipes)();
static void (*g_original_GetIngredientsForOneCraft)(void* recipe, void* player, void* ingredients);
static void (*g_original_SetCraftingFilter)(int x, int y, int byTileId);

// Hook 转发函数声明
void FindRecipes_T(bool canDelayCheck);
void GetThroughDelayedFindRecipes_T();
void GetIngredientsForOneCraft_T(void* recipe, void* player, void* ingredients);
void SetCraftingFilter_T(int x, int y, int byTileId);

// Hook 模板
inline TEFMod::HookTemplate HookTemplate_FindRecipes {
        reinterpret_cast<void*>(FindRecipes_T),
        {}
};

inline TEFMod::HookTemplate HookTemplate_GetThroughDelayedFindRecipes {
        reinterpret_cast<void*>(GetThroughDelayedFindRecipes_T),
        {}
};

inline TEFMod::HookTemplate HookTemplate_GetIngredientsForOneCraft {
        reinterpret_cast<void*>(GetIngredientsForOneCraft_T),
        {}
};

inline TEFMod::HookTemplate HookTemplate_SetCraftingFilter {
        reinterpret_cast<void*>(SetCraftingFilter_T),
        {}
};

// ============ 转发函数 ============

void FindRecipes_T(bool canDelayCheck) {
    if (g_original_FindRecipes) g_original_FindRecipes(canDelayCheck);
    for (const auto fun : HookTemplate_FindRecipes.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(bool)>(fun)(canDelayCheck);
    }
}

void GetThroughDelayedFindRecipes_T() {
    if (g_original_GetThroughDelayedFindRecipes) g_original_GetThroughDelayedFindRecipes();
    for (const auto fun : HookTemplate_GetThroughDelayedFindRecipes.FunctionArray) {
        if (fun) reinterpret_cast<void(*)()>(fun)();
    }
}

// 关键: 跳过原版, 让材料列表保持为空 -> 消耗循环不执行 -> 免费发放产物
void GetIngredientsForOneCraft_T(void* recipe, void* player, void* ingredients) {
    for (const auto fun : HookTemplate_GetIngredientsForOneCraft.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(void*, void*, void*)>(fun)(recipe, player, ingredients);
    }
}

// 关键: 跳过原版, 阻止设置"工作站过滤器"。
// 手机端合成界面(GUICrafting.UpdateFilter)按 requiredTile == 当前工作站
// 过滤配方; 让 SetCraftingFilter 空操作, 过滤器始终为空 ->
// get_TileFilter 返回 -1 -> 所有配方都被显示, 不再受工作站限制。
void SetCraftingFilter_T(int x, int y, int byTileId) {
    for (const auto fun : HookTemplate_SetCraftingFilter.FunctionArray) {
        if (fun) reinterpret_cast<void(*)(int, int, int)>(fun)(x, y, byTileId);
    }
}

// ============ 配方数据修改 ============

/*
 * RequiredItemEntry 是内联 struct (dump.cs:68137):
 *   itemIdOrRecipeGroup: int @ 0x0
 *   stack:               int @ 0x4
 * Recipe.requiredItemQuickLookup 字段 @ Recipe 偏移 0x30, 值是指向
 * RequiredItemEntry[] 数组的引用。IL2CPP 数组数据区在 0x20 处
 * (对象头 16 字节 + bounds 8 字节 + max_length 4 字节 + 对齐 4 字节)。
 * 为避免误写, 先校验 max_length(应为 15)再清空。
 */
static constexpr std::size_t kArrayDataOffset = 0x20;
static constexpr std::size_t kArrayLenOffset  = 0x18;

/** 检查配方是否已是"零前置"状态 */
static bool IsRecipeFree(void* recipe) {
    if (!recipe) return true;
    if (g_fRecipeRequiredTile && g_fRecipeRequiredTile->Get(recipe) != -1) return false;
    if (g_fRecipeQuickLookup) {
        void* arr = g_fRecipeQuickLookup->Get(recipe);
        if (arr) {
            const int32_t len = *reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(arr) + kArrayLenOffset);
            if (len > 0) {
                const int32_t first = *reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(arr) + kArrayDataOffset);
                if (first != 0) return false;
            }
        }
    }
    return true;
}

/** 清空配方的工作站/液体/环境前置条件 */
static void ClearEnvironmentRequirements(void* recipe) {
    if (g_fRecipeRequiredTile) g_fRecipeRequiredTile->Set(-1, recipe);
    if (g_fRecipeNeedWater)    g_fRecipeNeedWater->Set(false, recipe);
    if (g_fRecipeNeedHoney)    g_fRecipeNeedHoney->Set(false, recipe);
    if (g_fRecipeNeedLava)     g_fRecipeNeedLava->Set(false, recipe);
    if (g_fRecipeNeedTorch)    g_fRecipeNeedTorch->Set(false, recipe);
    if (g_fRecipeNeedSnow)     g_fRecipeNeedSnow->Set(false, recipe);
    if (g_fRecipeNeedGrave)    g_fRecipeNeedGrave->Set(false, recipe);
    if (g_fRecipeNeedMech)     g_fRecipeNeedMech->Set(false, recipe);
}

/** 清空配方材料表 (requiredItemQuickLookup) */
static void ClearMaterials(void* recipe) {
    if (!g_fRecipeQuickLookup) return;
    void* arr = g_fRecipeQuickLookup->Get(recipe);
    if (!arr) return;

    const int32_t len = *reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(arr) + kArrayLenOffset);
    if (len < 0 || len > 15) return;  // 安全校验, 防止误写野内存

    std::memset(reinterpret_cast<char*>(arr) + kArrayDataOffset, 0, static_cast<size_t>(len) * 8);
}

/** 把单个配方改成"零前置" */
static void MakeRecipeFree(void* recipe) {
    if (!recipe) return;
    if (IsRecipeFree(recipe)) return;  // 已经处理过, 跳过(降低每帧开销)
    ClearEnvironmentRequirements(recipe);
    ClearMaterials(recipe);
}

/** 把所有有效配方改成"零前置"并统计 */
static int PatchAllRecipes(TEFMod::Array<void*>* recipes) {
    if (!recipes) return 0;
    const std::size_t total = recipes->Size();
    int count = 0;
    for (std::size_t i = 0; i < total; ++i) {
        void* recipe = recipes->at(i);
        if (!recipe) continue;

        // 只处理"产物类型非空"的有效配方
        if (g_fRecipeCreateItem && g_fItemType) {
            void* item = g_fRecipeCreateItem->Get(recipe);
            if (!item) continue;
            if (g_fItemType->Get(item) <= 0) continue;
        }

        MakeRecipeFree(recipe);
        ++count;
    }
    return count;
}

/**
 * 说明: 不再手动覆盖 Main.availableRecipe。
 * 原版 Recipe.FindRecipes 会依次检查 PlayerMeetsTileRequirements /
 * PlayerMeetsEnvironmentConditions / CollectedEnoughItemsToCraft,
 * 而本 Mod 已把每个配方的 requiredTile / need 系列布尔字段 / 材料表全部清除,
 * 这三项检查恒为通过, 原版会自然地(按配方下标顺序)把所有配方加入列表,
 * 并正确维护 focusRecipe, 避免合成后选中项跳变。
 */

// 采样验证日志(只打一次)
static bool g_verifiedOnce = false;

static void VerifyPatch(TEFMod::Array<void*>* recipes) {
    if (g_verifiedOnce) return;
    g_verifiedOnce = true;
    if (!recipes || recipes->Size() == 0) return;
    if (g_log) {
        void* r0 = recipes->at(0);
        int tile = -2;
        if (g_fRecipeRequiredTile) tile = g_fRecipeRequiredTile->Get(r0);
        int firstReq = -1;
        if (g_fRecipeQuickLookup) {
            void* arr = g_fRecipeQuickLookup->Get(r0);
            if (arr) {
                firstReq = *reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(arr) + kArrayDataOffset);
            }
        }
        g_log->i("FreeCraft", "verify: recipe[0] requiredTile=", tile, " firstRequiredItem=", firstReq);
    }
}

// ============ Hook 逻辑 ============

void Hook_FindRecipes(bool canDelayCheck) {
    if (!g_fMainRecipe) return;
    TEFMod::Array<void*>* recipes = ParseObjArray ? ParseObjArray(g_fMainRecipe->Get()) : nullptr;
    if (!recipes) return;

    PatchAllRecipes(recipes);
    VerifyPatch(recipes);
}

void Hook_GetThroughDelayedFindRecipes() {
    Hook_FindRecipes(true);
}

void Hook_GetIngredientsForOneCraft(void* recipe, void* player, void* ingredients) {
    // 空实现: 原版在转发函数中已被跳过, 材料列表保持为空
}

void Hook_SetCraftingFilter(int x, int y, int byTileId) {
    // 空实现: 原版在转发函数中已被跳过, 过滤器保持为空
}

// ============ Mod 主体 ============

class FreeCraft final : public EFMod {
public:
    int Initialize(const std::string &path, MultiChannel *multiChannel) override {
        return 0;
    }

    int UnLoad(const std::string &path, MultiChannel *multiChannel) override {
        return 0;
    }

    int Load(const std::string &path, MultiChannel* channel) override {
        g_log = channel->receive<TEFMod::Logger*(*)(const std::string&, const std::string&, const std::size_t)>(
                "TEFMod::CreateLogger")("FreeCraft", "", 0);
        g_api = channel->receive<TEFMod::TEFModAPI*>("TEFMod::TEFModAPI");
        if (!g_api) return 1;
        g_log->init();
        g_log->i("FreeCraft", "mod loaded");
        return 0;
    }

    void Send(const std::string &path, MultiChannel* channel) override {
        // Hook: 合成配方表刷新 (无需工作站 / 无需材料)
        g_api->registerFunctionDescriptor({
                "Terraria", "Recipe", "FindRecipes", "hook>>void", 1,
                &HookTemplate_FindRecipes, { reinterpret_cast<void*>(Hook_FindRecipes) }
        });
        g_api->registerFunctionDescriptor({
                "Terraria", "Recipe", "GetThroughDelayedFindRecipes", "hook>>void", 0,
                &HookTemplate_GetThroughDelayedFindRecipes, { reinterpret_cast<void*>(Hook_GetThroughDelayedFindRecipes) }
        });

        // Hook: 合成时材料列表 (免费合成, 不消耗材料)
        g_api->registerFunctionDescriptor({
                "Terraria", "Recipe", "GetIngredientsForOneCraft", "hook>>void", 2,
                &HookTemplate_GetIngredientsForOneCraft, { reinterpret_cast<void*>(Hook_GetIngredientsForOneCraft) }
        });

        // Hook: 工作站过滤器 (阻止按工作站过滤, 使所有配方可见)
        g_api->registerFunctionDescriptor({
                "Terraria", "Recipe", "SetCraftingFilter", "hook>>void", 3,
                &HookTemplate_SetCraftingFilter, { reinterpret_cast<void*>(Hook_SetCraftingFilter) }
        });

        // 字段
        g_api->registerApiDescriptor({"Terraria", "Main", "recipe", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "createItem", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Item", "type", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "requiredTile", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "needWater", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "needHoney", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "needLava", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "needTorchGodsFavor", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "needSnowBiome", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "needGraveyardBiome", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "needMechdusa", "Field"});
        g_api->registerApiDescriptor({"Terraria", "Recipe", "requiredItemQuickLookup", "Field"});
    }

    void Receive(const std::string &path, MultiChannel* channel) override {
        // 解析器
        ParseObjField = channel->receive<TEFMod::Field<void*>*(*)(void*)>(
                "TEFMod::Field<Other>::ParseFromPointer");
        ParseIntField = channel->receive<TEFMod::Field<int>*(*)(void*)>(
                "TEFMod::Field<Int>::ParseFromPointer");
        ParseBoolField = channel->receive<TEFMod::Field<bool>*(*)(void*)>(
                "TEFMod::Field<Bool>::ParseFromPointer");
        ParseObjArray = channel->receive<TEFMod::Array<void*>*(*)(void*)>(
                "TEFMod::Array<Other>::ParseFromPointer");

        // 原版函数
        g_original_FindRecipes = g_api->GetAPI<void(*)(bool)>({
                "Terraria", "Recipe", "FindRecipes", "old_fun", 1
        });
        g_original_GetThroughDelayedFindRecipes = g_api->GetAPI<void(*)()>({
                "Terraria", "Recipe", "GetThroughDelayedFindRecipes", "old_fun", 0
        });
        g_original_GetIngredientsForOneCraft = g_api->GetAPI<void(*)(void*, void*, void*)>({
                "Terraria", "Recipe", "GetIngredientsForOneCraft", "old_fun", 2
        });
        g_original_SetCraftingFilter = g_api->GetAPI<void(*)(int, int, int)>({
                "Terraria", "Recipe", "SetCraftingFilter", "old_fun", 3
        });

        // 字段
        g_fMainRecipe = ParseObjField(g_api->GetAPI<void*>({"Terraria", "Main", "recipe", "Field"}));
        g_fRecipeCreateItem = ParseObjField(g_api->GetAPI<void*>({"Terraria", "Recipe", "createItem", "Field"}));
        g_fItemType = ParseIntField(g_api->GetAPI<void*>({"Terraria", "Item", "type", "Field"}));
        g_fRecipeRequiredTile = ParseIntField(g_api->GetAPI<void*>({"Terraria", "Recipe", "requiredTile", "Field"}));
        g_fRecipeNeedWater = ParseBoolField(g_api->GetAPI<void*>({"Terraria", "Recipe", "needWater", "Field"}));
        g_fRecipeNeedHoney = ParseBoolField(g_api->GetAPI<void*>({"Terraria", "Recipe", "needHoney", "Field"}));
        g_fRecipeNeedLava = ParseBoolField(g_api->GetAPI<void*>({"Terraria", "Recipe", "needLava", "Field"}));
        g_fRecipeNeedTorch = ParseBoolField(g_api->GetAPI<void*>({"Terraria", "Recipe", "needTorchGodsFavor", "Field"}));
        g_fRecipeNeedSnow = ParseBoolField(g_api->GetAPI<void*>({"Terraria", "Recipe", "needSnowBiome", "Field"}));
        g_fRecipeNeedGrave = ParseBoolField(g_api->GetAPI<void*>({"Terraria", "Recipe", "needGraveyardBiome", "Field"}));
        g_fRecipeNeedMech = ParseBoolField(g_api->GetAPI<void*>({"Terraria", "Recipe", "needMechdusa", "Field"}));
        g_fRecipeQuickLookup = ParseObjField(g_api->GetAPI<void*>({"Terraria", "Recipe", "requiredItemQuickLookup", "Field"}));

        if (g_log) {
            g_log->i("FreeCraft", "parsers: objF=", (void*)ParseObjField,
                     " intF=", (void*)ParseIntField,
                     " boolF=", (void*)ParseBoolField,
                     " objArr=", (void*)ParseObjArray);
            g_log->i("FreeCraft", "orig: findRecipes=", (void*)g_original_FindRecipes,
                     " delayed=", (void*)g_original_GetThroughDelayedFindRecipes,
                     " ingredients=", (void*)g_original_GetIngredientsForOneCraft,
                     " setCraftingFilter=", (void*)g_original_SetCraftingFilter);
            g_log->i("FreeCraft", "fields: recipe=", (void*)g_fMainRecipe,
                     " createItem=", (void*)g_fRecipeCreateItem,
                     " itemType=", (void*)g_fItemType,
                     " requiredTile=", (void*)g_fRecipeRequiredTile,
                     " quickLookup=", (void*)g_fRecipeQuickLookup);
        }
    }

    Metadata GetMetadata() override {
        return {
                "FreeCraft",  // Mod名称(英文标识)
                "lzup",       // 作者
                "1.0.1",      // 版本
                20250711,     // 标准规范(与经典EFMod API一致)
                ModuleType::Game,
                { false }
        };
    }
};

EFMod* CreateMod() {
    static FreeCraft instance;
    return &instance;
}
