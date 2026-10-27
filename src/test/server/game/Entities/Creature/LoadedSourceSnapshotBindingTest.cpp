#include "Config.h"
#include "Database/Field.h"
#include "Globals/ObjectMgr.h"
#include "Globals/ObjectAccessor.h"
#include "Entities/Item/ItemTemplate.h"
#include "Cache/CharacterCache.h"
#include "DataStores/DBCStores.h"
#include "Quests/QuestDef.h"
#include "Scripting/ScriptDefines/AllCreatureScript.h"
#include "Scripting/ScriptDefines/CreatureScript.h"
#include "Hooks/Defines/CreatureHook.h"
#include "Scripting/ScriptMgr.h"
#include "Scripting/ScriptDefines/AchievementScript.h"
#include "PopulationBridge.h"
#include "Bot/PlayerbotAI.h"
#include "Bot/PlayerbotMgr.h"
#include "Bot/RandomPlayerbotMgr.h"
#include "Bot/Engine/AiObjectContext.h"
#include "Bot/Engine/Value/Value.h"
#include "PlayerbotAIConfig.h"
#include "ProgressionSystem.h"
#include "PythonEngine.h"
#include "IntegrationTestFixture.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <set>
#include <thread>
#include <vector>

void RegisterPythonPopulationBridge();
void AddSC_progression_system_shared_phase_snapshot();
void AddSC_PythonCreatureScripts();
void AddSC_npcs_special();
bool RegisterCpi211TestOnlyPopulationActor(std::string const&, Player*);
void UnregisterCpi211TestOnlyPopulationActor(std::string const&, Player*);

class QuestHookFallbackScript final : public CreatureScript
{
public:
    QuestHookFallbackScript() : CreatureScript("cpi211_quest_hook_fallback_test") { }

    bool OnQuestSelect(Player*, Creature*, Quest const*) override
    {
        ++selectCalls;
        return selectHandled;
    }

    bool OnQuestComplete(Player*, Creature*, Quest const*) override
    {
        ++completeCalls;
        return completeHandled;
    }

    bool OnQuestAccept(Player*, Creature*, Quest const*) override
    {
        ++acceptCalls;
        return acceptHandled;
    }

    uint32 selectCalls{0};
    uint32 completeCalls{0};
    uint32 acceptCalls{0};
    bool selectHandled{false};
    bool completeHandled{false};
    bool acceptHandled{false};
};

class PolicyTestQuest final : public Quest
{
public:
    explicit PolicyTestQuest(::Field* fields) : Quest(fields) { }

    void SetPolicy(uint32 id, int32 zoneOrSort, uint32 minimumLevel, int32 questLevel, uint32 flags)
    {
        Id = id;
        ZoneOrSort = zoneOrSort;
        MinLevel = minimumLevel;
        Level = questLevel;
        Flags = flags;
    }
};

class AchievementHookRegistryFixture
{
public:
    using Registry = ScriptRegistry<AchievementScript>;

    AchievementHookRegistryFixture() : _previousHooks(Registry::EnabledHooks)
    {
        if (Registry::EnabledHooks.empty())
            Registry::InitEnabledHooksIfNeeded(ACHIEVEMENTHOOK_END);
    }

    ~AchievementHookRegistryFixture()
    {
        Registry::EnabledHooks = std::move(_previousHooks);
    }

private:
    Registry::EnabledHooksVector _previousHooks;
};

class VendorSourceTestAccess
{
public:
    static bool HasCurrentBot(RandomPlayerbotMgr const& manager, uint32 guid)
    {
        return manager.currentBots.contains(guid);
    }

    static void SetCurrentBotMembership(RandomPlayerbotMgr& manager, uint32 guid, bool present)
    {
        if (present)
            manager.currentBots.insert(guid);
        else
            manager.currentBots.erase(guid);
    }

    static BotEventCache SnapshotEventCache(RandomPlayerbotMgr const& manager, uint32 guid, bool& existed)
    {
        auto const found = manager.eventCache.find(guid);
        existed = found != manager.eventCache.end();
        return existed ? found->second : BotEventCache{};
    }

    static void SetEventCache(RandomPlayerbotMgr& manager, uint32 guid, BotEventCache cache)
    {
        manager.eventCache[guid] = std::move(cache);
    }

    static void RestoreEventCache(RandomPlayerbotMgr& manager, uint32 guid, bool existed, BotEventCache cache)
    {
        if (existed)
            manager.eventCache[guid] = std::move(cache);
        else
            manager.eventCache.erase(guid);
    }

    struct VendorRowsSnapshot
    {
        bool existed{false};
        std::vector<VendorItem> rows;
    };

    static VendorRowsSnapshot SnapshotRows(ObjectMgr const& manager, uint32 entry)
    {
        VendorRowsSnapshot snapshot;
        auto const found = manager._cacheVendorItemStore.find(entry);
        if (found == manager._cacheVendorItemStore.end())
            return snapshot;
        snapshot.existed = true;
        snapshot.rows.reserve(found->second.m_items.size());
        for (VendorItem const* row : found->second.m_items)
            if (row)
                snapshot.rows.emplace_back(*row);
        return snapshot;
    }

    static void RestoreRows(ObjectMgr& manager, uint32 entry, VendorRowsSnapshot const& snapshot)
    {
        if (snapshot.existed)
            ReplaceRows(manager, entry, snapshot.rows);
        else
            ClearRows(manager, entry);
    }

    static ItemTemplate* ReplaceItem(ObjectMgr& manager, uint32 entry, ItemTemplate* replacement)
    {
        auto& items = manager._itemTemplateStoreFast;
        if (items.size() <= entry)
            items.resize(entry + 1, nullptr);
        ItemTemplate* previous = items[entry];
        items[entry] = replacement;
        return previous;
    }

    static void RestoreItem(ObjectMgr& manager, uint32 entry, ItemTemplate* previous)
    {
        manager._itemTemplateStoreFast[entry] = previous;
    }

    static ItemTemplate SnapshotStoreItem(ObjectMgr const& manager, uint32 entry, bool& existed)
    {
        auto const& items = manager._itemTemplateStore;
        auto const found = items.find(entry);
        existed = found != items.end();
        return existed ? found->second : ItemTemplate{};
    }

    static void ReplaceStoreItem(ObjectMgr& manager, uint32 entry, ItemTemplate const& item)
    {
        manager._itemTemplateStore[entry] = item;
    }

    static void RestoreStoreItem(ObjectMgr& manager, uint32 entry, bool existed, ItemTemplate const& previous)
    {
        if (existed)
            manager._itemTemplateStore[entry] = previous;
        else
            manager._itemTemplateStore.erase(entry);
    }

    static void ReplaceRows(ObjectMgr& manager, uint32 entry, std::vector<VendorItem> const& rows)
    {
        VendorItemData& list = manager._cacheVendorItemStore[entry];
        list.Clear();
        for (VendorItem const& row : rows)
            list.AddItem(row.item, row.maxcount, row.incrtime, row.ExtendedCost, row.sourceKind, row.sourceSlot);
    }

    static void ClearRows(ObjectMgr& manager, uint32 entry)
    {
        auto const found = manager._cacheVendorItemStore.find(entry);
        if (found == manager._cacheVendorItemStore.end())
            return;
        found->second.Clear();
        manager._cacheVendorItemStore.erase(found);
    }

    static size_t QuestCount(ObjectMgr const& manager) { return manager._questTemplatesFast.size(); }

    static Quest* ReplaceQuest(ObjectMgr& manager, uint32 entry, Quest* replacement)
    {
        auto& quests = manager._questTemplatesFast;
        if (quests.size() <= entry)
            quests.resize(entry + 1, nullptr);
        Quest* previous = quests[entry];
        quests[entry] = replacement;
        return previous;
    }

    static void RestoreQuest(ObjectMgr& manager, uint32 entry, Quest* previous, size_t originalCount)
    {
        manager._questTemplatesFast[entry] = previous;
        manager._questTemplatesFast.resize(originalCount);
    }

    static CreatureTemplate* SetCreatureScript(ObjectMgr& manager, uint32 entry, uint32 scriptId,
                                               CreatureTemplate*& previous)
    {
        auto& templates = manager._creatureTemplateStoreFast;
        if (templates.size() <= entry)
            templates.resize(entry + 1, nullptr);

        previous = templates[entry];
        auto* replacement = new CreatureTemplate();
        replacement->Entry = entry;
        replacement->ScriptID = scriptId;
        templates[entry] = replacement;
        return replacement;
    }

