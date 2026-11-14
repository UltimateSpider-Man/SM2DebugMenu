// SPDX-License-Identifier: GPL-3.0-or-later
#include "debugmenu.h"
#include "overlay.h"
#include "screen_capture.h"
#include "reference_catalog.h"
#include "native_dev_catalog.h"
#include "reload_mission_catalog.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace sm2debug {
namespace {
std::mutex inputMutex;
std::deque<std::pair<MenuInput, DWORD>> inputQueue;
std::atomic<bool> visible{false};
std::atomic<bool> preview{false}, uiReady{false};
bool traceInput = false;
DWORD controllerProducer = 0;
int page = -1, selected = 0, firstRow = 0, character = 0;
std::string node = "debug";
std::vector<std::string> history;
float healthLimit = 100, moveStep = 10;
ReloadToken reloadSelectionContext;
uint32_t reloadCheckpointKey = 0;
bool reloadResetActorSave = false;
bool reloadKeepCurrentLoadout = false;
bool reloadReplayEnabled = false, reloadFilterCheckpoints = false;
bool reloadMissionDescending = false, reloadObjectiveDescending = false;
ReplaySelection reloadReplaySelection;
MissionStartState reloadStartState=MissionStartState::Active;
struct ExtraBranch { ReloadMissionBranch value; bool pickFirstObjective=false; };
std::vector<ExtraBranch> reloadExtraBranches;
bool reloadMissionEdited=false,reloadPickFirstObjective=false,reloadPickCheckpoint=false;
ReloadMissionSelection publishedMissionSelection;
ReloadToken publishedMissionWorld;
bool havePublishedMission=false;
const std::array<const char*, 5> pages{{"Common Items", "Health", "Hero", "Rendering", "Engine"}};
enum class Kind { Page, Toggle, Action, HealthValue, StepValue, Character, Setting, ReloadCheckpoint, ReloadActorReset,
    ReloadReplay, ReloadMission, ReloadObjective, ReloadGraph, ReloadMissionSort, ReloadObjectiveSort, ReloadFilter,
    ReloadNewGamePlus, ReloadKeepLoadout, CaptureFormat, CaptureDisplay, CameraShake,
    ReloadStartState, BranchAdd, BranchRemoveEmpty, BranchRemoveAll, BranchMission, BranchObjective, BranchRemove, Info, Close };
struct Row {
    std::string name, value;
    Kind kind = Kind::Info;
    Action action = Action::ExportDiagnostics;
    bool enabled = true, checked = false;
    int destination = -1;
    std::string target{};
    bool heading = false;
    std::string settingId{};
    ReloadState reload{};
    uint32_t checkpointKey = 0;
    PrimaryObjectiveState primary{};
    ActorSaveState actorSave{};
    bool resetActorSave = false;
    ReplayState replay{};
    ReplaySelection replaySelection{};
    bool replayMode = false;
    std::shared_ptr<const std::vector<ReloadCheckpoint>> checkpointChoices{};
    ReloadOptionsState reloadOptions{};
    std::string sourceHelp{};
    bool keepCurrentLoadout = true;
    CameraDebugState cameraDebug{};
    ReloadSelectionState missionSelection{};
    bool applyMissionSelection=false,allowAvailable=false;
    std::size_t branchIndex=0;
    std::vector<std::uint32_t> missionChoices{},objectiveChoices{};
};
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::string Number(double value, int decimals = 1) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}
Row Info(const char* name, std::string value = {}) { return {name, std::move(value), Kind::Info}; }
Row Command(const char* name, Action action, bool enabled = true) {
    return {name, {}, Kind::Action, action, enabled};
}
Row Toggle(const char* name, Action action, bool checked, bool enabled) {
    return {name, checked ? "On" : "Off", Kind::Toggle, action, enabled, checked};
}
void BindSetting(Row& row, const Snapshot& s, const std::string& pageId) {
    for (const auto& setting : s.settings) {
        if (setting.pageId != pageId || setting.label != row.name || !setting.available) continue;
        row.kind = Kind::Setting;
        row.settingId = setting.id;
        row.enabled = s.engineActionsReady;
        if (setting.type == DebugSettingType::Boolean) row.value = setting.number != 0 ? "On" : "Off";
        else if (setting.type == DebugSettingType::Enum32 && setting.number >= 0 &&
                 setting.number < setting.choices.size()) row.value = setting.choices[static_cast<size_t>(setting.number)];
        else row.value = Number(setting.number, setting.type == DebugSettingType::Float32 ? 4 : 0);
        return;
    }
}
ReloadMissionSelection EditedMissionSelection() {
    ReloadMissionSelection result;result.graphId=reloadReplaySelection.graphId;
    if(reloadReplaySelection.missionHash)result.branches.push_back({reloadReplaySelection.missionHash,reloadReplaySelection.objectiveHash,reloadStartState});
    for(const auto& extra:reloadExtraBranches)result.branches.push_back(extra.value);
    // Native mission ordering is read through this adapter; Replay execution
    // remains a separate verified API because its old array flag was removed.
    return result;
}
bool SameMissionSelection(const ReloadMissionSelection& a,const ReloadMissionSelection& b) {
    if(a.graphId!=b.graphId||a.replayMode!=b.replayMode||a.branches.size()!=b.branches.size())return false;
    for(std::size_t i=0;i<a.branches.size();++i)if(a.branches[i].missionHash!=b.branches[i].missionHash||
        a.branches[i].objectiveHash!=b.branches[i].objectiveHash||a.branches[i].state!=b.branches[i].state)return false;
    return true;
}
bool MissionMetadataMatches(const Snapshot& s) {
    return s.reloadSelection.supported&&SameReloadContext(s.reload.token,s.reloadSelection.token.world)&&
        SameMissionSelection(EditedMissionSelection(),s.reloadSelection.selection);
}
void PublishMissionSelection(bool publishReplay=false) {
    const auto selection=EditedMissionSelection();
    if(!havePublishedMission||!SameReloadContext(publishedMissionWorld,reloadSelectionContext)||!SameMissionSelection(publishedMissionSelection,selection)){
        SubmitReloadMissionSelection(selection);publishedMissionSelection=selection;
        publishedMissionWorld=reloadSelectionContext;havePublishedMission=true;
    }
    if(publishReplay)SubmitReplaySelection(reloadReplayEnabled?reloadReplaySelection:ReplaySelection{});
}
const char* StartStateName(MissionStartState state) {
    switch(state){case MissionStartState::Active:return "Active";case MissionStartState::Available:return "Available";case MissionStartState::Complete:return "Complete";}
    return "Unavailable";
}
std::vector<std::uint32_t> MissionChoices(const Snapshot& s) {
    if(reloadMissionDescending){
        if(s.reloadSelection.selection.graphId==reloadReplaySelection.graphId&&SameReloadContext(s.reload.token,s.reloadSelection.token.world))return s.reloadSelection.missionOrder;
        return {};
    }
    std::vector<std::uint32_t> result;
    if(const auto* graph=FindReloadMissionGraph(reloadReplaySelection.graphId))for(const auto& mission:graph->missions)result.push_back(mission.nameCrc);
    return result;
}
std::vector<std::uint32_t> ObjectiveChoices(const Snapshot& s,std::size_t index,std::uint32_t mission) {
    std::vector<std::uint32_t> result{0};
    if(reloadObjectiveDescending){
        if(s.reloadSelection.selection.graphId!=reloadReplaySelection.graphId||!SameReloadContext(s.reload.token,s.reloadSelection.token.world)||
           index>=s.reloadSelection.selection.branches.size()||index>=s.reloadSelection.branches.size()||
           s.reloadSelection.selection.branches[index].missionHash!=mission)return {};
        const auto& native=s.reloadSelection.branches[index].objectiveOrder;result.insert(result.end(),native.begin(),native.end());
    }else if(const auto* selectedMission=FindReloadMission(reloadReplaySelection.graphId,mission))
        for(const auto& objective:selectedMission->objectives)result.push_back(objective.nameCrc);
    return result;
}
std::uint32_t CycleExactName(const std::vector<std::uint32_t>& choices,std::uint32_t current,int direction) {
    if(choices.empty())return current;
    const auto found=std::find(choices.begin(),choices.end(),current);
    if(found==choices.end())return direction<0?choices.back():choices.front();
    const auto index=static_cast<std::size_t>(found-choices.begin());
    return choices[(index+choices.size()+(direction<0?-1:1))%choices.size()];
}
void SyncReloadSelection(const Snapshot& s) {
    const auto priorReplay=reloadReplaySelection;
    const bool clearReplay=reloadReplayEnabled&&!SameReloadContext(reloadSelectionContext,s.reload.token);
    if (!SameReloadContext(reloadSelectionContext, s.reload.token)) {
        reloadCheckpointKey = 0;
        reloadResetActorSave = false;
        reloadReplayEnabled = false;
        reloadFilterCheckpoints = false;
        reloadReplaySelection = {};
        reloadStartState=MissionStartState::Active;reloadExtraBranches.clear();
        reloadMissionEdited=reloadPickFirstObjective=reloadPickCheckpoint=false;
        reloadSelectionContext = s.reload.token;
    }
    if (!reloadMissionEdited&&(!reloadReplayEnabled || !reloadReplaySelection.graphId) && s.primary.ready &&
        FindReloadMission(s.primary.token.missionGraphAssetId, s.primary.token.missionHash)) {
        reloadReplaySelection = {s.primary.token.missionGraphAssetId, s.primary.token.missionHash, 0};
        if (FindReloadMissionObjective(reloadReplaySelection.graphId, reloadReplaySelection.missionHash, s.primary.token.objectiveHash))
            reloadReplaySelection.objectiveHash = s.primary.token.objectiveHash;
    }
    // Browsing immutable mission metadata does not require an active primary
    // objective. Native replay remains gated by the separately validated state.
    if (!FindReloadMission(reloadReplaySelection.graphId, reloadReplaySelection.missionHash)) {
        const auto* graph = s.primary.ready ? FindReloadMissionGraph(s.primary.token.missionGraphAssetId) : nullptr;
        if (!graph || graph->missions.empty()) {
            const auto& catalog = ReloadMissionCatalog();
            const auto first = std::find_if(catalog.begin(), catalog.end(), [](const ReloadMissionGraph& item) {
                return !item.missions.empty();
            });
            graph = first != catalog.end() ? &*first : nullptr;
        }
        reloadReplaySelection = graph ? ReplaySelection{graph->graphAssetId, graph->missions.front().nameCrc, 0} : ReplaySelection{};
    }
    if(MissionMetadataMatches(s)) {
        const auto& details=s.reloadSelection.branches;
        if(reloadPickFirstObjective&&!details.empty()&&(!details[0].objectiveOrder.empty()||s.reloadSelection.ready)){
            reloadReplaySelection.objectiveHash=details[0].objectiveOrder.empty()?0:details[0].objectiveOrder.front();
            reloadPickFirstObjective=false;
        }
        for(std::size_t i=0;i<reloadExtraBranches.size()&&i+1<details.size();++i)if(reloadExtraBranches[i].pickFirstObjective&&
            (!details[i+1].objectiveOrder.empty()||s.reloadSelection.ready)){
            reloadExtraBranches[i].value.objectiveHash=details[i+1].objectiveOrder.empty()?0:details[i+1].objectiveOrder.front();
            reloadExtraBranches[i].pickFirstObjective=false;
        }
        if(reloadPickCheckpoint&&!reloadPickFirstObjective&&!details.empty()&&s.reloadSelection.ready&&MissionMetadataMatches(s)){
            const auto key=details[0].checkpoint;
            if(key&&s.reload.checkpoints&&std::any_of(s.reload.checkpoints->begin(),s.reload.checkpoints->end(),[&](const ReloadCheckpoint& item){return item.key==key;}))reloadCheckpointKey=key;
            reloadPickCheckpoint=false;
        }
    }
    PublishMissionSelection(clearReplay||(reloadReplayEnabled&&!SameReplaySelection(priorReplay,reloadReplaySelection)));
}
std::shared_ptr<const std::vector<ReloadCheckpoint>> ReloadChoices(const Snapshot& s) {
    if (!reloadFilterCheckpoints) return s.reload.checkpoints;
    auto choices = std::make_shared<std::vector<ReloadCheckpoint>>();
    const auto* mission = FindReloadMission(s.primary.token.missionGraphAssetId, s.primary.token.missionHash);
    if (!mission || !s.reload.checkpoints) return choices;
    for (const auto& checkpoint : *s.reload.checkpoints) {
        const auto matches = [&checkpoint](const ReloadMissionCheckpoint& item) {
            return checkpoint.key == item.nameCrc && Utf8(checkpoint.name) == item.name;
        };
        if (matches(mission->availableCheckpoint) || std::any_of(mission->objectives.begin(), mission->objectives.end(),
            [&](const ReloadMissionObjective& objective) { return matches(objective.checkpoint); })) choices->push_back(checkpoint);
    }
    return choices;
}
void BindReload(Row& row, const Snapshot& s, const std::shared_ptr<const std::vector<ReloadCheckpoint>>& choices) {
    std::string checkpoint = "Current checkpoint";
    bool found = reloadCheckpointKey == 0;
    if (choices) for (const auto& item : *choices) {
        if (item.key == reloadCheckpointKey) { checkpoint = Utf8(item.name); found = true; break; }
    }
    if (!found) reloadCheckpointKey = 0;
    const bool ready = s.supported && s.reload.ready;
    const bool metadata=MissionMetadataMatches(s);
    if (row.name == "Checkpoint") {
        row.kind = Kind::ReloadCheckpoint;
        row.enabled = ready && !reloadReplayEnabled;
        row.value = reloadReplayEnabled ? "From replay objective" : (ready ? checkpoint : "Waiting for level");
    } else if (row.name == "Reload") {
        row = reloadReplayEnabled ? Command("Reload", Action::ReplayMission, ready && s.engineActionsReady && s.replay.ready &&
            SameReplaySelection(s.replay.selection, reloadReplaySelection)) :
            Command("Reload", Action::ReloadCheckpoint, ready && s.engineActionsReady && (!reloadResetActorSave || s.actorSave.ready));
    } else if (row.name == "Full Reload") {
        row = Command("Full Reload", Action::FullReload, ready && s.engineActionsReady && (!reloadResetActorSave || s.actorSave.ready));
    } else if (row.name == "Exit to Frontend") {
        row = Command("Exit to Frontend", Action::ExitFrontend, ready && s.engineActionsReady && s.reload.frontendReady);
    } else if (row.name == "Exit to Lobby") {
        row = Command("Exit to Lobby", Action::ExitLobby, ready && s.engineActionsReady && s.reload.lobbyReady);
    } else if (row.name == "Copy Reload Data to Clipboard") {
        row = Command("Copy Reload Data to Clipboard", Action::CopyReloadData, ready);
    } else if (row.name == "Complete Primary Objective (Preserve Progression)") {
        row = Command("Complete Primary Objective (Preserve Progression)", Action::CompletePrimary,
                      s.supported && s.primary.ready && s.engineActionsReady);
        row.primary = s.primary;
    } else if (row.name == "Reset Actor Save on Manual Reload") {
        row.kind = Kind::ReloadActorReset;
        row.enabled = !reloadReplayEnabled && (reloadResetActorSave || (ready && s.actorSave.ready));
        row.value = reloadResetActorSave ? "On" : "Off";
        row.checked = reloadResetActorSave;
    } else if (row.name == "Keep Current Loadout") {
        row.kind = Kind::ReloadKeepLoadout;
        row.enabled = ready;
        row.checked = reloadKeepCurrentLoadout;
        row.value = row.checked ? "On" : "Off";
    } else if (row.name == "NG+") {
        row.kind = Kind::ReloadNewGamePlus;
        row.enabled = ready && s.engineActionsReady && s.reloadOptions.supported && s.reloadOptions.ready;
        row.checked = s.reloadOptions.newGamePlus;
        row.value = s.reloadOptions.ready ? (row.checked ? "On" : "Off") : "Waiting for save";
        row.reloadOptions = s.reloadOptions;
    } else if (row.name == "Replay Mode") {
        row.kind = Kind::ReloadReplay; row.enabled = reloadReplayEnabled ||
            (ready && FindReloadMission(reloadReplaySelection.graphId, reloadReplaySelection.missionHash));
        row.checked = reloadReplayEnabled; row.value = reloadReplayEnabled ? "On" : "Off";
    } else if (row.name == "Mission") {
        const auto* mission = FindReloadMission(reloadReplaySelection.graphId, reloadReplaySelection.missionHash);
        row.kind = Kind::ReloadMission; row.enabled = ready && mission;
        row.missionChoices=MissionChoices(s);row.enabled=row.enabled&&!row.missionChoices.empty();
        row.value = mission ? mission->name : "Waiting for mission";
    } else if (row.name == "Objective") {
        const auto* objective = FindReloadMissionObjective(reloadReplaySelection.graphId, reloadReplaySelection.missionHash, reloadReplaySelection.objectiveHash);
        row.kind = Kind::ReloadObjective; row.enabled = ready && reloadStartState==MissionStartState::Active && reloadReplaySelection.missionHash;
        row.objectiveChoices=ObjectiveChoices(s,0,reloadReplaySelection.missionHash);row.enabled=row.enabled&&!row.objectiveChoices.empty();
        row.value = reloadStartState!=MissionStartState::Active?"Not used for this state":(objective ? objective->name : "Mission default");
    } else if(row.name=="Start State") {
        row.kind=Kind::ReloadStartState;row.enabled=ready&&!reloadReplayEnabled&&metadata&&!s.reloadSelection.branches.empty();
        row.value=StartStateName(reloadStartState);
        row.allowAvailable=metadata&&!s.reloadSelection.branches.empty()&&s.reloadSelection.branches[0].allowAvailable;
    } else if(row.name=="Extra Mission Branches") {
        row.kind=Kind::Page;row.enabled=true;row.target="debug.reload_map.extra_branches";
        row.value=std::to_string(reloadExtraBranches.size())+" >";
    } else if (row.name == "Graph Filter") {
        const auto* graph = FindReloadMissionGraph(reloadReplaySelection.graphId);
        row.kind = Kind::ReloadGraph; row.enabled = ready && graph;
        row.value = graph ? graph->graphPath : "Waiting for graph";
    } else if (row.name == "Mission Sort Order" || row.name == "Objective Sort Order") {
        const bool missionSort = row.name == "Mission Sort Order";
        row.kind = missionSort ? Kind::ReloadMissionSort : Kind::ReloadObjectiveSort;
        row.enabled = ready;
        row.value = (missionSort ? reloadMissionDescending : reloadObjectiveDescending) ? "Mission Order" : "Alphabetical";
    } else if (row.name == "Filter Checkpoint Selection") {
        row.kind = Kind::ReloadFilter; row.enabled = ready && !reloadReplayEnabled && s.primary.ready &&
            FindReloadMission(s.primary.token.missionGraphAssetId, s.primary.token.missionHash);
        row.checked = reloadFilterCheckpoints; row.value = reloadFilterCheckpoints ? "Current mission" : "All loaded checkpoints";
    } else return;
    row.reload = s.reload;
    row.checkpointKey = reloadCheckpointKey;
    row.actorSave = s.actorSave;
    row.resetActorSave = reloadResetActorSave;
    row.replay = s.replay;
    row.replaySelection = reloadReplaySelection;
    row.replayMode = reloadReplayEnabled;
    row.checkpointChoices = choices;
    row.keepCurrentLoadout = reloadKeepCurrentLoadout;
    row.reloadOptions = s.reloadOptions;
    row.missionSelection=s.reloadSelection;
    row.applyMissionSelection=reloadMissionEdited&&!reloadReplayEnabled&&
        (row.action==Action::ReloadCheckpoint||row.action==Action::FullReload);
    if(row.applyMissionSelection&&(!metadata||!s.reloadSelection.ready||reloadPickFirstObjective||
        std::any_of(reloadExtraBranches.begin(),reloadExtraBranches.end(),[](const ExtraBranch& branch){return branch.pickFirstObjective;}))) {
        row.enabled=false;row.value="Checking mission selection";
    }
    if (!reloadKeepCurrentLoadout && (row.action == Action::ReloadCheckpoint || row.action == Action::FullReload ||
        row.action == Action::ReplayMission) && (!s.reloadOptions.supported || !s.reloadOptions.ready ||
        !SameReloadContext(s.reload.token, s.reloadOptions.token.world))) {
        row.enabled = false; row.value = "Waiting for save";
    }
}
std::vector<Row> ExtraMissionRows(const Snapshot& s) {
    std::vector<Row> rows;
    const auto* main=FindReloadMission(reloadReplaySelection.graphId,reloadReplaySelection.missionHash);
    const auto* objective=FindReloadMissionObjective(reloadReplaySelection.graphId,reloadReplaySelection.missionHash,reloadReplaySelection.objectiveHash);
    rows.push_back(Info("Main Mission",main?main->name:"Waiting for mission"));
    rows.push_back(Info("Main Objective",objective?objective->name:"Mission default"));
    rows.push_back(Info("Main Start State",StartStateName(reloadStartState)));
    const bool editable=s.supported&&s.reload.ready&&!reloadReplayEnabled;
    const bool metadata=MissionMetadataMatches(s);
    auto action=[&](const char* label,Kind kind,bool enabled){Row row{label,{},kind,Action::ExportDiagnostics,enabled};row.reload=s.reload;rows.push_back(std::move(row));};
    action("Add",Kind::BranchAdd,editable&&reloadExtraBranches.size()<63&&main);
    action("Remove Empty",Kind::BranchRemoveEmpty,editable&&std::any_of(reloadExtraBranches.begin(),reloadExtraBranches.end(),[](const ExtraBranch& b){return !b.value.missionHash&&!b.value.objectiveHash;}));
    action("Remove All",Kind::BranchRemoveAll,editable&&!reloadExtraBranches.empty());
    for(std::size_t i=0;i<reloadExtraBranches.size();++i){
        const auto& branch=reloadExtraBranches[i].value;
        rows.push_back(Info(("Branch "+std::to_string(i+1)).c_str()));rows.back().heading=true;
        const auto* mission=FindReloadMission(reloadReplaySelection.graphId,branch.missionHash);
        const auto* selectedObjective=FindReloadMissionObjective(reloadReplaySelection.graphId,branch.missionHash,branch.objectiveHash);
        auto item=[&](const char* label,const std::string& value,Kind kind,bool enabled){
            Row row{label,value,kind,Action::ExportDiagnostics,enabled};row.reload=s.reload;row.branchIndex=i+1;
            row.missionSelection=s.reloadSelection;row.missionChoices=MissionChoices(s);
            row.objectiveChoices=ObjectiveChoices(s,i+1,branch.missionHash);
            row.allowAvailable=metadata&&i+1<s.reloadSelection.branches.size()&&s.reloadSelection.branches[i+1].allowAvailable;
            rows.push_back(std::move(row));
        };
        item("Mission",mission?mission->name:"Empty",Kind::BranchMission,editable&&!MissionChoices(s).empty());
        item("Objective",selectedObjective?selectedObjective->name:"Mission default",Kind::BranchObjective,
             editable&&mission&&branch.state==MissionStartState::Active&&!ObjectiveChoices(s,i+1,branch.missionHash).empty());
        item("Start State",StartStateName(branch.state),Kind::ReloadStartState,
             editable&&mission&&metadata&&i+1<s.reloadSelection.branches.size());
        item("Remove Branch","",Kind::BranchRemove,editable);
    }
    return rows;
}
std::vector<Row> LocalRows(const Snapshot& s) {
    std::vector<Row> rows;
    if (page < 0) {
        for (size_t i = 0; i < pages.size(); ++i) {
            Row row{pages[i], ">", Kind::Page}; row.destination = static_cast<int>(i); rows.push_back(std::move(row));
        }
        rows.push_back({"Close Debug Menu", {}, Kind::Close});
    } else if (page == 0) {
        rows = {Info("Build", s.supported ? "1.0130.0.0" : (preview ? "Preview" : "Unsupported")),
                Info("Hero", s.heroReady ? "Ready" : "Waiting"),
                Info("Health", Number(s.health) + " / " + Number(s.maxHealth)),
                Info("Position X", Number(s.x, 3)), Info("Position Y", Number(s.y, 3)), Info("Position Z", Number(s.z, 3)),
                Command("Export Diagnostics", Action::ExportDiagnostics), Command("Reset All Overrides", Action::ResetAll)};
    } else if (page == 1) {
        const bool canChangeHealth = s.healthReady && s.engineActionsReady;
        rows = {Toggle("Heroes do not take damage", Action::HeroNoDamage, s.heroNoDamage, s.healthFlagsReady),
                Toggle("Heroes do not die", Action::HeroNoDeath, s.heroNoDeath, s.healthFlagsReady),
                Toggle("Enemy Bots do not die", Action::EnemyNoDeath, s.enemyNoDeath, s.healthFlagsReady),
                Toggle("Enemy Bots don't take damage", Action::EnemyNoDamage, s.enemyNoDamage, s.healthFlagsReady),
                Info("Current health", Number(s.health)), Info("Maximum health", Number(s.maxHealth)),
                Command("Heal to full", Action::Heal, canChangeHealth),
                Toggle("Auto-refill health", Action::AutoHeal, s.autoHeal, canChangeHealth),
                {"Maximum health value", Number(healthLimit), Kind::HealthValue, Action::SetMaxHealth, canChangeHealth},
                Command("Apply maximum health", Action::SetMaxHealth, canChangeHealth),
                Command("Reset health overrides", Action::ResetHealth, s.healthFlagsReady || s.healthReady)};
    } else if (page == 2) {
        if (s.characterNames.empty()) character = 0;
        else character = std::clamp(character, 0, static_cast<int>(s.characterNames.size()) - 1);
        rows = {{"Character", s.characterNames.empty() ? "Waiting for hero" : Utf8(s.characterNames[character]), Kind::Character,
                 Action::SwitchCharacter, s.heroReady && !s.characterNames.empty()},
                Command("Switch character", Action::SwitchCharacter, s.engineActionsReady && s.heroReady && !s.characterNames.empty()),
                Info("Position X", Number(s.x, 3)), Info("Position Y", Number(s.y, 3)), Info("Position Z", Number(s.z, 3)),
                Command("Save position", Action::SavePosition, s.teleportReady),
                Command("Load position", Action::LoadPosition, s.engineActionsReady && s.teleportReady && s.savedPosition),
                Command("Undo move", Action::UndoTeleport, s.engineActionsReady && s.teleportReady && s.undoPosition),
                {"Movement step", Number(moveStep), Kind::StepValue},
                Command("Move X -", Action::MoveXMinus, s.engineActionsReady && s.teleportReady), Command("Move X +", Action::MoveXPlus, s.engineActionsReady && s.teleportReady),
                Command("Move Y -", Action::MoveYMinus, s.engineActionsReady && s.teleportReady), Command("Move Y +", Action::MoveYPlus, s.engineActionsReady && s.teleportReady),
                Command("Move Z -", Action::MoveZMinus, s.engineActionsReady && s.teleportReady), Command("Move Z +", Action::MoveZPlus, s.engineActionsReady && s.teleportReady)};
        Row respawn{"Respawn Clear", "Unavailable", Kind::Setting, Action::DebugSetting, false};
        BindSetting(respawn, s, "local.hero");
        rows.push_back(std::move(respawn));
        Row healthPage{"Health controls", ">", Kind::Page};
        healthPage.target = "local.health";
        rows.push_back(std::move(healthPage));
    } else if (page == 3) {
        rows = {Info("Viewport", std::to_string(s.clientWidth) + " x " + std::to_string(s.clientHeight)),
                Info("Process memory (MiB)", Number(s.memoryMiB, 0)), Info("CPU usage", Number(s.cpuPercent) + "%"),
                Info("Session uptime (seconds)", std::to_string(s.uptimeSeconds)),
                Toggle("Borderless window", Action::Borderless, s.borderless, s.gameWindow != nullptr),
                Command("Restore game window", Action::RestoreWindow, s.gameWindow != nullptr)};
    } else {
        rows = {Info("Bindings", s.supported ? "Verified" : (preview ? "Preview only" : "Unavailable")),
                Info("Hero", s.heroReady ? "Ready" : "Waiting"), Info("Health", s.healthReady ? "Ready" : "Waiting"),
                Info("Renderer", "D3D12 in-game overlay"), Info("Gameplay actions", s.engineActionsReady ? "Ready" : "Unavailable"),
                Command("Export Diagnostics", Action::ExportDiagnostics), Command("Reset All Overrides", Action::ResetAll)};
    }
    return rows;
}
std::vector<Row> Rows(const Snapshot& s) {
    if(node=="debug.reload_map.extra_branches"){SyncReloadSelection(s);return ExtraMissionRows(s);}
    const bool local = node.rfind("local.", 0) == 0;
    std::vector<Row> rows = local && page >= 0 ? LocalRows(s) : std::vector<Row>{};
    const auto* definition = FindReferencePage(node);
    if (!definition) {
        if (rows.empty()) rows.push_back(Info("Menu unavailable", "Unavailable"));
        return rows;
    }
    if (node == "debug.reload_map") SyncReloadSelection(s);
    if (node=="debug.weapons" || node=="debug.weapons.weapon_outfitting") {
        auto flame=Command("Equip M2 flamethrower",Action::EquipMJFlamethrower,
                           s.supported&&s.engineActionsReady&&s.mjFlamethrowerReady);
        flame.sourceHelp="Mary Jane: equip the M2 and its fuel-tank backpack, then aim and hold Fire.";
        if (!flame.enabled) flame.value="Requires Mary Jane";
        rows.push_back(std::move(flame));
    }
    const auto reloadChoices = node == "debug.reload_map" ? ReloadChoices(s) : nullptr;
    for (const auto& item : definition->rows) {
        // Existing local controls retain their live bindings and order.
        if (local && std::any_of(rows.begin(), rows.end(), [&](const Row& row) { return row.name == item.label; })) continue;
        const char* unavailable = node.rfind("debug", 0) == 0 || local ? "Unavailable" : "N/A";
        Row row{item.label, item.heading ? "" : (item.target.empty() ? unavailable : ">"),
                item.heading ? Kind::Info : (item.target.empty() ? Kind::Action : Kind::Page),
                Action::ExportDiagnostics, item.heading || !item.target.empty()};
        row.target = item.target; row.heading = item.heading;
        if (node == "debug") row.value.clear();
        BindSetting(row, s, node);
        if (node == "debug.reload_map") BindReload(row, s, reloadChoices);
        if (node == "debug.health") {
            std::string name = item.label;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name == "heroes do not take damage") row = Toggle(item.label.c_str(), Action::HeroNoDamage, s.heroNoDamage, s.healthFlagsReady);
            else if (name == "heroes do not die") row = Toggle(item.label.c_str(), Action::HeroNoDeath, s.heroNoDeath, s.healthFlagsReady);
            else if (name == "enemy bots do not die") row = Toggle(item.label.c_str(), Action::EnemyNoDeath, s.enemyNoDeath, s.healthFlagsReady);
            else if (name == "enemy bots don't take damage") row = Toggle(item.label.c_str(), Action::EnemyNoDamage, s.enemyNoDamage, s.healthFlagsReady);
        }
        if (node == "debug.enemies.enemybot" && item.label == "Bots Take No Damage")
            row = Toggle(item.label.c_str(), Action::EnemyNoDamage, s.enemyNoDamage, s.healthFlagsReady);
        if (node=="debug.camera"&&item.label=="Enable Camera Shake") {
            row.kind=Kind::CameraShake;row.enabled=s.supported&&s.engineActionsReady&&s.cameraDebug.supported&&s.cameraDebug.ready;
            row.checked=s.cameraDebug.shakeEnabled;row.cameraDebug=s.cameraDebug;
            row.value=s.cameraDebug.ready?(row.checked?"On":"Off"):"Waiting for camera";
        }
        if (node == "debug.rendering.screencapture") {
            const auto capture=ReadScreenCaptureState();
            if (item.label=="Capture Image Format") {
                row.kind=Kind::CaptureFormat;row.enabled=!capture.busy;
                row.value=Utf8(CaptureFormatExtension(capture.format));
            } else if (item.label=="Capture With HUD") {
                row.kind=Kind::CaptureDisplay;row.enabled=capture.ready&&!capture.busy;
                row.value=capture.busy?"Capturing":(capture.ready?"":"Waiting for display");
            }
        }
        row.sourceHelp = item.help;
        rows.push_back(std::move(row));
    }
    return rows;
}
void SetNode(const std::string& next, bool remember = true) {
    if (remember) { if (history.size() >= 32) history.erase(history.begin()); history.push_back(node); }
    node = next; page = -1; selected = firstRow = 0;
    const std::array<const char*, 5> locals{{"local.common", "local.health", "local.hero", "local.rendering", "local.engine"}};
    for (size_t index = 0; index < locals.size(); ++index) if (node == locals[index]) page = static_cast<int>(index);
    const auto rows = Rows(GetSnapshot());
    for (size_t i = 0; i < rows.size(); ++i) if (rows[i].kind != Kind::Info) { selected = static_cast<int>(i); break; }
}
void RecordInput(MenuInput input, DWORD producer) {
    if (!preview && !traceInput) return;
    const auto filename = preview ? L"overlay-ui-events.jsonl" : L"live-ui-events.jsonl";
    std::ofstream output(std::filesystem::path(OutputDirectory()) / L"verification" / filename, std::ios::app);
    if (output) output << "{\"tick\":" << GetTickCount64() << ",\"visible\":" << (visible ? "true" : "false")
        << ",\"page\":" << page << ",\"node\":\"" << node << "\",\"selected\":" << selected << ",\"input\":" << static_cast<int>(input)
        << ",\"source\":\"" << (!preview && producer == controllerProducer ? "controller" : "keyboard_or_test") << "\"}\n";
}
void Activate(const Row& row, int direction) {
    if (!row.enabled) return;
    if (!row.target.empty()) { SetNode(row.target); return; }
    if (row.kind == Kind::Page) return;
    if (row.kind == Kind::Setting) { SubmitDebugSetting(row.settingId, direction); return; }
    if (row.kind == Kind::CameraShake) { SubmitCameraShake(row.cameraDebug,direction?direction>0:!row.checked);return; }
    if (row.kind == Kind::CaptureFormat) { CycleCaptureImageFormat(direction); return; }
    if (row.kind == Kind::CaptureDisplay) {
        if (!direction && RequestDisplayCapture()) QueueMenuInput(MenuInput::Close);
        return;
    }
    if(row.kind==Kind::ReloadStartState||(row.kind>=Kind::BranchAdd&&row.kind<=Kind::BranchRemove)) {
        if(reloadReplayEnabled||!SameReloadContext(row.reload.token,GetSnapshot().reload.token))return;
        if(direction&&(row.kind==Kind::BranchAdd||row.kind==Kind::BranchRemoveEmpty||
            row.kind==Kind::BranchRemoveAll||row.kind==Kind::BranchRemove))return;
        if(row.kind==Kind::BranchAdd){
            if(reloadExtraBranches.size()>=63)return;
            reloadExtraBranches.push_back({});
        }else if(row.kind==Kind::BranchRemoveEmpty){
            reloadExtraBranches.erase(std::remove_if(reloadExtraBranches.begin(),reloadExtraBranches.end(),
                [](const ExtraBranch& b){return !b.value.missionHash&&!b.value.objectiveHash;}),reloadExtraBranches.end());
        }else if(row.kind==Kind::BranchRemoveAll)reloadExtraBranches.clear();
        else if(row.kind==Kind::ReloadStartState){
            if(row.branchIndex>reloadExtraBranches.size())return;
            auto& start=row.branchIndex?reloadExtraBranches[row.branchIndex-1].value.state:reloadStartState;
            for(unsigned attempt=0;attempt<3;++attempt){
                start=static_cast<MissionStartState>((static_cast<unsigned>(start)+(direction<0?2:1))%3);
                if(start!=MissionStartState::Available||row.allowAvailable)break;
            }
            auto& objective=row.branchIndex?reloadExtraBranches[row.branchIndex-1].value.objectiveHash:reloadReplaySelection.objectiveHash;
            auto& pick=row.branchIndex?reloadExtraBranches[row.branchIndex-1].pickFirstObjective:reloadPickFirstObjective;
            objective=0;pick=start==MissionStartState::Active;
            if(!row.branchIndex)reloadPickCheckpoint=true;
        }else{
            if(!row.branchIndex||row.branchIndex>reloadExtraBranches.size())return;
            auto& extra=reloadExtraBranches[row.branchIndex-1];
            if(row.kind==Kind::BranchRemove)reloadExtraBranches.erase(reloadExtraBranches.begin()+row.branchIndex-1);
            else if(row.kind==Kind::BranchMission){
                extra.value.missionHash=CycleExactName(row.missionChoices,extra.value.missionHash,direction);
                extra.value.objectiveHash=0;extra.value.state=MissionStartState::Active;extra.pickFirstObjective=true;
            }else if(row.kind==Kind::BranchObjective){
                extra.value.objectiveHash=CycleExactName(row.objectiveChoices,extra.value.objectiveHash,direction);extra.pickFirstObjective=false;
            }
        }
        reloadMissionEdited=true;PublishMissionSelection();return;
    }
    if (row.kind == Kind::ReloadNewGamePlus) {
        SubmitSetNewGamePlus(row.reloadOptions, direction ? direction > 0 : !row.checked);
        return;
    }
    if (row.kind == Kind::ReloadKeepLoadout) {
        if (!SameReloadContext(row.reload.token, GetSnapshot().reload.token)) return;
        reloadKeepCurrentLoadout = direction ? direction > 0 : !row.checked;
        return;
    }
    if (row.kind >= Kind::ReloadReplay && row.kind <= Kind::ReloadFilter) {
        if (!SameReloadContext(row.reload.token, GetSnapshot().reload.token)) return;
        if (row.kind == Kind::ReloadReplay) {
            reloadReplayEnabled = direction ? direction > 0 : !row.checked;
            reloadCheckpointKey = 0; reloadResetActorSave = false; reloadFilterCheckpoints = false;
            reloadExtraBranches.clear();reloadStartState=MissionStartState::Active;
            reloadMissionEdited=reloadPickFirstObjective=reloadPickCheckpoint=false;
        } else if (row.kind == Kind::ReloadFilter) {
            reloadFilterCheckpoints = direction ? direction > 0 : !row.checked;
            reloadCheckpointKey = 0;
        } else if (row.kind == Kind::ReloadMissionSort || row.kind == Kind::ReloadObjectiveSort) {
            auto& descending = row.kind == Kind::ReloadMissionSort ? reloadMissionDescending : reloadObjectiveDescending;
            descending = !descending;
        } else {
            // Preserve exact unsigned IDs. Sorting changes navigation only.
            const auto next = [direction](size_t index, size_t count, bool descending = false) {
                const bool backwards = (direction < 0) != descending;
                return backwards ? (index + count - 1) % count : (index + 1) % count;
            };
            if (row.kind == Kind::ReloadGraph) {
                const auto& graphs = ReloadMissionCatalog();
                if (graphs.empty()) return;
                size_t index = 0;
                for (size_t i = 0; i < graphs.size(); ++i) if (graphs[i].graphAssetId == reloadReplaySelection.graphId) index = i;
                const auto& graph = graphs[next(index, graphs.size())];
                if (graph.missions.empty()) return;
                reloadReplaySelection = {graph.graphAssetId, graph.missions.front().nameCrc, 0};
                reloadStartState=MissionStartState::Active;reloadExtraBranches.clear();
                reloadMissionEdited=!reloadReplayEnabled;reloadPickFirstObjective=true;reloadPickCheckpoint=!reloadReplayEnabled;
            } else if (row.kind == Kind::ReloadMission) {
                reloadReplaySelection.missionHash=CycleExactName(row.missionChoices,reloadReplaySelection.missionHash,direction);
                reloadReplaySelection.objectiveHash = 0;
                reloadStartState=MissionStartState::Active;reloadPickFirstObjective=true;
                reloadMissionEdited=!reloadReplayEnabled;reloadPickCheckpoint=!reloadReplayEnabled;
            } else if (row.kind == Kind::ReloadObjective) {
                reloadReplaySelection.objectiveHash=CycleExactName(row.objectiveChoices,reloadReplaySelection.objectiveHash,direction);
                reloadPickFirstObjective=false;reloadMissionEdited=!reloadReplayEnabled;reloadPickCheckpoint=!reloadReplayEnabled;
            }
        }
        PublishMissionSelection(true);
        return;
    }
    if (row.kind == Kind::ReloadActorReset) {
        if (!SameReloadContext(row.reload.token, GetSnapshot().reload.token)) return;
        reloadResetActorSave = direction ? direction > 0 : !row.checked;
        return;
    }
    if (row.kind == Kind::ReloadCheckpoint) {
        if (!SameReloadContext(row.reload.token, GetSnapshot().reload.token)) return;
        const auto& choices = row.checkpointChoices;
        const size_t count = choices ? choices->size() : 0;
        size_t index = 0;
        if (choices) for (size_t i = 0; i < count; ++i) if ((*choices)[i].key == row.checkpointKey) { index = i + 1; break; }
        index = direction < 0 ? (index + count) % (count + 1) : (index + 1) % (count + 1);
        reloadCheckpointKey = index ? (*choices)[index - 1].key : 0;
        reloadSelectionContext = row.reload.token;
        return;
    }
    if (row.kind == Kind::Close) { InvalidateMenuSnapshot(); visible = false; return; }
    if (row.kind == Kind::HealthValue) { healthLimit = std::clamp(healthLimit + (direction < 0 ? -10.0f : 10.0f), 1.0f, 100000.0f); return; }
    if (row.kind == Kind::StepValue) { moveStep = std::clamp(moveStep + (direction < 0 ? -1.0f : 1.0f), 0.1f, 1000.0f); return; }
    if (row.kind == Kind::Character) {
        const auto s = GetSnapshot(); const int count = static_cast<int>(s.characterNames.size());
        if (count) character = (character + count + (direction < 0 ? -1 : 1)) % count;
        return;
    }
    if (row.kind == Kind::Toggle) {
        bool enabled = direction ? direction > 0 : !row.checked;
        if (row.action == Action::Borderless) SubmitAction(enabled ? Action::Borderless : Action::RestoreWindow);
        else SubmitAction(row.action, enabled ? 1.0f : 0.0f);
    } else if (row.kind == Kind::Action && !direction) {
        const ReloadPolicy policy{row.keepCurrentLoadout,row.resetActorSave,row.reloadOptions,row.actorSave,row.applyMissionSelection,row.missionSelection};
        if (row.action == Action::ReplayMission) { SubmitReplayWithPolicy(row.replay, policy); return; }
        if (row.action == Action::CompletePrimary) { SubmitCompletePrimary(row.primary); return; }
        if (row.action == Action::ReloadCheckpoint || row.action == Action::FullReload || row.action == Action::ExitFrontend ||
            row.action == Action::ExitLobby || row.action == Action::CopyReloadData) {
            if (row.action == Action::CopyReloadData && row.replayMode)
                SubmitReplayDataCopy(row.reload, row.replaySelection);
            else if (row.action == Action::ReloadCheckpoint || row.action == Action::FullReload)
                SubmitReloadWithPolicy(row.action, row.reload, row.checkpointKey, policy);
            else SubmitReloadAction(row.action, row.reload, row.checkpointKey);
            return;
        }
        float value = 0;
        if (row.action == Action::SetMaxHealth) value = healthLimit;
        else if (row.action == Action::SwitchCharacter) value = static_cast<float>(character);
        else if (row.action >= Action::MoveXMinus && row.action <= Action::MoveZPlus) value = moveStep;
        SubmitAction(row.action, value);
    }
}
void Apply(MenuInput input, DWORD producer) {
    if (input == MenuInput::Close) { InvalidateMenuSnapshot(); visible = false; RecordInput(input, producer); return; }
    if (input == MenuInput::Toggle) {
        const bool opening = !visible;
        if (opening && node.rfind("dev", 0) == 0) { history.clear(); SetNode("debug", false); }
        InvalidateMenuSnapshot();
        visible = opening;
        RecordInput(input, producer);
        return;
    }
    if (!visible) return;
    if (input == MenuInput::Back) {
        if (!history.empty()) { const auto parent = history.back(); history.pop_back(); SetNode(parent, false); }
        else if (node != "debug" && node != "dev") SetNode(node.rfind("dev", 0) == 0 ? "dev" : "debug", false);
        else { InvalidateMenuSnapshot(); visible = false; }
    }
    else if (input == MenuInput::PreviousPage || input == MenuInput::NextPage) {
        const bool developer = node.rfind("dev", 0) == 0;
        history.clear(); SetNode(developer ? "debug" : "dev", false);
    } else {
        const auto rows = Rows(GetSnapshot());
        if (rows.empty()) return;
        selected = std::clamp(selected, 0, static_cast<int>(rows.size()) - 1);
        if (input == MenuInput::Up || input == MenuInput::Down) {
            const int increment = input == MenuInput::Down ? 1 : -1;
            for (size_t attempt = 0; attempt < rows.size(); ++attempt) {
                selected = (selected + static_cast<int>(rows.size()) + increment) % static_cast<int>(rows.size());
                if (rows[selected].kind != Kind::Info) break;
            }
        } else if (input == MenuInput::Accept) Activate(rows[selected], 0);
        else if (input == MenuInput::Left) Activate(rows[selected], -1);
        else if (input == MenuInput::Right) Activate(rows[selected], 1);
    }
    RecordInput(input, producer);
}
void Keyboard() {
    struct Key { int code; MenuInput input; bool repeat; bool held = false; ULONGLONG next = 0; };
    static std::array<Key, 9> keys{{{VK_F10, MenuInput::Toggle, false}, {VK_UP, MenuInput::Up, true},
        {VK_DOWN, MenuInput::Down, true}, {VK_LEFT, MenuInput::Left, true}, {VK_RIGHT, MenuInput::Right, true},
        {VK_RETURN, MenuInput::Accept, false}, {VK_ESCAPE, MenuInput::Back, false},
        {VK_PRIOR, MenuInput::PreviousPage, false}, {VK_NEXT, MenuInput::NextPage, false}}};
    const HWND foreground = GetForegroundWindow(); DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    wchar_t cls[64]{}; GetClassNameW(foreground, cls, 64);
    const bool focused = process == GetCurrentProcessId() && (preview || std::wstring(cls) == L"GameNxApp");
    const auto now = GetTickCount64();
    for (auto& key : keys) {
        const bool down = (GetAsyncKeyState(key.code) & 0x8000) != 0;
        if (!focused) { key.held = down; key.next = now + 350; continue; }
        if (down && (!key.held || (key.repeat && now >= key.next))) {
            if (key.input == MenuInput::Toggle || visible) QueueMenuInput(key.input);
            key.next = now + (key.held ? 110 : 350);
        }
        key.held = down;
    }
}
void Text(ImDrawList* draw, ImFont* font, float size, float x, float y, const std::string& text, float wrap = 0) {
    draw->AddText(font, size, ImVec2(x + 1, y + 1), IM_COL32(0, 0, 0, 190), text.c_str(), nullptr, wrap);
    draw->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), text.c_str(), nullptr, wrap);
}
float Width(ImFont* font, float size, const std::string& text) { return font->CalcTextSizeA(size, 100000, 0, text.c_str()).x; }
std::string HintForRow(const Row& row, const Snapshot& s) {
    std::string hint = !row.enabled ? "Waiting for a playable hero / verified binding" : "";
    if(row.kind==Kind::CameraShake)return row.enabled?"Left / Right to change camera shake":
        (!s.engineActionsReady?"Waiting for the verified engine dispatcher":Utf8(row.cameraDebug.status));
    if (row.kind==Kind::CaptureFormat) return row.enabled?
        "Left / Right: bmp, dds, jpg or png; applies to the next display capture":"Wait for the current capture to finish";
    if (row.kind==Kind::CaptureDisplay) {
        const auto capture=ReadScreenCaptureState();
        return Utf8(capture.status)+"; saves to SM2DebugMenu/captures after this menu closes";
    }
    if(row.kind==Kind::ReloadStartState)return row.enabled?
        "Active, Available or Complete on the next manual Reload; Available requires mission support":
        (reloadReplayEnabled?"Native Replay controls its own mission state":"Waiting for the selected mission's native definition");
    if(node=="debug.reload_map.extra_branches")return reloadReplayEnabled?"Turn Replay Mode off to edit extra mission branches":
        "Edit up to 63 extra branches; empty rows are removed when Reload applies the selection";
    if (!row.enabled && node.rfind("local.", 0) != 0)
        hint = node.rfind("debug", 0) == 0 ? "Unavailable in this game build" : "Unavailable: no verified retail-PC binding";
    if (!row.enabled && !s.engineActionsReady &&
        (row.kind == Kind::Setting || node.rfind("local.", 0) == 0)) hint = "Waiting for the verified engine dispatcher";
    if (!row.enabled && node == "local.hero" && s.engineActionsReady && s.teleportReady) {
        if (row.action == Action::LoadPosition && !s.savedPosition) hint = "Use Save position for this hero first";
        else if (row.action == Action::UndoTeleport && !s.undoPosition) hint = "Move or load a position first";
    }
    if (!row.enabled) if (const auto* option = FindNativeDevOption(node, row.name)) {
        if (option->missingMenu) hint = "This entry names a menu absent from this build";
        else if (option->missingAssets) hint = "The required scene data is absent from this build";
    }
    if (row.enabled && (row.kind == Kind::HealthValue || row.kind == Kind::StepValue || row.kind == Kind::Character)) hint = "Left / Right to change";
    else if (row.kind == Kind::Toggle && row.enabled) hint = "Cross / Enter to toggle";
    else if (row.kind == Kind::Setting && row.enabled) {
        hint = "Left / Right to change";
        if (row.settingId == "EngineDebug.PrototypeMessageEnable") hint += "; affects prototype-message events";
        else if (row.settingId == "EngineDebug.DebugMessageEnable") hint += "; new message contexts may require a reload";
        else if (row.settingId == "Engine.VFXEnable") hint += "; existing particles may remain";
        else if (row.settingId == "Weapon.InfiniteAmmo") hint = "Left / Right: Off or On; NoReload is not implemented";
        else if (row.settingId == "Traffic.PedestrianLoader.Enabled") hint += "; existing pedestrians may remain";
        else if (row.settingId == "ManagedPrefabActor.AllSystemsEnabled") hint = "Changes prefab populations; reset restores the flag only";
        else if (row.settingId == "SpeedRun.AllowFastTravelDuringDialog") hint = "Allow fast travel during dialog; other fast-travel restrictions still apply";
        else if (row.settingId == "SpeedRun.AllowCharacterSwitchDuringDialog") hint = "Allow character switching during dialog; other switching restrictions still apply";
        if (hint == "Left / Right to change" && !row.sourceHelp.empty()) hint += "; " + row.sourceHelp;
    }
    if (node == "debug.reload_map") {
        const bool bound = row.kind == Kind::ReloadCheckpoint || row.action == Action::ReloadCheckpoint || row.action == Action::FullReload ||
            row.action == Action::ExitFrontend || row.action == Action::ExitLobby || row.action == Action::CopyReloadData;
        if (bound && !row.enabled) hint = !s.reload.ready ? Utf8(s.reload.status) :
            (!s.engineActionsReady && row.action != Action::CopyReloadData ? "Waiting for the verified engine dispatcher" :
             (row.action == Action::ExitLobby ? "Native unload routing is unavailable" : "Frontend routing is unavailable"));
        else if (row.kind == Kind::ReloadCheckpoint) hint = "Left / Right: current checkpoint or a checkpoint in this loaded level";
        else if (row.action == Action::ReloadCheckpoint) hint = "Reload the selected checkpoint; unsaved actions since it will be lost";
        else if (row.action == Action::FullReload) hint = "Fully unload and reload the selected checkpoint; unsaved actions will be lost";
        else if (row.action == Action::ExitFrontend) hint = "Leave the current game and return to the frontend";
        else if (row.action == Action::ExitLobby) hint = "Unload the current game using its existing lobby/menu routing";
        else if (row.action == Action::CopyReloadData) hint = "Copy this checkpoint selection and level identifier";
        if (row.action == Action::CompletePrimary) {
            hint = row.enabled ? "Complete the current primary objective; normal mission scripts and rewards apply" :
                (s.primary.ready && !s.engineActionsReady ? "Waiting for the verified engine dispatcher" : Utf8(s.primary.status));
        }
        if (row.kind == Kind::ReloadActorReset) hint = row.enabled ?
            "On manual Reload, reset saved deleted actors and actor flags; keep collected items. Reset can remain if Reload fails." : Utf8(s.actorSave.status);
        if (row.resetActorSave && (row.action == Action::ReloadCheckpoint || row.action == Action::FullReload))
            hint = row.enabled ? "Reset actor save, then reload. Collected items stay; reset cannot be undone if Reload fails." : Utf8(s.actorSave.status);
        if (row.kind == Kind::ReloadReplay) hint = "Replay Mode uses the selected mission and objective when you press Reload";
        if (row.kind == Kind::ReloadNewGamePlus) hint = row.enabled ?
            "Changes the current save's NG+ flag immediately; does not start a new playthrough or increase the NG+ count" :
            (!s.engineActionsReady ? "Waiting for the verified engine dispatcher" : Utf8(s.reloadOptions.status));
        if (row.kind == Kind::ReloadKeepLoadout) hint =
            "On: keep default-loadout state. Off: reapply the default loadout on Reload. Mission scripts can still change items.";
        if (row.kind == Kind::ReloadMission || row.kind == Kind::ReloadObjective || row.kind == Kind::ReloadGraph)
            hint = "Left / Right: browse missions. Changes apply on Reload after native mission validation.";
        if (row.kind == Kind::ReloadMissionSort || row.kind == Kind::ReloadObjectiveSort)
            hint = "Alphabetical or the native depth-first Mission Order; ordering does not change mission progression";
        if (row.kind == Kind::ReloadFilter) hint = "Filter live checkpoints to exact checkpoint names stored by the current mission";
        if (row.action == Action::ReplayMission) hint = row.enabled ? "Replay the selected mission/objective; native replay changes mission state and reloads the level" :
            (!SameReplaySelection(s.replay.selection, reloadReplaySelection) ? "Checking selected replay target" : Utf8(s.replay.status));
        if (reloadReplayEnabled && row.action == Action::FullReload && row.enabled)
            hint = "Fully unload and reload the current world at its current checkpoint; Reload uses the replay mission";
        if (reloadReplayEnabled && row.kind == Kind::ReloadActorReset)
            hint = "Turn Replay Mode off to use this manual checkpoint-reload policy";
        if (reloadReplayEnabled && row.kind == Kind::ReloadCheckpoint) hint = "Replay resolves its checkpoint from the selected objective";
        if (!row.keepCurrentLoadout && (row.action == Action::ReloadCheckpoint || row.action == Action::FullReload ||
            row.action == Action::ReplayMission)) {
            if (row.value == "Waiting for save") hint = "Waiting for a validated campaign save; Keep Current Loadout On skips the default-loadout reset";
            else if (row.enabled) hint += "; default-loadout reset remains applied if the game later refuses the transition";
        }
        if(row.applyMissionSelection){
            if(row.value=="Checking mission selection")hint=MissionMetadataMatches(s)?Utf8(s.reloadSelection.status):"Checking the selected mission branches";
            else if(row.enabled)hint+="; applies selected mission states and extra branches before reloading";
        }
    }
    if (!row.enabled && row.kind == Kind::Action && row.action == Action::ExportDiagnostics &&
        (node.rfind("debug", 0) == 0 || node.rfind("local.", 0) == 0)) {
        hint = "Unavailable: no verified binding in this game build";
        if (!row.sourceHelp.empty()) hint += "; original help: " + row.sourceHelp;
    }
    if (preview) hint = "Preview: gameplay controls remain disabled";
    return hint;
}
}
void QueueMenuInput(MenuInput input) {
    std::lock_guard lock(inputMutex);
    if (input == MenuInput::Close) inputQueue.clear();
    if (inputQueue.size() < 64) inputQueue.push_back({input, GetCurrentThreadId()});
}
bool MenuVisible() { return visible.load(); }
bool PumpMenuInput() {
    // Factory hooks are installed synchronously, while binding verification
    // completes on the worker. Publish the backend before reading snapshots.
    if (!uiReady.load()) return false;
    Keyboard();
    std::deque<std::pair<MenuInput, DWORD>> inputs;
    { std::lock_guard lock(inputMutex); inputs.swap(inputQueue); }
    for (const auto& input : inputs) Apply(input.first, input.second);
    return visible.load();
}
void RenderMenuFrame() {
    if (!visible) return;
    const auto s = GetSnapshot();
    const auto rows = Rows(s);
    if (rows.empty()) return;
    selected = std::clamp(selected, 0, static_cast<int>(rows.size()) - 1);
    const auto viewport = ImGui::GetIO().DisplaySize;
    auto* draw = ImGui::GetBackgroundDrawList();
    auto* font = ImGui::GetFont();
    const auto* definition = FindReferencePage(node);
    const bool developer = definition && definition->developer;
    if (developer && ImGui::GetIO().Fonts->Fonts.Size > 1) font = ImGui::GetIO().Fonts->Fonts[1];
    const float size = std::clamp(viewport.y / 48.0f, 15.0f, 23.0f);
    const float center = viewport.x * 0.5f, line = size * 1.35f;
    const float top = std::max(24.0f, viewport.y * 0.045f);
    const std::string title = node=="debug.reload_map.extra_branches"?"Extra Mission Branches":
        (definition ? definition->title : (page < 0 ? "Debug Menu" : pages[page]));
    draw->AddRectFilled(ImVec2(0, 0), viewport, IM_COL32(0, 0, 0, developer ? 95 : 153));
    const float rowSize = developer ? std::clamp(viewport.y / 33.0f, 20.0f, 34.0f) : size;
    const float rowLine = developer ? rowSize * 1.85f : line;
    const float rowTop = developer ? top + rowLine * 1.5f : top + line * 2;
    if (developer) {
        draw->AddRectFilled(ImVec2(viewport.x * 0.027f, top - 14), ImVec2(viewport.x * 0.43f, viewport.y - 85), IM_COL32(0, 0, 0, 95));
        Text(draw, font, rowSize * 1.1f, viewport.x * 0.04f, top, title);
        draw->AddLine(ImVec2(viewport.x * 0.03f, rowTop - rowLine * 0.45f), ImVec2(viewport.x * 0.43f, rowTop - rowLine * 0.45f), IM_COL32(210, 210, 210, 130));
    } else Text(draw, font, size, center - Width(font, size, title) / 2, top, title);
    const float footerSize = std::max(12.0f, (developer ? rowSize : size) * 0.73f);
    const auto hint = HintForRow(rows[selected], s);
    const float hintWidth = std::max(32.0f, viewport.x - 32.0f);
    const auto hintSize = font->CalcTextSizeA(footerSize, 100000, hintWidth, hint.c_str());
    const float hintY = std::min(viewport.y - 80.0f, viewport.y - 64.0f - hintSize.y);
    const int capacity = std::max(4, static_cast<int>((hintY - rowTop - 20) / rowLine));
    firstRow = std::clamp(firstRow, 0, std::max(0, static_cast<int>(rows.size()) - capacity));
    if (selected < firstRow) firstRow = selected;
    if (selected >= firstRow + capacity) firstRow = selected - capacity + 1;
    const float column = center + std::min(125.0f, viewport.x * 0.11f);
    for (int index = firstRow; index < static_cast<int>(rows.size()) && index < firstRow + capacity; ++index) {
        const auto& row = rows[index];
        const float y = rowTop + (index - firstRow) * rowLine;
        const float left = developer ? viewport.x * 0.062f : (row.value.empty() ? center - Width(font, size, row.name) / 2 : column - 16 - Width(font, size, row.name));
        const float right = developer ? viewport.x * 0.42f : (row.value.empty() ? left + Width(font, size, row.name) : column + Width(font, size, row.value) + 8);
        if (index == selected && row.kind != Kind::Info)
            draw->AddRectFilled(ImVec2(left - 5, y - 1), ImVec2(right + 5, y + rowSize * 1.25f), developer ? IM_COL32(17, 73, 88, 220) : IM_COL32(26, 105, 208, 245));
        const std::string label = developer && !row.target.empty() ? row.name + " >" : row.name;
        Text(draw, font, rowSize, left, y, label);
        if (!row.value.empty() && !developer) Text(draw, font, size, column, y, row.value);
    }
    Text(draw, font, footerSize, std::max(16.0f, center - hintSize.x / 2), hintY, hint, hintWidth);
    const std::string controls = "PS / F10: show/hide   D-pad: navigate   Cross: select   Circle: back   L1/R1: Debug / Dev";
    Text(draw, font, footerSize, std::max(8.0f, center - Width(font, footerSize, controls) / 2), viewport.y - 50, controls);
    const std::string status = Utf8(s.status);
    Text(draw, font, footerSize, std::max(8.0f, center - Width(font, footerSize, status) / 2), viewport.y - 28, status);
}
int RunMenu(HMODULE module, bool isPreview) {
    preview = isPreview;
    controllerProducer = GetCurrentThreadId();
    char trace[4]{};
    traceInput = GetEnvironmentVariableA("SM2_DEBUG_INPUT_TRACE", trace, 4) == 1 && trace[0] == '1';
    uiReady = true;
    if (preview) { visible = false; return RunOverlayPreview(module); }
    if (!InitializeOverlay(module)) return 1;
    ULONGLONG nextBackend = 0;
    HWND gameWindow = nullptr;
    for (;;) {
        const auto now = GetTickCount64();
        if (now >= nextBackend) { TickBackend(); gameWindow = GetSnapshot().gameWindow; nextBackend = now + 100; }
        PollControllerInput(gameWindow);
        Sleep(8);
    }
}
} // namespace sm2debug
