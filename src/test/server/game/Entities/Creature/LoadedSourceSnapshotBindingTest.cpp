#include "Config.h"
#include "Database/Field.h"
#include "Globals/ObjectMgr.h"
#include "DataStores/DBCStores.h"
#include "Quests/QuestDef.h"
#include "Scripting/ScriptDefines/AllCreatureScript.h"
#include "Scripting/ScriptDefines/CreatureScript.h"
#include "Hooks/Defines/CreatureHook.h"
#include "Scripting/ScriptMgr.h"
#include "PopulationBridge.h"
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

class VendorSourceTestAccess
{
public:
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
    std::filesystem::path const configPath = testRoot / "native-source-observation.conf";
    struct ConfigCleanup
    {
        std::filesystem::path path;
        ~ConfigCleanup()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } configCleanup{configPath};

    auto writeConfig = [&](bool gateEnabled)
    {
        std::ofstream config(configPath, std::ios::trunc);
        config << "[worldserver]\n"
               << "Python.Enabled = 1\n"
               << "Python.Population.Enabled = 1\n"
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

    VendorSourceTestAccess::ReplaceRows(*sObjectMgr, VendorEntry, {
        VendorItem(ItemEntry, 3, 45, 0, VendorItemSourceKind::NpcVendorRow, SourceSlot)
    });
    VendorItemData const* const shiftedList = sObjectMgr->GetNpcVendorItemList(VendorEntry);
    ASSERT_NE(shiftedList, nullptr);
    ASSERT_EQ(shiftedList->GetItemCount(), 1u);
    ASSERT_NE(shiftedList->GetItem(0), nullptr);
    EXPECT_EQ(shiftedList->GetItem(0)->sourceSlot, SourceSlot)
        << "the loaded-list index 0 must remain distinct from the original npc_vendor slot 37";
    RegisterPythonPopulationBridge();
    PyEng::Population::Bridge* const registeredProvider = PyEng::Population::GetBridge();
    ASSERT_NE(registeredProvider, nullptr);

    struct Cleanup
    {
        ObjectMgr& manager;
        uint32 entry;
        PyEng::Population::Bridge* provider;
        ~Cleanup()
        {
            if (sPythonEngine->IsEnabled())
                sPythonEngine->Shutdown();
            if (provider)
                PyEng::Population::UnregisterBridge(provider);
            VendorSourceTestAccess::ClearRows(manager, entry);
        }
    } cleanup{*sObjectMgr, VendorEntry, registeredProvider};

    sPythonEngine->Initialize();
    ASSERT_TRUE(sPythonEngine->IsEnabled());

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
        "assert source_repeat['capture_epoch_monotonic_ns'] >= source_first['capture_epoch_monotonic_ns']\n");
    ASSERT_TRUE(initial) << initial.error;

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
        "assert source_ambiguous['coverage']['unmatched_rows'] == 1\n");
    ASSERT_TRUE(ambiguous) << ambiguous.error;

    VendorSourceTestAccess::ReplaceRows(*sObjectMgr, VendorEntry, {
        VendorItem(ItemEntry, 5, 45, 0, VendorItemSourceKind::NpcVendorRow, SourceSlot)
    });
    auto const changed = sPythonEngine->ExecuteCode(
        "source_changed = ac.strategic_read_loaded_source_snapshot(source_request)\n"
        "assert source_changed['rows'][0]['matched'] is True\n"
        "assert source_changed['source_snapshot_id'] != source_first['source_snapshot_id']\n");
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
        "assert source_refused['coverage']['unmatched_rows'] == 0\n");
    EXPECT_TRUE(phaseRefusal) << phaseRefusal.error;
}