    static void RestoreCreatureScript(ObjectMgr& manager, uint32 entry,
                                      CreatureTemplate* previous, CreatureTemplate* replacement)
    {
        manager._creatureTemplateStoreFast[entry] = previous;
        delete replacement;
    }
};

TEST_F(IntegrationTestFixture, PhaseQuestSourcePolicyMatchesAcceptedLevelAndFlags)
{
    constexpr uint32 QuestId = 3861;
    ASSERT_NE(sObjectMgr, nullptr);
    std::array<::Field, 104> fields{};
    auto* quest = new PolicyTestQuest(fields.data());
    size_t const previousQuestCount = VendorSourceTestAccess::QuestCount(*sObjectMgr);
    Quest* previousQuest = VendorSourceTestAccess::ReplaceQuest(*sObjectMgr, QuestId, quest);

    struct Cleanup
    {
        ObjectMgr& manager;
        uint32 questId;
        Quest* previousQuest;
        Quest* replacementQuest;
        size_t previousQuestCount;
        ~Cleanup()
        {
            VendorSourceTestAccess::RestoreQuest(manager, questId, previousQuest, previousQuestCount);
            delete replacementQuest;
        }
    } cleanup{*sObjectMgr, QuestId, previousQuest, quest, previousQuestCount};

    auto allowed = [&] { return ProgressionSystem::ReadQuestSourcePolicy(QuestId).templateAllowed; };
    quest->SetPolicy(QuestId, 0, 1, 19, 0);
    EXPECT_TRUE(allowed()) << "level-19 quest with minimum level 1 remains eligible";

    quest->SetPolicy(QuestId, 0, 1, 20, 0);
    EXPECT_FALSE(allowed()) << "quest-level ceiling remains enforced";
    quest->SetPolicy(QuestId, 0, 20, 19, 0);
    EXPECT_FALSE(allowed()) << "minimum-level ceiling remains enforced";

    quest->SetPolicy(QuestId, 0, 1, 10, QUEST_FLAGS_TBC);
    EXPECT_FALSE(allowed()) << "TBC-flagged quests remain excluded under accepted compatibility policy";
    quest->SetPolicy(QuestId, 0, 1, 10, QUEST_FLAGS_RAID);
    EXPECT_FALSE(allowed()) << "raid quests remain excluded";
    quest->SetPolicy(QuestId, 0, 1, 10, QUEST_FLAGS_UNAVAILABLE);
    EXPECT_FALSE(allowed()) << "unavailable quests remain excluded";
}

TEST_F(IntegrationTestFixture, RegisteredChickenQuestRewardResetsItsAI)
{
    using Registry = ScriptRegistry<CreatureScript>;
    auto const savedScriptMap = Registry::ScriptPointerList;
    auto const savedAfterLoad = Registry::ALScripts;
    struct RegistryCleanup
    {
        decltype(Registry::ScriptPointerList) scriptMap;
        decltype(Registry::ALScripts) afterLoad;
        ~RegistryCleanup()
        {
            std::set<ScriptObject*> newlyRegistered;
            for (auto const& [script, _] : Registry::ALScripts)
                if (std::none_of(afterLoad.begin(), afterLoad.end(), [script](auto const& saved)
                    { return saved.first == script; }))
                    newlyRegistered.insert(script);
            Registry::ScriptPointerList = scriptMap;
            Registry::ALScripts = afterLoad;
            for (ScriptObject* script : newlyRegistered)
                delete dynamic_cast<CreatureScript*>(script);
        }
    } registryCleanup{savedScriptMap, savedAfterLoad};

    auto findChicken = []() -> CreatureScript*
    {
        auto const found = std::find_if(Registry::ScriptPointerList.begin(), Registry::ScriptPointerList.end(),
            [](auto const& item) { return item.second->GetName() == "npc_chicken_cluck"; });
        if (found != Registry::ScriptPointerList.end())
            return found->second;

        auto const pending = std::find_if(Registry::ALScripts.begin(), Registry::ALScripts.end(),
            [](auto const& item) { return item.first->GetName() == "npc_chicken_cluck"; });
        return pending == Registry::ALScripts.end() ? nullptr : pending->first;
    };
    if (!findChicken())
        AddSC_npcs_special();
    CreatureScript* chicken = findChicken();
    ASSERT_NE(chicken, nullptr);

    constexpr uint32 ChickenEntry = 3900003;
    CreatureTemplate* previousTemplate = nullptr;
    CreatureTemplate* replacementTemplate = VendorSourceTestAccess::SetCreatureScript(
        *sObjectMgr, ChickenEntry, sObjectMgr->GetScriptId("npc_chicken_cluck"), previousTemplate);
    struct TemplateCleanup
    {
        ObjectMgr& manager;
        uint32 entry;
        CreatureTemplate* previous;
        CreatureTemplate* replacement;
        ~TemplateCleanup()
        {
            VendorSourceTestAccess::RestoreCreatureScript(manager, entry, previous, replacement);
        }
    } templateCleanup{*sObjectMgr, ChickenEntry, previousTemplate, replacementTemplate};

    TestCreature* const creature = CreateTestCreature(119, ChickenEntry, TEST_FACTION_HOSTILE_TO_MONSTERS);
    creature->SetUInt32Value(OBJECT_FIELD_ENTRY, ChickenEntry);
    CreatureAI* const ai = chicken->GetAI(creature);
    ASSERT_NE(ai, nullptr);
    creature->SetAI(ai);
    ai->InitializeAI();
    EXPECT_FALSE(creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER));

    TestPlayer* const player = CreateTestPlayer(120);
    std::array<::Field, 104> questFields{};
    PolicyTestQuest chickenQuest(questFields.data());
    chickenQuest.SetPolicy(3861, 0, 1, 1, 0);
    creature->SetNpcFlag(UNIT_NPC_FLAG_QUESTGIVER);
    EXPECT_FALSE(chicken->OnQuestComplete(player, creature, &chickenQuest));
    EXPECT_TRUE(creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
        << "the questgiver must remain available until the reward request is processed";
    EXPECT_FALSE(chicken->OnQuestReward(player, creature, &chickenQuest, 0));
    EXPECT_FALSE(creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER));
}

