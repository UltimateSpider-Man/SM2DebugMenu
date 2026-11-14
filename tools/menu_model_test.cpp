// SPDX-License-Identifier: GPL-3.0-or-later
// Tests the actual UI translation unit. No game, HID, window or GPU is opened.
#include "../src/menu_ui.cpp"
#include <functional>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>

namespace fixture {
sm2debug::Snapshot snapshot;
std::vector<std::pair<sm2debug::Action, float>> submitted;
std::vector<std::pair<std::string, int>> settingsSubmitted;
struct ReloadRequest { sm2debug::Action action; sm2debug::ReloadState captured; uint32_t key; sm2debug::ReloadPolicy policy{}; bool policyQueue=false; };
std::vector<ReloadRequest> reloadSubmitted;
struct ActorReloadRequest { sm2debug::Action action; sm2debug::ReloadState captured; uint32_t key; sm2debug::ActorSaveState reset; };
std::vector<ActorReloadRequest> actorReloadSubmitted;
std::vector<sm2debug::ReplaySelection> replaySelections;
std::vector<sm2debug::ReloadMissionSelection> missionSelections;
std::vector<sm2debug::ReplayState> replaySubmitted;
std::vector<sm2debug::ReloadPolicy> replayPolicies;
unsigned legacyReplayCalls=0;
std::vector<std::pair<sm2debug::ReloadState, sm2debug::ReplaySelection>> replayCopies;
std::vector<sm2debug::PrimaryObjectiveState> primarySubmitted;
std::vector<std::pair<sm2debug::ReloadOptionsState, bool>> newGamePlusSubmitted;
std::vector<std::pair<sm2debug::CameraDebugState, bool>> cameraShakeSubmitted;
unsigned backendCalls = 0, controllerCalls = 0, overlayCalls = 0, runtimeContextCalls = 0, snapshotInvalidations = 0;
sm2debug::ScreenCaptureState capture;
std::vector<int> captureFormatDirections;
unsigned captureRequests = 0;
bool captureRequestAccepted = true;
}
namespace sm2debug {
Snapshot GetSnapshot() { return fixture::snapshot; }
void InvalidateMenuSnapshot() { ++fixture::snapshotInvalidations; }
void SubmitAction(Action action, float value) { fixture::submitted.emplace_back(action, value); }
void SubmitDebugSetting(const std::string& id, int direction) { fixture::settingsSubmitted.emplace_back(id, direction); }
void SubmitReloadAction(Action action, const ReloadState& captured, uint32_t key) {
    fixture::reloadSubmitted.push_back({action, captured, key});
}
void SubmitCompletePrimary(const PrimaryObjectiveState& captured) { fixture::primarySubmitted.push_back(captured); }
void SubmitReloadWithActorReset(Action action, const ReloadState& captured, uint32_t key, const ActorSaveState& reset) {
    fixture::actorReloadSubmitted.push_back({action,captured,key,reset});
}
void SubmitReplaySelection(const ReplaySelection& selection) { fixture::replaySelections.push_back(selection); }
void SubmitReloadMissionSelection(const ReloadMissionSelection& selection) { fixture::missionSelections.push_back(selection); }
void SubmitReplayAction(const ReplayState& captured) { ++fixture::legacyReplayCalls; fixture::replaySubmitted.push_back(captured); }
void SubmitReloadWithPolicy(Action action, const ReloadState& captured, uint32_t key, const ReloadPolicy& policy) {
    fixture::reloadSubmitted.push_back({action,captured,key,policy,true});
}
void SubmitReplayWithPolicy(const ReplayState& captured, const ReloadPolicy& policy) {
    fixture::replaySubmitted.push_back(captured); fixture::replayPolicies.push_back(policy);
}
void SubmitReplayDataCopy(const ReloadState& world, const ReplaySelection& selection) { fixture::replayCopies.emplace_back(world,selection); }
void SubmitSetNewGamePlus(const ReloadOptionsState& captured, bool enabled) { fixture::newGamePlusSubmitted.emplace_back(captured, enabled); }
void SubmitCameraShake(const CameraDebugState& captured, bool enabled) { fixture::cameraShakeSubmitted.emplace_back(captured, enabled); }
std::wstring OutputDirectory() { return L"."; }
void TickBackend() { ++fixture::backendCalls; }
bool InitializeOverlay(HMODULE) { ++fixture::overlayCalls; return false; }
int RunOverlayPreview(HINSTANCE) { ++fixture::overlayCalls; return 99; }
void PollControllerInput(HWND) { ++fixture::controllerCalls; }
bool IsWithinVerifiedEngineCallback() noexcept { ++fixture::runtimeContextCalls; return false; }
ScreenCaptureState ReadScreenCaptureState() { return fixture::capture; }
const wchar_t* CaptureFormatExtension(CaptureImageFormat format) {
    static const wchar_t* extensions[]{L"bmp",L"dds",L"jpg",L"png"};
    return static_cast<unsigned>(format)<4?extensions[static_cast<unsigned>(format)]:L"invalid";
}
bool CycleCaptureImageFormat(int direction) { fixture::captureFormatDirections.push_back(direction); return true; }
bool RequestDisplayCapture() { ++fixture::captureRequests; return fixture::captureRequestAccepted; }
}
namespace {
using namespace sm2debug;
struct Result { std::string name, failure; };
std::vector<Result> results;
size_t unsupportedLeaves = 0, loadingRowsVisited = 0, nativeRowsRejected = 0;
size_t newLiveRowsChecked = 0, catalogRoutesChecked = 0, localChildRoutesChecked = 0;
size_t reloadBoundControlsChecked = 0, manualReloadRowsEnabled = 0, replayRowsEnabled = 0;
struct AddedSetting {
    const char* root;
    const char* page;
    const char* label;
    const char* id;
    bool enumeration;
    const char* child = nullptr;
};
const std::array<AddedSetting, 11> addedSettings{{
    {"DebugDraw", "debug.debug_draw", "Show tile boundaries", "EngineDebug.TileBoundaryEnable", false},
    {"DebugScriptMessages", "debug.script_messages", "Prototype messages", "EngineDebug.PrototypeMessageEnable", false},
    {"DebugScriptMessages", "debug.script_messages", "Debug messages", "EngineDebug.DebugMessageEnable", false},
    {"Weapons", "debug.weapons", "Infinite ammo", "Weapon.InfiniteAmmo", true},
    {"Rendering", "debug.rendering", "Motion blur", "Render.MotionBlurEnable", false},
    {"Rendering", "debug.rendering", "Visual effects processing", "Engine.VFXEnable", false},
    {"Traffic", "debug.traffic.pedestrians", "Enable Pedestrians", "Traffic.PedestrianLoader.Enabled", false, "Pedestrians"},
    {"Prefab Actor Systems", "debug.prefab_actor_systems", "All systems enabled", "ManagedPrefabActor.AllSystemsEnabled", false},
    {"GameControls", "debug.gamecontrols", "AllowFastTravelDuringDialog", "SpeedRun.AllowFastTravelDuringDialog", false},
    {"GameControls", "debug.gamecontrols", "AllowCharacterSwitchDuringDialog", "SpeedRun.AllowCharacterSwitchDuringDialog", false},
    {"GameControls", "debug.gamecontrols", "BubbleSkipOption", "SpeedRun.BubbleSkipOption", true}
}};
struct PortedSetting { const char* id; const char* page; const char* label; DebugSettingType type; };
const std::array<PortedSetting,39> portedSettings{{
    {"Render.EnableSubsurfaceBrdf","debug.rendering.lighting","Enable Subsurface Brdf",DebugSettingType::Boolean},
    {"Render.CsmSdsmDepthBias","debug.rendering.lighting.shadows.csmshadows","SDSM Depth Bias",DebugSettingType::Float32},
    {"Render.ModelLoDTransitionTime","debug.rendering.models","LoD Transition Time",DebugSettingType::Float32},
    {"Render.DepthPrePassBias","debug.rendering.models","Adjust Depth Pre-Pass",DebugSettingType::Float32},
    {"Render.TerrainMeshRenderStartLevel","debug.rendering.terrain","Render Mesh Start Level",DebugSettingType::UInt32},
    {"Render.TerrainMeshShadowsStartLevel","debug.rendering.terrain","Shadow Mesh Start Level",DebugSettingType::UInt32},
    {"EngineDebug.FailAllAssetLoads","local.engine.assetmanager","Fail All Loads",DebugSettingType::Boolean},
    {"EngineDebug.FailAssetsAsMissing","local.engine.assetmanager","Fail As Missing",DebugSettingType::Boolean},
    {"Physics.RagdollUpdateSkipUnmovedObjects","local.engine.physics","Skip keyframing nonmoving objects",DebugSettingType::Boolean},
    {"EngineDebug.TileBoundaryEnable","debug.rendering.global","Show tile boundaries",DebugSettingType::Boolean},
    {"Render.EnableAmbientShadows","debug.rendering.lighting","Enable Ambient Shadows",DebugSettingType::Boolean},
    {"RtEnvProbeSettings.FarClip","debug.rendering.lighting.envprobe","RTEP Far Clip",DebugSettingType::Float32},
    {"Render.LightGridFadeTime","debug.rendering.lighting.lightgrid","LightGridFadeTime",DebugSettingType::Float32},
    {"Render.MotionBlurEnable","debug.rendering.post","Enable Motion Blur",DebugSettingType::Boolean},
    {"PhysicsDiagnostics.WarnOnThisManyManifoldsGloballyInOneFrame","local.engine.physics","WarnOnThisManyManifoldsGloballyInOneFrame",DebugSettingType::Int16},
    {"PhysicsDiagnostics.WarnOnThisManyBodyActivationsGloballyInOneFrame","local.engine.physics","WarnOnThisManyBodyActivationsGloballyInOneFrame",DebugSettingType::Int16},
    {"PhysicsDiagnostics.MinimumSecondsBetweenExcessiveManifoldReports","local.engine.physics","MinimumSecondsBetweenExcessiveManifoldReports",DebugSettingType::Float32},
    {"Save.DebugVerbosity","local.engine.saveload","TTY Verbosity",DebugSettingType::Enum32},
    {"Render.ExtraFlushesEnabled","debug.profiling.gpuprofiling","Extra Flushes Enabled",DebugSettingType::Boolean},
    {"Difficulty.DynamicDifficulty","debug.game_systems.difficulty","DynamicDifficulty",DebugSettingType::Boolean},
    {"GameUI.MapARBeacons","debug.game_systems.gameui","MapARBeacons",DebugSettingType::Boolean},
    {"GameUI.ARBeacons","debug.game_systems.openworldactivities","ARBeacons",DebugSettingType::Boolean},
    {"PartialRagdoll.ManagerMode","debug.game_systems.partialragdoll","ManagerMode",DebugSettingType::Enum32},
    {"ProgressionSystem.TechboxOutline","debug.game_systems.progression","TechboxOutline",DebugSettingType::Boolean},
    {"Bot.EnableCrossbowNetSwap","debug.enemies.kraven","Swap crossbow net actor",DebugSettingType::Boolean},
    {"GUI.PrintErrorMarkers","debug.sonytest","Print error markers",DebugSettingType::Boolean},
    {"Health.DisplayHealthValues","debug.health","Draw Health Values",DebugSettingType::Boolean},
    {"Health.DisplayHeroHealthBar","debug.health","Draw Hero Health Bar",DebugSettingType::Boolean},
    {"Health.DisplayEnemyHealthBars","debug.health","Draw Enemy Health Bars",DebugSettingType::Boolean},
    {"Health.DebugHealthDrops","debug.health","Debug Health Drops",DebugSettingType::Boolean},
    {"Health.DisableHitPause","debug.health","Disable Hit Pause",DebugSettingType::Boolean},
    {"TrafficShared.VerboseRidableDebug","debug.traffic.vehicles","VerboseRidableDebug",DebugSettingType::Boolean},
    {"Weapon.InfiniteAmmo","debug.weapons","Infinite Ammo",DebugSettingType::Enum32},
    {"Weapon.TapMaxHoldTime","debug.weapons","Tap Max Hold Time",DebugSettingType::Float32},
    {"GUI.PrintErrorMarkers","debug.debug_draw","Print error markers",DebugSettingType::Boolean},
    {"EngineDebug.PrototypeMessageEnable","debug.debug_draw","Prototype Messages",DebugSettingType::Boolean},
    {"Physics.EnableUnderSampleUpdatingRagdolls","local.engine.physics","Enable Undersampling on Faraway Ragdolls",DebugSettingType::Boolean},
    {"Render.TerrainQuadsPerMeter","debug.rendering.terrain","Quads Per Meter",DebugSettingType::UInt32},
    {"Reactions.DisableReactGraph","debug.reactions","Disable React Graph",DebugSettingType::Boolean},
}};
void AddNativeSettingsSnapshot() {
    fixture::snapshot.settings.clear();
    for (const auto& expected : addedSettings) {
        DebugSettingValue setting;
        setting.available = true;
        setting.id = expected.id; setting.pageId = expected.page; setting.label = expected.label;
        setting.type = expected.enumeration ? DebugSettingType::Enum32 : DebugSettingType::Boolean;
        setting.number = 0;
        if (expected.enumeration) setting.choices = std::string(expected.id) == "SpeedRun.BubbleSkipOption" ?
            std::vector<std::string>{"kNone", "kSkipObeyToken", "kSkipIgnoreToken"} : std::vector<std::string>{"Off", "On", "NoReload"};
        fixture::snapshot.settings.push_back(std::move(setting));
    }
}
void Check(bool pass, const std::string& message) { if (!pass) throw std::runtime_error(message); }
void Reset() {
    fixture::snapshot = {};
    auto& s = fixture::snapshot;
    s.supported = s.heroReady = s.healthReady = s.teleportReady = s.healthFlagsReady = true;
    s.engineActionsReady = false;
    s.savedPosition = s.undoPosition = true;
    s.health = 50; s.maxHealth = 100;
    s.characterNames = {L"Peter", L"Miles"};
    fixture::submitted.clear();
    fixture::settingsSubmitted.clear();
    fixture::reloadSubmitted.clear();
    fixture::primarySubmitted.clear();
    fixture::actorReloadSubmitted.clear(); fixture::replaySelections.clear();
    fixture::missionSelections.clear();
    fixture::replaySubmitted.clear(); fixture::replayCopies.clear();
    fixture::replayPolicies.clear(); fixture::legacyReplayCalls=0;
    fixture::newGamePlusSubmitted.clear();
    fixture::cameraShakeSubmitted.clear();
    fixture::capture={};fixture::captureFormatDirections.clear();fixture::captureRequests=0;fixture::captureRequestAccepted=true;
    visible = false; preview = false; traceInput = false; uiReady = false;
    inputQueue.clear(); history.clear(); node = "debug";
    page = -1; selected = firstRow = character = 0; healthLimit = 100; moveStep = 10;
    reloadSelectionContext = {}; reloadCheckpointKey = 0;
    reloadResetActorSave = reloadReplayEnabled = reloadFilterCheckpoints = false;
    // Legacy cases explicitly keep the current loadout. Dedicated policy cases
    // below exercise the actual production default (Off) and save admission.
    reloadKeepCurrentLoadout = true;
    reloadMissionDescending = reloadObjectiveDescending = false;
    reloadReplaySelection = {};
    reloadStartState=MissionStartState::Active;reloadExtraBranches.clear();
    reloadMissionEdited=reloadPickFirstObjective=reloadPickCheckpoint=false;
    publishedMissionSelection={};publishedMissionWorld={};havePublishedMission=false;
}
ReloadState ReadyReload() {
    ReloadState result;
    result.supported = result.ready = result.frontendReady = result.lobbyReady = true;
    result.token.imageBase = 0x140000000ull;
    result.token.manager = 0x200000100ull; result.token.managerVtable = 0x145100000ull;
    result.token.loadedLevel = 0x200001000ull; result.token.mode = 0x200002000ull;
    result.token.kernel = 0x200003000ull; result.token.kernelOwner = result.token.mode;
    result.token.componentTable = 0x200004000ull; result.token.checkpointRecords = 0x200005000ull;
    result.token.saveSystem = 0x200006000ull; result.token.saveSystemVtable = 0x1454a1ca0ull;
    result.token.levelAsset = 0xfedcba9876543210ull; result.token.contextSerial = 19;
    result.token.kernelHandle = 0x700001u; result.token.kernelGeneration = 7;
    result.token.currentCheckpoint = 0x87654321u; result.token.checkpointCount = 2;
    result.token.managerState = result.token.modeValue = 1;
    result.token.pendingInstances = result.token.pendingDeletions = 0;
    result.token.managerReady = result.token.kernelState = 1; result.token.kernelSequence = 7;
    result.checkpoints = std::make_shared<const std::vector<ReloadCheckpoint>>(
        std::vector<ReloadCheckpoint>{{0x01000001u, 0, L"First checkpoint"}, {0xfedcba99u, 1, L"High checkpoint"}});
    result.status = L"Ready";
    return result;
}
PrimaryObjectiveState ReadyPrimary() {
    PrimaryObjectiveState result;
    result.supported=result.ready=true;result.world=ReadyReload().token;
    result.token.objectiveSystem=0x200007000ull;result.token.objectiveVtable=0x145485118ull;
    result.token.saveSystem=result.world.saveSystem;result.token.saveBlock=0x200008000ull;result.token.systemData=0x200009000ull;
    result.token.primaryAddress=result.token.systemData+0x38;result.token.missionHash=0xdeadbeefu;result.token.objectiveHash=0xfedcba99u;
    result.token.missionNode=0x20000a000ull;result.token.objectiveNode=0x20000b000ull;
    result.token.missionGraphHandle=0x500002u;result.token.objectiveGraphHandle=0x700003u;
    result.token.missionNodeId=0x8000000012345678ull;result.token.objectiveNodeId=0x8000000087654321ull;
    result.status=L"Active primary objective ready";
    return result;
}
ReloadOptionsState ReadyReloadOptions() {
    ReloadOptionsState result;
    result.supported = result.ready = true;
    result.token.world = ReadyReload().token;
    result.token.block = 0x14a196288ull; result.token.container = 0x20000d000ull;
    result.token.entry = 0x20000e000ull; result.token.campaign = 0x20000f000ull;
    result.token.keys = 0x200010000ull; result.token.values = 0x200011000ull;
    result.token.mapFingerprint = 0xfedcba9876543210ull;
    result.status = L"Current campaign NG+ flag ready";
    return result;
}
CameraDebugState ReadyCamera() {
    CameraDebugState result;result.supported=result.ready=result.shakeEnabled=true;
    result.token.world=ReadyReload().token;
    result.token.cameraSystem=0x200012000ull;result.token.flagsAddress=result.token.cameraSystem+0xd0;
    result.token.allocations={0x200013000ull,0x200014000ull,0x200015000ull,0x200016000ull,0x200017000ull,0x200018000ull,0x200019000ull};
    result.token.viewCount=2;result.token.flags=0x800000a0u;
    result.status=L"Current camera shake ready";return result;
}
void RefreshMissionMetadata(bool ready=true,bool allowAvailable=false) {
    ReloadSelectionState result;result.supported=true;result.ready=ready;
    result.selection=EditedMissionSelection();result.token.world=fixture::snapshot.reload.token;
    result.token.objectiveSystem=0x20001a000ull;result.token.graphFingerprint=0xfedcba9876543210ull;
    result.token.priorOverrides[0]=0x12;result.token.priorOverrides[0x30f]=0xe7;result.token.priorEndReason=13;
    if(const auto* graph=FindReloadMissionGraph(result.selection.graphId))
        for(const auto& mission:graph->missions)result.missionOrder.push_back(mission.nameCrc);
    if(result.missionOrder.size()>1)std::rotate(result.missionOrder.begin(),result.missionOrder.begin()+1,result.missionOrder.end());
    for(const auto& branch:result.selection.branches) {
        ReloadBranchInfo info;info.allowAvailable=allowAvailable;info.checkpoint=0xfedcba99u;
        if(const auto* mission=FindReloadMission(result.selection.graphId,branch.missionHash))
            for(const auto& objective:mission->objectives)info.objectiveOrder.push_back(objective.nameCrc);
        if(info.objectiveOrder.size()>1)std::rotate(info.objectiveOrder.begin(),info.objectiveOrder.begin()+1,info.objectiveOrder.end());
        result.branches.push_back(std::move(info));
        if(result.branches.size()==1&&!branch.missionHash)result.ready=false;
    }
    result.status=result.ready?L"Explicit mission branches ready":L"Selected mission branches unavailable";
    fixture::snapshot.reloadSelection=std::move(result);
}
void SettleMissionMetadata(bool allowAvailable=false) {
    for(unsigned attempt=0;attempt<3;++attempt){RefreshMissionMetadata(true,allowAvailable);Rows(GetSnapshot());}
}
PrimaryObjectiveState ReadyCatalogPrimary() {
    auto result=ReadyPrimary();
    result.token.missionGraphAssetId=0xbaca0cde336b5792ull;
    result.token.missionHash=0x85375597u; result.token.objectiveHash=0xbd415500u;
    result.token.missionNodeId=0xfb73d3674181be28ull; result.token.objectiveNodeId=0xece915d33e8c0a7dull;
    return result;
}
ActorSaveState ReadyActorSave() {
    ActorSaveState result; result.supported=result.ready=true; result.token.world=ReadyReload().token;
    result.token.block=0x14a196290ull; result.token.container=0x200007000ull;
    result.token.entry=0x200008000ull; result.token.saveData=0x200009000ull;
    result.token.fingerprint=0xfedcba9876543210ull; result.token.mapFingerprints={0x123456789abcdef0ull,0xfedcba9876543210ull,0xabcdef1234567890ull};
    result.token.entries={2,3,4}; result.status=L"Ready: collected items stay"; return result;
}
ReplayState ReadyReplay(const ReplaySelection& selection) {
    ReplayState result; result.supported=result.ready=true; result.origin=ReadyCatalogPrimary();result.selection=selection;result.requestEpoch=1;
    result.target.node=0x20000c000ull;result.target.graphId=selection.graphId;
    result.target.nodeId=0xfb73d3674181be28ull;result.target.graphHandle=0x700003u;
    result.target.checkpoint=0xfedcba99u;result.status=L"Native replay target ready";return result;
}
void CatalogReloadSnapshot() {
    fixture::snapshot.reload=ReadyReload();fixture::snapshot.primary=ReadyCatalogPrimary();
    fixture::snapshot.actorSave=ReadyActorSave();fixture::snapshot.engineActionsReady=true;
    fixture::snapshot.reloadOptions=ReadyReloadOptions();
    visible=true;SetNode("debug.reload_map",false);
    RefreshMissionMetadata();
}
bool NoReloadGameplay() {
    return fixture::submitted.empty() && fixture::settingsSubmitted.empty() && fixture::reloadSubmitted.empty() &&
        fixture::actorReloadSubmitted.empty() && fixture::primarySubmitted.empty() && fixture::replaySubmitted.empty() && fixture::newGamePlusSubmitted.empty() && fixture::cameraShakeSubmitted.empty();
}
void Input(MenuInput input) { Apply(input, 0x1234); }
int Index(const std::string& name) {
    const auto rows = Rows(GetSnapshot());
    for (size_t i = 0; i < rows.size(); ++i) if (rows[i].name == name) return static_cast<int>(i);
    throw std::runtime_error("Missing expected UI entry: " + name);
}
void Open(const std::string& name) { selected = Index(name); Input(MenuInput::Accept); }
void Frame(float height = 480) {
    ImGui::GetIO().DisplaySize = ImVec2(960, height);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame(); RenderMenuFrame(); ImGui::Render();
}
bool HealthFlag(Action a) {
    return a == Action::HeroNoDamage || a == Action::HeroNoDeath ||
           a == Action::EnemyNoDeath || a == Action::EnemyNoDamage;
}
bool NativeAction(Action a) {
    return a == Action::Heal || a == Action::AutoHeal || a == Action::SetMaxHealth ||
           a == Action::SwitchCharacter || a == Action::LoadPosition || a == Action::UndoTeleport ||
           (a >= Action::MoveXMinus && a <= Action::MoveZPlus);
}
void Run(const char* name, const std::function<void()>& test) {
    Reset();
    try { test(); results.push_back({name, {}}); }
    catch (const std::exception& error) { results.push_back({name, error.what()}); }
}
std::string Escape(const std::string& value) {
    std::string out;
    for (char c : value) { if (c == '"' || c == '\\') out += '\\'; if (c == '\n') out += "\\n"; else out += c; }
    return out;
}
}
int main() {
    using namespace sm2debug;
    const bool actualInitialKeepLoadout=reloadKeepCurrentLoadout;
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().Fonts->AddFontDefault();
    ImGui::GetIO().Fonts->Build();
    Run("actual_initial_keep_loadout_default_is_off", [actualInitialKeepLoadout] {
        Check(!actualInitialKeepLoadout,"Production Keep Current Loadout default changed from original Off");
    });
    Run("all_39_source_port_settings_bind_exact_metadata_and_directions", [] {
        fixture::snapshot.engineActionsReady=true;visible=true;
        for(const auto& expected:portedSettings) {
            fixture::snapshot.settings.clear();fixture::settingsSubmitted.clear();
            DebugSettingValue setting;setting.available=true;setting.id=expected.id;setting.pageId=expected.page;
            setting.label=expected.label;setting.type=expected.type;setting.number=1;
            setting.choices={"first","second"};fixture::snapshot.settings.push_back(setting);
            SetNode(expected.page,false);selected=Index(expected.label);const auto row=Rows(GetSnapshot())[selected];
            Check(row.kind==Kind::Setting && row.enabled && row.settingId==expected.id,
                std::string("Source row failed exact metadata binding: ")+expected.page+"/"+expected.label);
            const auto value=expected.type==DebugSettingType::Boolean?"On":expected.type==DebugSettingType::Enum32?"second":
                expected.type==DebugSettingType::Float32?"1.0000":"1";
            Check(row.value==value,std::string("Typed setting value lost its format: ")+expected.id);
            Check(NoReloadGameplay(),"Merely displaying a new source control changed gameplay");
            Input(MenuInput::Left);Input(MenuInput::Right);Input(MenuInput::Accept);
            Check(fixture::settingsSubmitted==std::vector<std::pair<std::string,int>>{{expected.id,-1},{expected.id,1},{expected.id,0}},
                std::string("Setting queue changed exact identifier/directions: ")+expected.id);
            fixture::settingsSubmitted.clear();fixture::snapshot.engineActionsReady=false;
            Input(MenuInput::Accept);Input(MenuInput::Left);Input(MenuInput::Right);
            Check(NoReloadGameplay(),"Unavailable engine dispatcher accepted a newly ported control");
            fixture::snapshot.engineActionsReady=true;fixture::snapshot.settings[0].available=false;
            selected=Index(expected.label);Check(!Rows(GetSnapshot())[selected].enabled,"Unavailable typed setting retained an enabled source control");
            Input(MenuInput::Accept);Check(NoReloadGameplay(),"Unavailable field manufactured a setting request");
        }
    });
    Run("original_enemybot_no_damage_alias_uses_existing_health_action", [] {
        visible=true;SetNode("debug.enemies.enemybot",false);selected=Index("Bots Take No Damage");
        auto row=Rows(GetSnapshot())[selected];
        Check(row.kind==Kind::Toggle && row.action==Action::EnemyNoDamage && row.enabled && !row.checked,"Original EnemyBot alias does not share health binding");
        Input(MenuInput::Right);Input(MenuInput::Left);Input(MenuInput::Accept);
        Check(fixture::submitted==std::vector<std::pair<Action,float>>{{Action::EnemyNoDamage,1},{Action::EnemyNoDamage,0},{Action::EnemyNoDamage,1}},
            "EnemyBot alias emitted another action or wrong toggle values");
        fixture::submitted.clear();fixture::snapshot.enemyNoDamage=true;row=Rows(GetSnapshot())[selected];
        Check(row.checked && row.value=="On","EnemyBot alias displays shared live health flag");
        fixture::snapshot.healthFlagsReady=false;Input(MenuInput::Accept);
        Check(!Rows(GetSnapshot())[selected].enabled && NoReloadGameplay(),"EnemyBot alias bypasses health binding readiness");
    });
    Run("catalog_graph_and_reference_roots", [] {
        const auto* debug = FindReferencePage("debug");
        const auto* dev = FindReferencePage("dev");
        Check(debug && dev, "Both reference roots must exist");
        Check(debug->rows.size() == 29, "Reference Debug root must expose 29 entries");
        Check(dev->rows.size() == 8, "Dev root must preserve seven reference entries and link the full retail inventory");
        Check(!debug->developer && dev->developer, "Root style flags must differ");
        Check(ReferencePages().size() == 398, "Combined catalog must preserve the complete source and retail page inventory");
        size_t rowCount = 0;
        for (const auto& p : ReferencePages()) rowCount += p.rows.size();
        Check(rowCount == 2987, "Combined catalog lost imported source or retained retail rows");
        std::set<std::string> ids;
        for (const auto& p : ReferencePages()) {
            Check(ids.insert(p.id).second, "Duplicate page id: " + p.id);
            Check(!p.title.empty(), "Empty page title");
            for (const auto& r : p.rows) {
                Check(!r.label.empty(), "Empty row label");
                if (!r.target.empty()) Check(r.target.rfind("local.", 0) == 0 || FindReferencePage(r.target), "Dangling target: " + r.target);
            }
        }
    });
    Run("ps_toggle_and_backend_publication_barrier", [] {
        QueueMenuInput(MenuInput::Toggle);
        Check(!PumpMenuInput() && !MenuVisible() && inputQueue.size() == 1, "UI must wait for backend publication");
        uiReady = true;
        Check(PumpMenuInput() && MenuVisible() && inputQueue.empty(), "Queued PS-equivalent toggle must show UI");
        QueueMenuInput(MenuInput::Toggle);
        Check(!PumpMenuInput() && !MenuVisible(), "Second toggle must hide UI");
        Input(MenuInput::Accept);
        Check(node == "debug" && fixture::submitted.empty(), "Hidden menu must ignore selection");
    });
    Run("cross_enters_and_circle_returns_to_parent", [] {
        visible = true;
        Open("Enemies"); Check(node == "debug.enemies", "Cross must open Enemies");
        Open("Aggro"); Check(FindReferencePage(node)->title == "Aggro", "Cross must open child page");
        Input(MenuInput::Back); Check(node == "debug.enemies", "Circle must return one level");
        Input(MenuInput::Back); Check(node == "debug" && history.empty(), "Second Circle must return root");
        Input(MenuInput::Back); Check(!MenuVisible(), "Circle at root must close menu");
        Check(fixture::submitted.empty(), "Navigation must not submit gameplay commands");
    });
    Run("visibility_changes_revoke_backend_snapshot", [] {
        const auto before = fixture::snapshotInvalidations;
        visible = true; node = "debug"; history.clear();
        Input(MenuInput::Back);
        Check(!visible && fixture::snapshotInvalidations == before + 1, "Root Back did not invalidate snapshot");
        Input(MenuInput::Toggle);
        Check(visible && fixture::snapshotInvalidations == before + 2, "Opening did not invalidate snapshot");
        Row close; close.kind = Kind::Close; close.enabled = true;
        Activate(close, 0);
        Check(!visible && fixture::snapshotInvalidations == before + 3, "Close row did not invalidate snapshot");
        Input(MenuInput::Toggle); Input(MenuInput::Close);
        Check(!visible && fixture::snapshotInvalidations == before + 5, "Queued Close did not invalidate snapshot");
    });
    Run("every_catalog_page_edge_opens_and_returns_without_dispatch", [] {
        visible = true;
        std::set<std::string> localParents;
        for (const auto& definition : ReferencePages()) {
            history.clear(); SetNode(definition.id, false);
            const auto rows = Rows(GetSnapshot());
            Check(!rows.empty(), "Catalog page has no visible rows: " + definition.id);
            for (size_t index = 0; index < rows.size(); ++index) {
                const auto& row = rows[index];
                if (row.target.empty()) continue;
                Check(row.kind == Kind::Page && row.enabled, "Catalog submenu is disabled: " + definition.id + "/" + row.name);
                selected = static_cast<int>(index); Input(MenuInput::Accept);
                Check(node == row.target && !Rows(GetSnapshot()).empty(), "Cross failed to open populated page: " + row.target);
                if (row.target.rfind("local.", 0) == 0 && row.target.find('.', 6) != std::string::npos) {
                    Check(page == -1, "Original local child inherited unrelated local controls: " + row.target);
                    const auto child = Rows(GetSnapshot());
                    Check(std::none_of(child.begin(), child.end(), [](const Row& r) { return r.name == "Close Debug Menu"; }),
                          "Original local child displayed the local root launcher");
                    ++localChildRoutesChecked; localParents.insert(definition.id);
                }
                Frame();
                Check(ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0, "Imported submenu produced no visible geometry");
                Input(MenuInput::Back);
                Check(node == definition.id && history.empty(), "Circle failed to return to catalog parent: " + definition.id);
                ++catalogRoutesChecked;
                Check(NoReloadGameplay() && fixture::replaySelections.empty() && fixture::replayCopies.empty(),
                      "Catalog navigation dispatched gameplay, save changes, or replay selection");
            }
        }
        Check(catalogRoutesChecked > 350 && localChildRoutesChecked > 20 && localParents.count("local.hero"),
              "Imported page-edge traversal omitted the expanded local Hero hierarchy");
        std::vector<std::string> pending{"debug", "dev"};
        std::set<std::string> reachable;
        while (!pending.empty()) {
            const auto current = pending.back(); pending.pop_back();
            if (!reachable.insert(current).second) continue;
            SetNode(current, false);
            for (const auto& row : Rows(GetSnapshot())) if (!row.target.empty()) pending.push_back(row.target);
        }
        for (const auto& definition : ReferencePages())
            Check(reachable.count(definition.id) != 0, "Catalog page is unreachable from both menu roots: " + definition.id);
    });
    Run("source_help_and_local_controls_survive_catalog_merge", [] {
        visible = true;
        size_t helpRows = 0;
        for (const auto& definition : ReferencePages()) {
            SetNode(definition.id, false);
            const auto rows = Rows(GetSnapshot());
            for (const auto& source : definition.rows) {
                if (source.help.empty()) continue;
                const auto row = std::find_if(rows.begin(), rows.end(), [&](const Row& value) { return value.name == source.label && value.sourceHelp == source.help; });
                if (row == rows.end() && definition.id.rfind("local.", 0) == 0 && page >= 0) {
                    const auto local = LocalRows(GetSnapshot());
                    if (std::any_of(local.begin(), local.end(), [&](const Row& value) { return value.name == source.label; })) continue;
                }
                Check(row != rows.end(), "Source-help row disappeared: " + source.label);
                if (row->enabled || row->kind != Kind::Action) continue;
                Check(row->sourceHelp == source.help && HintForRow(*row, GetSnapshot()).find(source.help) != std::string::npos,
                      "Unsupported original control lost its recovered help: " + source.label);
                ++helpRows;
            }
        }
        Check(helpRows > 1000, "Original help preservation covered unexpectedly few controls");
        for (const char* id : {"local.common", "local.health", "local.hero", "local.rendering", "local.engine"}) {
            SetNode(id, false);
            const auto merged = Rows(GetSnapshot()), local = LocalRows(GetSnapshot());
            for (const auto& expected : local) {
                const auto count = std::count_if(merged.begin(), merged.end(), [&](const Row& row) { return row.name == expected.name; });
                Check(count == 1, "Local control lost or duplicated by source merge: " + expected.name);
                const auto actual = std::find_if(merged.begin(), merged.end(), [&](const Row& row) { return row.name == expected.name; });
                Check(actual->kind == expected.kind && actual->action == expected.action && actual->enabled == expected.enabled && actual->target == expected.target,
                      "Catalog merge replaced a working local control: " + expected.name);
            }
        }
        Check(NoReloadGameplay(), "Inspecting source help or local control merge dispatched an action");
    });
    Run("unavailable_debug_controls_are_labeled_and_still_refuse_actions", [] {
        struct Control { const char* page; const char* label; };
        const std::array<Control, 5> controls{{
            {"debug.health", "Kill heroes"}, {"debug.loading", "Filter Files"},
            {"debug.reactions", "Draw Debug Parry Meter"}, {"debug.materials", "Aniso Filtering"},
            {"debug.reload_map", "Item Loadout"}
        }};
        visible = true; fixture::snapshot.engineActionsReady = true;
        for (const auto& control : controls) {
            SetNode(control.page, false); selected = Index(control.label);
            const auto row = Rows(GetSnapshot())[selected];
            Check(!row.enabled && row.target.empty() && row.value == "Unavailable" &&
                  HintForRow(row, GetSnapshot()).rfind("Unavailable: no verified binding in this game build", 0) == 0,
                  "Unbound Debug control is not clearly labeled: " + row.name);
            for (MenuInput input : {MenuInput::Accept, MenuInput::Left, MenuInput::Right}) Input(input);
            Check(node == control.page, "Unavailable leaf opened an invented submenu");
        }
        SetNode("local.hero", false); selected = Index("Respawn Clear");
        const auto respawn = Rows(GetSnapshot())[selected];
        Check(!respawn.enabled && respawn.value == "Unavailable" && respawn.settingId.empty(),
              "Missing local Hero binding must be labeled unavailable");
        for (MenuInput input : {MenuInput::Accept, MenuInput::Left, MenuInput::Right}) Input(input);
        Check(NoReloadGameplay() && fixture::replaySelections.empty() && fixture::replayCopies.empty(),
              "Relabeling unavailable controls enabled an action");
        SetNode("dev.retail.act_1_missions", false);
        const auto dev = Rows(GetSnapshot())[Index("Sandman")];
        Check(!dev.enabled && dev.value == "N/A" && HintForRow(dev, GetSnapshot()) == "Unavailable: no verified retail-PC binding",
              "Debug access changes must preserve the existing Dev availability presentation");
    });
    Run("dev_reference_links_open_existing_retail_menus_and_return", [] {
        struct Link { const char* parent; const char* label; const char* target; const char* nativeParent; };
        const std::array<Link, 4> links{{
            {"dev", "Tracking Demo", "dev.retail.tracking_demo", "dev.retail"},
            {"dev.missions", "Act 2 Missions", "dev.retail.act_2_missions", "dev.retail.gp_missions_and_ow_activities"},
            {"dev.missions", "Act 3 Missions", "dev.retail.act_3_missions", "dev.retail.gp_missions_and_ow_activities"},
            {"dev.missions", "Silver Path", "dev.retail.silver_path", "dev.retail.gp_missions_and_ow_activities"}
        }};
        for (const auto& link : links) {
            visible = true; history.clear(); SetNode("dev", false);
            if (std::string(link.parent) == "dev.missions") Open("Missions and Activities");
            Check(node == link.parent, "Reference parent must remain reachable from Dev root");
            const auto* native = FindNativeDevOption(link.nativeParent, link.label);
            Check(native && native->nativeType == "UITitleScreenOptionGotoMenu" && native->targetPageId == link.target,
                  "Navigation target must match the retained native GotoMenu metadata");
            const auto* destination = FindReferencePage(link.target);
            Check(destination && !destination->rows.empty(), "Navigation must open an existing populated retail page");
            Open(link.label);
            Check(node == link.target, std::string("Cross did not open ") + link.label);
            Input(MenuInput::Back);
            Check(node == link.parent, std::string("Circle did not return from ") + link.label);
        }
        Check(NoReloadGameplay() && fixture::replaySelections.empty(), "Menu navigation must not dispatch gameplay or replay changes");
    });
    Run("full_retail_dev_inventory_is_reachable", [] {
        visible = true; SetNode("dev", false); Open("All retail Dev menus");
        Check(node == "dev.retail.index", "Dev root must expose complete retained inventory");
        Check(Rows(GetSnapshot()).size() == 25, "Index must expose every retained menu including disconnected pages");
        size_t leaves = 0;
        for (const auto& p : NativeDevPages()) {
            Open(p.title); Check(node == p.id, "Index opened the wrong retained menu");
            const auto rows = Rows(GetSnapshot());
            Check(rows.size() == p.rows.size(), "Retained menu lost an option");
            for (const auto& row : rows) if (row.target.empty()) {
                Check(!row.enabled, "Metadata alone must not enable a native action"); ++leaves;
            }
            Input(MenuInput::Back); Check(node == "dev.retail.index", "Retained menu must return to index");
        }
        Check(leaves == 155, "All154 native action leaves and the missing menu reference must be preserved");
    });
    Run("ps_reopens_debug_root_after_closing_dev_menu", [] {
        visible = true; SetNode("dev", false);
        const auto rows = Rows(GetSnapshot());
        bool childOpened = false;
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].target.rfind("dev.", 0) == 0) {
            selected = static_cast<int>(i); Input(MenuInput::Accept); childOpened = true; break;
        }
        Check(childOpened && node.rfind("dev.", 0) == 0, "Fixture must navigate into a genuine Dev submenu");
        const auto previousNode = node;
        Input(MenuInput::Toggle);
        Check(!MenuVisible() && node == previousNode, "PS must close the currently open Dev submenu");
        Input(MenuInput::Toggle);
        Check(MenuVisible() && node == "debug" && history.empty(), "PS reopening must show Debug root, not retained Dev page");
        Input(MenuInput::NextPage);
        Check(node == "dev", "Dev menu must remain reachable through shoulder navigation");
        Check(fixture::submitted.empty(), "PS and shoulder navigation must not submit gameplay commands");
    });
    Run("shoulder_buttons_switch_debug_and_dev_roots", [] {
        visible = true; Open("Health");
        Input(MenuInput::NextPage);
        Check(node == "dev" && history.empty() && selected == 0, "R1 must select Dev root and clear old history");
        Input(MenuInput::PreviousPage);
        Check(node == "debug" && history.empty(), "L1 must select Debug root");
        Input(MenuInput::PreviousPage); Check(node == "dev", "L1 must switch from Debug to Dev");
        Input(MenuInput::NextPage); Check(node == "debug", "R1 must switch from Dev to Debug");
    });
    Run("loading_scroll_reaches_every_nonheading_row", [] {
        visible = true; SetNode("debug.loading", false);
        const auto rows = Rows(GetSnapshot());
        const int start = selected;
        std::set<int> visited;
        bool scrolled = false;
        do {
            Check(rows[selected].kind != Kind::Info, "Heading must not receive navigation selection");
            visited.insert(selected);
            Frame();
            Check(firstRow <= selected, "Selected row must not be above viewport");
            const float size = 15, line = size * 1.35f, top = 24, rowTop = top + line * 2;
            const int capacity = std::max(4, static_cast<int>((480 - rowTop - 100) / line));
            Check(selected < firstRow + capacity, "Selected row must remain in rendered viewport");
            if (firstRow > 0) scrolled = true;
            Input(MenuInput::Down);
            Check(visited.size() <= rows.size(), "Navigation failed to wrap");
        } while (selected != start);
        size_t selectable = 0;
        for (const auto& row : rows) if (row.kind != Kind::Info) ++selectable;
        Check(visited.size() == selectable && scrolled, "Every Loading control must be reachable beyond first viewport");
        loadingRowsVisited = visited.size();
        Input(MenuInput::Up);
        Check(selected == *visited.rbegin(), "Up must wrap to last Loading control");
    });
    Run("all_unsupported_catalog_leaves_submit_no_actions", [] {
        visible = true;
        for (const auto& p : ReferencePages()) {
            SetNode(p.id, false);
            const auto rows = Rows(GetSnapshot());
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto& r = rows[i];
                if (r.kind == Kind::Info || !r.target.empty() || r.enabled) continue;
                Check(!r.enabled, "Unsupported row is enabled: " + p.id + "/" + r.name);
                selected = static_cast<int>(i); fixture::submitted.clear();
                Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
                Check(NoReloadGameplay() && fixture::replaySelections.empty() && fixture::replayCopies.empty(), "Unsupported leaf submitted an action: " + p.id + "/" + r.name);
                Check(node == p.id, "Unsupported leaf changed navigation");
                ++unsupportedLeaves;
            }
        }
        Check(unsupportedLeaves > 2000, "Policy sweep omitted the imported unsupported catalog leaves");
    });
    Run("reload_uncataloged_primary_browses_catalog_but_requires_native_start_state_metadata", [] {
        fixture::snapshot.reload = ReadyReload(); fixture::snapshot.engineActionsReady = true;
        fixture::snapshot.primary=ReadyPrimary();
        visible = true; SetNode("debug.reload_map", false);
        const std::array<const char*, 20> expected{{"Mission", "Objective", "Start State", "Replay Mode",
            "Extra Mission Branches", "Checkpoint", "Item Loadout", "Graph Filter", "Mission Sort Order",
            "Objective Sort Order", "Keep Current Loadout", "NG+", "Filter Checkpoint Selection",
            "Reset Actor Save on Manual Reload", "Copy Reload Data to Clipboard", "Reload", "Full Reload",
            "Complete Primary Objective (Preserve Progression)", "Exit to Frontend", "Exit to Lobby"}};
        const auto rows = Rows(GetSnapshot());
        Check(rows.size() == expected.size(), "Reload lost a reference row");
        size_t enabled = 0, disabled = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            Check(rows[i].name == expected[i], "Reload reference spelling/order differs");
            const bool bound = i==0||i==1||i==3||i==4||i==5||i==7||i==8||i==9||i==10||i==14||i==15||i==16||i==17||i==18||i==19;
            Check(rows[i].enabled == bound, "Reload reference row gained/lost an admitted control: " + rows[i].name);
            if (bound) { ++enabled; continue; }
            ++disabled; selected = static_cast<int>(i);
            Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
        }
        Check(enabled==15&&disabled==5,"Uncataloged primary must expose local catalog browsing while preserving native-state/save gates");
        Check(fixture::submitted.empty() && fixture::settingsSubmitted.empty() && fixture::reloadSubmitted.empty() && fixture::primarySubmitted.empty(),
              "Unsupported Reload row dispatched an action");
    });
    Run("mission_picker_without_active_primary_browses_catalog_without_executing_replay", [] {
        fixture::snapshot.reload = ReadyReload();
        fixture::snapshot.primary = {};
        fixture::snapshot.engineActionsReady = true;
        visible = true; SetNode("debug.reload_map", false);
        const auto& graph = ReloadMissionCatalog().front();
        Check(graph.missions.size() > 1, "Fixture requires multiple catalog missions");
        const auto initial = reloadReplaySelection;
        Check(initial.graphId == graph.graphAssetId && initial.missionHash == graph.missions.front().nameCrc && !initial.objectiveHash,
              "Missing active primary must initialize a real catalog mission and mission-default objective");
        auto mission = Rows(GetSnapshot())[Index("Mission")];
        Check(mission.value == graph.missions.front().name && mission.enabled &&
              Rows(GetSnapshot())[Index("Replay Mode")].enabled,
              "Mission display must have real metadata and an available Replay Mode selector");
        Check(Rows(GetSnapshot())[Index("Reload")].action == Action::ReloadCheckpoint &&
              Rows(GetSnapshot())[Index("Reload")].enabled,
              "Catalog initialization must preserve ordinary manual checkpoint Reload");
        Check(fixture::replaySelections.empty() && NoReloadGameplay(), "Opening Reload must not publish or execute a replay request");
        fixture::snapshot.engineActionsReady = false;
        selected = Index("Replay Mode"); Input(MenuInput::Right);
        Check(reloadReplayEnabled && fixture::replaySelections.size() == 1 &&
              SameReplaySelection(fixture::replaySelections.back(), initial),
              "Data-only Replay Mode selection must not require an active primary or dispatcher");
        for (const char* label : {"Mission", "Objective", "Graph Filter"})
            Check(Rows(GetSnapshot())[Index(label)].enabled, "Catalog selector is blocked without an active primary");
        selected = Index("Mission"); Input(MenuInput::Right);
        Check(reloadReplaySelection.missionHash == graph.missions[1].nameCrc && !reloadReplaySelection.objectiveHash,
              "Mission selector failed to advance to the next real catalog mission");
        const auto* picked = FindReloadMission(reloadReplaySelection.graphId, reloadReplaySelection.missionHash);
        Check(picked && !picked->objectives.empty(), "Fixture requires a real objective for the selected mission");
        selected = Index("Objective"); Input(MenuInput::Right);
        Check(reloadReplaySelection.objectiveHash == picked->objectives.front().nameCrc,
              "Objective selector did not use the chosen mission's real objective IDs");
        Check(SameReplaySelection(fixture::replaySelections.back(), reloadReplaySelection), "Selection publication lost its exact IDs");
        fixture::snapshot.engineActionsReady = true;
        fixture::snapshot.replay.selection = reloadReplaySelection;
        fixture::snapshot.replay.status = L"Replay requires a current loaded mission in this graph";
        const auto reload = Rows(GetSnapshot())[Index("Reload")];
        Check(!reload.enabled && reload.action == Action::ReplayMission &&
              HintForRow(reload, GetSnapshot()) == "Replay requires a current loaded mission in this graph",
              "Unvalidated catalog selection must retain native replay refusal and its reason");
        Activate(reload, 0); Activate(reload, -1); Activate(reload, 1);
        Check(NoReloadGameplay() && fixture::replayCopies.empty(), "Catalog browsing or an unready Reload executed gameplay");
    });
    Run("mission_picker_fallback_prefers_only_a_ready_primarys_known_catalog_graph", [] {
        const auto& graphs = ReloadMissionCatalog();
        Check(graphs.size() > 1 && !graphs.back().missions.empty(), "Fixture requires distinct populated graphs");
        for (int scenario = 0; scenario < 3; ++scenario) {
            fixture::snapshot.reload = ReadyReload(); fixture::snapshot.reload.token.contextSerial += scenario;
            fixture::snapshot.primary = ReadyPrimary(); fixture::snapshot.primary.world = fixture::snapshot.reload.token;
            fixture::snapshot.primary.token.missionGraphAssetId = scenario == 1 ? 0x1122334455667788ull : graphs.back().graphAssetId;
            fixture::snapshot.primary.token.missionHash = 0xdeadbeefu;
            fixture::snapshot.primary.token.objectiveHash = 0xfedcba99u;
            fixture::snapshot.primary.ready = scenario != 2;
            Check(!FindReloadMission(fixture::snapshot.primary.token.missionGraphAssetId, 0xdeadbeefu),
                  "Fixture must use an uncatalogued primary mission");
            visible = true; SetNode("debug.reload_map", false);
            const auto& expected = scenario == 0 ? graphs.back() : graphs.front();
            Check(reloadReplaySelection.graphId == expected.graphAssetId &&
                  reloadReplaySelection.missionHash == expected.missions.front().nameCrc && !reloadReplaySelection.objectiveHash,
                  "Fallback selected the wrong graph, retained an invalid mission, or reused an unrelated objective");
            Check(Rows(GetSnapshot())[Index("Mission")].value == expected.missions.front().name &&
                  Rows(GetSnapshot())[Index("Replay Mode")].enabled,
                  "An uncatalogued or unavailable primary blocked catalog selection");
        }
        Check(NoReloadGameplay() && fixture::replaySelections.empty(), "Fallback initialization must remain local UI state");
    });
    Run("mission_picker_user_choice_survives_rendering_and_late_primary_readiness", [] {
        fixture::snapshot.reload = ReadyReload(); visible = true; SetNode("debug.reload_map", false);
        selected = Index("Replay Mode"); Input(MenuInput::Right);
        selected = Index("Graph Filter"); Input(MenuInput::Right);
        const auto picked = reloadReplaySelection;
        Check(FindReloadMission(picked.graphId, picked.missionHash) && picked.graphId != ReadyCatalogPrimary().token.missionGraphAssetId,
              "Fixture must choose a valid graph distinct from the later active primary");
        const auto publications = fixture::replaySelections.size();
        for (int frame = 0; frame < 3; ++frame) { Frame(); Rows(GetSnapshot()); }
        Check(SameReplaySelection(picked, reloadReplaySelection), "Rendering reset the user-selected catalog mission");
        fixture::snapshot.primary = ReadyCatalogPrimary();
        for (int frame = 0; frame < 3; ++frame) { Frame(); Rows(GetSnapshot()); }
        Check(SameReplaySelection(picked, reloadReplaySelection) && fixture::replaySelections.size() == publications,
              "A late primary or rendering replaced/republished an active user selection");
        selected = Index("Replay Mode"); Input(MenuInput::Left);
        Check(!reloadReplayEnabled && SameReplaySelection(fixture::replaySelections.back(), ReplaySelection{}),
              "Leaving Replay Mode must clear the backend's desired replay selection");
        Rows(GetSnapshot());
        Check(reloadReplaySelection.graphId == fixture::snapshot.primary.token.missionGraphAssetId &&
              reloadReplaySelection.missionHash == fixture::snapshot.primary.token.missionHash,
              "Manual mode must resume its existing preference for the real current primary");
        Check(NoReloadGameplay() && fixture::replayCopies.empty(), "Selection/rendering/mode changes executed gameplay");
    });
    Run("mission_picker_world_change_resets_fallback_and_rejects_stale_selector", [] {
        fixture::snapshot.reload = ReadyReload(); visible = true; SetNode("debug.reload_map", false);
        selected = Index("Replay Mode"); Input(MenuInput::Right);
        selected = Index("Mission"); Input(MenuInput::Right);
        const auto stale = Rows(GetSnapshot())[Index("Mission")];
        const auto previous = reloadReplaySelection;
        const auto publications = fixture::replaySelections.size();
        ++fixture::snapshot.reload.token.contextSerial; fixture::snapshot.reload.token.levelAsset ^= 0x1000u;
        Activate(stale, 1);
        Check(SameReplaySelection(previous, reloadReplaySelection) && fixture::replaySelections.size() == publications,
              "Old-world selector mutated or published a mission for the new world");
        Rows(GetSnapshot());
        const auto& graph = ReloadMissionCatalog().front();
        Check(!reloadReplayEnabled && !reloadResetActorSave && !reloadFilterCheckpoints && !reloadCheckpointKey &&
              SameReloadContext(reloadSelectionContext, fixture::snapshot.reload.token),
              "World transition retained replay/manual policies from the old world");
        Check(reloadReplaySelection.graphId == graph.graphAssetId &&
              reloadReplaySelection.missionHash == graph.missions.front().nameCrc && !reloadReplaySelection.objectiveHash,
              "New world without a primary did not reset to valid default catalog metadata");
        Check(fixture::replaySelections.size()==publications+1&&SameReplaySelection(fixture::replaySelections.back(),ReplaySelection{})&&NoReloadGameplay(),
              "World reset failed to clear the old replay target exactly once or executed gameplay");
    });
    Run("mission_picker_catalog_metadata_does_not_unlock_unsupported_or_unloaded_worlds", [] {
        for (int scenario = 0; scenario < 2; ++scenario) {
            fixture::snapshot.reload = ReadyReload(); fixture::snapshot.reload.token.contextSerial += scenario;
            fixture::snapshot.supported = scenario != 0;
            fixture::snapshot.reload.ready = scenario == 0;
            fixture::snapshot.engineActionsReady = true;
            visible = true; SetNode("debug.reload_map", false);
            for (const char* label : {"Replay Mode", "Mission", "Objective", "Graph Filter", "Mission Sort Order",
                                     "Objective Sort Order", "Reload", "Full Reload"}) {
                selected = Index(label); const auto row = Rows(GetSnapshot())[selected];
                Check(!row.enabled, "Catalog metadata unlocked a control without a supported loaded world: " + row.name);
                Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
            }
        }
        Check(!reloadReplayEnabled && fixture::replaySelections.empty() && NoReloadGameplay(),
              "Unsupported/unloaded selector queued a replay or gameplay request");
    });
    Run("reload_checkpoint_cycles_current_and_exact_uint32_keys", [] {
        fixture::snapshot.reload = ReadyReload(); visible = true; SetNode("debug.reload_map", false);
        selected = Index("Checkpoint");
        auto expect = [&](uint32_t key, const char* value) {
            const auto row = Rows(GetSnapshot())[selected];
            Check(row.enabled && row.kind == Kind::ReloadCheckpoint && row.checkpointKey == key && row.value == value,
                  "Checkpoint selector changed uint32 identity or display name");
        };
        expect(0, "Current checkpoint"); Input(MenuInput::Right); expect(0x01000001u, "First checkpoint");
        Input(MenuInput::Accept); expect(0xfedcba99u, "High checkpoint");
        Input(MenuInput::Right); expect(0, "Current checkpoint");
        Input(MenuInput::Left); expect(0xfedcba99u, "High checkpoint");
        Input(MenuInput::Left); expect(0x01000001u, "First checkpoint");
        Input(MenuInput::Left); expect(0, "Current checkpoint");
        Check(static_cast<uint32_t>(static_cast<float>(0x01000001u)) != 0x01000001u,
              "Fixture must use a key that demonstrates float precision loss");
        Check(fixture::submitted.empty() && fixture::reloadSubmitted.empty(), "Local checkpoint selection dispatched gameplay");
    });
    Run("reload_context_change_resets_selection_and_stale_row_cannot_cycle", [] {
        fixture::snapshot.reload = ReadyReload(); visible = true; SetNode("debug.reload_map", false);
        selected = Index("Checkpoint"); Input(MenuInput::Left);
        const auto stale = Rows(GetSnapshot())[selected];
        Check(stale.checkpointKey == 0xfedcba99u, "Fixture did not select a named checkpoint");
        fixture::snapshot.reload.token.loadedLevel += 0x1000;
        ++fixture::snapshot.reload.token.contextSerial;
        Activate(stale, 1);
        Check(reloadCheckpointKey == 0xfedcba99u, "Stale checkpoint row cycled selection for another world");
        const auto current = Rows(GetSnapshot())[selected];
        Check(current.checkpointKey == 0 && current.value == "Current checkpoint", "New level retained the previous level checkpoint key");
        Check(SameReloadContext(reloadSelectionContext, fixture::snapshot.reload.token), "Selection did not bind to the new level token");
        Check(fixture::reloadSubmitted.empty(), "Stale selector submitted a native request");
    });
    Run("reload_native_context_gate_and_typed_captured_payload", [] {
        fixture::snapshot.reload = ReadyReload(); visible = true; SetNode("debug.reload_map", false);
        fixture::snapshot.heroReady = fixture::snapshot.healthReady = fixture::snapshot.teleportReady = false;
        fixture::snapshot.actorHandle = 0;
        selected = Index("Checkpoint"); Input(MenuInput::Left);
        Check(Rows(GetSnapshot())[Index("Checkpoint")].enabled && Rows(GetSnapshot())[Index("Copy Reload Data to Clipboard")].enabled,
              "Local selector/copy incorrectly require engine context");
        for (const char* label : {"Reload", "Full Reload", "Exit to Frontend"}) {
            selected = Index(label); Check(!Rows(GetSnapshot())[selected].enabled, "Native world action enabled without dispatcher");
            Input(MenuInput::Accept);
        }
        Check(fixture::reloadSubmitted.empty(), "Unready dispatcher accepted native world request");
        selected = Index("Copy Reload Data to Clipboard"); Input(MenuInput::Accept);
        fixture::snapshot.engineActionsReady = true;
        for (const char* label : {"Reload", "Full Reload", "Exit to Frontend"}) { selected = Index(label); Input(MenuInput::Accept); }
        Check(fixture::reloadSubmitted.size() == 4, "Bound Reload rows must each submit once");
        const std::array<Action,4> actions{{Action::CopyReloadData, Action::ReloadCheckpoint, Action::FullReload, Action::ExitFrontend}};
        for (size_t i = 0; i < actions.size(); ++i) {
            const auto& request = fixture::reloadSubmitted[i];
            Check(request.action == actions[i] && request.key == 0xfedcba99u &&
                  SameReloadContext(request.captured.token, fixture::snapshot.reload.token) &&
                  request.captured.checkpoints == fixture::snapshot.reload.checkpoints,
                  "Typed Reload request lost the exact selection/context snapshot");
        }
        Check(fixture::submitted.empty(), "Reload world actions traveled through the float-based generic action API");
        Check(HintForRow(Rows(GetSnapshot())[Index("Full Reload")],GetSnapshot())==
              "Fully unload and reload the selected checkpoint; unsaved actions will be lost",
              "Full Reload must disclose its distinct unload/reload effect");
        fixture::snapshot.reload.frontendReady = false;
        Check(!Rows(GetSnapshot())[Index("Exit to Frontend")].enabled && Rows(GetSnapshot())[Index("Reload")].enabled,
              "Missing frontend routing incorrectly disabled or enabled another action");
        fixture::snapshot.reload.ready = false;
        for (const auto& row : Rows(GetSnapshot())) Check(!row.enabled||row.kind==Kind::Page, "Unavailable loaded level retained an active Reload action");
    });
    Run("primary_completion_requires_active_primary_and_verified_dispatcher", [] {
        fixture::snapshot.reload=ReadyReload();fixture::snapshot.engineActionsReady=true;
        fixture::snapshot.primary.status=L"No active supported primary objective";
        visible=true;SetNode("debug.reload_map",false);
        selected=Index("Complete Primary Objective (Preserve Progression)");
        auto row=Rows(GetSnapshot())[selected];
        Check(!row.enabled && HintForRow(row,GetSnapshot())=="No active supported primary objective",
              "Missing active primary must remain disabled with its actual readiness reason");
        Input(MenuInput::Accept);Input(MenuInput::Left);Input(MenuInput::Right);
        Check(fixture::primarySubmitted.empty(),"Unavailable primary submitted a native completion request");
        fixture::snapshot.primary=ReadyPrimary();fixture::snapshot.engineActionsReady=false;
        row=Rows(GetSnapshot())[selected];
        Check(!row.enabled && HintForRow(row,GetSnapshot())=="Waiting for the verified engine dispatcher",
              "Primary completion bypassed or misreported missing dispatcher");
        Input(MenuInput::Accept);Check(fixture::primarySubmitted.empty(),"Primary request submitted without dispatcher");
        fixture::snapshot.engineActionsReady=true;fixture::snapshot.heroReady=false;fixture::snapshot.actorHandle=0;
        row=Rows(GetSnapshot())[selected];
        Check(row.enabled && row.action==Action::CompletePrimary &&
              HintForRow(row,GetSnapshot())=="Complete the current primary objective; normal mission scripts and rewards apply",
              "Ready primary action requires an actor or hides native progression consequences");
        Input(MenuInput::Left);Input(MenuInput::Right);Check(fixture::primarySubmitted.empty(),"Left/right executed primary completion");
        Input(MenuInput::Accept);
        Check(fixture::primarySubmitted.size()==1 && fixture::submitted.empty() && fixture::reloadSubmitted.empty(),
              "Primary Cross must use exactly one dedicated typed queue");
    });
    Run("primary_completion_preserves_exact_captured_identity_and_uint32_hashes", [] {
        fixture::snapshot.reload=ReadyReload();fixture::snapshot.primary=ReadyPrimary();fixture::snapshot.engineActionsReady=true;
        visible=true;SetNode("debug.reload_map",false);selected=Index("Complete Primary Objective (Preserve Progression)");
        const auto captured=Rows(GetSnapshot())[selected];
        ++fixture::snapshot.primary.token.objectiveHash;++fixture::snapshot.primary.token.objectiveGraphHandle;
        Activate(captured,0);
        Check(fixture::primarySubmitted.size()==1,"Captured primary row did not retain a dedicated request");
        const auto& request=fixture::primarySubmitted.front();
        Check(request.token.missionHash==0xdeadbeefu && request.token.objectiveHash==0xfedcba99u &&
              request.token.objectiveGraphHandle==0x700003u && request.token.objectiveNodeId==0x8000000087654321ull &&
              request.token.objectiveNode==captured.primary.token.objectiveNode && SameReloadContext(request.world,captured.primary.world),
              "Primary request substituted current UI state or rounded native hashes/IDs");
        Check(fixture::submitted.empty() && fixture::reloadSubmitted.empty(),"Primary identity traveled through a float or checkpoint action");
    });
    Run("reload_nineteen_controls_have_exact_manual_and_replay_gates", [] {
        CatalogReloadSnapshot();
        const std::set<std::string> unavailable{"Item Loadout"};
        const auto manual=Rows(GetSnapshot());
        for (const auto& row : manual) {
            if (unavailable.count(row.name)) {
                Check(!row.enabled && row.value=="Unavailable","Unsupported Reload policy gained a fabricated binding: "+row.name);
                Activate(row,0);Activate(row,-1);Activate(row,1);
            } else {
                ++reloadBoundControlsChecked;
                Check(row.enabled,"Manual mode bound row has wrong readiness: "+row.name);
                manualReloadRowsEnabled+=row.enabled;
            }
        }
        Check(reloadBoundControlsChecked==19&&manualReloadRowsEnabled==19,"Ready manual Reload must expose19 working controls including the extra-branches submenu");
        Check(NoReloadGameplay() && fixture::replaySelections.empty(),"Read-only binding inventory dispatched a request");
        selected=Index("Replay Mode");Input(MenuInput::Right);
        fixture::snapshot.replay=ReadyReplay(reloadReplaySelection);
        const std::set<std::string> manualOnly{"Checkpoint","Reset Actor Save on Manual Reload","Filter Checkpoint Selection","Start State"};
        for (const auto& row : Rows(GetSnapshot())) {
            const bool expected=!unavailable.count(row.name) && !manualOnly.count(row.name);
            Check(row.enabled==expected,"Replay mode enabled an unsupported/incompatible row: "+row.name);
            replayRowsEnabled+=row.enabled;
            if (!expected) {Activate(row,0);Activate(row,-1);Activate(row,1);}
        }
        Check(replayRowsEnabled==15,"Ready Replay mode must expose15 controls, including read-only extra-branches navigation");
        Check(NoReloadGameplay(),"Disabled Replay combinations dispatched gameplay");
    });
    Run("manual_mission_edit_defers_to_native_metadata_and_captures_exact_policy", [] {
        CatalogReloadSnapshot();const auto initial=Rows(GetSnapshot())[Index("Reload")];
        Check(initial.enabled&&!initial.applyMissionSelection&&NoReloadGameplay(),"Passive Reload page enabled an implicit mission override");
        const auto publications=fixture::missionSelections.size();Frame();Frame();
        Check(fixture::missionSelections.size()==publications&&fixture::replaySelections.empty()&&NoReloadGameplay(),
            "Unchanged rendering republished selections or dispatched gameplay");
        selected=Index("Mission");Input(MenuInput::Right);
        auto pending=Rows(GetSnapshot())[Index("Reload")];
        Check(reloadMissionEdited&&!pending.enabled&&pending.applyMissionSelection&&pending.value=="Checking mission selection",
            "Edited mission executed before matching native metadata");
        Activate(pending,0);Check(NoReloadGameplay(),"Unvalidated mission selection dispatched Reload");
        SettleMissionMetadata();const auto captured=Rows(GetSnapshot())[Index("Reload")];
        Check(captured.enabled&&captured.applyMissionSelection&&captured.checkpointKey==0xfedcba99u&&
            captured.missionSelection.selection.branches[0].objectiveHash==reloadReplaySelection.objectiveHash,
            "Native first-objective/checkpoint metadata did not admit the explicit selection");
        fixture::snapshot.reloadSelection.token.graphFingerprint++;fixture::snapshot.reloadSelection.token.priorOverrides[0]=0;
        fixture::snapshot.reloadSelection.selection.branches[0].missionHash^=1;
        Activate(captured,0);
        Check(fixture::reloadSubmitted.size()==1&&fixture::replaySubmitted.empty()&&fixture::submitted.empty(),"Mission edit did not use one typed Reload policy queue");
        const auto& request=fixture::reloadSubmitted.front();
        Check(request.policyQueue&&request.policy.applyMissionSelection&&request.action==Action::ReloadCheckpoint&&
            request.policy.missions.token.graphFingerprint==0xfedcba9876543210ull&&
            request.policy.missions.token.priorOverrides==captured.missionSelection.token.priorOverrides&&
            request.policy.missions.token.priorOverrides[0]==0x12&&request.policy.missions.token.priorOverrides[0x30f]==0xe7&&
            request.policy.missions.token.priorEndReason==13&&
            SameReloadContext(request.policy.missions.token.world,captured.reload.token)&&
            SameMissionSelection(request.policy.missions.selection,captured.missionSelection.selection),
            "Reload policy refreshed or rounded the captured mission selection/pending-native-state guard");
        fixture::reloadSubmitted.clear();SettleMissionMetadata();
        selected=Index("Full Reload");Input(MenuInput::Accept);
        Check(fixture::reloadSubmitted.size()==1&&fixture::reloadSubmitted[0].action==Action::FullReload&&fixture::reloadSubmitted[0].policy.applyMissionSelection,
            "Full Reload omitted explicit mission state preparation");
    });
    Run("manual_start_state_uses_native_available_capability_and_clears_objective", [] {
        CatalogReloadSnapshot();selected=Index("Start State");Input(MenuInput::Accept);
        Check(reloadStartState==MissionStartState::Complete&&!reloadReplaySelection.objectiveHash&&
            !Rows(GetSnapshot())[Index("Objective")].enabled,"Unavailable native Available state was not skipped or retained an active objective");
        SettleMissionMetadata();selected=Index("Start State");Input(MenuInput::Left);
        Check(reloadStartState==MissionStartState::Active&&reloadPickFirstObjective,"Returning to Active failed to request native first objective");
        SettleMissionMetadata(true);selected=Index("Start State");Input(MenuInput::Right);
        Check(reloadStartState==MissionStartState::Available&&!reloadReplaySelection.objectiveHash,"Available-capable mission skipped its original Available state");
        SettleMissionMetadata(true);selected=Index("Start State");Input(MenuInput::Right);
        Check(reloadStartState==MissionStartState::Complete&&!reloadReplaySelection.objectiveHash,"Available did not advance to Complete");
        SettleMissionMetadata(true);selected=Index("Start State");Input(MenuInput::Right);SettleMissionMetadata(true);
        Check(reloadStartState==MissionStartState::Active&&reloadReplaySelection.objectiveHash&&NoReloadGameplay(),
            "Start State cycle executed gameplay or failed to restore the native active objective");
        fixture::snapshot.reloadSelection.supported=false;selected=Index("Start State");const auto refused=Rows(GetSnapshot())[selected];
        Check(!refused.enabled,"Missing native definition enabled Start State");Input(MenuInput::Accept);
        Check(reloadStartState==MissionStartState::Active&&NoReloadGameplay(),"Unavailable state selector changed mission state");
    });
    Run("extra_mission_branches_open_edit_remove_and_cap_without_gameplay", [] {
        CatalogReloadSnapshot();Open("Extra Mission Branches");
        Check(node=="debug.reload_map.extra_branches"&&Rows(GetSnapshot()).size()==6,"Cross did not open the dynamic branch editor");
        const auto arrowsDoNothing=[](const char* label) {
            selected=Index(label);const auto before=EditedMissionSelection();Input(MenuInput::Left);Input(MenuInput::Right);
            Check(SameMissionSelection(before,EditedMissionSelection())&&NoReloadGameplay(),"Branch command accepted Left/Right instead of requiring Cross");
        };
        arrowsDoNothing("Add");
        selected=Index("Add");Input(MenuInput::Accept);
        Check(reloadExtraBranches.size()==1&&!reloadExtraBranches[0].value.missionHash,"Add did not create an empty original editor row");
        RefreshMissionMetadata();Check(fixture::snapshot.reloadSelection.ready,"An empty extra row incorrectly invalidated the main branch");
        selected=Index("Mission");Input(MenuInput::Right);Input(MenuInput::Right);SettleMissionMetadata(true);
        Check(reloadExtraBranches[0].value.missionHash&&reloadExtraBranches[0].value.objectiveHash,"Extra mission did not pick its native first objective");
        selected=Index("Start State");Input(MenuInput::Right);
        Check(reloadExtraBranches[0].value.state==MissionStartState::Available&&!reloadExtraBranches[0].value.objectiveHash,
            "Extra Available state retained an incompatible objective");
        selected=Index("Add");Input(MenuInput::Accept);Check(reloadExtraBranches.size()==2,"Second extra row was not added");
        arrowsDoNothing("Remove Empty");
        selected=Index("Remove Empty");Input(MenuInput::Accept);Check(reloadExtraBranches.size()==1&&reloadExtraBranches[0].value.missionHash,
            "Remove Empty removed a populated branch or retained an empty one");
        for(unsigned i=0;i<65;++i){selected=Index("Add");Input(MenuInput::Accept);}
        Check(reloadExtraBranches.size()==63&&!Rows(GetSnapshot())[Index("Add")].enabled,"Extra editor exceeded native64 total branches");
        selected=Index("Remove Empty");Input(MenuInput::Accept);Check(reloadExtraBranches.size()==1,"Bulk empty removal changed populated branch identity");
        arrowsDoNothing("Remove Branch");
        selected=Index("Remove Branch");Input(MenuInput::Accept);Check(reloadExtraBranches.empty(),"Remove Branch failed");
        selected=Index("Add");Input(MenuInput::Accept);arrowsDoNothing("Remove All");selected=Index("Remove All");Input(MenuInput::Accept);
        Check(reloadExtraBranches.empty()&&NoReloadGameplay(),"Extra-branch editing executed gameplay or failed Remove All");
        Input(MenuInput::Back);Check(node=="debug.reload_map","Circle did not return to Reload from branch editor");
    });
    Run("mission_editor_gates_stale_native_order_and_replay_clears_manual_intent", [] {
        CatalogReloadSnapshot();selected=Index("Mission Sort Order");Input(MenuInput::Accept);
        const auto stale=Rows(GetSnapshot())[Index("Mission")];const auto previous=reloadReplaySelection;
        fixture::snapshot.reloadSelection.token.world.contextSerial++;
        Check(!Rows(GetSnapshot())[Index("Mission")].enabled,"Native mission ordering accepted metadata from a different world");
        selected=Index("Mission");Input(MenuInput::Right);Check(SameReplaySelection(previous,reloadReplaySelection),"Missing native order guessed a mission");
        fixture::snapshot.reload.token.contextSerial++;Activate(stale,1);
        Check(SameReplaySelection(previous,reloadReplaySelection),"Stale world row changed mission editor state");
        fixture::snapshot.reload=ReadyReload();SetNode("debug.reload_map",false);SettleMissionMetadata();
        Open("Extra Mission Branches");selected=Index("Add");Input(MenuInput::Accept);Input(MenuInput::Back);
        Check(!reloadExtraBranches.empty()&&reloadMissionEdited,"Fixture did not arm an explicit manual branch edit");
        selected=Index("Replay Mode");Input(MenuInput::Right);
        Check(reloadReplayEnabled&&reloadExtraBranches.empty()&&!reloadMissionEdited&&reloadStartState==MissionStartState::Active,
            "Replay retained manual extra-branch or Start State intent");
        fixture::snapshot.replay=ReadyReplay(reloadReplaySelection);
        const auto replay=Rows(GetSnapshot())[Index("Reload")];Check(replay.enabled&&!replay.applyMissionSelection,"Replay acquired native mission-array overrides");
        Open("Extra Mission Branches");for(const auto& row:Rows(GetSnapshot()))if(row.kind!=Kind::Info) {
            Check(!row.enabled,"Replay enabled a branch editor mutation");Activate(row,0);
        }
        Check(NoReloadGameplay(),"Unavailable branch edits invoked gameplay");
    });
    Run("ngplus_displays_live_flag_without_implicit_save_or_reload_requests", [] {
        CatalogReloadSnapshot();
        selected = Index("NG+");
        auto row = Rows(GetSnapshot())[selected];
        Check(row.kind == Kind::ReloadNewGamePlus && row.enabled && !row.checked && row.value == "Off",
              "Ready current campaign must expose its live NG+ value");
        Check(HintForRow(row, GetSnapshot()) == "Changes the current save's NG+ flag immediately; does not start a new playthrough or increase the NG+ count",
              "NG+ must disclose its exact saved-flag behavior");
        Frame();
        fixture::snapshot.reloadOptions.newGamePlus = true;
        row = Rows(GetSnapshot())[selected];
        Check(row.checked && row.value == "On", "NG+ display reused local UI state instead of the live campaign snapshot");
        Frame();
        Check(NoReloadGameplay() && fixture::replaySelections.empty() && fixture::replayCopies.empty(),
              "Opening, rendering, or observing NG+ implicitly wrote the save or initiated a reload");
        for (MenuInput input : {MenuInput::Accept, MenuInput::Left, MenuInput::Right}) Input(input);
        Check(fixture::newGamePlusSubmitted.size() == 3 && !fixture::newGamePlusSubmitted[0].second &&
              !fixture::newGamePlusSubmitted[1].second && fixture::newGamePlusSubmitted[2].second,
              "NG+ inputs must submit explicit Boolean false/false/true values from the checked snapshot");
        Check(fixture::submitted.empty() && fixture::reloadSubmitted.empty() && fixture::replaySubmitted.empty() &&
              fixture::settingsSubmitted.empty() && fixture::actorReloadSubmitted.empty() && fixture::primarySubmitted.empty(),
              "NG+ request fell back to a float, generic setting, actor reset, or reload action");
    });
    Run("keep_loadout_local_cross_arrows_and_world_persistence", [] {
        reloadKeepCurrentLoadout=false;CatalogReloadSnapshot();selected=Index("Keep Current Loadout");
        auto row=Rows(GetSnapshot())[selected];
        Check(row.kind==Kind::ReloadKeepLoadout && row.enabled && !row.checked && row.value=="Off","Original Off policy is displayed as an editable toggle");
        Input(MenuInput::Accept);Check(reloadKeepCurrentLoadout,"Cross enables Keep Current Loadout");
        Input(MenuInput::Left);Check(!reloadKeepCurrentLoadout,"Left disables Keep Current Loadout");
        Input(MenuInput::Right);Check(reloadKeepCurrentLoadout,"Right enables Keep Current Loadout");
        Input(MenuInput::Right);Check(reloadKeepCurrentLoadout,"Right is idempotent");
        ++fixture::snapshot.reload.token.contextSerial;Rows(GetSnapshot());
        Check(reloadKeepCurrentLoadout,"Keep loadout preference survives a world change");
        selected=Index("Keep Current Loadout");Input(MenuInput::Accept);
        ++fixture::snapshot.reload.token.contextSerial;Rows(GetSnapshot());
        Check(!reloadKeepCurrentLoadout && NoReloadGameplay() && fixture::replaySelections.empty(),"Off preference persists and local toggles never dispatch save or gameplay writes");
    });
    Run("loadout_off_manual_and_full_capture_exact_combined_policy", [] {
        for (const auto* label : {"Reload","Full Reload"}) {
            Reset();reloadKeepCurrentLoadout=false;CatalogReloadSnapshot();
            fixture::snapshot.reloadOptions.defaultLoadoutApplied=true;
            selected=Index("Checkpoint");Input(MenuInput::Left);
            selected=Index("Reset Actor Save on Manual Reload");Input(MenuInput::Right);
            const auto row=Rows(GetSnapshot())[Index(label)];
            Check(row.enabled && !row.keepCurrentLoadout && row.resetActorSave,"Off loadout and actor reset can be captured together");
            Activate(row,-1);Activate(row,1);Check(NoReloadGameplay(),"Reload left/right cannot execute combined preparation");
            ++fixture::snapshot.reloadOptions.token.campaign;fixture::snapshot.reloadOptions.defaultLoadoutApplied=false;
            ++fixture::snapshot.actorSave.token.fingerprint;++fixture::snapshot.reload.token.contextSerial;
            Activate(row,0);
            Check(fixture::reloadSubmitted.size()==1 && fixture::actorReloadSubmitted.empty() && fixture::submitted.empty(),"Combined policy uses one typed queue only");
            const auto& request=fixture::reloadSubmitted.front();const auto& policy=request.policy;
            Check(request.policyQueue && request.action==row.action && request.key==0xfedcba99u &&
                SameReloadContext(request.captured.token,row.reload.token) && request.captured.checkpoints==row.reload.checkpoints,
                "Typed queue preserves captured world and unsigned high checkpoint key");
            const auto& expected=row.reloadOptions.token;const auto& actual=policy.campaign.token;
            Check(!policy.keepCurrentLoadout && policy.resetActorSave && policy.campaign.defaultLoadoutApplied &&
                SameReloadContext(actual.world,expected.world) && actual.block==expected.block && actual.container==expected.container &&
                actual.entry==expected.entry && actual.campaign==expected.campaign && actual.keys==expected.keys &&
                actual.values==expected.values && actual.mapFingerprint==expected.mapFingerprint &&
                policy.actors.token.fingerprint==row.actorSave.token.fingerprint && policy.actors.token.mapFingerprints==row.actorSave.token.mapFingerprints &&
                policy.actors.token.entries==row.actorSave.token.entries,"Policy retains every campaign ownership field and actor map fingerprint");
            Check(fixture::newGamePlusSubmitted.empty() && fixture::replaySubmitted.empty(),"Deferred preparation never queues a separate save write/replay");
        }
    });
    Run("loadout_off_rejects_missing_or_stale_campaign_keep_on_bypasses_only_save_gate", [] {
        for (bool replayMode : {false,true}) for (unsigned failure=0;failure<3;++failure) {
            Reset();reloadKeepCurrentLoadout=false;CatalogReloadSnapshot();
            if (replayMode) {selected=Index("Replay Mode");Input(MenuInput::Right);fixture::snapshot.replay=ReadyReplay(reloadReplaySelection);}
            if(failure==0)fixture::snapshot.reloadOptions.ready=false;
            if(failure==1)fixture::snapshot.reloadOptions.supported=false;
            if(failure==2)++fixture::snapshot.reloadOptions.token.world.contextSerial;
            for(const auto* label:{"Reload","Full Reload"}) {
                const auto row=Rows(GetSnapshot())[Index(label)];Check(!row.enabled && row.value=="Waiting for save","Off rejects unavailable or stale campaign in manual and replay modes");
                Activate(row,0);Activate(row,-1);Activate(row,1);
            }
            Check(NoReloadGameplay(),"Refused campaign gate queued a request");
            selected=Index("Keep Current Loadout");Input(MenuInput::Right);
            Check(Rows(GetSnapshot())[Index("Reload")].enabled && Rows(GetSnapshot())[Index("Full Reload")].enabled,"Keep On skips default-loadout preparation save gate");
            fixture::snapshot.engineActionsReady=false;
            Check(!Rows(GetSnapshot())[Index("Reload")].enabled && !Rows(GetSnapshot())[Index("Full Reload")].enabled,"Keep On cannot bypass dispatcher");
        }
    });
    Run("replay_policy_keeps_exact_target_campaign_and_keep_on_flag", [] {
        for(bool keep:{false,true}) {
            Reset();reloadKeepCurrentLoadout=keep;CatalogReloadSnapshot();selected=Index("Replay Mode");Input(MenuInput::Right);
            fixture::snapshot.replay=ReadyReplay(reloadReplaySelection);
            const auto row=Rows(GetSnapshot())[Index("Reload")];Check(row.enabled,"Validated replay policy is available");
            Activate(row,-1);Activate(row,1);Check(NoReloadGameplay(),"Replay left/right cannot prepare loadout");
            ++fixture::snapshot.reloadOptions.token.mapFingerprint;++fixture::snapshot.replay.target.checkpoint;
            Activate(row,0);
            Check(fixture::replaySubmitted.size()==1 && fixture::replayPolicies.size()==1 && !fixture::legacyReplayCalls && fixture::reloadSubmitted.empty(),"Replay uses one policy queue and no checkpoint fallback");
            const auto& target=fixture::replaySubmitted.front();const auto& policy=fixture::replayPolicies.front();
            Check(target.target.checkpoint==0xfedcba99u && target.target.nodeId==row.replay.target.nodeId &&
                SameReplaySelection(target.selection,row.replay.selection) && policy.keepCurrentLoadout==keep && !policy.resetActorSave &&
                policy.campaign.token.mapFingerprint==row.reloadOptions.token.mapFingerprint &&
                SameReloadContext(policy.campaign.token.world,row.reloadOptions.token.world),"Replay target and preparation policy preserve captured 32/64-bit identities");
        }
    });
    Run("lobby_uses_owned_action_snapshot_and_independent_admission", [] {
        CatalogReloadSnapshot();reloadKeepCurrentLoadout=false;fixture::snapshot.reloadOptions={};fixture::snapshot.reload.frontendReady=false;
        selected=Index("Exit to Lobby");const auto row=Rows(GetSnapshot())[selected];
        Check(row.enabled && row.action==Action::ExitLobby && row.kind==Kind::Action,"Lobby depends on its own unload capability, not frontend or campaign reset");
        Input(MenuInput::Left);Input(MenuInput::Right);Check(NoReloadGameplay(),"Lobby left/right cannot unload");
        ++fixture::snapshot.reload.token.contextSerial;Activate(row,0);
        Check(fixture::reloadSubmitted.size()==1 && fixture::reloadSubmitted[0].action==Action::ExitLobby &&
            !fixture::reloadSubmitted[0].policyQueue && SameReloadContext(fixture::reloadSubmitted[0].captured.token,row.reload.token),"Lobby carries original captured world through its typed action");
        fixture::reloadSubmitted.clear();fixture::snapshot.reload.lobbyReady=false;
        Check(!Rows(GetSnapshot())[Index("Exit to Lobby")].enabled,"Lobby rejects missing unload capability");
        selected=Index("Exit to Lobby");Input(MenuInput::Accept);Check(NoReloadGameplay(),"Disabled Lobby cannot queue unload");
        fixture::snapshot.reload.lobbyReady=true;fixture::snapshot.engineActionsReady=false;
        Check(!Rows(GetSnapshot())[Index("Exit to Lobby")].enabled,"Lobby retains engine callback gate");
    });
    Run("close_event_is_idempotent_and_discards_prior_queued_navigation", [] {
        uiReady=true;visible=true;QueueMenuInput(MenuInput::Toggle);QueueMenuInput(MenuInput::Accept);QueueMenuInput(MenuInput::Close);
        Check(inputQueue.size()==1 && inputQueue.front().first==MenuInput::Close,"Close replaces stale queued inputs");
        PumpMenuInput();Check(!MenuVisible(),"Native transition close hides open menu");
        QueueMenuInput(MenuInput::Close);PumpMenuInput();Input(MenuInput::Close);
        Check(!MenuVisible() && NoReloadGameplay(),"Repeated Close cannot reopen a hidden menu or dispatch gameplay");
    });
    Run("camera_shake_reads_live_state_and_submits_explicit_boolean_directions", [] {
        fixture::snapshot.engineActionsReady=true;fixture::snapshot.cameraDebug=ReadyCamera();
        visible=true;SetNode("debug.camera",false);selected=Index("Enable Camera Shake");
        auto row=Rows(GetSnapshot())[selected];
        Check(row.kind==Kind::CameraShake&&row.enabled&&row.checked&&row.value=="On","Ready live camera shake was not exposed");
        Frame();Check(NoReloadGameplay(),"Reading/rendering camera state submitted a camera change");
        Input(MenuInput::Left);Input(MenuInput::Right);Input(MenuInput::Accept);
        Check(fixture::cameraShakeSubmitted.size()==3&&!fixture::cameraShakeSubmitted[0].second&&
            fixture::cameraShakeSubmitted[1].second&&!fixture::cameraShakeSubmitted[2].second,
            "Camera Left/Right/Cross did not submit explicit false/true/false values");
        fixture::cameraShakeSubmitted.clear();fixture::snapshot.cameraDebug.shakeEnabled=false;
        row=Rows(GetSnapshot())[selected];Check(!row.checked&&row.value=="Off","Camera state reused a stale local toggle");
        Input(MenuInput::Accept);Check(fixture::cameraShakeSubmitted.size()==1&&fixture::cameraShakeSubmitted.back().second,
            "Cross did not enable an unchecked live camera snapshot");
        fixture::cameraShakeSubmitted.clear();Check(NoReloadGameplay(),"Camera UI also routed into a generic or reload action");
    });
    Run("camera_shake_preserves_captured_world_camera_mask_and_allocation_identity", [] {
        fixture::snapshot.engineActionsReady=true;fixture::snapshot.cameraDebug=ReadyCamera();
        visible=true;SetNode("debug.camera",false);const auto captured=Rows(GetSnapshot())[Index("Enable Camera Shake")];
        auto& current=fixture::snapshot.cameraDebug;
        current.shakeEnabled=false;current.token.world.contextSerial++;current.token.cameraSystem+=0x100;
        current.token.flagsAddress+=0x100;current.token.allocations[6]+=0x100;current.token.viewCount++;current.token.flags=0;
        Activate(captured,0);
        Check(fixture::cameraShakeSubmitted.size()==1&&!fixture::cameraShakeSubmitted.front().second,
            "Camera action did not retain the captured checked state");
        const auto& actual=fixture::cameraShakeSubmitted.front().first;const auto& expected=captured.cameraDebug;
        Check(actual.supported&&actual.ready&&actual.shakeEnabled&&SameReloadContext(actual.token.world,expected.token.world)&&
            actual.token.cameraSystem==expected.token.cameraSystem&&actual.token.flagsAddress==expected.token.flagsAddress&&
            actual.token.allocations==expected.token.allocations&&actual.token.viewCount==2&&actual.token.flags==0x800000a0u,
            "Camera request refreshed or truncated the captured world, object, allocation or mask identity");
        fixture::cameraShakeSubmitted.clear();Check(NoReloadGameplay(),"Captured camera action gained a fallback operation");
    });
    Run("camera_shake_refuses_missing_support_dispatcher_or_camera", [] {
        for(int missing=0;missing<4;++missing) {
            fixture::snapshot.supported=fixture::snapshot.engineActionsReady=true;fixture::snapshot.cameraDebug=ReadyCamera();
            fixture::snapshot.cameraDebug.status=L"Current camera unavailable";
            if(missing==0)fixture::snapshot.supported=false;
            if(missing==1)fixture::snapshot.engineActionsReady=false;
            if(missing==2)fixture::snapshot.cameraDebug.supported=false;
            if(missing==3)fixture::snapshot.cameraDebug.ready=false;
            visible=true;SetNode("debug.camera",false);selected=Index("Enable Camera Shake");const auto row=Rows(GetSnapshot())[selected];
            Check(row.kind==Kind::CameraShake&&!row.enabled,"Camera shake accepted a missing capability");
            Check(row.value==(missing==3?"Waiting for camera":"On"),"Camera readiness display lost readable live state");
            Check(HintForRow(row,GetSnapshot())==(missing==1?"Waiting for the verified engine dispatcher":"Current camera unavailable"),
                "Camera unavailable hint lost its actual reason");
            Input(MenuInput::Left);Input(MenuInput::Right);Input(MenuInput::Accept);
            Check(NoReloadGameplay(),"Unavailable camera dispatched a camera or fallback change");
        }
    });
    Run("capture_format_uses_original_order_and_queues_only_explicit_changes", [] {
        visible=true;SetNode("debug.rendering.screencapture",false);
        const CaptureImageFormat formats[]{CaptureImageFormat::Bmp,CaptureImageFormat::Dds,CaptureImageFormat::Jpg,CaptureImageFormat::Png};
        const char* labels[]{"bmp","dds","jpg","png"};
        for(unsigned i=0;i<4;++i) {
            Check(static_cast<unsigned>(formats[i])==i,"Original capture format ordinals changed");
            fixture::capture.format=formats[i];const auto row=Rows(GetSnapshot())[Index("Capture Image Format")];
            Check(row.kind==Kind::CaptureFormat&&row.enabled&&row.value==labels[i],"Capture format did not use authoritative format state");
        }
        Frame();Check(fixture::captureFormatDirections.empty()&&fixture::captureRequests==0&&NoReloadGameplay(),"Opening/rendering capture page started a capture or changed format");
        selected=Index("Capture Image Format");Input(MenuInput::Left);Input(MenuInput::Right);Input(MenuInput::Accept);
        Check(fixture::captureFormatDirections==std::vector<int>{-1,1,0}&&fixture::captureRequests==0&&NoReloadGameplay(),
            "Capture format changes did not preserve requested direction or dispatched gameplay");
        fixture::captureFormatDirections.clear();fixture::capture.busy=true;
        auto busy=Rows(GetSnapshot())[Index("Capture Image Format")];
        Check(!busy.enabled&&HintForRow(busy,GetSnapshot()).find("finish")!=std::string::npos,"Busy format mutation remains enabled");
        selected=Index("Capture Image Format");Input(MenuInput::Accept);Input(MenuInput::Left);Input(MenuInput::Right);
        Check(fixture::captureFormatDirections.empty(),"Busy capture accepted a format mutation");
    });
    Run("capture_request_closes_only_after_backend_acceptance_and_clears_queued_navigation", [] {
        visible=true;uiReady=true;fixture::capture.ready=true;fixture::capture.status=L"Display ready";
        SetNode("debug.rendering.screencapture",false);selected=Index("Capture With HUD");
        const auto row=Rows(GetSnapshot())[selected];
        Check(row.enabled&&row.kind==Kind::CaptureDisplay&&HintForRow(row,GetSnapshot()).find("Display ready")!=std::string::npos,
            "Capture does not expose independent display readiness/status");
        Input(MenuInput::Left);Input(MenuInput::Right);
        Check(fixture::captureRequests==0&&inputQueue.empty(),"Arrows captured an image or closed the menu");
        fixture::captureRequestAccepted=false;Input(MenuInput::Accept);
        Check(fixture::captureRequests==1&&inputQueue.empty()&&MenuVisible(),"Rejected capture closed the menu or retried automatically");
        fixture::captureRequestAccepted=true;QueueMenuInput(MenuInput::Toggle);QueueMenuInput(MenuInput::Accept);
        Input(MenuInput::Accept);
        Check(fixture::captureRequests==2&&inputQueue.size()==1&&inputQueue.front().first==MenuInput::Close&&MenuVisible(),
            "Accepted capture did not replace stale navigation with a queued Close");
        PumpMenuInput();
        Check(!MenuVisible()&&fixture::captureRequests==2&&NoReloadGameplay(),"Capture close reopened UI, recaptured, or dispatched gameplay");
    });
    Run("capture_display_missing_or_busy_refuses_requests_and_other_modes_stay_unbound", [] {
        visible=true;SetNode("debug.rendering.screencapture",false);
        selected=Index("Capture With HUD");auto row=Rows(GetSnapshot())[selected];
        Check(!row.enabled&&row.value=="Waiting for display","Missing swapchain capture was enabled");
        Input(MenuInput::Accept);Check(fixture::captureRequests==0&&MenuVisible(),"Missing display queued capture/close");
        fixture::capture.ready=true;fixture::capture.busy=true;row=Rows(GetSnapshot())[selected];
        Check(!row.enabled&&row.value=="Capturing","In-flight capture was enabled again");
        Input(MenuInput::Accept);Check(fixture::captureRequests==0&&MenuVisible(),"Busy display accepted capture");
        fixture::capture.busy=false;
        for(const char* label:{"Capture Without HUD","Capture In HDR","Trigger Continuous Capture","Multi-pass Capture","Capture Depth"}) {
            const auto unsupported=Rows(GetSnapshot())[Index(label)];
            Check(!unsupported.enabled,"Unimplemented capture mode was advertised as enabled");Activate(unsupported,0);
        }
        Check(fixture::captureRequests==0&&fixture::captureFormatDirections.empty()&&NoReloadGameplay(),"Unsupported mode routed to a different capture operation");
    });
    Run("ngplus_preserves_captured_campaign_and_world_identity", [] {
        CatalogReloadSnapshot();
        const auto captured = Rows(GetSnapshot())[Index("NG+")];
        fixture::snapshot.reloadOptions.newGamePlus = true;
        fixture::snapshot.reloadOptions.token.world.contextSerial++;
        fixture::snapshot.reloadOptions.token.campaign += 0x100;
        fixture::snapshot.reloadOptions.token.mapFingerprint++;
        Activate(captured, 0);
        Check(fixture::newGamePlusSubmitted.size() == 1 && fixture::newGamePlusSubmitted.front().second,
              "Captured unchecked NG+ row did not retain an explicit enable request");
        const auto& actual = fixture::newGamePlusSubmitted.front().first;
        const auto& expected = captured.reloadOptions;
        Check(!actual.newGamePlus && SameReloadContext(actual.token.world, expected.token.world) &&
              actual.token.block == expected.token.block && actual.token.container == expected.token.container &&
              actual.token.entry == expected.token.entry && actual.token.campaign == expected.token.campaign &&
              actual.token.keys == expected.token.keys && actual.token.values == expected.token.values &&
              actual.token.mapFingerprint == 0xfedcba9876543210ull,
              "NG+ request replaced its captured campaign identity or truncated the fingerprint");
        Check(fixture::reloadSubmitted.empty() && fixture::replaySubmitted.empty() && fixture::submitted.empty(),
              "Captured NG+ request acquired an implicit reload fallback");
    });
    Run("ngplus_requires_supported_world_dispatcher_and_campaign_state", [] {
        for (int missing = 0; missing < 5; ++missing) {
            CatalogReloadSnapshot();
            fixture::snapshot.reloadOptions.status = L"Current campaign unavailable";
            if (missing == 0) fixture::snapshot.supported = false;
            if (missing == 1) fixture::snapshot.reload.ready = false;
            if (missing == 2) fixture::snapshot.engineActionsReady = false;
            if (missing == 3) fixture::snapshot.reloadOptions.supported = false;
            if (missing == 4) fixture::snapshot.reloadOptions.ready = false;
            selected = Index("NG+"); const auto row = Rows(GetSnapshot())[selected];
            Check(row.kind == Kind::ReloadNewGamePlus && !row.enabled, "NG+ accepted an unavailable capability");
            Check(row.value == (missing == 4 ? "Waiting for save" : "Off"), "NG+ hid readable campaign state on unrelated capability loss");
            if (missing == 2) Check(HintForRow(row, GetSnapshot()) == "Waiting for the verified engine dispatcher", "NG+ lost dispatcher prerequisite hint");
            if (missing == 4) Check(HintForRow(row, GetSnapshot()) == "Current campaign unavailable", "NG+ lost its campaign availability reason");
            for (MenuInput input : {MenuInput::Accept, MenuInput::Left, MenuInput::Right}) Input(input);
            Check(NoReloadGameplay(), "Disabled NG+ dispatched a save or reload request");
        }
    });
    Run("replay_selection_uses_catalog_scopes_and_exact_integer_ids", [] {
        CatalogReloadSnapshot();
        Check(reloadReplaySelection.graphId==0xbaca0cde336b5792ull && reloadReplaySelection.missionHash==0x85375597u &&
              reloadReplaySelection.objectiveHash==0xbd415500u,"Initial replay selection did not follow the actual scoped primary");
        selected=Index("Replay Mode");Input(MenuInput::Right);
        Check(reloadReplayEnabled && fixture::replaySelections.size()==1 &&
              SameReplaySelection(fixture::replaySelections.back(),reloadReplaySelection),"Replay toggle lost its typed pending selection");
        selected=Index("Graph Filter");
        const auto& graphs=ReloadMissionCatalog();std::set<std::uint64_t> visited;
        for (std::size_t i=0;i<graphs.size();++i) {
            Input(MenuInput::Right);const auto& selection=reloadReplaySelection;
            const auto* graph=FindReloadMissionGraph(selection.graphId);
            Check(graph && selection.missionHash==graph->missions.front().nameCrc && selection.objectiveHash==0,
                  "Graph navigation mixed a prior graph's mission/objective into the next graph");
            Check(SameReplaySelection(fixture::replaySelections.back(),selection),"Graph ID traveled through a rounded/obsolete payload");
            visited.insert(selection.graphId);
        }
        Check(visited.size()==5 && visited.count(0xbaca0cde336b5792ull),"Graph Filter must visit all five exact retail graph IDs");
        Check(NoReloadGameplay(),"Browsing mission graph metadata executed a native action");
    });
    Run("replay_objective_default_and_native_sorts_change_navigation_only", [] {
        CatalogReloadSnapshot();selected=Index("Replay Mode");Input(MenuInput::Right);
        reloadReplaySelection.objectiveHash=0;
        const auto* mission=FindReloadMission(reloadReplaySelection.graphId,reloadReplaySelection.missionHash);
        Check(mission && mission->objectives.size()>1,"Fixture requires multiple real objectives");
        selected=Index("Objective");Input(MenuInput::Right);
        Check(reloadReplaySelection.objectiveHash==mission->objectives.front().nameCrc,"Alphabetical objective navigation must start with exact first objective");
        Input(MenuInput::Left);Check(reloadReplaySelection.objectiveHash==0,"Objective left must return to native mission-default sentinel");
        Input(MenuInput::Left);Check(reloadReplaySelection.objectiveHash==mission->objectives.back().nameCrc,"Objective left at default must wrap to last exact objective");
        Input(MenuInput::Right);Check(reloadReplaySelection.objectiveHash==0,"Objective right at last must wrap to mission-default sentinel");
        RefreshMissionMetadata();const auto nativeObjective=fixture::snapshot.reloadSelection.branches[0].objectiveOrder.front();
        selected=Index("Objective Sort Order");Input(MenuInput::Accept);
        Check(reloadObjectiveDescending && Rows(GetSnapshot())[selected].value=="Mission Order","Objective sort must expose native DFT order");
        selected=Index("Objective");Input(MenuInput::Right);
        Check(reloadReplaySelection.objectiveHash==nativeObjective,"Objective navigation ignored the native DFT snapshot");
        const auto* graph=FindReloadMissionGraph(reloadReplaySelection.graphId);
        reloadReplaySelection.missionHash=graph->missions.front().nameCrc;reloadReplaySelection.objectiveHash=0;
        selected=Index("Mission");Input(MenuInput::Right);
        Check(reloadReplaySelection.missionHash==graph->missions[1].nameCrc && !reloadReplaySelection.objectiveHash,"Mission selection must reset objective to default in its new scope");
        selected=Index("Mission Sort Order");Input(MenuInput::Right);
        RefreshMissionMetadata();
        Check(reloadMissionDescending && Rows(GetSnapshot())[selected].value=="Mission Order","Mission sort did not expose native order");
        const auto nativeNext=fixture::snapshot.reloadSelection.missionOrder[1];
        selected=Index("Mission");Input(MenuInput::Right);
        Check(reloadReplaySelection.missionHash==nativeNext,"Mission selector guessed reverse alphabetical instead of native DFT");
        Check(NoReloadGameplay() && !fixture::replaySelections.empty(),"Local sort/selection must publish metadata only");
    });
    Run("replay_reload_uses_only_dedicated_immutable_target_request", [] {
        CatalogReloadSnapshot();selected=Index("Replay Mode");Input(MenuInput::Right);
        auto row=Rows(GetSnapshot())[Index("Reload")];
        Check(!row.enabled && row.action==Action::ReplayMission,"Unvalidated pending target must not execute a normal Reload fallback");
        Activate(row,0);Check(fixture::replaySubmitted.empty() && NoReloadGameplay(),"Unready Replay target queued gameplay");
        fixture::snapshot.replay=ReadyReplay(reloadReplaySelection);
        row=Rows(GetSnapshot())[Index("Reload")];
        Check(row.enabled && row.action==Action::ReplayMission,"Validated selection did not bind native replay");
        Activate(row,-1);Activate(row,1);Check(fixture::replaySubmitted.empty(),"Left/right executed native replay");
        const auto captured=row.replay;
        ++fixture::snapshot.replay.target.graphHandle;++fixture::snapshot.replay.target.checkpoint;
        Activate(row,0);
        Check(fixture::replaySubmitted.size()==1 && fixture::reloadSubmitted.empty() && fixture::actorReloadSubmitted.empty() && fixture::submitted.empty(),
              "Replay must use exactly one dedicated queue and never issue a second checkpoint reload");
        const auto& request=fixture::replaySubmitted.front();
        Check(SameReplaySelection(request.selection,captured.selection) && request.target.nodeId==0xfb73d3674181be28ull &&
              request.target.graphId==0xbaca0cde336b5792ull && request.target.checkpoint==0xfedcba99u &&
              request.target.graphHandle==captured.target.graphHandle,"Replay payload did not preserve exact captured64/32-bit identities");
        fixture::snapshot.replay.selection.objectiveHash^=1u;
        Check(!Rows(GetSnapshot())[Index("Reload")].enabled,"Snapshot for another target enabled the currently selected replay");
        fixture::snapshot.replay=ReadyReplay(reloadReplaySelection);fixture::snapshot.engineActionsReady=false;
        Check(!Rows(GetSnapshot())[Index("Reload")].enabled,"Replay bypassed dispatcher capability");
    });
    Run("replay_full_reload_uses_current_world_without_replay_target", [] {
        CatalogReloadSnapshot();selected=Index("Checkpoint");Input(MenuInput::Left);
        Check(reloadCheckpointKey==0xfedcba99u,"Fixture must first select a noncurrent checkpoint");
        selected=Index("Reset Actor Save on Manual Reload");Input(MenuInput::Right);
        selected=Index("Replay Mode");Input(MenuInput::Right);
        selected=Index("Mission");Input(MenuInput::Right);
        const auto selection=reloadReplaySelection;
        const auto publications=fixture::replaySelections.size();
        fixture::snapshot.actorSave.ready=false;
        fixture::snapshot.replay.selection=selection;
        fixture::snapshot.replay.status=L"Selected mission is not ready for native replay";
        const auto row=Rows(GetSnapshot())[Index("Full Reload")];
        Check(row.enabled && row.action==Action::FullReload && row.replayMode && !row.checkpointKey && !row.resetActorSave,
              "Replay Full Reload must use the current world without a ready replay target or actor-reset policy");
        Check(HintForRow(row,GetSnapshot())==
              "Fully unload and reload the current world at its current checkpoint; Reload uses the replay mission",
              "Replay Full Reload must distinguish current-world reload from the selected replay mission");
        selected=Index("Reload");const auto replay=Rows(GetSnapshot())[selected];
        Check(!replay.enabled && replay.action==Action::ReplayMission,
              "Enabling Full Reload must not introduce a fallback for an unavailable replay Reload");
        Input(MenuInput::Accept);Input(MenuInput::Left);Input(MenuInput::Right);
        selected=Index("Full Reload");Input(MenuInput::Left);Input(MenuInput::Right);
        Check(NoReloadGameplay(),"Unready replay or Full Reload left/right executed gameplay");
        ++fixture::snapshot.reload.token.currentCheckpoint;
        Activate(row,0);
        Check(fixture::reloadSubmitted.size()==1 && fixture::replaySubmitted.empty() &&
              fixture::actorReloadSubmitted.empty() && fixture::submitted.empty() && fixture::primarySubmitted.empty(),
              "Replay Full Reload must submit exactly one typed world reload and no mission replay or actor reset");
        const auto& request=fixture::reloadSubmitted.front();
        Check(request.action==Action::FullReload && request.key==0 &&
              SameReloadContext(request.captured.token,row.reload.token) &&
              request.captured.token.currentCheckpoint==0x87654321u &&
              request.captured.checkpoints==row.reload.checkpoints,
              "Replay Full Reload lost the captured world token or current-checkpoint sentinel");
        Check(reloadReplayEnabled && SameReplaySelection(reloadReplaySelection,selection) &&
              fixture::replaySelections.size()==publications && fixture::replayCopies.empty(),
              "Full Reload changed Replay Mode, its selected mission, or published a replacement replay selection");
    });
    Run("replay_full_reload_keeps_world_and_dispatcher_gates_and_reasons", [] {
        CatalogReloadSnapshot();selected=Index("Replay Mode");Input(MenuInput::Right);
        fixture::snapshot.replay=ReadyReplay(reloadReplaySelection);
        const auto selection=reloadReplaySelection;
        const auto expectDisabled=[&](const char* reason) {
            selected=Index("Full Reload");const auto row=Rows(GetSnapshot())[selected];
            Check(!row.enabled && row.action==Action::FullReload,"Replay Full Reload bypassed a required capability");
            Check(HintForRow(row,GetSnapshot())==reason,"Replay Full Reload hid its unavailable capability reason");
            Input(MenuInput::Accept);Input(MenuInput::Left);Input(MenuInput::Right);
            Check(NoReloadGameplay(),"Unavailable Replay Full Reload queued a gameplay request");
        };
        fixture::snapshot.engineActionsReady=false;
        expectDisabled("Waiting for the verified engine dispatcher");
        fixture::snapshot.reload.ready=false;fixture::snapshot.reload.status=L"Waiting for current loaded world";
        expectDisabled("Waiting for current loaded world");
        fixture::snapshot.engineActionsReady=true;
        expectDisabled("Waiting for current loaded world");
        fixture::snapshot.reload.ready=true;fixture::snapshot.supported=false;
        selected=Index("Full Reload");Check(!Rows(GetSnapshot())[selected].enabled,"Unsupported build enabled Replay Full Reload");
        Input(MenuInput::Accept);Check(NoReloadGameplay(),"Unsupported build queued Replay Full Reload");
        fixture::snapshot.supported=true;fixture::snapshot.replay.ready=false;
        Check(Rows(GetSnapshot())[Index("Full Reload")].enabled && !Rows(GetSnapshot())[Index("Reload")].enabled,
              "World readiness recovery incorrectly required replay readiness or enabled its Reload fallback");
        Check(reloadReplayEnabled && SameReplaySelection(reloadReplaySelection,selection),
              "Temporary capability loss changed Replay Mode or its selected mission");
    });
    Run("manual_actor_reset_is_deferred_and_captures_exact_token", [] {
        CatalogReloadSnapshot();selected=Index("Checkpoint");Input(MenuInput::Left);
        const auto checkpoint=reloadCheckpointKey;
        selected=Index("Reset Actor Save on Manual Reload");
        auto row=Rows(GetSnapshot())[selected];
        Check(row.enabled && !row.checked && HintForRow(row,GetSnapshot()).find("Reset can remain if Reload fails")!=std::string::npos,
              "Actor-reset row must disclose its before-only persistent consequence");
        Input(MenuInput::Right);
        Check(reloadResetActorSave && NoReloadGameplay(),"Enabling deferred actor policy invoked a reset/game action");
        const auto captured=Rows(GetSnapshot())[Index("Reload")];
        fixture::snapshot.actorSave.token.fingerprint=0x0100000000000001ull;
        Activate(captured,0);
        Check(fixture::reloadSubmitted.size()==1 && fixture::actorReloadSubmitted.empty() && fixture::submitted.empty(),"Manual reset must use only the dedicated typed Reload policy queue");
        const auto& request=fixture::reloadSubmitted.front();
        Check(request.policyQueue && request.policy.resetActorSave && request.action==Action::ReloadCheckpoint && request.key==checkpoint && request.policy.actors.token.fingerprint==0xfedcba9876543210ull &&
              request.policy.actors.token.mapFingerprints==captured.actorSave.token.mapFingerprints && SameReloadContext(request.captured.token,captured.reload.token),
              "Deferred actor policy substituted current state or rounded immutable identifiers");
        fixture::snapshot.actorSave.ready=false;fixture::snapshot.actorSave.status=L"Actor-save map changed";
        Check(!Rows(GetSnapshot())[Index("Reload")].enabled && !Rows(GetSnapshot())[Index("Full Reload")].enabled,
              "Unavailable reset state enabled a manual Reload with pending actor policy");
        selected=Index("Reset Actor Save on Manual Reload");Input(MenuInput::Left);
        Check(!reloadResetActorSave && Rows(GetSnapshot())[Index("Reload")].enabled,"User must be able to cancel an unavailable pending actor reset");
    });
    Run("replay_disables_and_clears_manual_policy_combinations", [] {
        CatalogReloadSnapshot();selected=Index("Checkpoint");Input(MenuInput::Left);
        selected=Index("Reset Actor Save on Manual Reload");Input(MenuInput::Right);
        Check(reloadResetActorSave && reloadCheckpointKey!=0,"Fixture must arm manual policies first");
        selected=Index("Replay Mode");Input(MenuInput::Right);
        Check(reloadReplayEnabled && !reloadResetActorSave && !reloadCheckpointKey,"Replay must discard incompatible manual checkpoint/reset intent");
        fixture::snapshot.replay=ReadyReplay(reloadReplaySelection);
        for (const char* label : {"Checkpoint","Reset Actor Save on Manual Reload","Filter Checkpoint Selection"}) {
            const auto row=Rows(GetSnapshot())[Index(label)];Check(!row.enabled,"Replay enabled an incompatible manual policy");
            Activate(row,0);Activate(row,1);Activate(row,-1);
        }
        Check(NoReloadGameplay(),"Disabled policy combinations queued a native request");
        selected=Index("Replay Mode");Input(MenuInput::Left);
        Check(!reloadReplayEnabled && !fixture::replaySelections.back().graphId && !fixture::replaySelections.back().missionHash &&
              !fixture::replaySelections.back().objectiveHash,"Disabling Replay did not clear the backend pending target");
        selected=Index("Filter Checkpoint Selection");Input(MenuInput::Right);
        Check(reloadFilterCheckpoints,"Fixture must first enable the manual checkpoint filter");
        selected=Index("Replay Mode");Input(MenuInput::Right);
        Check(reloadReplayEnabled && !reloadFilterCheckpoints && !reloadCheckpointKey && !reloadResetActorSave,
              "Entering Replay retained a manual checkpoint-filter policy");
        Check(NoReloadGameplay(),"Changing Replay/manual policies executed gameplay");
    });
    Run("checkpoint_filter_matches_both_native_crc_and_exact_owned_name", [] {
        CatalogReloadSnapshot();const auto* mission=FindReloadMission(0xbaca0cde336b5792ull,0x85375597u);
        const auto& available=mission->availableCheckpoint;const auto& objective=mission->objectives.front().checkpoint;
        const auto wide=[](const char* text){return std::wstring(text,text+std::strlen(text));};
        fixture::snapshot.reload.checkpoints=std::make_shared<const std::vector<ReloadCheckpoint>>(std::vector<ReloadCheckpoint>{
            {available.nameCrc,0,wide(available.name)},{objective.nameCrc,1,wide(objective.name)},
            {objective.nameCrc,2,L"Different name with matching CRC"},{objective.nameCrc^1u,3,wide(objective.name)},
            {0xfedcba99u,4,L"Unrelated checkpoint"}});
        selected=Index("Checkpoint");Input(MenuInput::Left);Check(reloadCheckpointKey==0xfedcba99u,"Unfiltered selector lost unrelated live checkpoint");
        selected=Index("Filter Checkpoint Selection");Input(MenuInput::Right);
        Check(reloadFilterCheckpoints && !reloadCheckpointKey,"Filter must reset a possibly excluded checkpoint selection");
        const auto choices=ReloadChoices(GetSnapshot());
        Check(choices && choices->size()==2 && (*choices)[0].recordIndex==0 && (*choices)[1].recordIndex==1,
              "Checkpoint filter must use exact owned name and native CRC, preserving live native records");
        selected=Index("Checkpoint");Input(MenuInput::Right);
        Check(reloadCheckpointKey==available.nameCrc,"Filtered selector submitted a guessed/native-unowned checkpoint");
        Check(NoReloadGameplay(),"Changing a local checkpoint filter executed gameplay");
        selected=Index("Filter Checkpoint Selection");Input(MenuInput::Left);
        Check(!reloadFilterCheckpoints && ReloadChoices(GetSnapshot())->size()==5 && !reloadCheckpointKey,"Filter off must restore all actual live records without retaining a stale choice");
    });
    Run("new_reload_world_clears_pending_policies_and_rejects_stale_selectors", [] {
        CatalogReloadSnapshot();selected=Index("Reset Actor Save on Manual Reload");Input(MenuInput::Right);
        selected=Index("Filter Checkpoint Selection");Input(MenuInput::Right);
        selected=Index("Replay Mode");Input(MenuInput::Right);
        const auto staleMission=Rows(GetSnapshot())[Index("Mission")];
        const auto oldSelection=reloadReplaySelection;
        ++fixture::snapshot.reload.token.contextSerial;fixture::snapshot.reload.token.levelAsset^=0x1000u;
        fixture::snapshot.primary.world=fixture::snapshot.reload.token;fixture::snapshot.actorSave.token.world=fixture::snapshot.reload.token;
        Activate(staleMission,1);
        Check(SameReplaySelection(oldSelection,reloadReplaySelection),"Stale selector changed a new world's pending mission");
        Rows(GetSnapshot());
        Check(!reloadReplayEnabled && !reloadResetActorSave && !reloadFilterCheckpoints && !reloadCheckpointKey &&
              SameReloadContext(reloadSelectionContext,fixture::snapshot.reload.token),"New world retained a prior gameplay policy/checkpoint intent");
        Check(reloadReplaySelection.graphId==fixture::snapshot.primary.token.missionGraphAssetId &&
              reloadReplaySelection.missionHash==fixture::snapshot.primary.token.missionHash,"New world must seed metadata only from its actual current primary");
        Check(NoReloadGameplay(),"Context reset executed a native action");
    });
    Run("replay_clipboard_uses_typed_selection_without_dispatcher_or_native_call", [] {
        CatalogReloadSnapshot();selected=Index("Replay Mode");Input(MenuInput::Right);
        fixture::snapshot.engineActionsReady=false;
        const auto row=Rows(GetSnapshot())[Index("Copy Reload Data to Clipboard")];
        Check(row.enabled && row.replayMode,"Read-only Replay copy must not need dispatcher capability");
        ++reloadReplaySelection.missionHash;
        Activate(row,0);
        Check(fixture::replayCopies.size()==1 && fixture::reloadSubmitted.empty() && NoReloadGameplay(),"Replay clipboard must use its dedicated data-only queue");
        Check(SameReloadContext(fixture::replayCopies.front().first.token,row.reload.token) &&
              SameReplaySelection(fixture::replayCopies.front().second,row.replaySelection) &&
              fixture::replayCopies.front().second.graphId==0xbaca0cde336b5792ull &&
              fixture::replayCopies.front().second.missionHash==0x85375597u,"Clipboard selection lost exact captured graph/mission IDs");
    });
    Run("four_verified_health_flags_route_exact_actions", [] {
        visible = true; SetNode("debug.health", false);
        const std::array<std::pair<const char*, Action>, 4> flags{{
            {"Heroes do not take damage", Action::HeroNoDamage}, {"Heroes do not die", Action::HeroNoDeath},
            {"Enemy bots do not die", Action::EnemyNoDeath}, {"Enemy bots don't take damage", Action::EnemyNoDamage}}};
        for (const auto& f : flags) {
            selected = Index(f.first); fixture::submitted.clear();
            const auto r = Rows(GetSnapshot())[selected];
            Check(r.kind == Kind::Toggle && r.enabled, "Verified flag must be enabled toggle");
            Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
            Check(fixture::submitted.size() == 3, "Health input must emit one command each");
            const float expected[3] = {1, 0, 1};
            for (size_t i = 0; i < 3; ++i) Check(fixture::submitted[i].first == f.second && fixture::submitted[i].second == expected[i], "Wrong health action/value routing");
        }
        fixture::snapshot.heroNoDamage = fixture::snapshot.heroNoDeath = fixture::snapshot.enemyNoDeath = fixture::snapshot.enemyNoDamage = true;
        for (const auto& f : flags) {
            selected = Index(f.first); fixture::submitted.clear(); Input(MenuInput::Accept);
            Check(fixture::submitted.size() == 1 && fixture::submitted[0].first == f.second && fixture::submitted[0].second == 0, "Cross must turn an already checked flag off");
        }
    });
    Run("health_flags_wait_for_verified_binding", [] {
        fixture::snapshot.healthFlagsReady = false; visible = true; SetNode("debug.health", false);
        const auto rows = Rows(GetSnapshot());
        for (size_t i = 0; i < rows.size(); ++i) if (HealthFlag(rows[i].action) && rows[i].kind == Kind::Toggle) {
            Check(!rows[i].enabled, "Unbound flag must be disabled"); selected = static_cast<int>(i);
            Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
        }
        Check(fixture::submitted.empty(), "Unbound health flag must not queue writes");
    });
    Run("retained_settings_show_live_values_and_queue_only_with_dispatcher", [] {
        DebugSettingValue boolean;
        boolean.available = true; boolean.id = "health.display";
        boolean.pageId = "debug.health"; boolean.label = "Draw health values";
        boolean.type = DebugSettingType::Boolean; boolean.number = 1;
        fixture::snapshot.settings = {boolean};
        visible = true; SetNode("debug.health", false);
        selected = Index(boolean.label);
        auto row = Rows(GetSnapshot())[selected];
        Check(row.value == "On" && !row.enabled, "Live value must remain visible while writes await engine context");
        Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
        Check(fixture::settingsSubmitted.empty(), "Unavailable context must not queue setting writes");
        fixture::snapshot.engineActionsReady = true;
        row = Rows(GetSnapshot())[selected];
        Check(row.kind == Kind::Setting && row.enabled, "Verified retained setting must become editable");
        Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
        Check(fixture::settingsSubmitted == std::vector<std::pair<std::string,int>>{
            {boolean.id,0},{boolean.id,-1},{boolean.id,1}}, "Setting must preserve exact id and requested direction");
        DebugSettingValue scalar = boolean;
        scalar.pageId = "debug.loading"; scalar.label = "ZoneInstantiationBudgetMin";
        scalar.id = "loading.zone.min"; scalar.type = DebugSettingType::Float32; scalar.number = .002;
        fixture::snapshot.settings = {scalar}; SetNode("debug.loading", false);
        row = Rows(GetSnapshot())[Index(scalar.label)];
        Check(row.value == "0.0020" && row.settingId == scalar.id, "Budget must show actual scalar with precision");
    });
    Run("eleven_added_runtime_controls_show_snapshot_values_and_return_to_debug", [] {
        AddNativeSettingsSnapshot(); visible = true;
        for (size_t index = 0; index < addedSettings.size(); ++index) {
            const auto& expected = addedSettings[index];
            history.clear(); SetNode("debug", false); Open(expected.root);
            if (expected.child) {
                Check(node == "debug.traffic", "Traffic root must first open its observed parent page");
                Open(expected.child);
            }
            Check(node == expected.page, "Debug root opened the wrong added native page");
            selected = Index(expected.label);
            auto row = Rows(GetSnapshot())[selected];
            Check(row.kind == Kind::Setting && row.settingId == expected.id && row.value == (std::string(expected.id) == "SpeedRun.BubbleSkipOption" ? "kNone" : "Off") && !row.enabled,
                  "Added control must display the exact disabled live snapshot value: " + std::string(expected.label));
            Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
            Check(fixture::settingsSubmitted.empty() && fixture::submitted.empty(), "Unobserved dispatcher accepted a new setting write");
            fixture::snapshot.settings[index].number = 1;
            row = Rows(GetSnapshot())[selected];
            Check(row.value == (std::string(expected.id) == "SpeedRun.BubbleSkipOption" ? "kSkipObeyToken" : "On") && !row.enabled, "Snapshot updates must display while dispatcher remains unavailable");
            fixture::snapshot.engineActionsReady = true;
            row = Rows(GetSnapshot())[selected];
            Check(row.enabled && row.kind == Kind::Setting && row.value == (std::string(expected.id) == "SpeedRun.BubbleSkipOption" ? "kSkipObeyToken" : "On"), "Ready native control failed to enable from snapshot");
            Input(MenuInput::Back);
            if (expected.child) {
                Check(node == "debug.traffic" && history.size() == 1, "Pedestrians must return one level to Traffic");
                Input(MenuInput::Back);
            }
            Check(node == "debug" && history.empty(), "Added native page must return directly to Debug root");
            fixture::snapshot.engineActionsReady = false;
            ++newLiveRowsChecked;
        }
    });
    Run("eleven_added_runtime_controls_preserve_exact_setting_ids_and_directions", [] {
        AddNativeSettingsSnapshot(); fixture::snapshot.engineActionsReady = true; visible = true;
        for (const auto& expected : addedSettings) {
            SetNode(expected.page, false); selected = Index(expected.label);
            fixture::settingsSubmitted.clear(); fixture::submitted.clear();
            Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
            Check(fixture::settingsSubmitted == std::vector<std::pair<std::string, int>>{
                {expected.id, 0}, {expected.id, -1}, {expected.id, 1}},
                "Added control changed exact native setting id or input direction: " + std::string(expected.label));
            Check(fixture::submitted.empty(), "Added setting incorrectly submitted an actor/native-call action");
        }
        SetNode("debug.weapons", false);
        Check(Rows(GetSnapshot())[Index("Infinite ammo")].settingId == "Weapon.InfiniteAmmo",
              "Source Weapons merge lost the verified added control");
        Check(HintForRow(Rows(GetSnapshot())[Index("Infinite ammo")], GetSnapshot()) == "Left / Right: Off or On; NoReload is not implemented",
              "Infinite-ammo hint must disclose the unimplemented native enum choice");
    });
    Run("eleven_added_controls_reject_missing_or_mismatched_snapshot_bindings", [] {
        AddNativeSettingsSnapshot(); fixture::snapshot.engineActionsReady = true; visible = true;
        for (auto& setting : fixture::snapshot.settings) setting.available = false;
        for (const auto& expected : addedSettings) {
            SetNode(expected.page, false); selected = Index(expected.label);
            const auto row = Rows(GetSnapshot())[selected];
            Check(!row.enabled && row.value == "Unavailable" && row.settingId.empty(), "Unavailable native binding gained UI edit capability");
            Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
        }
        Check(fixture::settingsSubmitted.empty() && fixture::submitted.empty(), "Missing snapshot bindings queued commands");
        AddNativeSettingsSnapshot();
        fixture::snapshot.settings[0].pageId = "debug.weapons";
        fixture::snapshot.settings[1].label = "prototype messages";
        for (size_t index : {size_t(0), size_t(1)}) {
            const auto& expected = addedSettings[index]; SetNode(expected.page, false);
            const auto row = Rows(GetSnapshot())[Index(expected.label)];
            Check(!row.enabled && row.settingId.empty(), "Wrong-page or wrong-case setting was bound to a different control");
        }
    });
    Run("gamecontrols_source_settings_have_exact_values_hints_and_context_gates", [] {
        AddNativeSettingsSnapshot(); visible = true; Open("GameControls");
        const std::array<std::pair<const char*, const char*>, 3> expected{{
            {"AllowFastTravelDuringDialog", "Allow fast travel during dialog; other fast-travel restrictions still apply"},
            {"AllowCharacterSwitchDuringDialog", "Allow character switching during dialog; other switching restrictions still apply"},
            {"BubbleSkipOption", "Left / Right to change; Select kNone, kSkipObeyToken, or kSkipIgnoreToken. Bubble skipping can obey or ignore exposition token gates."}
        }};
        Frame();
        Check(NoReloadGameplay(), "Opening original GameControls implicitly changed a setting");
        for (const auto& entry : expected) {
            selected = Index(entry.first); auto row = Rows(GetSnapshot())[selected];
            Check(!row.enabled && row.kind == Kind::Setting && HintForRow(row, GetSnapshot()) == "Waiting for the verified engine dispatcher",
                  "Original GameControls setting bypassed dispatcher readiness");
            fixture::snapshot.engineActionsReady = true; row = Rows(GetSnapshot())[selected];
            Check(row.enabled && HintForRow(row, GetSnapshot()) == entry.second,
                  "GameControls setting failed to disclose its verified scope: " + row.name);
            fixture::snapshot.engineActionsReady = false;
            for (MenuInput input : {MenuInput::Accept, MenuInput::Left, MenuInput::Right}) Input(input);
            Check(NoReloadGameplay(), "GameControls queued a write after dispatcher capability was lost");
        }
        fixture::snapshot.settings.back().number = 2;
        Check(Rows(GetSnapshot())[Index("BubbleSkipOption")].value == "kSkipIgnoreToken", "Bubble skip failed to show its third exact native enum choice");
        for (auto& setting : fixture::snapshot.settings) if (setting.pageId == "debug.gamecontrols") setting.pageId = "debug.camera";
        fixture::snapshot.engineActionsReady = true;
        for (const auto& entry : expected) {
            selected = Index(entry.first); const auto row = Rows(GetSnapshot())[selected];
            Check(!row.enabled && row.settingId.empty(), "GameControls accepted a setting from the wrong page");
            Input(MenuInput::Accept);
        }
        Check(NoReloadGameplay(), "Mismatched GameControls settings dispatched a request");
    });
    Run("traffic_prefab_population_hints_context_loss_and_no_automatic_toggle", [] {
        AddNativeSettingsSnapshot(); visible = true;
        Open("Traffic");
        Check(node == "debug.traffic", "Traffic root navigation is missing");
        Open("Pedestrians");
        Check(node == "debug.traffic.pedestrians" && history.size() == 2, "Traffic/Pedestrians hierarchy differs from documented path");
        selected = Index("Enable Pedestrians");
        auto row = Rows(GetSnapshot())[selected];
        Check(!row.enabled && row.value == "Off" &&
              HintForRow(row, GetSnapshot()) == "Waiting for the verified engine dispatcher",
              "Pedestrian value/hint must await the actual dispatcher");
        Frame();
        Check(fixture::submitted.empty() && fixture::settingsSubmitted.empty(), "Opening/rendering Traffic automatically toggled a setting");
        fixture::snapshot.engineActionsReady = true;
        row = Rows(GetSnapshot())[selected];
        Check(row.enabled && row.settingId == "Traffic.PedestrianLoader.Enabled" &&
              HintForRow(row, GetSnapshot()) == "Left / Right to change; existing pedestrians may remain",
              "Pedestrian control must disclose its spawning/population limit");
        fixture::snapshot.settings[6].number = 1;
        row = Rows(GetSnapshot())[selected];
        Check(row.value == "On", "Traffic must reflect the current snapshot rather than a recorded default");
        fixture::snapshot.engineActionsReady = false;
        for (MenuInput input : {MenuInput::Accept, MenuInput::Left, MenuInput::Right}) Input(input);
        Check(fixture::settingsSubmitted.empty(), "Traffic queued a write after dispatcher capability was lost");
        Check(Rows(GetSnapshot())[selected].value == "On", "Context loss hid the last readable pedestrian value");
        Input(MenuInput::Back); Check(node == "debug.traffic", "Pedestrian Back skipped the Traffic parent");
        Input(MenuInput::Back); Check(node == "debug" && history.empty(), "Traffic Back failed to return to Debug root");
        Open("Prefab Actor Systems");
        Check(node == "debug.prefab_actor_systems", "Prefab root opened the wrong native page");
        selected = Index("All systems enabled");
        row = Rows(GetSnapshot())[selected];
        Check(!row.enabled && HintForRow(row, GetSnapshot()) == "Waiting for the verified engine dispatcher",
              "Prefab control must not authorize writes from metadata alone");
        fixture::snapshot.engineActionsReady = true;
        row = Rows(GetSnapshot())[selected];
        Check(row.enabled && row.settingId == "ManagedPrefabActor.AllSystemsEnabled" &&
              HintForRow(row, GetSnapshot()) == "Changes prefab populations; reset restores the flag only",
              "Prefab control must disclose that flag restoration does not restore world populations");
        Frame();
        Check(fixture::settingsSubmitted.empty(), "Displaying the prefab impact note automatically changed its Boolean");
        fixture::snapshot.settings[7].available = false;
        row = Rows(GetSnapshot())[selected];
        Check(!row.enabled && row.value == "Unavailable" && row.settingId.empty(), "Unavailable prefab metadata remained editable");
        for (MenuInput input : {MenuInput::Accept, MenuInput::Left, MenuInput::Right}) Input(input);
        Check(fixture::submitted.empty() && fixture::settingsSubmitted.empty(), "A lost prefab binding queued a native or setting action");
        Input(MenuInput::Back); Check(node == "debug" && history.empty(), "Prefab Back failed to return to Debug root");
    });
    Run("load_and_undo_hints_follow_exact_position_and_dispatcher_prerequisites", [] {
        visible = true; SetNode("local.hero", false);
        auto expect = [&](const char* label, bool enabled, const char* hint) {
            const auto row = Rows(GetSnapshot())[Index(label)];
            Check(row.enabled == enabled, "Position command enabled state differs from exact prerequisites: " + std::string(label));
            Check(HintForRow(row, GetSnapshot()) == hint, "Position command gave the wrong actionable prerequisite hint: " + std::string(label));
        };
        fixture::snapshot.savedPosition = fixture::snapshot.undoPosition = false;
        expect("Load position", false, "Waiting for the verified engine dispatcher");
        expect("Undo move", false, "Waiting for the verified engine dispatcher");
        fixture::snapshot.engineActionsReady = true; fixture::snapshot.teleportReady = false;
        expect("Load position", false, "Waiting for a playable hero / verified binding");
        expect("Undo move", false, "Waiting for a playable hero / verified binding");
        fixture::snapshot.teleportReady = true;
        expect("Load position", false, "Use Save position for this hero first");
        expect("Undo move", false, "Move or load a position first");
        fixture::snapshot.savedPosition = true;
        expect("Load position", true, "");
        expect("Undo move", false, "Move or load a position first");
        fixture::snapshot.undoPosition = true; fixture::snapshot.heroReady = false;
        expect("Load position", true, "");
        expect("Undo move", true, "");
        fixture::snapshot.engineActionsReady = false;
        expect("Load position", false, "Waiting for the verified engine dispatcher");
        expect("Undo move", false, "Waiting for the verified engine dispatcher");
        Check(fixture::submitted.empty(), "Inspecting hints must not submit actor commands");
    });
    Run("native_option_limits_and_preview_hint_override_remain_explicit", [] {
        visible = true;
        SetNode("dev.retail.start_pre_branch", false);
        auto missingMenu = Rows(GetSnapshot())[Index("Crimes")];
        Check(!missingMenu.enabled && HintForRow(missingMenu, GetSnapshot()) == "This entry names a menu absent from this build",
              "Missing native submenu reference must explain its actual limit");
        SetNode("dev.retail.audio_deliverables", false);
        auto missingAsset = Rows(GetSnapshot())[Index("Propagation")];
        Check(!missingAsset.enabled && HintForRow(missingAsset, GetSnapshot()) == "The required scene data is absent from this build",
              "Missing native scene data must explain its actual limit");
        SetNode("dev.retail.act_1_missions", false);
        auto unbound = Rows(GetSnapshot())[Index("Sandman")];
        Check(!unbound.enabled && HintForRow(unbound, GetSnapshot()) == "Unavailable: no verified retail-PC binding",
              "Present mission metadata must remain an unbound action");
        AddNativeSettingsSnapshot(); fixture::snapshot.engineActionsReady = true;
        SetNode("debug.script_messages", false);
        auto prototype = Rows(GetSnapshot())[Index("Prototype messages")];
        auto messages = Rows(GetSnapshot())[Index("Debug messages")];
        Check(HintForRow(prototype, GetSnapshot()).find("prototype-message events") != std::string::npos,
              "Prototype control must disclose event scope");
        Check(HintForRow(messages, GetSnapshot()).find("may require a reload") != std::string::npos,
              "Debug-message control must disclose new-context/reload limit");
        SetNode("debug.rendering", false);
        auto vfx = Rows(GetSnapshot())[Index("Visual effects processing")];
        Check(HintForRow(vfx, GetSnapshot()).find("existing particles may remain") != std::string::npos,
              "Visual-effects control must disclose processing limit");
        preview = true;
        for (const auto& row : {missingMenu, missingAsset, unbound, prototype, messages, vfx})
            Check(HintForRow(row, GetSnapshot()) == "Preview: gameplay controls remain disabled", "Preview hint failed to override gameplay/native details");
        preview = false;
        Check(fixture::submitted.empty() && fixture::settingsSubmitted.empty(), "Hint inspection queued game actions");
    });
    Run("hero_retained_setting_and_health_navigation", [] {
        DebugSettingValue respawn;
        respawn.available = true; respawn.id = "Hero.RespawnClear";
        respawn.pageId = "local.hero"; respawn.label = "Respawn Clear";
        respawn.type = DebugSettingType::Boolean; respawn.number = 0;
        fixture::snapshot.settings = {respawn};
        visible = true; SetNode("local.hero", false); selected = Index(respawn.label);
        auto row = Rows(GetSnapshot())[selected];
        Check(row.value == "Off" && !row.enabled, "Hero setting must wait for verified context");
        Input(MenuInput::Accept); Check(fixture::settingsSubmitted.empty(), "Unready Hero setting queued a write");
        fixture::snapshot.engineActionsReady = true;
        Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
        Check(fixture::settingsSubmitted == std::vector<std::pair<std::string,int>>{
            {respawn.id,0},{respawn.id,-1},{respawn.id,1}}, "Hero setting lost exact binding or input direction");
        Check(fixture::submitted.empty(), "Hero setting incorrectly called a native actor action");
        Open("Health controls"); Check(node == "local.health", "Hero must expose working health controls");
        Input(MenuInput::Back); Check(node == "local.hero", "Health controls must return to Hero");
    });
    Run("native_local_actions_disabled_without_dispatcher", [] {
        visible = true; std::set<Action> rejected;
        for (const char* id : {"local.health", "local.hero"}) {
            SetNode(id, false); const auto rows = Rows(GetSnapshot());
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto& r = rows[i];
                if (!NativeAction(r.action) || r.kind == Kind::Character || r.kind == Kind::Info) continue;
                Check(!r.enabled, "Native row must be disabled without dispatcher: " + r.name);
                selected = static_cast<int>(i); fixture::submitted.clear();
                Input(MenuInput::Accept); Input(MenuInput::Left); Input(MenuInput::Right);
                Check(fixture::submitted.empty(), "Native engine call escaped disabled UI: " + r.name);
                rejected.insert(r.action); ++nativeRowsRejected;
            }
        }
        Check(rejected.size() == 12, "Native rejection sweep missed an action category");
        Check(healthLimit == 100, "Disabled maximum-health editor must not change value");
        SetNode("local.hero", false); selected = Index("Character"); Input(MenuInput::Right);
        Check(character == 1 && fixture::submitted.empty(), "Character selection may change locally without invoking switch");
    });
    Run("read_only_information_skipped_by_selection", [] {
        visible = true; SetNode("local.common", false);
        Check(selected == Index("Export Diagnostics"), "Initial selection must skip read-only snapshot fields");
        const auto rows = Rows(GetSnapshot());
        for (size_t i = 0; i < rows.size() * 2; ++i) {
            Input(MenuInput::Down); Check(rows[selected].kind != Kind::Info, "Down selected read-only information");
            Input(MenuInput::Up); Check(rows[selected].kind != Kind::Info, "Up selected read-only information");
        }
        Check(fixture::submitted.empty(), "Moving selection must not submit diagnostics/actions");
    });
    Run("no_game_controller_or_overlay_backend_called", [] {
        Check(fixture::backendCalls == 0 && fixture::controllerCalls == 0 && fixture::overlayCalls == 0 && fixture::runtimeContextCalls == 0,
              "Model test must not enter backend, HID, window or GPU lifecycle");
    });
    ImGui::DestroyContext();
    bool passed = true;
    size_t rows = 0; for (const auto& p : ReferencePages()) rows += p.rows.size();
    std::cout << "{\"schema\":1,\"mode\":\"actual_menu_translation_unit_cpu_only\",\"catalog_pages\":" << ReferencePages().size()
              << ",\"catalog_rows\":" << rows << ",\"unsupported_leaves_checked\":" << unsupportedLeaves
              << ",\"loading_rows_visited\":" << loadingRowsVisited << ",\"native_rows_rejected\":" << nativeRowsRejected
              << ",\"new_live_rows_checked\":" << newLiveRowsChecked
              << ",\"catalog_routes_checked\":" << catalogRoutesChecked << ",\"local_child_routes_checked\":" << localChildRoutesChecked
              << ",\"reload_bound_controls\":" << reloadBoundControlsChecked
              << ",\"manual_reload_enabled_rows\":" << manualReloadRowsEnabled
              << ",\"replay_enabled_rows\":" << replayRowsEnabled << ",\"tests\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i]; if (i) std::cout << ',';
        std::cout << "{\"name\":\"" << Escape(r.name) << "\",\"passed\":" << (r.failure.empty() ? "true" : "false");
        if (!r.failure.empty()) { passed = false; std::cout << ",\"failure\":\"" << Escape(r.failure) << '"'; }
        std::cout << '}';
    }
    std::cout << "],\"passed\":" << (passed ? "true" : "false") << "}\n";
    return passed ? 0 : 1;
}