TEST_F(IntegrationTestFixture, UsesProductionProviderBindingAndCurrentObjectMgrRows)
{
    constexpr uint32 VendorEntry = 3900001;
    constexpr uint32 SourceSlot = 37;
    constexpr uint32 ItemEntry = 1999999999;
    ASSERT_NE(sObjectMgr, nullptr);

    char const* testRootValue = std::getenv("CPI211_NATIVE_TEST_ROOT");
    ASSERT_NE(testRootValue, nullptr);
    std::filesystem::path const testRoot(testRootValue);
    std::filesystem::create_directories(testRoot);
    std::filesystem::path const scriptsRoot = testRoot / "population-scripts";
    std::filesystem::create_directories(scriptsRoot);
    std::filesystem::path const populationEntry = scriptsRoot / "cpi211-test-population.py";
    {
        std::ofstream script(populationEntry, std::ios::trunc);
        script << "import azerothcore as ac\n"
               << "def population_tick(diff_ms):\n"
               << "    global cpi211_population_tick_result, cpi211_population_tick_error, cpi211_replay_result, cpi211_native_offer, cpi211_test_gear_snapshot, cpi211_test_probe_removed_identity, cpi211_removed_identity\n"
               << "    if cpi211_test_witness is None:\n"
               << "        cpi211_test_gear_snapshot = ac.strategic_read_gear_snapshot(cpi211_test_actor_identity, 0)\n"
               << "        cpi211_population_tick_result = True\n"
               << "        return True\n"
               << "    if cpi211_test_probe_removed_identity:\n"
               << "        cpi211_test_probe_removed_identity = False\n"
               << "        cpi211_removed_identity = ac.strategic_read_quest_snapshot(cpi211_test_actor_identity, 0)\n"
               << "        cpi211_population_tick_result = True\n"
               << "        return True\n"
               << "    try:\n"
               << "        offers = ac.strategic_read_vendor_offers(cpi211_test_witness.key, [103])\n"
               << "        cpi211_native_offer = next(row for row in offers if row['vendor_entry'] == 3900002 and row['vendor_slot'] == 0)\n"
               << "        request_id = cpi211_native_offer['request_id']\n"
               << "        assert len(request_id) == 64 and all(character in '0123456789abcdef' for character in request_id)\n"
               << "        assert any(character in 'abcdef' for character in request_id), repr(request_id)\n"
               << "        uppercase = ac.strategic_buy_vendor_item(cpi211_test_witness.key, request_id.upper(), cpi211_native_offer['source_id'], 103, 16)\n"
               << "        malformed = ac.strategic_buy_vendor_item(cpi211_test_witness.key, request_id[:-1], cpi211_native_offer['source_id'], 103, 16)\n"
               << "        assert uppercase['status'] == malformed['status'] == 'refused_before_effect'\n"
               << "        assert uppercase['reason'] == malformed['reason'] == 'consumed_request'\n"
               << "        cpi211_population_tick_result = cpi211_test_witness.tick(diff_ms)\n"
               << "        purchase = next(row for row in cpi211_test_witness._cpi211_test_audit_rows if row.get('event') == 'vendor_purchase')\n"
               << "        receipt = purchase['native_receipt']\n"
               << "        assert cpi211_native_offer['request_id'] == receipt['request_id']\n"
               << "        cpi211_replay_result = ac.strategic_buy_vendor_item(cpi211_test_witness.key, cpi211_native_offer['request_id'], cpi211_native_offer['source_id'], receipt['item'], purchase['target']['slot'])\n"
               << "        return True\n"
               << "    except Exception as error:\n"
               << "        cpi211_population_tick_error = repr((error, offers if 'offers' in locals() else None, cpi211_test_gear_snapshot, cpi211_population_tick_result, cpi211_test_witness.status, cpi211_test_witness._cpi211_test_audit_rows))\n"
               << "        raise\n";
        ASSERT_TRUE(static_cast<bool>(script));
    }
    std::filesystem::path const configPath = testRoot / "native-source-observation.conf";
    struct ConfigCleanup
    {
        std::filesystem::path path;
        std::filesystem::path scriptsRoot;
        ~ConfigCleanup()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove_all(scriptsRoot, error);
        }
    } configCleanup{configPath, scriptsRoot};

    auto writeConfig = [&](bool gateEnabled)
    {
        std::ofstream config(configPath, std::ios::trunc);
        config << "[worldserver]\n"
               << "Python.Enabled = 1\n"
               << "Python.Population.Enabled = 1\n"
               << "Python.ScriptsPath = " << scriptsRoot.string() << "\n"
               << "Python.Population.EntryScript = cpi211-test-population.py\n"
               << "ProgressionSystem.LoadScripts = 1\n"
               << "ProgressionSystem.LoadDatabase = 1\n"
               << "ProgressionSystem.SharedPhaseGate.Enable = " << (gateEnabled ? 1 : 0) << "\n"
               << "ProgressionSystem.SharedPhaseGate.CurrentPhase = 1_19\n"
               << "ProgressionSystem.Bracket_0 = 1\n"
               << "ProgressionSystem.Bracket_1_19 = 1\n";
        config.close();
        return static_cast<bool>(config);
    };

    ASSERT_TRUE(writeConfig(true));
    sConfigMgr->Configure(configPath.string(), {});
    ASSERT_TRUE(sConfigMgr->LoadAppConfigs());
    AddSC_progression_system_shared_phase_snapshot();
    sScriptMgr->OnAfterConfigLoad(false);
    ProgressionSystem::SharedPhaseSnapshot const phase = ProgressionSystem::GetSharedPhaseSnapshot();
    ASSERT_TRUE(phase.enabled);
    ASSERT_TRUE(phase.policyValid);
    ASSERT_EQ(phase.phaseId, "1_19");

    constexpr uint32 TestVendorEntry = 3900002;
    constexpr uint32 TestItemEntry = 103;
    auto const previousVendorRows = VendorSourceTestAccess::SnapshotRows(*sObjectMgr, VendorEntry);
    auto const previousTestVendorRows = VendorSourceTestAccess::SnapshotRows(*sObjectMgr, TestVendorEntry);
    bool previousStoreItemExisted = false;
    ItemTemplate const previousStoreItem = VendorSourceTestAccess::SnapshotStoreItem(
        *sObjectMgr, TestItemEntry, previousStoreItemExisted);
    struct Cleanup
    {
        ObjectMgr& manager;
        uint32 entry;
        VendorSourceTestAccess::VendorRowsSnapshot previousRows;
        uint32 testVendorEntry;
        VendorSourceTestAccess::VendorRowsSnapshot previousTestVendorRows;
        uint32 testItemEntry;
        bool previousStoreItemExisted;
        ItemTemplate previousStoreItem;
        ItemTemplate* previousItem{nullptr};
        ItemTemplate* testItem{nullptr};
        bool itemReplaced{false};
        bool storeItemReplaced{false};
        PyEng::Population::Bridge* provider{nullptr};
        ~Cleanup()
        {
            if (sPythonEngine->IsEnabled())
                sPythonEngine->Shutdown();
            if (provider)
                PyEng::Population::UnregisterBridge(provider);
            VendorSourceTestAccess::RestoreRows(manager, entry, previousRows);
            VendorSourceTestAccess::RestoreRows(manager, testVendorEntry, previousTestVendorRows);
            if (itemReplaced)
                VendorSourceTestAccess::RestoreItem(manager, testItemEntry, previousItem);
            if (storeItemReplaced)
                VendorSourceTestAccess::RestoreStoreItem(
                    manager, testItemEntry, previousStoreItemExisted, previousStoreItem);
            delete testItem;
        }
    } cleanup{*sObjectMgr, VendorEntry, previousVendorRows, TestVendorEntry, previousTestVendorRows,
        TestItemEntry, previousStoreItemExisted, previousStoreItem};

    VendorSourceTestAccess::ReplaceRows(*sObjectMgr, VendorEntry, {
        VendorItem(ItemEntry, 3, 45, 0, VendorItemSourceKind::NpcVendorRow, SourceSlot),
        VendorItem(1999999998, 0, 0, 0, VendorItemSourceKind::VendorReferenceExpansion),
        VendorItem(1999999997, 0, 0, 0, VendorItemSourceKind::Runtime)
    });
    VendorSourceTestAccess::ReplaceRows(*sObjectMgr, TestVendorEntry, {
        VendorItem(TestItemEntry, 0, 0, 0, VendorItemSourceKind::NpcVendorRow, SourceSlot)
    });
    cleanup.testItem = new ItemTemplate{};
    ItemTemplate* const testItem = cleanup.testItem;
    testItem->ItemId = TestItemEntry;
    testItem->Class = ITEM_CLASS_ARMOR;
    testItem->SubClass = ITEM_SUBCLASS_ARMOR_SHIELD;
    testItem->Quality = ITEM_QUALITY_UNCOMMON;
    testItem->BuyCount = 1;
    testItem->BuyPrice = 100;
    testItem->InventoryType = INVTYPE_SHIELD;
    testItem->AllowableClass = 1u << (CLASS_WARRIOR - 1);
    testItem->AllowableRace = 0xffffffff;
    testItem->ItemLevel = 15;
    testItem->RequiredLevel = 1;
    testItem->Stackable = 1;
    testItem->StatsCount = 1;
    testItem->ItemStat[0].ItemStatType = ITEM_MOD_STAMINA;
    testItem->ItemStat[0].ItemStatValue = 20;
    cleanup.previousItem = VendorSourceTestAccess::ReplaceItem(*sObjectMgr, TestItemEntry, testItem);
    cleanup.itemReplaced = true;
    VendorSourceTestAccess::ReplaceStoreItem(*sObjectMgr, TestItemEntry, *testItem);
    cleanup.storeItemReplaced = true;
    VendorItemData const* const shiftedList = sObjectMgr->GetNpcVendorItemList(VendorEntry);
    ASSERT_NE(shiftedList, nullptr);
    ASSERT_EQ(shiftedList->GetItemCount(), 3u);
    ASSERT_NE(shiftedList->GetItem(0), nullptr);
    EXPECT_EQ(shiftedList->GetItem(0)->sourceSlot, SourceSlot)
        << "the loaded-list index 0 must remain distinct from the original npc_vendor slot 37";
    RegisterPythonPopulationBridge();
    cleanup.provider = PyEng::Population::GetBridge();
    ASSERT_NE(cleanup.provider, nullptr);

    sPythonEngine->Initialize();
    ASSERT_TRUE(sPythonEngine->IsEnabled());
    ASSERT_TRUE(sPythonEngine->LoadScripts());

    constexpr ObjectGuid::LowType GearActorGuid = 180;
    constexpr ObjectGuid::LowType GearVendorGuid = 181;
    constexpr uint32 TestDisplayId = 0;
    constexpr uint32 TestModelId = 0;
    std::string const gearIdentity(64, 'c');
    bool const previousBotConfigEnabled = sPlayerbotAIConfig.enabled;
    float const previousSightDistance = sPlayerbotAIConfig.sightDistance;
    std::vector<uint32> const previousRandomAccounts = sPlayerbotAIConfig.randomBotAccounts;
    ASSERT_NE(sCharacterCache, nullptr);
    ObjectGuid const gearActorGuid = ObjectGuid::Create<HighGuid::Player>(GearActorGuid);
    bool const previousCurrentBot = VendorSourceTestAccess::HasCurrentBot(sRandomPlayerbotMgr, GearActorGuid);
    bool previousHadEventCache = false;
    BotEventCache previousEventCache = VendorSourceTestAccess::SnapshotEventCache(
        sRandomPlayerbotMgr, GearActorGuid, previousHadEventCache);
    CharacterCacheEntry const* const previousGuidCache = sCharacterCache->GetCharacterCacheByGuid(gearActorGuid);
    CharacterCacheEntry const* const previousNameCache = sCharacterCache->GetCharacterCacheByName("Cpi211TestGearActor");
    struct GearActorCleanup
    {
        TestMap* map;
        TestPlayer* player;
        TestCreature* vendor;
        std::string identity;
        bool previousBotConfigEnabled;
        bool previousNewRpgStrategyEnabled;
        float previousSightDistance;
        std::vector<uint32> previousRandomAccounts;
        uint32 currentBotGuid;
        bool previousCurrentBot;
        bool previousHadEventCache;
        BotEventCache previousEventCache;
        CharacterCacheEntry const* previousGuidCache;
        CharacterCacheEntry const* previousNameCache;
        uint32 testDisplayId;
        uint32 testModelId;
        bool playerInMap{false};
        bool vendorInMap{false};
        bool vendorStored{false};
        bool objectAccessorRegistered{false};
        bool fakeDisplayRowsInstalled{false};
        bool playerbotAdded{false};
        bool cacheAdded{false};
        bool actorRegistered{false};
        bool cleaned{false};
        void Cleanup()
        {
            if (cleaned)
                return;
            cleaned = true;
            // Drop the population callback and its Python globals before the fixture actor/session dies.
            if (sPythonEngine && sPythonEngine->IsEnabled())
                sPythonEngine->Shutdown();
            if (player)
            {
                if (actorRegistered)
                    UnregisterCpi211TestOnlyPopulationActor(identity, player);
                if (playerbotAdded)
                {
                    PlayerbotAI* const ai = sPlayerbotsMgr.GetPlayerbotAI(player);
                    sPlayerbotsMgr.RemovePlayerBotData(player->GetGUID(), true);
                    delete ai;
                }
                if (cacheAdded && sCharacterCache && !previousGuidCache && !previousNameCache)
                    sCharacterCache->DeleteCharacterCacheEntry(player->GetGUID(), player->GetName());
                if (objectAccessorRegistered)
                    ObjectAccessor::RemoveObject(static_cast<Player*>(player));
                if (playerInMap)
                    map->RemovePlayerFromMap(player, false);
            }
            VendorSourceTestAccess::SetCurrentBotMembership(sRandomPlayerbotMgr, currentBotGuid, previousCurrentBot);
            VendorSourceTestAccess::RestoreEventCache(sRandomPlayerbotMgr, currentBotGuid,
                previousHadEventCache, std::move(previousEventCache));
            if (vendor && vendorInMap)
            {
                // TestCreature::RemoveFromWorld is intentionally a no-op; the
                // fixture set this flag for map lookup after AddToMap.
                vendor->SetInWorld(false);
                map->RemoveFromMap(static_cast<Creature*>(vendor), false);
                if (vendorStored)
                    map->GetObjectsStore().Remove<Creature>(vendor->GetGUID());
            }
            if (fakeDisplayRowsInstalled)
            {
                sCreatureDisplayInfoStore.SetEntry(testDisplayId, nullptr);
                sCreatureModelDataStore.SetEntry(testModelId, nullptr);
            }
            sPlayerbotAIConfig.enabled = previousBotConfigEnabled;
            sPlayerbotAIConfig.enableNewRpgStrategy = previousNewRpgStrategyEnabled;
            sPlayerbotAIConfig.sightDistance = previousSightDistance;
            sPlayerbotAIConfig.randomBotAccounts = std::move(previousRandomAccounts);
            player = nullptr;
            vendor = nullptr;
        }
        ~GearActorCleanup() { Cleanup(); }
    } gearActorCleanup{GetTestMap(), nullptr, nullptr, gearIdentity,
        previousBotConfigEnabled, sPlayerbotAIConfig.enableNewRpgStrategy, previousSightDistance,
        previousRandomAccounts, GearActorGuid, previousCurrentBot,
        previousHadEventCache, std::move(previousEventCache), previousGuidCache, previousNameCache,
        TestDisplayId, TestModelId};

    ASSERT_EQ(sCreatureDisplayInfoStore.LookupEntry(TestDisplayId), nullptr);
    ASSERT_EQ(sCreatureModelDataStore.LookupEntry(TestModelId), nullptr);
    auto* const testDisplay = new CreatureDisplayInfoEntry{};
    testDisplay->Displayid = TestDisplayId;
    testDisplay->ModelId = TestModelId;
    testDisplay->scale = 1.0f;
    auto* const testModel = new CreatureModelDataEntry{};
    testModel->Id = TestModelId;
    testModel->Scale = 1.0f;
    testModel->CollisionWidth = 1.0f;
    testModel->CollisionHeight = 2.0f;
    sCreatureDisplayInfoStore.SetEntry(TestDisplayId, testDisplay);
    sCreatureModelDataStore.SetEntry(TestModelId, testModel);
    gearActorCleanup.fakeDisplayRowsInstalled = true;

    TestPlayer* const gearActor = CreateTestPlayer(GearActorGuid, "Cpi211TestGearActor");
    gearActorCleanup.player = gearActor;
    TestCreature* const gearVendor = CreateTestCreature(GearVendorGuid, TestVendorEntry,
        TEST_FACTION_HOSTILE_TO_MONSTERS);
    gearActorCleanup.vendor = gearVendor;
    ASSERT_EQ(previousGuidCache, nullptr);
    ASSERT_EQ(previousNameCache, nullptr);
    EXPECT_FALSE(previousCurrentBot);
    ASSERT_NE(gearActor, nullptr);
    ASSERT_NE(gearVendor, nullptr);
    gearVendor->SetUInt32Value(OBJECT_FIELD_ENTRY, TestVendorEntry);
    ASSERT_EQ(gearVendor->GetEntry(), TestVendorEntry);
    gearActor->SetNativeDisplayId(TestDisplayId);
    gearActor->SetDisplayId(TestDisplayId);
    gearVendor->SetNativeDisplayId(TestDisplayId);
    gearVendor->SetDisplayId(TestDisplayId);
    ASSERT_EQ(sRandomPlayerbotMgr.GetPlayerBot(gearActor->GetGUID()), nullptr);
    ASSERT_EQ(sPlayerbotsMgr.GetPlayerbotAI(gearActor), nullptr);
    ASSERT_GT(gearActor->GetSession()->GetPartySessionIncarnation(), 0u);
    gearActor->SetByteValue(UNIT_FIELD_BYTES_0, 1, CLASS_WARRIOR);
    gearActor->setRace(RACE_HUMAN);
    ASSERT_EQ(gearActor->getClass(), CLASS_WARRIOR);
    gearActor->SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE, TEST_FACTION_HOSTILE_TO_MONSTERS);
    gearActor->SetLevel(8);
    // Seed fixture state without dispatching unrelated achievement-script hooks.
    gearActor->SetUInt32Value(PLAYER_FIELD_COINAGE, 1000);
    gearActor->Relocate(0.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_EQ(gearActor->GetMap(), GetTestMap());
    ASSERT_TRUE(gearActor->IsInWorld());
    ASSERT_EQ(ObjectAccessor::FindPlayer(gearActorGuid), nullptr);
    // CharacterHandler registers the session actor in ObjectAccessor before
    // adding it to the map. TestPlayer intentionally bypasses that path.
    ObjectAccessor::AddObject(static_cast<Player*>(gearActor));
    gearActorCleanup.objectAccessorRegistered = true;
    ASSERT_EQ(ObjectAccessor::FindPlayer(gearActorGuid), gearActor);
    // CreateTestPlayer marks the object in-world before it has map-grid
    // membership. Let AddPlayerToMap perform the single normal registration.
    gearActor->RemoveFromWorld();
    ASSERT_FALSE(gearActor->IsInWorld());
    ASSERT_TRUE(GetTestMap()->AddPlayerToMap(gearActor));
    gearActorCleanup.playerInMap = true;
    gearVendor->Relocate(1.0f, 0.0f, 0.0f, 0.0f);
    gearVendor->SetNpcFlag(UNIT_NPC_FLAG_VENDOR);
    ASSERT_TRUE(gearVendor->IsInWorld());
    // CreateTestCreature sets the world flag but does not add the creature to
    // the grid. Let AddToMap perform that registration exactly once.
    gearVendor->SetInWorld(false);
    ASSERT_FALSE(gearVendor->IsInWorld());
    ASSERT_TRUE(GetTestMap()->AddToMap(static_cast<Creature*>(gearVendor)));
    gearActorCleanup.vendorInMap = true;
    // TestCreature intentionally suppresses Creature::AddToWorld. Restore its
    // controlled world flag and map GUID lookup entry after grid insertion.
    ASSERT_EQ(GetTestMap()->GetCreature(gearVendor->GetGUID()), nullptr);
    gearVendor->SetInWorld(true);
    GetTestMap()->GetObjectsStore().Insert<Creature>(gearVendor->GetGUID(), gearVendor);
    gearActorCleanup.vendorStored = true;
    sPlayerbotAIConfig.enabled = true;
    sPlayerbotAIConfig.enableNewRpgStrategy = true;
    // PlayerbotAIConfig::Initialize normally loads this before creating value
    // contexts; the native integration fixture bypasses that server startup.
    sPlayerbotAIConfig.sightDistance = 20.0f;
    if (!sPlayerbotAIConfig.IsInRandomAccountList(gearActor->GetSession()->GetAccountId()))
        sPlayerbotAIConfig.randomBotAccounts.push_back(gearActor->GetSession()->GetAccountId());
    // The test has no PlayerbotsDatabase connection. Seed the same in-memory
    // event-cache state used after a production read, with no specNo event so
    // the real IsSpecPvp path remains false without attempting SQL.
    VendorSourceTestAccess::SetEventCache(sRandomPlayerbotMgr, GearActorGuid, BotEventCache{true, {}});
    sCharacterCache->AddCharacterCacheEntry(gearActor->GetGUID(), gearActor->GetSession()->GetAccountId(),
        gearActor->GetName(), gearActor->getGender(), gearActor->getRace(), gearActor->getClass(), gearActor->GetLevel());
    gearActorCleanup.cacheAdded = true;
    // Production initializes the shared Playerbot value/strategy contexts from
    // PlayerbotAIConfig::Initialize; the unit-test harness does not run that
    // startup path. Initialize them once before constructing the fixture AI.
    static bool sharedAiContextsInitialized = false;
    if (!sharedAiContextsInitialized)
    {
        AiObjectContext::BuildAllSharedContexts();
        sharedAiContextsInitialized = true;
    }
    sPlayerbotsMgr.AddPlayerbotData(gearActor, true);
    gearActorCleanup.playerbotAdded = true;
    PlayerbotAI* const gearAI = sPlayerbotsMgr.GetPlayerbotAI(gearActor);
    ASSERT_NE(gearAI, nullptr);
    gearAI->ChangeStrategy("+new rpg", BOT_STATE_NON_COMBAT);
    ASSERT_TRUE(gearAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT));
    ASSERT_NE(gearAI->GetAiObjectContext(), nullptr);
    auto* const nearestNpcs = gearAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs");
    ASSERT_NE(nearestNpcs, nullptr);
    ASSERT_TRUE(gearVendor->IsInGrid());
    EXPECT_TRUE(gearActor->InSamePhase(gearVendor));
    EXPECT_TRUE(gearActor->IsWithinDistInMap(gearVendor, sPlayerbotAIConfig.sightDistance));
    EXPECT_TRUE(gearActor->IsWithinLOSInMap(gearVendor));
    GuidVector const nearbyVendorGuids = nearestNpcs->Get();
    ASSERT_NE(std::find(nearbyVendorGuids.begin(), nearbyVendorGuids.end(), gearVendor->GetGUID()),
        nearbyVendorGuids.end());
    ASSERT_FALSE(gearAI->rpgInfo.pythonQuestGoal);
    ASSERT_TRUE(std::holds_alternative<NewRpgInfo::Idle>(gearAI->rpgInfo.data));
    ASSERT_EQ(gearActor->GetSession()->GetCurrentVendor(), 0u);
    ASSERT_EQ(gearActor->GetNPCIfCanInteractWith(gearVendor->GetGUID(), UNIT_NPC_FLAG_VENDOR), gearVendor);
    ASSERT_NE(gearVendor->GetVendorItems(), nullptr);
    ASSERT_EQ(gearVendor->GetVendorItems()->GetItemCount(), 1u);
    ASSERT_EQ(gearVendor->GetVendorItems()->GetItem(0)->sourceSlot, SourceSlot);
    EXPECT_FALSE(sRandomPlayerbotMgr.IsRandomBot(gearActor));
    EXPECT_FALSE(RegisterCpi211TestOnlyPopulationActor(gearIdentity, gearActor))
        << "an ordinary bot identity without scheduler membership must remain inadmissible";
    // The fixture helper is the sole test path into scheduler membership; the
    // production IsRandomBot and admission checks remain unchanged.
    VendorSourceTestAccess::SetCurrentBotMembership(sRandomPlayerbotMgr, GearActorGuid, true);
    EXPECT_TRUE(sRandomPlayerbotMgr.IsRandomBot(gearActor));
    ASSERT_TRUE(RegisterCpi211TestOnlyPopulationActor(gearIdentity, gearActor));
    gearActorCleanup.actorRegistered = true;

    // Exercise the real PythonCreatureScript -> ScriptMgr global dispatch and
    // its native per-entry fallback with registered callbacks. This covers
    // default-false, exception continuation, callback order, and the actual
    // CreatureScript registry lookup without invoking a live world session.
    bool pythonCreatureScriptRegistered = std::any_of(
        ScriptRegistry<AllCreatureScript>::ScriptPointerList.begin(),
        ScriptRegistry<AllCreatureScript>::ScriptPointerList.end(),
        [](auto const& entry) { return entry.second->GetName() == "PythonCreatureScript"; });
    if (!pythonCreatureScriptRegistered)
        AddSC_PythonCreatureScripts();

    constexpr uint32 TestCreatureEntry = 101;
    constexpr uint32 TestCreatureScriptId = 0x7f21112;
    CreatureTemplate* previousTemplate = nullptr;
    CreatureTemplate* replacementTemplate = VendorSourceTestAccess::SetCreatureScript(
        *sObjectMgr, TestCreatureEntry, TestCreatureScriptId, previousTemplate);
    auto& creatureScripts = ScriptRegistry<CreatureScript>::ScriptPointerList;
    auto previousScript = creatureScripts.find(TestCreatureScriptId);
    bool hadPreviousScript = previousScript != creatureScripts.end();
    CreatureScript* savedScript = hadPreviousScript ? previousScript->second : nullptr;
    auto* fallbackScript = new QuestHookFallbackScript();
    creatureScripts[TestCreatureScriptId] = fallbackScript;

    struct QuestHookCleanup
    {
        ObjectMgr& manager;
        uint32 entry;
        CreatureTemplate* previousTemplate;
        CreatureTemplate* replacementTemplate;
        uint32 scriptId;
        CreatureScript* fallbackScript;
        CreatureScript* savedScript;
        bool hadPreviousScript;
        ~QuestHookCleanup()
        {
            VendorSourceTestAccess::RestoreCreatureScript(manager, entry, previousTemplate, replacementTemplate);
            auto& scripts = ScriptRegistry<CreatureScript>::ScriptPointerList;
            if (hadPreviousScript)
                scripts[scriptId] = savedScript;
            else
                scripts.erase(scriptId);
            auto& afterLoad = ScriptRegistry<CreatureScript>::ALScripts;
            afterLoad.erase(std::remove_if(afterLoad.begin(), afterLoad.end(),
                [this](auto const& script) { return script.first == fallbackScript; }), afterLoad.end());
            delete fallbackScript;
        }
    } questHookCleanup{*sObjectMgr, TestCreatureEntry, previousTemplate, replacementTemplate,
                       TestCreatureScriptId, fallbackScript, savedScript, hadPreviousScript};

    TestPlayer* const hookPlayer = CreateTestPlayer(117);
    TestCreature* const hookCreature = CreateTestCreature(118, TestCreatureEntry, TEST_FACTION_HOSTILE_TO_ALL);
    hookCreature->SetUInt32Value(OBJECT_FIELD_ENTRY, TestCreatureEntry);
    std::array<::Field, 104> questFields{};
    Quest const hookQuest(questFields.data());
    auto const hookCallbacks = sPythonEngine->ExecuteCode(
        "import azerothcore as ac\n"
        "quest_hook_calls = []\n"
        "def quest_hook_global(*args):\n"
        "    quest_hook_calls.append('global')\n"
        "    return False\n"
        "def quest_hook_error(*args):\n"
        "    quest_hook_calls.append('error')\n"
        "    raise RuntimeError('expected test hook exception')\n"
        "def quest_hook_entry(*args):\n"
        "    quest_hook_calls.append('entry')\n"
        "    return False\n"
        "def quest_hook_handled(*args):\n"
        "    quest_hook_calls.append('handled')\n"
        "    return True\n"
        "ac.Register('CREATURE_ON_QUEST_SELECT', quest_hook_global)\n"
        "ac.Register('CREATURE_ON_QUEST_SELECT', quest_hook_error)\n"
        "ac.Register('CREATURE_ON_QUEST_SELECT', quest_hook_entry, 101)\n"
        "ac.Register('CREATURE_ON_QUEST_COMPLETE', quest_hook_global)\n"
        "ac.Register('CREATURE_ON_QUEST_COMPLETE', quest_hook_entry, 101)\n");
    ASSERT_TRUE(hookCallbacks) << hookCallbacks.error;

    fallbackScript->selectHandled = true;
    EXPECT_TRUE(sScriptMgr->OnQuestSelect(hookPlayer, hookCreature, &hookQuest));
    EXPECT_EQ(fallbackScript->selectCalls, 1u);
    auto const callbackOrder = sPythonEngine->ExecuteCode(
        "assert quest_hook_calls == ['global', 'error', 'entry']\n");
    EXPECT_TRUE(callbackOrder) << callbackOrder.error;

    fallbackScript->selectHandled = false;
    EXPECT_FALSE(sScriptMgr->OnQuestSelect(hookPlayer, hookCreature, &hookQuest));
    EXPECT_EQ(fallbackScript->selectCalls, 2u);
    EXPECT_FALSE(sPythonEngine->TriggerResult(PyEng::Hooks::Creature::ON_GOSSIP_HELLO, 0, false));

    auto const globalHandledRegistration = sPythonEngine->ExecuteCode(
        "ac.Register('CREATURE_ON_QUEST_SELECT', quest_hook_handled)\n");
    ASSERT_TRUE(globalHandledRegistration) << globalHandledRegistration.error;
    EXPECT_TRUE(sScriptMgr->OnQuestSelect(hookPlayer, hookCreature, &hookQuest));
    EXPECT_EQ(fallbackScript->selectCalls, 2u)
        << "a true global notification must short-circuit native per-entry fallback";
    auto const globalHandledOrder = sPythonEngine->ExecuteCode(
        "assert quest_hook_calls[-4:] == ['global', 'error', 'handled', 'entry']\n");
    EXPECT_TRUE(globalHandledOrder) << globalHandledOrder.error;

    // CREATURE_ON_QUEST_ACCEPT has no Python callbacks registered above, so
    // this verifies the native per-entry fallback when the global hook is absent.
    fallbackScript->acceptHandled = true;
    EXPECT_TRUE(sScriptMgr->OnQuestAccept(hookPlayer, hookCreature, &hookQuest));
    EXPECT_EQ(fallbackScript->acceptCalls, 1u);

    fallbackScript->completeHandled = true;
    EXPECT_TRUE(sScriptMgr->OnQuestComplete(hookPlayer, hookCreature, &hookQuest));
    EXPECT_EQ(fallbackScript->completeCalls, 1u);
    fallbackScript->completeHandled = false;
    EXPECT_FALSE(sScriptMgr->OnQuestComplete(hookPlayer, hookCreature, &hookQuest));
    EXPECT_EQ(fallbackScript->completeCalls, 2u);

    std::string const request =
        "[{'source_table':'npc_vendor','natural_key':{'entry':3900001,'slot':37,'item':1999999999}}]";
    auto const initial = sPythonEngine->ExecuteCode(
        "import azerothcore as ac\n"
        "source_request = " + request + "\n"
        "source_first = ac.strategic_read_loaded_source_snapshot(source_request)\n"
        "assert source_first['schema'] == 'cpi211-loaded-item-sources-v1'\n"
        "assert source_first['valid'] is True\n"
        "assert source_first['coverage']['requested_rows'] == 1\n"
        "assert source_first['coverage']['matched_rows'] == 1\n"
        "assert source_first['coverage']['unmatched_rows'] == 0\n"
        "assert source_first['coverage']['not_evaluated_rows'] == 0\n"
        "assert source_first['coverage']['all_fourteen_family_parity'] is False\n"
        "assert source_first['action_authority'] is False and source_first['quote_bound'] is False\n"
        "assert source_first['reload_generation_available'] is False\n"
        "assert source_first['reload_generation'] is None\n"
        "assert len(source_first['source_snapshot_id']) == 64\n"
        "assert source_first['source_snapshot_id'].upper() == source_first['source_snapshot_id']\n"
        "source_row = source_first['rows'][0]\n"
        "assert source_row['source_table'] == 'npc_vendor'\n"
        "assert source_row['natural_key'] == {'entry':3900001,'slot':37,'item':1999999999}\n"
        "assert source_row['stable_join_key'] == 'vendor:3900001:slot:37:item:1999999999'\n"
        "assert source_row['matched'] is True\n"
        "assert source_row['native_semantics']['maxcount'] == 3\n"
        "assert source_row['native_semantics']['incrtime'] == 45\n"
        "assert source_row['native_semantics']['ExtendedCost'] == 0\n"
        "assert source_row['native_semantics']['item_template']['known'] is False\n"
        "assert source_row['native_semantics']['item_template']['buy_price'] is None\n"
        "assert source_row['metrics']['risk']['state'] == 'unknown'\n"
        "assert source_row['metrics']['risk']['value'] is None\n"
        "assert source_row['metrics']['contention']['state'] == 'unknown'\n"
        "assert 'VerifiedBuild' in source_row['unknown_semantics']\n"
        "source_repeat = ac.strategic_read_loaded_source_snapshot(source_request)\n"
        "assert source_repeat['source_snapshot_id'] == source_first['source_snapshot_id']\n"
        "assert source_repeat['capture_epoch_monotonic_ns'] >= source_first['capture_epoch_monotonic_ns']\n"
        "loaded_enum = ac.strategic_enumerate_loaded_vendor_sources(16384)\n"
        "assert loaded_enum['schema'] == 'cpi211-loaded-vendor-enumeration-v1'\n"
        "assert loaded_enum['valid'] is True and loaded_enum['complete'] is True\n"
        "assert loaded_enum['truncated'] is False and loaded_enum['one_shot'] is True\n"
        "assert loaded_enum['cursor'] is None and loaded_enum['reload_generation_available'] is False\n"
        "assert loaded_enum['actor_eligibility_evaluated'] is False and loaded_enum['resources_evaluated'] is False\n"
        "assert loaded_enum['action_authority'] is False and loaded_enum['quote_bound'] is False\n"
        "assert len(loaded_enum['native_source_snapshot_id']) == 64\n"
        "assert loaded_enum['native_source_snapshot_id'].upper() == loaded_enum['native_source_snapshot_id']\n"
        "loaded_enum_repeat = ac.strategic_enumerate_loaded_vendor_sources(16384)\n"
        "assert loaded_enum_repeat['native_source_snapshot_id'] == loaded_enum['native_source_snapshot_id']\n"
        "assert loaded_enum['native_source_snapshot_id'] != source_first['source_snapshot_id']\n"
        "assert loaded_enum['coverage']['raw_source_generation_coverage'] == 'unknown'\n"
        "assert loaded_enum['coverage']['all_fourteen_family_parity'] is False\n"
        "direct = next(row for row in loaded_enum['rows'] if row['source_key_known'] and row['natural_key']['entry'] == 3900001)\n"
        "assert direct['natural_key'] == {'entry':3900001,'slot':37,'item':1999999999}\n"
        "assert direct['loaded_index'] == 0 and direct['loaded_item'] == 1999999999\n"
        "assert direct['source_kind'] == 'npc_vendor_row'\n"
        "assert direct['metrics']['risk']['state'] == 'unknown' and direct['metrics']['risk']['value'] is None\n"
        "assert direct['metrics']['contention']['state'] == 'unknown' and direct['metrics']['contention']['value'] is None\n"
        "reference = next(row for row in loaded_enum['rows'] if row['source_kind'] == 'vendor_reference_expansion')\n"
        "runtime = next(row for row in loaded_enum['rows'] if row['source_kind'] == 'runtime_vendor_item')\n"
        "assert reference['source_key_known'] is False and reference['natural_key'] is None\n"
        "assert runtime['source_key_known'] is False and runtime['natural_key'] is None\n"
        "assert loaded_enum['coverage']['unsupported_reference_rows'] >= 1\n"
        "assert loaded_enum['coverage']['runtime_rows'] >= 1\n"
        "import os, pathlib, sys\n"
        "native_test_root = pathlib.Path(os.environ['CPI211_NATIVE_TEST_ROOT'])\n"
        "task_root = pathlib.Path(os.environ['CPI211_TASK_ROOT'])\n"
        "compiled_package_dir = pathlib.Path(os.environ['CPI211_COMPILED_PACKAGE_DIR'])\n"
        "package_dir = task_root / 'worker/wow-private-server/tools/playerbots-strategic-director'\n"
        "consumer_dir = task_root / 'worker/wow-private-server/integration/cpi-225-native-earned-upgrade-20261021/runtime/population/python/lib'\n"
        "os.environ['PBGUIDE_TEST_PACKAGE_DIR'] = str(package_dir)\n"
        "os.environ['PBGUIDE_TEST_CONSUMER_DIR'] = str(consumer_dir)\n"
        "sys.path.insert(0, str(consumer_dir))\n"
        "sys.path.insert(0, str(consumer_dir.parents[3] / 'tests'))\n"
        "sys.path.insert(0, str(compiled_package_dir))\n"
        "sys.path.insert(0, str(package_dir))\n"
        "import test_native_acquisition_v2_consumer as v2_consumer_test\n"
        "for bad_cap in [0, True, '16384', 16385]:\n"
        "    try:\n"
        "        ac.strategic_enumerate_loaded_vendor_sources(bad_cap)\n"
        "        raise AssertionError('invalid loaded vendor enumeration cap accepted')\n"
        "    except RuntimeError as error:\n"
        "        assert '1..16384' in str(error)\n"
        "partial = ac.strategic_enumerate_loaded_vendor_sources(1)\n"
        "assert partial['valid'] is True and partial['complete'] is False and partial['truncated'] is True\n"
        "assert partial['native_source_snapshot_id'] is None\n"
        "assert partial['copied_rows'] == 1 and partial['row_cap'] == 1\n");
    ASSERT_TRUE(initial) << initial.error;

    auto const bootstrapSetup = sPythonEngine->ExecuteCode(
        "cpi211_test_witness = None\n"
        "cpi211_test_actor_identity = '" + gearIdentity + "'\n"
        "cpi211_test_gear_snapshot = None\n"
        "cpi211_test_probe_removed_identity = False\n"
        "cpi211_removed_identity = None\n"
        "cpi211_population_tick_result = None\n");
    ASSERT_TRUE(bootstrapSetup) << bootstrapSetup.error;
    EXPECT_TRUE(sPythonEngine->RunPopulationTick(1));
    auto const bootstrapReadback = sPythonEngine->ExecuteCode(
        "assert cpi211_population_tick_result is True, repr(cpi211_population_tick_result)\n"
        "assert cpi211_test_gear_snapshot is not None, repr(cpi211_test_gear_snapshot)\n"
        "assert cpi211_test_gear_snapshot['control_allowed'] is True, repr(cpi211_test_gear_snapshot)\n"
        "assert cpi211_test_gear_snapshot['class_id'] == 1, repr(cpi211_test_gear_snapshot)\n");
    ASSERT_TRUE(bootstrapReadback) << bootstrapReadback.error;

    AchievementHookRegistryFixture achievementHookRegistryFixture;
    auto const stagedTestConsumer = sPythonEngine->ExecuteCode(
        "cpi211_test_witness = v2_consumer_test.run_embedded_compiled_provider_consumer(ac, '" + gearIdentity + "', cpi211_test_gear_snapshot)\n"
        "cpi211_population_tick_result = None\n"
        "cpi211_population_tick_error = None\n"
        "cpi211_replay_result = None\n");
    ASSERT_TRUE(stagedTestConsumer) << stagedTestConsumer.error;
    EXPECT_TRUE(sPythonEngine->RunPopulationTick(1));
    auto const consumerTickDiagnostic = sPythonEngine->ExecuteCode(
        "assert cpi211_population_tick_result is True, repr((cpi211_population_tick_result, cpi211_population_tick_error, cpi211_test_witness.status, cpi211_test_witness._cpi211_test_audit_rows))\n"
        "assert cpi211_population_tick_error is None, repr((cpi211_population_tick_error, cpi211_test_witness.status, cpi211_test_witness._cpi211_test_audit_rows))\n");
    ASSERT_TRUE(consumerTickDiagnostic) << consumerTickDiagnostic.error;
    auto const testConsumerReceipt = sPythonEngine->ExecuteCode(
        "assert cpi211_population_tick_result is True, repr((cpi211_population_tick_result, cpi211_population_tick_error))\n"
        "assert cpi211_native_offer['source_id'] == 'vendor:3900002:slot:0:item:103:cost:0'\n"
        "assert len(cpi211_native_offer['request_id']) == 64 and all(character in '0123456789abcdef' for character in cpi211_native_offer['request_id'])\n"
        "assert any(character in 'abcdef' for character in cpi211_native_offer['request_id'])\n"
        "assert cpi211_native_offer['request_id'] == [row for row in cpi211_test_witness._cpi211_test_audit_rows if row.get('event') == 'vendor_purchase'][0]['native_receipt']['request_id']\n"
        "assert cpi211_test_witness.status['reason'] == ''\n"
        "assert type(cpi211_test_witness.strategy) is v2_consumer_test.GearPolicy, repr((type(cpi211_test_witness.strategy), v2_consumer_test.GearPolicy, type(cpi211_test_witness.strategy).__module__, getattr(__import__(type(cpi211_test_witness.strategy).__module__), '__file__', None)))\n"
        "plans = [row for row in cpi211_test_witness._cpi211_test_audit_rows if row.get('event') == 'gear_recommendation']\n"
        "assert len(plans) == 1 and plans[0]['plan']['selected_source_id'] == 'source:03'\n"
        "assert plans[0]['plan']['selected_target_id'] == 'target:item-103-slot-16'\n"
        "purchases = [row for row in cpi211_test_witness._cpi211_test_audit_rows if row.get('event') == 'vendor_purchase']\n"
        "assert len(purchases) == 1\n"
        "assert purchases[0]['source_id'] == 'source:03' and purchases[0]['target']['slot'] == 16\n"
        "receipt = purchases[0]['native_receipt']\n"
        "assert receipt['status'] == 'committed' and receipt['item'] == 103, repr(receipt)\n"
        "assert receipt['money_before'] - receipt['money_after'] == receipt['price_copper'] == 100\n"
        "assert receipt['item_before'] == 0 and receipt['item_after'] == 1 and receipt['item_guid'] > 0\n"
        "assert cpi211_replay_result['status'] == 'refused_before_effect'\n"
        "assert cpi211_replay_result['reason'] != ''\n");
    ASSERT_TRUE(testConsumerReceipt) << testConsumerReceipt.error;
    EXPECT_EQ(gearActor->GetMoney(), 900u);
    EXPECT_EQ(gearActor->GetItemCount(TestItemEntry, false), 1u);

    auto const negativeCases = sPythonEngine->ExecuteCode(
        "missing = ac.strategic_read_loaded_source_snapshot([{'source_table':'npc_vendor','natural_key':{'entry':3900001,'slot':38,'item':1999999999}}])\n"
        "assert missing['valid'] is True and missing['rows'][0]['matched'] is False\n"
        "assert missing['rows'][0]['reason'] == 'vendor_source_row_not_loaded_or_filtered'\n"
        "assert missing['coverage']['unmatched_rows'] == 1 and missing['coverage']['not_evaluated_rows'] == 0\n"
        "reference = ac.strategic_read_loaded_source_snapshot([{'source_table':'npc_vendor','natural_key':{'entry':3900001,'slot':8,'item':-2048}}])\n"
        "assert reference['rows'][0]['lookup_evaluated'] is True\n"
        "assert reference['rows'][0]['reason'] == 'vendor_reference_row_not_direct_item'\n"
        "for bad in [True, '4294967280', [1,2]]:\n"
        "    try:\n"
        "        ac.strategic_read_loaded_source_snapshot([{'source_table':'npc_vendor','natural_key':{'entry':3900001,'slot':37,'item':bad}}])\n"
        "        raise AssertionError('bad item key accepted')\n"
        "    except RuntimeError as error:\n"
        "        assert 'signed 32-bit integer' in str(error)\n"
        "try:\n"
        "    ac.strategic_read_loaded_source_snapshot([{'source_table':'npc_vendor','natural_key':{'entry':3900001,'slot':37,'item':1999999999}}] * 65)\n"
        "    raise AssertionError('over-bound request accepted')\n"
        "except RuntimeError as error:\n"
        "    assert '1..64' in str(error)\n"
        "try:\n"
        "    ac.strategic_read_loaded_source_snapshot([{'source_table':'npc_vendor','natural_key':{'entry':3900001,'slot':37,'item':1999999999}}, {'source_table':'npc_vendor','natural_key':{'entry':3900001,'slot':37,'item':1999999999}}])\n"
        "    raise AssertionError('duplicate natural key accepted')\n"
        "except RuntimeError as error:\n"
        "    assert 'duplicated' in str(error)\n");
    ASSERT_TRUE(negativeCases) << negativeCases.error;

    VendorSourceTestAccess::ReplaceRows(*sObjectMgr, VendorEntry, {
        VendorItem(ItemEntry, 3, 45, 0, VendorItemSourceKind::NpcVendorRow, SourceSlot),
        VendorItem(ItemEntry, 4, 90, 0, VendorItemSourceKind::NpcVendorRow, SourceSlot)
    });
    auto const ambiguous = sPythonEngine->ExecuteCode(
        "source_ambiguous = ac.strategic_read_loaded_source_snapshot(source_request)\n"
        "assert source_ambiguous['valid'] is True\n"
        "assert source_ambiguous['rows'][0]['matched'] is False\n"
        "assert source_ambiguous['rows'][0]['reason'] == 'vendor_source_row_ambiguous'\n"
        "assert source_ambiguous['coverage']['unmatched_rows'] == 1\n"
        "ambiguous_enum = ac.strategic_enumerate_loaded_vendor_sources(16384)\n"
        "duplicate_rows = [row for row in ambiguous_enum['rows'] if row['source_key_known'] and row['natural_key']['entry'] == 3900001]\n"
        "assert len(duplicate_rows) == 2\n"
        "assert duplicate_rows[0]['natural_key'] == duplicate_rows[1]['natural_key']\n"
        "assert duplicate_rows[0]['loaded_index'] != duplicate_rows[1]['loaded_index']\n");
    ASSERT_TRUE(ambiguous) << ambiguous.error;

    VendorSourceTestAccess::ReplaceRows(*sObjectMgr, VendorEntry, {
        VendorItem(ItemEntry, 5, 45, 0, VendorItemSourceKind::NpcVendorRow, SourceSlot)
    });
    auto const changed = sPythonEngine->ExecuteCode(
        "source_changed = ac.strategic_read_loaded_source_snapshot(source_request)\n"
        "assert source_changed['rows'][0]['matched'] is True\n"
        "assert source_changed['source_snapshot_id'] != source_first['source_snapshot_id']\n"
        "loaded_changed = ac.strategic_enumerate_loaded_vendor_sources(16384)\n"
        "assert loaded_changed['complete'] is True\n"
        "assert loaded_changed['native_source_snapshot_id'] != loaded_enum['native_source_snapshot_id']\n");
    ASSERT_TRUE(changed) << changed.error;

    std::string wrongThreadError;
    bool wrongThreadExecutionSucceeded = false;
    std::thread wrongThread([&]
    {
        auto const refusal = sPythonEngine->ExecuteCode(
            "try:\n"
            "    ac.strategic_read_loaded_source_snapshot(source_request)\n"
            "    raise AssertionError('native read accepted off world thread')\n"
            "except RuntimeError as error:\n"
            "    assert 'native world thread' in str(error)\n"
            "try:\n"
            "    ac.strategic_enumerate_loaded_vendor_sources(16384)\n"
            "    raise AssertionError('loaded vendor enumeration accepted off world thread')\n"
            "except RuntimeError as error:\n"
            "    assert 'native world thread' in str(error)\n");
        wrongThreadExecutionSucceeded = static_cast<bool>(refusal);
        wrongThreadError = refusal.error;
    });
    wrongThread.join();
    EXPECT_TRUE(wrongThreadExecutionSucceeded) << wrongThreadError;

    ASSERT_TRUE(writeConfig(false));
    sConfigMgr->Configure(configPath.string(), {});
    ASSERT_TRUE(sConfigMgr->LoadAppConfigs());
    sScriptMgr->OnAfterConfigLoad(false);
    auto const phaseRefusal = sPythonEngine->ExecuteCode(
        "source_refused = ac.strategic_read_loaded_source_snapshot(source_request)\n"
        "assert source_refused['valid'] is False\n"
        "assert source_refused['reason'] == 'shared_phase_unavailable'\n"
        "assert source_refused['coverage']['not_evaluated_rows'] == 1\n"
        "assert source_refused['coverage']['unmatched_rows'] == 0\n"
        "enum_refused = ac.strategic_enumerate_loaded_vendor_sources(16384)\n"
        "assert enum_refused['valid'] is False and enum_refused['complete'] is False\n"
        "assert enum_refused['native_source_snapshot_id'] is None\n");
    EXPECT_TRUE(phaseRefusal) << phaseRefusal.error;

    UnregisterCpi211TestOnlyPopulationActor(gearIdentity, gearActor);
    gearActorCleanup.actorRegistered = false;
    auto const removedIdentityProbe = sPythonEngine->ExecuteCode("cpi211_test_probe_removed_identity = True\n");
    ASSERT_TRUE(removedIdentityProbe) << removedIdentityProbe.error;
    EXPECT_TRUE(sPythonEngine->RunPopulationTick(1));
    auto const fixtureIdentityRemoval = sPythonEngine->ExecuteCode(
        "assert cpi211_removed_identity['reason'] == 'not_admitted'\n");
    EXPECT_TRUE(fixtureIdentityRemoval) << fixtureIdentityRemoval.error;
    gearActorCleanup.Cleanup();
    EXPECT_EQ(sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(GearActorGuid)), previousGuidCache);
    EXPECT_EQ(sCharacterCache->GetCharacterCacheByName("Cpi211TestGearActor"), previousNameCache);
    EXPECT_EQ(sRandomPlayerbotMgr.GetPlayerBot(GearActorGuid), nullptr);
    EXPECT_EQ(VendorSourceTestAccess::HasCurrentBot(sRandomPlayerbotMgr, GearActorGuid), previousCurrentBot);
}
