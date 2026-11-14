// Synthetic, current-process-only action tests. No game, game TLS, hooks or HWND.
// The dispatcher below is a test boundary; it is not real engine-context proof.
#include <windows.h>
namespace monitored_reads {
bool transitionAccepted = false;
unsigned afterTransition = 0;
BOOL WINAPI Read(HANDLE process, LPCVOID address, LPVOID output, SIZE_T length, SIZE_T* copied) {
    if (transitionAccepted) ++afterTransition;
    return ::ReadProcessMemory(process, address, output, length, copied);
}
}
#define ReadProcessMemory monitored_reads::Read
#define main RawSnapshotFixtureMain
#include "raw_snapshot_fixture.cpp"
#undef main
#undef ReadProcessMemory

#include <functional>
#include <thread>
#include <tuple>

namespace simulated_dispatch {
bool observed = false, published = false, active = false;
DWORD ownerThread = 0;
HMODULE owner = nullptr;
sm2debug::EngineDispatchCallback callback = nullptr;
uint64_t token = 0;
unsigned scopedTicks = 0;
}
// Deliberate module boundary double: the separate reload-runtime fixture tests
// production identity/byte/native-guard logic. Here acceptance poisons the old
// world to verify that actual backend queue handling never reuses its pointers.
namespace snapshot_reads { unsigned reload=0, primary=0, actor=0, replay=0, options=0, camera=0, selection=0, settings=0; std::function<void()> onReload; }
namespace simulated_reload {
bool pending=false;
sm2debug::ReloadState current;
struct Call { sm2debug::Action action; sm2debug::ReloadState captured; uint32_t key; };
std::vector<Call> attempts, accepted;
std::function<void()> onAccepted;
bool admissionAllowed = true, requestAllowed = true;
std::vector<Call> preflights;
std::vector<std::string> events;
}
namespace simulated_primary {
sm2debug::PrimaryObjectiveState current;
std::vector<sm2debug::PrimaryObjectiveState> attempts,accepted;
std::function<void()> onAccepted;
}
namespace simulated_actor_save {
sm2debug::ActorSaveState current;
sm2debug::ActorSaveResetOutcome outcome = sm2debug::ActorSaveResetOutcome::Applied;
std::vector<sm2debug::ActorSaveState> attempts;
unsigned persistentResets = 0;
}
namespace simulated_replay {
bool pending=false;
sm2debug::ReplayState current;
sm2debug::ReplayResult outcome{};
std::vector<sm2debug::ReplayState> attempts;
std::function<void()> onInvoked;
}

namespace sm2debug {
bool InitializeEngineDispatch(HMODULE, EngineDispatchCallback callback) noexcept { return callback == nullptr; }
void ShutdownEngineDispatch() noexcept { simulated_dispatch::published = false; }
EngineDispatchObservation GetEngineDispatchObservation() noexcept {
    EngineDispatchObservation result;
    result.status = simulated_dispatch::observed ? EngineDispatchStatus::ContextObserved : EngineDispatchStatus::NotStarted;
    result.contextReady = simulated_dispatch::observed;
    result.consecutiveVerifiedFrames = simulated_dispatch::observed ? 30 : 0;
    result.callbackEnabled = simulated_dispatch::published;
    result.callbackToken = simulated_dispatch::token;
    return result;
}
bool IsWithinVerifiedEngineCallback() noexcept {
    return simulated_dispatch::published && simulated_dispatch::active &&
        simulated_dispatch::ownerThread == GetCurrentThreadId();
}
bool EngineDispatchCallbacksAvailable() noexcept { return simulated_dispatch::published; }
bool RegisterEngineDispatchCallback(HMODULE owner, EngineDispatchCallback callback, uint64_t* token) noexcept {
    if (!simulated_dispatch::observed || simulated_dispatch::published || !owner || !callback || !token) return false;
    simulated_dispatch::owner = owner;
    simulated_dispatch::callback = callback;
    simulated_dispatch::published = true;
    simulated_dispatch::ownerThread = GetCurrentThreadId();
    *token = ++simulated_dispatch::token;
    return true;
}
bool UnregisterEngineDispatchCallback(uint64_t token, DWORD) noexcept {
    if (!token || token != simulated_dispatch::token) return false;
    simulated_dispatch::published = false;
    return true;
}
void ReportEngineDispatchCallbackFailure(uint64_t token, uint32_t) noexcept {
    if (token == simulated_dispatch::token) simulated_dispatch::published = false;
}
void InitializeReloadRuntime(HMODULE, bool) {}
ReloadState ReadReloadState() { ++snapshot_reads::reload; if(snapshot_reads::onReload) { auto callback=std::move(snapshot_reads::onReload); callback(); } return simulated_reload::current; }
bool ReloadTransitionPending() { return simulated_reload::pending; }
bool SameReloadContext(const ReloadToken& left, const ReloadToken& right) noexcept {
    // Boundary comparison includes the fixture's independently changed world
    // identity; the production comparator is linked by the UI/runtime runners.
    return std::tie(left.imageBase, left.manager, left.managerVtable, left.loadedLevel,
        left.mode, left.componentTable, left.kernel, left.kernelOwner, left.kernelHandle,
        left.kernelGeneration, left.kernelSequence, left.levelAsset, left.contextSerial,
        left.checkpointRecords, left.checkpointCount, left.saveSystem, left.saveSystemVtable) ==
        std::tie(right.imageBase, right.manager, right.managerVtable, right.loadedLevel,
        right.mode, right.componentTable, right.kernel, right.kernelOwner, right.kernelHandle,
        right.kernelGeneration, right.kernelSequence, right.levelAsset, right.contextSerial,
        right.checkpointRecords, right.checkpointCount, right.saveSystem, right.saveSystemVtable);
}
bool FixtureReload(Action action, const ReloadState& captured, uint32_t key, std::wstring& status) {
    simulated_reload::attempts.push_back({action, captured, key});
    simulated_reload::events.push_back("reload");
    const auto validKey = [key](const ReloadState& state) {
        return !key || (state.checkpoints && std::any_of(state.checkpoints->begin(), state.checkpoints->end(),
            [key](const ReloadCheckpoint& item) { return item.key == key; }));
    };
    if (!simulated_reload::requestAllowed || !IsWithinVerifiedEngineCallback() || !captured.supported || !captured.ready ||
        !simulated_reload::current.ready || !SameReloadContext(captured.token, simulated_reload::current.token) ||
        !validKey(captured) || !validKey(simulated_reload::current) ||
        (action == Action::ExitFrontend && !captured.frontendReady)) {
        status = L"Fixture runtime rejected stale/invalid request."; return false;
    }
    simulated_reload::accepted.push_back({action, captured, key});
    monitored_reads::transitionAccepted = true;
    if (simulated_reload::onAccepted) simulated_reload::onAccepted();
    status = L"Fixture runtime accepted native transition.";
    return true;
}
bool ExecuteReload(const ReloadState& captured, uint32_t key, std::wstring& status) {
    return FixtureReload(Action::ReloadCheckpoint, captured, key, status);
}
bool ExecuteFullReload(const ReloadState& captured,uint32_t key,std::wstring& status) {
    return FixtureReload(Action::FullReload,captured,key,status);
}
bool ExecuteExitFrontend(const ReloadState& captured, std::wstring& status) {
    return FixtureReload(Action::ExitFrontend, captured, 0, status);
}
bool CheckReloadExecutionContext(const ReloadState& captured,std::wstring& status,uint32_t key) {
    simulated_reload::preflights.push_back({Action::ReloadCheckpoint,captured,key});
    simulated_reload::events.push_back("admit");
    const auto member=[key](const ReloadState& state)->const ReloadCheckpoint*{
        if(state.checkpoints)for(const auto& item:*state.checkpoints)if(item.key==key)return &item;
        return nullptr;
    };
    const auto old=member(captured),fresh=member(simulated_reload::current);
    const bool valid=!key||(old&&fresh&&old->name==fresh->name&&old->recordIndex==fresh->recordIndex);
    if(!simulated_reload::admissionAllowed||!IsWithinVerifiedEngineCallback()||!captured.ready||
        !simulated_reload::current.ready||!SameReloadContext(captured.token,simulated_reload::current.token)||!valid){
        status=L"Fixture native admission refused before persistent reset.";return false;
    }
    status=L"Fixture native admission passed.";return true;
}
void InitializeActorSaveRuntime(HMODULE,bool) {}
ActorSaveState ReadActorSaveState(const ReloadState&) { ++snapshot_reads::actor; return simulated_actor_save::current; }
ActorSaveResetOutcome ExecuteActorSaveReset(const ActorSaveState& captured,std::wstring& status) {
    simulated_actor_save::attempts.push_back(captured);simulated_reload::events.push_back("reset");
    if(!IsWithinVerifiedEngineCallback()||!captured.ready||!simulated_actor_save::current.ready||
        !SameReloadContext(captured.token.world,simulated_reload::current.token)||
        captured.token.fingerprint!=simulated_actor_save::current.token.fingerprint){
        status=L"Fixture reset refused stale actor save.";return ActorSaveResetOutcome::Refused;
    }
    if(simulated_actor_save::outcome!=ActorSaveResetOutcome::Refused)++simulated_actor_save::persistentResets;
    status=simulated_actor_save::outcome==ActorSaveResetOutcome::Applied?L"Fixture actor-save reset applied.":
        L"Fixture reset refused or post-reset verification failed.";
    return simulated_actor_save::outcome;
}
void InitializeReplayRuntime(HMODULE,bool) {}
ReplayState ReadReplayState(const PrimaryObjectiveState&,const ReplaySelection&) { ++snapshot_reads::replay; return simulated_replay::current; }
bool ReplayTransitionPending() { return simulated_replay::pending; }
ReplayResult ExecuteReplay(const ReplayState& captured,std::wstring& status) {
    simulated_replay::attempts.push_back(captured);
    if(!IsWithinVerifiedEngineCallback()||!captured.ready||!simulated_replay::current.ready||
        !captured.requestEpoch||captured.requestEpoch!=simulated_replay::current.requestEpoch||
        !SameReplaySelection(captured.selection,simulated_replay::current.selection)||
        !SameReloadContext(captured.origin.world,simulated_reload::current.token)){
        status=L"Fixture replay rejected stale typed target.";return {};
    }
    const auto result=simulated_replay::outcome;
    if(result.invoked){monitored_reads::transitionAccepted=true;if(simulated_replay::onInvoked)simulated_replay::onInvoked();}
    status=result.accepted?L"Fixture replay accepted.":(result.invoked?L"Fixture replay invoked but rejected.":L"Fixture replay refused before invocation.");
    return result;
}
void InitializePrimaryObjectiveRuntime(HMODULE,bool) {}
PrimaryObjectiveState ReadPrimaryObjectiveState(const ReloadState&) { ++snapshot_reads::primary; return simulated_primary::current; }
bool ExecuteCompletePrimary(const PrimaryObjectiveState& captured,std::wstring& status) {
    // This is deliberately a module boundary double. The separate22-case
    // primary runtime fixture exercises real raw lookup and native-call guards.
    simulated_primary::attempts.push_back(captured);
    const auto& current=simulated_primary::current;
    if (!IsWithinVerifiedEngineCallback() || !captured.supported || !captured.ready || !current.supported || !current.ready ||
        !captured.token.missionHash || !captured.token.objectiveHash || !SameReloadContext(captured.world,current.world) ||
        !SameReloadContext(captured.world,simulated_reload::current.token) ||
        std::tie(captured.token.objectiveSystem,captured.token.saveBlock,captured.token.systemData,
            captured.token.missionHash,captured.token.objectiveHash,captured.token.missionNode,captured.token.objectiveNode,
            captured.token.missionGraphHandle,captured.token.objectiveGraphHandle,captured.token.missionNodeId,captured.token.objectiveNodeId) !=
        std::tie(current.token.objectiveSystem,current.token.saveBlock,current.token.systemData,
            current.token.missionHash,current.token.objectiveHash,current.token.missionNode,current.token.objectiveNode,
            current.token.missionGraphHandle,current.token.objectiveGraphHandle,current.token.missionNodeId,current.token.objectiveNodeId)) {
        status=L"Fixture primary runtime rejected stale/invalid request.";return false;
    }
    simulated_primary::accepted.push_back(captured);
    monitored_reads::transitionAccepted=true;
    if (simulated_primary::onAccepted)simulated_primary::onAccepted();
    status=L"Fixture native primary completion requested.";return true;
}
}

namespace {
struct PositionCall { void* transform; Vec3 value; };
struct CharacterCall { void* manager; uint32_t crc; int mode; void* request; };
std::vector<PositionCall> positionCalls;
std::vector<CharacterCall> characterCalls;
std::vector<std::string> passedCases;

bool Equal(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
void PositionCapture(void* transform, const Vec3* position) {
    Check(IsWithinVerifiedEngineCallback(), "position trap entered outside simulated registered scope");
    Check(transform && position, "position trap received a null argument");
    positionCalls.push_back({transform, *position});
    // Model the setter's visible position result in fixture-owned bytes only.
    std::memcpy(static_cast<unsigned char*>(transform) + 0x30, position, sizeof(*position));
}
void CharacterCapture(void* manager, uint32_t crc, int mode, void* request) {
    Check(IsWithinVerifiedEngineCallback(), "character trap entered outside simulated registered scope");
    characterCalls.push_back({manager, crc, mode, request});
}
struct Scope {
    Scope() { Check(!simulated_dispatch::active, "nested simulated scope"); simulated_dispatch::active = true; }
    ~Scope() { simulated_dispatch::active = false; }
};
void ScopedTick() { Scope scope; ++simulated_dispatch::scopedTicks; GameTick(); }
void Start(Fixture& fixture) {
    monitored_reads::transitionAccepted = false; monitored_reads::afterTransition = 0;
    simulated_reload::current = {}; simulated_reload::attempts.clear(); simulated_reload::accepted.clear();
    simulated_reload::onAccepted = {};
    simulated_reload::admissionAllowed=simulated_reload::requestAllowed=true;
    simulated_reload::preflights.clear();simulated_reload::events.clear();
    simulated_primary::current={};simulated_primary::attempts.clear();simulated_primary::accepted.clear();simulated_primary::onAccepted={};
    simulated_actor_save::current={};simulated_actor_save::outcome=ActorSaveResetOutcome::Applied;
    simulated_actor_save::attempts.clear();simulated_actor_save::persistentResets=0;
    simulated_replay::current={};simulated_replay::outcome={};simulated_replay::attempts.clear();simulated_replay::onInvoked={};
    desiredReplaySelection={};
    simulated_dispatch::observed = simulated_dispatch::published = simulated_dispatch::active = false;
    simulated_dispatch::ownerThread = 0;
    simulated_dispatch::owner = nullptr;
    simulated_dispatch::callback = nullptr;
    engineOwnsSimulation.store(false);
    dispatchToken.store(0);
    dispatchRegistrationAttempted.store(false);
    shutdownReset.store(false); shutdownResetComplete.store(false);
    settingsRestorePending.store(false); tickPending.store(false);
    targetWindow = nullptr; previousProc = nullptr; selfModule = GetModuleHandleW(nullptr);
    tickMessage = WM_APP + 0x48;
    positionCalls.clear(); characterCalls.clear(); explicitSetterCalls = 0;
    g.setPosition = &PositionCapture;
    g.switchCharacter = &CharacterCapture;
    GameTick(); // Real production raw snapshot: no simulated capability yet.
    Check(GetSnapshot().heroReady && GetSnapshot().actorHandle == fixture.handle, "fixture initial hero was not readable");
    Check(GetSnapshot().characterNames == std::vector<std::wstring>{L"Peter", L"Miles"}, "fixture initial characters differ");
    simulated_dispatch::observed = true;
    uint64_t token = 0;
    Check(RegisterEngineDispatchCallback(selfModule, &EngineFrame, &token), "simulated callback registration failed");
    dispatchToken.store(token);
    // Invoke the actual backend entry once to establish sticky simulation ownership.
    { Scope scope; simulated_dispatch::callback(); }
    Check(engineOwnsSimulation.load(), "actual backend callback did not claim simulation ownership");
    ScopedTick();
    Check(GetSnapshot().engineActionsReady, "registered fixture capability was not published");
}
void Apply(Action action, float value = 0) {
    SubmitAction(action, value);
    ScopedTick();
    Check(commands.empty(), "scoped tick failed to consume one queued action");
}
Vec3 PositionOf(Fixture& fixture) {
    Vec3 result{};
    Check(ReadAt(fixture.transform.address(), 0x30, result), "fixture position unreadable");
    return result;
}
void ExpectPosition(Fixture& fixture, size_t count, Vec3 expected) {
    Check(positionCalls.size() == count, "position setter call count differs");
    Check(positionCalls.back().transform == reinterpret_cast<void*>(fixture.transform.address()), "setter received actor instead of exact transform");
    Check(Equal(positionCalls.back().value, expected), "setter target coordinates differ");
    Check(Equal(PositionOf(fixture), expected), "synthetic setter result was not observed");
}
void SaveLoadUndo() {
    Fixture fixture; Start(fixture);
    const Vec3 original{12.5f, -42, 300}, moved{55, 66, 77};
    Apply(Action::SavePosition);
    Check(savedPosition.valid && savedPosition.handle == fixture.handle && Equal(savedPosition.value, original), "SavePosition lost coordinates or generation");
    Check(positionCalls.empty(), "SavePosition called a native setter");
    fixture.transform.set(0x30, moved);
    Apply(Action::LoadPosition);
    ExpectPosition(fixture, 1, original);
    Check(undoPosition.valid && undoPosition.handle == fixture.handle && Equal(undoPosition.value, moved), "LoadPosition did not retain prior position for undo");
    Apply(Action::UndoTeleport);
    ExpectPosition(fixture, 2, moved);
    Check(Equal(undoPosition.value, original), "UndoTeleport did not retain its pre-call position");
    passedCases.push_back("save_load_undo_exact_transform_and_coordinates");
}
void SixDirections() {
    Fixture fixture; Start(fixture);
    const Vec3 origin{10, 20, 30};
    const std::array<Action, 6> actions{{Action::MoveXMinus, Action::MoveXPlus, Action::MoveYMinus,
        Action::MoveYPlus, Action::MoveZMinus, Action::MoveZPlus}};
    const std::array<Vec3, 6> expected{{{2.5f,20,30},{17.5f,20,30},{10,12.5f,30},
        {10,27.5f,30},{10,20,22.5f},{10,20,37.5f}}};
    for (size_t index = 0; index < actions.size(); ++index) {
        fixture.transform.set(0x30, origin);
        Apply(actions[index], 7.5f);
        ExpectPosition(fixture, index + 1, expected[index]);
        Check(undoPosition.handle == fixture.handle && Equal(undoPosition.value, origin), "direction move did not retain the original position");
        Check(GetSnapshot().moveStep == 7.5f, "direction move lost its selected step");
    }
    passedCases.push_back("six_axis_directions_exact_sign_and_untouched_axes");
}
void PositionValidation() {
    Fixture fixture; Start(fixture);
    const auto original = fixture.transform.data;
    for (float value : {.09f, -1.0f, 1001.0f, std::numeric_limits<float>::quiet_NaN()}) Apply(Action::MoveXPlus, value);
    Apply(Action::LoadPosition); Apply(Action::UndoTeleport);
    Check(positionCalls.empty() && fixture.transform.data == original, "invalid step or missing saved position reached setter");
    fixture.transform.set(0x30, Vec3{99999, 20, 30});
    Apply(Action::MoveXPlus, 1000);
    Check(positionCalls.empty(), "out-of-range target coordinates reached setter");
    passedCases.push_back("invalid_steps_missing_positions_and_coordinate_limits_rejected");
}
void StaleGeneration() {
    const std::array<Action, 4> actions{{Action::SavePosition, Action::MoveXPlus, Action::LoadPosition, Action::SwitchCharacter}};
    for (Action action : actions) {
        Fixture fixture; Start(fixture);
        savedPosition = {{1,2,3}, fixture.handle, true};
        undoPosition = {{4,5,6}, fixture.handle, true};
        SubmitAction(action, action == Action::SwitchCharacter ? 1.0f : 5.0f);
        const uint32_t replacement = (8u << 20) | 1u;
        fixture.pool.set(0xc8, uint16_t(8)); fixture.entry.set(4, replacement);
        ScopedTick();
        Check(positionCalls.empty() && characterCalls.empty(), "queued stale-generation command reached a native trap");
        Check(GetSnapshot().status.find(L"Hero changed") != std::wstring::npos, "stale-generation rejection was not diagnosed");
        Check(commands.empty(), "stale-generation command remained queued indefinitely");
        Apply(Action::LoadPosition);
        Apply(Action::UndoTeleport);
        Check(positionCalls.empty(), "saved or undo position from earlier generation reached setter");
    }
    passedCases.push_back("queued_commands_and_saved_positions_reject_reused_actor_generation");
}
void CharacterArguments() {
    Fixture fixture; Start(fixture);
    savedPosition = {{1,2,3}, fixture.handle, true}; undoPosition = {{4,5,6}, fixture.handle, true};
    SubmitAction(Action::SwitchCharacter, 1);
    Check(commands.size() == 1 && commands.front().characterName == L"Miles" && commands.front().actorHandle == fixture.handle,
        "selected character identity was not captured at submit time");
    ScopedTick();
    Check(characterCalls.size() == 1, "valid character selection did not call the switch trap exactly once");
    const auto& call = characterCalls.front();
    // Independently calculated with Python zlib, seed0xEDB88320 and no final xor.
    Check(call.manager == reinterpret_cast<void*>(fixture.manager.address()) && call.crc == 0x68b6bd89u &&
        call.mode == 4 && call.request == nullptr, "character switch ABI arguments differ from verified contract");
    Check(!savedPosition.valid && !undoPosition.valid, "character switch retained old hero teleport state");
    Check(positionCalls.empty() && explicitSetterCalls == 0, "character switch caused an unrelated setter call");
    passedCases.push_back("character_switch_exact_manager_crc_mode4_and_null_request");
}
void CharacterValidation() {
    Fixture fixture; Start(fixture);
    SubmitAction(Action::SwitchCharacter, 1);
    fixture.characters.set(0x2a0 + 8, reinterpret_cast<uintptr_t>("Peter"));
    ScopedTick();
    Check(characterCalls.empty() && GetSnapshot().status.find(L"Character list changed") != std::wstring::npos,
        "queued selection ignored a changed character name");
    for (float value : {-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()}) Apply(Action::SwitchCharacter, value);
    Check(characterCalls.empty(), "invalid character index reached the switch trap");
    fixture.manager.set(0x10, fixture.actor + 0xc0);
    Apply(Action::SwitchCharacter, 0);
    Check(characterCalls.empty(), "character manager with a foreign owner reached the switch trap");
    passedCases.push_back("changed_character_names_bad_indices_and_foreign_manager_rejected");
}
void LostContextWindowPath() {
    Fixture fixture; Start(fixture);
    SubmitAction(Action::MoveXPlus, 5); SubmitAction(Action::SwitchCharacter, 1);
    const auto before = commands.size();
    const auto transformBefore = fixture.transform.data;
    simulated_dispatch::published = false;
    GameTick();
    Check(commands.size() == before && positionCalls.empty() && characterCalls.empty(), "lost context consumed or executed gameplay queue");
    commands.push_back({Action::ExportDiagnostics, 0});
    const auto diagnostics = outputPath / L"diagnostics.json";
    std::filesystem::remove(diagnostics);
    { std::lock_guard busySimulation(simulationMutex);
      GameWindowProc(nullptr, tickMessage, reinterpret_cast<WPARAM>(selfModule), 0); }
    Check(std::filesystem::exists(diagnostics), "window diagnostics stalled behind simulation ownership");
    Check(commands.size() == before && commands.front().action == Action::MoveXPlus,
        "window path consumed a gameplay command during context loss");
    Check(fixture.transform.data == transformBefore && positionCalls.empty() && characterCalls.empty(),
        "window path called a native trap after engine ownership loss");
    simulated_dispatch::published = true;
    GameTick();
    Check(commands.size() == before && positionCalls.empty(), "published capability alone authorized a window-path native call");
    ScopedTick(); ExpectPosition(fixture, 1, Vec3{17.5f,-42,300});
    ScopedTick();
    Check(characterCalls.size() == 1 && commands.empty(), "restored simulated scope did not preserve and service gameplay order");
    passedCases.push_back("lost_context_preserves_gameplay_queue_and_window_only_diagnostics");
}
void ForeignThreadDenied() {
    Fixture fixture; Start(fixture);
    SubmitAction(Action::MoveZPlus, 2);
    simulated_dispatch::active = true;
    bool foreignAuthorized = true;
    std::thread foreign([&] { foreignAuthorized = IsWithinVerifiedEngineCallback(); GameTick(); });
    foreign.join(); simulated_dispatch::active = false;
    Check(!foreignAuthorized && commands.size() == 1 && positionCalls.empty(), "foreign fixture thread gained scope authorization");
    ScopedTick(); ExpectPosition(fixture, 1, Vec3{12.5f,-42,302});
    passedCases.push_back("foreign_thread_and_published_capability_do_not_authorize_native_calls");
}
ReloadState ReadyReloadState() {
    ReloadState value;
    value.supported = value.ready = value.frontendReady = true;
    value.token.imageBase = 0x140000000ull; value.token.manager = 0x200001000ull;
    value.token.managerVtable = 0x145400000ull; value.token.loadedLevel = 0x200002000ull;
    value.token.mode = 0x200003000ull; value.token.kernel = 0x200004000ull;
    value.token.kernelHandle = 0x700001u; value.token.kernelGeneration = 7;
    value.token.levelAsset = 0xfedcba9876543210ull; value.token.contextSerial = 23;
    value.token.currentCheckpoint = 0x87654321u; value.token.checkpointCount = 1;
    value.checkpoints = std::make_shared<const std::vector<ReloadCheckpoint>>(
        std::vector<ReloadCheckpoint>{{0xfedcba99u, 3, L"High checkpoint"}});
    value.status = L"Ready";
    return value;
}
PrimaryObjectiveState ReadyPrimaryState() {
    PrimaryObjectiveState value;
    value.supported=value.ready=true;value.world=ReadyReloadState().token;
    value.token.objectiveSystem=0x20000a000ull;value.token.saveBlock=0x20000b000ull;value.token.systemData=0x20000c000ull;
    value.token.primaryAddress=value.token.systemData+0x38;value.token.missionHash=0xdeadbeefu;value.token.objectiveHash=0xfedcba99u;
    value.token.missionNode=0x20000d000ull;value.token.objectiveNode=0x20000e000ull;
    value.token.missionGraphHandle=0x500002u;value.token.objectiveGraphHandle=0x700003u;
    value.token.missionNodeId=0x8000000012345678ull;value.token.objectiveNodeId=0x8000000087654321ull;
    value.status=L"Active fixture primary";return value;
}
ActorSaveState ReadyActorSaveState() {
    ActorSaveState value;value.supported=value.ready=true;value.token.world=ReadyReloadState().token;
    value.token.block=0x200010000ull;value.token.container=0x200011000ull;
    value.token.entry=0x200012000ull;value.token.saveData=0x200013000ull;
    value.token.fingerprint=0xfedcba9876543210ull;value.token.entries={7,11,13};
    value.status=L"Fixture actor save ready";return value;
}
ReplayState ReadyReplayState() {
    const auto& catalog=ReloadMissionCatalog();
    Check(!catalog.empty()&&!catalog.front().missions.empty()&&!catalog.front().missions.front().objectives.empty(),
        "actual typed mission catalog lacks test selection");
    const auto& graph=catalog.front();const auto& mission=graph.missions.front();const auto& objective=mission.objectives.front();
    Check(mission.nameCrc>0x1000000u&&objective.nameCrc>0x1000000u,"catalog test hashes must expose float precision loss");
    ReplayState value;value.supported=value.ready=true;value.requestEpoch=17;value.origin=ReadyPrimaryState();
    value.selection={graph.graphAssetId,mission.nameCrc,objective.nameCrc};
    value.target.node=0x200014000ull;value.target.definition=0x200015000ull;
    value.target.graph=0x200016000ull;value.target.resource=0x200017000ull;
    value.target.graphId=graph.graphAssetId;value.target.nodeId=mission.assetNodeId;
    value.target.objectiveGraphId=mission.objectiveGraphAssetId;
    value.target.graphHandle=0x700001u;value.target.checkpoint=objective.checkpoint.nameCrc;
    value.target.missionState=3;value.target.replayType=1;value.status=L"Fixture replay target ready";return value;
}
void ActorResetPreflightAndOutcomeOrder() {
    for(Action action:{Action::ReloadCheckpoint,Action::FullReload}) {
        Fixture fixture;Start(fixture);simulated_reload::current=ReadyReloadState();
        simulated_actor_save::current=ReadyActorSaveState();ScopedTick();
        const auto captured=GetSnapshot();
        SubmitReloadWithActorReset(Action::ExitFrontend,captured.reload,0,captured.actorSave);
        auto invalid=captured.actorSave;++invalid.token.world.contextSerial;
        SubmitReloadWithActorReset(action,captured.reload,0,invalid);
        SubmitReloadWithActorReset(action,captured.reload,0x01000001u,captured.actorSave);
        Check(commands.empty(),"reset accepted frontend, mismatched world or unlisted checkpoint");
        SubmitReloadWithActorReset(action,captured.reload,0xfedcba99u,captured.actorSave);
        Check(commands.size()==1&&commands.front().resetActorSave&&commands.front().checkpointKey==0xfedcba99u&&
            commands.front().actorSave.token.fingerprint==0xfedcba9876543210ull&&commands.front().actorHandle==0,
            "typed reset command lost world, uint32 key or persistent-map identity");
        simulated_reload::admissionAllowed=false;ScopedTick();
        Check(simulated_reload::preflights.size()==1&&simulated_reload::preflights.front().key==0xfedcba99u&&
            simulated_actor_save::attempts.empty()&&simulated_reload::attempts.empty()&&!simulated_actor_save::persistentResets,
            "failed native admission mutated actor save or submitted Reload");
        Check(simulated_reload::events==std::vector<std::string>{"admit"}&&commands.empty()&&GetSnapshot().actorSave.ready,
            "preflight rejection did not retire just the command while preserving actor save");
        Check(GetSnapshot().status.find(L"before persistent reset")!=std::wstring::npos,"preflight refusal status was lost");
    }
    passedCases.push_back("actor_reset_preflight_refusal_preserves_persistent_save_for_reload_and_full");
    for(auto outcome:{ActorSaveResetOutcome::Refused,ActorSaveResetOutcome::AppliedButVerificationFailed}) {
        Fixture fixture;Start(fixture);simulated_reload::current=ReadyReloadState();
        simulated_actor_save::current=ReadyActorSaveState();simulated_actor_save::outcome=outcome;ScopedTick();
        SubmitReloadWithActorReset(Action::ReloadCheckpoint,GetSnapshot().reload,0,GetSnapshot().actorSave);ScopedTick();
        Check(simulated_reload::events==std::vector<std::string>{"admit","reset"}&&
            simulated_actor_save::attempts.size()==1&&simulated_reload::attempts.empty(),
            "refused or partially verified reset incorrectly reached Reload");
        const bool changed=outcome==ActorSaveResetOutcome::AppliedButVerificationFailed;
        Check(simulated_actor_save::persistentResets==unsigned(changed)&&GetSnapshot().actorSave.ready!=changed,
            "reset outcome did not preserve or invalidate cached actor-save state correctly");
        Check(!monitored_reads::transitionAccepted&&GetSnapshot().heroReady&&commands.empty(),
            "save-only reset outcome pretended to transition the world");
    }
    passedCases.push_back("actor_reset_refusal_and_applied_verification_failure_never_reload");
    {
        Fixture fixture;Start(fixture);simulated_reload::current=ReadyReloadState();
        simulated_actor_save::current=ReadyActorSaveState();simulated_reload::requestAllowed=false;ScopedTick();
        SubmitReloadWithActorReset(Action::FullReload,GetSnapshot().reload,0xfedcba99u,GetSnapshot().actorSave);ScopedTick();
        Check(simulated_reload::events==std::vector<std::string>{"admit","reset","reload"}&&
            simulated_actor_save::persistentResets==1&&simulated_reload::attempts.size()==1&&simulated_reload::accepted.empty(),
            "persistent reset was not applied strictly before the refused Full Reload");
        Check(GetSnapshot().status.find(L"reset remains applied")!=std::wstring::npos&&!GetSnapshot().actorSave.ready,
            "post-reset Reload refusal hid persistent side effect or retained stale reset token");
    }
    passedCases.push_back("actor_reset_before_refused_reload_is_truthful_and_not_rolled_back");
    {
        Fixture fixture;Start(fixture);simulated_reload::current=ReadyReloadState();
        simulated_actor_save::current=ReadyActorSaveState();ScopedTick();
        simulated_reload::onAccepted=[&]{fixture.poolPointer=0;};
        SubmitReloadWithActorReset(Action::ReloadCheckpoint,GetSnapshot().reload,0,GetSnapshot().actorSave);ScopedTick();
        Check(simulated_reload::events==std::vector<std::string>{"admit","reset","reload"}&&
            simulated_actor_save::persistentResets==1&&simulated_reload::accepted.size()==1,
            "successful combined request did not reset exactly once before Reload");
        Check(!GetSnapshot().actorSave.ready&&!GetSnapshot().heroReady&&monitored_reads::afterTransition==0,
            "accepted combined request read destroyed-world bytes or kept reset token");
    }
    passedCases.push_back("actor_reset_then_accepted_reload_discards_world_without_post_call_reads");
}
void ReplayTypedQueueAndInvocationLifetime() {
    for(const ReplayResult outcome:std::array<ReplayResult,3>{{{false,false},{true,false},{true,true}}}) {
        Fixture fixture;Start(fixture);simulated_reload::current=ReadyReloadState();simulated_primary::current=ReadyPrimaryState();
        simulated_actor_save::current=ReadyActorSaveState();simulated_replay::current=ReadyReplayState();
        simulated_replay::outcome=outcome;ScopedTick();const auto captured=GetSnapshot().replay;
        SubmitAction(Action::ReplayMission,static_cast<float>(captured.selection.missionHash));
        Check(commands.empty(),"Replay accepted lossy generic float API");
        auto invalid=captured;invalid.ready=false;SubmitReplayAction(invalid);
        invalid=captured;invalid.origin.ready=false;SubmitReplayAction(invalid);
        invalid=captured;invalid.selection.missionHash=0;SubmitReplayAction(invalid);
        previewMode=true;SubmitReplayAction(captured);previewMode=false;
        Check(commands.empty(),"invalid Replay state or preview entered native queue");
        SubmitReplayAction(captured);
        Check(commands.size()==1&&commands.front().action==Action::ReplayMission&&commands.front().actorHandle==0&&
            commands.front().value==0&&SameReplaySelection(commands.front().replay.selection,captured.selection)&&
            commands.front().replay.requestEpoch==captured.requestEpoch&&
            commands.front().replay.target.nodeId==captured.target.nodeId&&
            commands.front().replay.selection.graphId==ReloadMissionCatalog().front().graphAssetId,
            "typed Replay queue rounded IDs, lost token or acquired actor/float payload");
        savedPosition={{1,2,3},fixture.handle,true};undoPosition={{4,5,6},fixture.handle,true};
        healthEdit={fixture.health.address(),fixture.handle,80,100};autoHeal=true;fixture.health.set(0xd0,100.0f);
        simulated_replay::onInvoked=[&]{fixture.poolPointer=0;};ScopedTick();const auto snapshot=GetSnapshot();
        Check(simulated_replay::attempts.size()==1&&commands.empty(),"Replay command not dispatched exactly once");
        if(outcome.invoked) {
            Check(!autoHeal&&!healthEdit.component&&!savedPosition.valid&&!undoPosition.valid&&!snapshot.heroReady&&
                !snapshot.healthReady&&!snapshot.teleportReady&&!snapshot.actorHandle&&snapshot.characterNames.empty()&&
                !snapshot.reload.ready&&!snapshot.primary.ready&&!snapshot.actorSave.ready&&!snapshot.replay.ready,
                "Replay invocation retained old instance state when native accepted was false or true");
            Check(monitored_reads::afterTransition==0&&positionCalls.empty()&&characterCalls.empty()&&explicitSetterCalls==0,
                "backend touched old world after Replay invocation");
        } else Check(snapshot.heroReady&&snapshot.reload.ready&&snapshot.primary.ready&&snapshot.replay.ready&&savedPosition.valid,
            "Replay rejected before invocation invalidated unrelated instance state");
    }
    passedCases.push_back("replay_typed_catalog_ids_and_lifetime_invalidated_on_any_native_invocation");
}
void ReplaySelectionCopyAndLostContext() {
    Fixture fixture;Start(fixture);simulated_reload::current=ReadyReloadState();simulated_primary::current=ReadyPrimaryState();
    simulated_actor_save::current=ReadyActorSaveState();simulated_replay::current=ReadyReplayState();ScopedTick();
    const auto selected=simulated_replay::current.selection;
    SubmitReplaySelection(selected);
    Check(SameReplaySelection(desiredReplaySelection,selected)&&commands.empty()&&simulated_replay::attempts.empty(),
        "catalog selection performed native action instead of local state update");
    auto invalid=selected;invalid.graphId^=0x100;SubmitReplaySelection(invalid);
    invalid=selected;invalid.objectiveHash=0xffffffffu;SubmitReplaySelection(invalid);
    Check(SameReplaySelection(desiredReplaySelection,selected),"unknown typed catalog selection replaced valid local choice");
    SubmitReplayAction(GetSnapshot().replay);
    SubmitReloadWithActorReset(Action::FullReload,GetSnapshot().reload,0,GetSnapshot().actorSave);
    SubmitReplayDataCopy(GetSnapshot().reload,selected);simulated_dispatch::published=false;GameTick();
    Check(commands.size()==3&&simulated_replay::attempts.empty()&&simulated_actor_save::attempts.empty()&&simulated_reload::preflights.empty(),
        "lost context consumed Replay or reset request");
    Command window{Action::Heal,0};
    Check(TakeCommand(window,true)&&window.action==Action::CopyReloadData&&window.copyReplayMode&&
        SameReplaySelection(window.replay.selection,selected)&&commands.size()==2,
        "window dequeue failed to isolate exact typed Replay copy from native requests");
    Check(!WindowAction(Action::ReplayMission)&&!TakeCommand(window,true),"window route consumed native Replay/reset");
    RunWindowCommand(window); // No HWND exists: formatter runs, OS clipboard is never opened.
    Check(GetSnapshot().status==L"Reload data is unavailable; clipboard unchanged."&&
        simulated_replay::attempts.empty()&&simulated_actor_save::attempts.empty()&&simulated_reload::preflights.empty(),
        "Replay copy invoked native action or failed actual catalog validation");
    simulated_dispatch::published=true;++simulated_reload::current.token.contextSerial;ScopedTick();
    Check(simulated_replay::attempts.size()==1&&!monitored_reads::transitionAccepted&&commands.size()==1,
        "stale preserved Replay was invoked or combined reset request was consumed too soon");
    ScopedTick();Check(commands.empty()&&simulated_actor_save::attempts.empty()&&simulated_reload::attempts.empty(),
        "stale combined world request mutated persistent save");
    passedCases.push_back("replay_local_selection_copy_and_preserved_native_queue_during_context_loss");
}
void ReloadTypedQueueAndTransition() {
    for (Action action : {Action::ReloadCheckpoint, Action::FullReload, Action::ExitFrontend}) {
        Fixture fixture; Start(fixture);
        simulated_reload::current = ReadyReloadState(); ScopedTick();
        for (Action rejected : {Action::ReloadCheckpoint, Action::FullReload, Action::ExitFrontend, Action::CopyReloadData})
            SubmitAction(rejected, static_cast<float>(0xfedcba99u));
        Check(commands.empty(), "new world actions accepted the lossy generic float API");
        SubmitReloadAction(Action::Heal, simulated_reload::current, 0);
        SubmitReloadAction(action, simulated_reload::current, 0x01000001u);
        Check(commands.empty(), "unsupported action or unlisted uint32 key entered typed queue");
        const auto captured = simulated_reload::current;
        SubmitReloadAction(action, captured, 0xfedcba99u);
        Check(commands.size() == 1 && commands.front().actorHandle == 0 && commands.front().value == 0 &&
              commands.front().checkpointKey == 0xfedcba99u &&
              SameReloadContext(commands.front().reload.token, captured.token) &&
              commands.front().reload.checkpoints == captured.checkpoints,
              "typed queued request lost its exact key/token or acquired an actor dependency");
        savedPosition = {{1,2,3}, fixture.handle, true}; undoPosition = {{4,5,6}, fixture.handle, true};
        healthEdit = {fixture.health.address(), fixture.handle, 80, 100}; autoHeal = true;
        fixture.health.set(0xd0, 100.0f); // No periodic heal is needed before transition.
        simulated_reload::onAccepted = [&] { fixture.poolPointer = 0; };
        ScopedTick();
        Check(simulated_reload::accepted.size() == 1 && simulated_reload::accepted[0].action == action,
              "verified scoped request did not reach the runtime boundary exactly once");
        Check((action != Action::ReloadCheckpoint && action != Action::FullReload) || simulated_reload::accepted[0].key == 0xfedcba99u,
              "native Reload boundary received a rounded checkpoint key");
        const auto snapshot = GetSnapshot();
        Check(!autoHeal && !healthEdit.component && !savedPosition.valid && !undoPosition.valid &&
              !snapshot.heroReady && !snapshot.healthReady && !snapshot.teleportReady && !snapshot.actorHandle &&
              !snapshot.autoHeal && !snapshot.savedPosition && !snapshot.undoPosition && snapshot.characterNames.empty() &&
              !snapshot.reload.ready, "accepted transition retained old per-instance actor state");
        Check(monitored_reads::afterTransition == 0 && positionCalls.empty() && characterCalls.empty() && explicitSetterCalls == 0,
              "backend read old world bytes or called an actor trap after accepted transition");
        Check(commands.empty(), "accepted world command was not retired");
    }
    passedCases.push_back("reload_exit_typed_uint32_queue_and_no_post_transition_actor_access");
}
void ReloadWithoutHeroAndStaleContext() {
    Fixture fixture; Start(fixture);
    simulated_reload::current = ReadyReloadState(); fixture.poolPointer = 0; ScopedTick();
    Check(!GetSnapshot().heroReady && GetSnapshot().reload.ready, "fixture did not remove hero independently of loaded level");
    const auto stale = simulated_reload::current;
    SubmitReloadAction(Action::ReloadCheckpoint, stale, 0xfedcba99u);
    ++simulated_reload::current.token.contextSerial;
    ScopedTick();
    Check(simulated_reload::attempts.size() == 1 && simulated_reload::accepted.empty() && commands.empty(),
          "stale captured world bypassed runtime boundary rejection");
    Check(GetSnapshot().status.find(L"stale/invalid") != std::wstring::npos, "stale request rejection was not diagnosed");
    SubmitReloadAction(Action::ReloadCheckpoint, simulated_reload::current, 0);
    ScopedTick();
    Check(simulated_reload::accepted.size() == 1 && simulated_reload::accepted.back().key == 0,
          "current-checkpoint sentinel acquired an unnecessary hero dependency");
    Check(positionCalls.empty() && characterCalls.empty() && explicitSetterCalls == 0,
          "actor-less world request called an unrelated native setter");
    passedCases.push_back("reload_requires_fresh_world_context_but_does_not_require_hero");
}
void ReloadContextAndWindowQueue() {
    Fixture fixture; Start(fixture); simulated_reload::current = ReadyReloadState(); ScopedTick();
    SubmitReloadAction(Action::ReloadCheckpoint, simulated_reload::current, 0xfedcba99u);
    SubmitReloadAction(Action::CopyReloadData, simulated_reload::current, 0xfedcba99u);
    simulated_dispatch::published = false;
    GameTick();
    Check(commands.size() == 2 && simulated_reload::attempts.empty(), "context loss executed or consumed a native world request");
    Command window{Action::Heal, 0};
    Check(TakeCommand(window, true) && window.action == Action::CopyReloadData && window.checkpointKey == 0xfedcba99u &&
          commands.size() == 1 && commands.front().action == Action::ReloadCheckpoint,
          "window-only dequeue did not isolate clipboard from native world request");
    Check(WindowAction(Action::CopyReloadData) && !WindowAction(Action::ReloadCheckpoint) && !WindowAction(Action::FullReload) && !WindowAction(Action::ExitFrontend),
          "native transition was classified as a window command");
    Check(!TakeCommand(window, true), "window-only dequeue consumed a native request while context was lost");
    simulated_dispatch::published = true; ScopedTick();
    Check(simulated_reload::accepted.size() == 1 && commands.empty(), "restored verified scope failed to service preserved request");
    passedCases.push_back("clipboard_remains_window_only_while_native_reload_waits_for_verified_scope");
}
void ReloadClipboardAndInvalidPayloads() {
    Fixture fixture; Start(fixture); const auto captured = ReadyReloadState();
    const std::wstring expected =
        L"SM2 Debug Menu - native reload request\r\n"
        L"Build: retail 1.0130.0.0\r\n"
        L"Checkpoint: High checkpoint\r\n"
        L"Selection: Named checkpoint in the loaded level\r\n"
        L"Request key: 0xfedcba99\r\n"
        L"Current checkpoint key: 0x87654321\r\n"
        L"Level asset: 0xfedcba9876543210\r\n"
        L"Captured selection only; loading state is rechecked when Reload is selected.\r\n";
    Check(ReloadClipboardText(captured, 0xfedcba99u) == expected, "clipboard text changed exact captured data or hex precision");
    const auto current = ReloadClipboardText(captured, 0);
    Check(current.find(L"Checkpoint: Current checkpoint\r\nSelection: Current checkpoint\r\nRequest key: 0x00000000\r\n") != std::wstring::npos,
          "clipboard current sentinel was confused with the active checkpoint hash");
    Check(ReloadClipboardText(captured, 0x01000001u).empty(), "clipboard fabricated a name for an unlisted checkpoint key");
    auto invalid = captured; invalid.ready = false;
    Check(ReloadClipboardText(invalid, 0).empty(), "clipboard accepted an unavailable captured world");
    SubmitReloadAction(Action::ReloadCheckpoint, invalid, 0);
    invalid = captured; invalid.frontendReady = false; SubmitReloadAction(Action::ExitFrontend, invalid, 0);
    previewMode = true; SubmitReloadAction(Action::ReloadCheckpoint, captured, 0); previewMode = false;
    Check(commands.empty() && simulated_reload::attempts.empty(), "invalid readiness/frontend/preview payload entered native queue");
    Check(!monitored_reads::transitionAccepted && positionCalls.empty() && characterCalls.empty(), "clipboard formatting invoked runtime or actor action");
    passedCases.push_back("clipboard_exact_captured_hex_and_invalid_typed_payloads_rejected_without_os_clipboard");
}
void PrimaryTypedQueueAndLifetime() {
    Fixture fixture;Start(fixture);simulated_reload::current=ReadyReloadState();simulated_primary::current=ReadyPrimaryState();ScopedTick();
    const auto captured=GetSnapshot().primary;
    SubmitAction(Action::CompletePrimary,static_cast<float>(captured.token.objectiveHash));
    Check(commands.empty(),"primary accepted generic lossy float submission");
    auto invalid=captured;invalid.token.missionHash=0;SubmitCompletePrimary(invalid);
    invalid=captured;invalid.token.objectiveHash=0;SubmitCompletePrimary(invalid);
    invalid=captured;invalid.supported=false;SubmitCompletePrimary(invalid);
    invalid=captured;invalid.ready=false;SubmitCompletePrimary(invalid);
    previewMode=true;SubmitCompletePrimary(captured);previewMode=false;
    Check(commands.empty() && simulated_primary::attempts.empty(),"invalid primary flags/zero hashes/preview entered native queue");
    SubmitCompletePrimary(captured);
    Check(commands.size()==1 && commands.front().action==Action::CompletePrimary && commands.front().actorHandle==0 &&
          commands.front().value==0 && commands.front().primary.token.missionHash==0xdeadbeefu &&
          commands.front().primary.token.objectiveHash==0xfedcba99u && commands.front().primary.token.objectiveNodeId==0x8000000087654321ull,
          "typed primary request lost native identity or acquired actor/float payload");
    savedPosition={{1,2,3},fixture.handle,true};undoPosition={{4,5,6},fixture.handle,true};
    healthEdit={fixture.health.address(),fixture.handle,80,100};autoHeal=true;fixture.health.set(0xd0,100.0f);
    simulated_primary::onAccepted=[&]{fixture.poolPointer=0;};ScopedTick();
    const auto snapshot=GetSnapshot();
    Check(simulated_primary::accepted.size()==1 && commands.empty(),"scoped primary command was not accepted/retired once");
    Check(!autoHeal && !healthEdit.component && !savedPosition.valid && !undoPosition.valid && !snapshot.heroReady &&
          !snapshot.healthReady && !snapshot.teleportReady && !snapshot.actorHandle && !snapshot.autoHeal &&
          !snapshot.savedPosition && !snapshot.undoPosition && snapshot.characterNames.empty() && !snapshot.primary.ready && !snapshot.reload.ready,
          "completion retained destroyed-world per-instance state");
    Check(monitored_reads::afterTransition==0 && positionCalls.empty() && characterCalls.empty() && explicitSetterCalls==0,
          "backend read old world or called actor setter after native primary completion");
    passedCases.push_back("primary_typed_uint32_identity_and_no_post_completion_actor_access");
}
void PrimaryScopeActorAndStaleGuards() {
    Fixture fixture;Start(fixture);simulated_reload::current=ReadyReloadState();simulated_primary::current=ReadyPrimaryState();
    fixture.poolPointer=0;ScopedTick();Check(!GetSnapshot().heroReady && GetSnapshot().primary.ready,"primary readiness acquired an actor dependency");
    SubmitCompletePrimary(simulated_primary::current);simulated_dispatch::published=false;GameTick();
    Check(commands.size()==1 && simulated_primary::attempts.empty(),"context loss consumed primary world command");
    Command window{Action::Heal,0};Check(!WindowAction(Action::CompletePrimary) && !TakeCommand(window,true) && commands.size()==1,
          "window-only route consumed native primary completion");
    simulated_dispatch::published=true;++simulated_primary::current.token.objectiveHash;ScopedTick();
    Check(simulated_primary::attempts.size()==1 && simulated_primary::accepted.empty() && commands.empty(),"stale primary bypassed runtime boundary rejection");
    Check(GetSnapshot().status.find(L"stale/invalid")!=std::wstring::npos,"stale primary rejection was not diagnosed");
    SubmitCompletePrimary(simulated_primary::current);ScopedTick();
    Check(simulated_primary::accepted.size()==1 && positionCalls.empty() && characterCalls.empty() && explicitSetterCalls==0,
          "fresh primary without actor failed or invoked unrelated setter");
    Check(monitored_reads::afterTransition==0,"actor-less completion read old engine data after acceptance");
    passedCases.push_back("primary_scope_window_exclusion_stale_rejection_and_actor_independence");
}
}

#ifndef BACKEND_PERFORMANCE_FIXTURE
int main(int argc, char** argv) {
    try {
        (void)&Exercise; // Existing raw fixture stays compiled but is not executed.
        outputPath = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path() / L"hero-actions-fixture";
        std::filesystem::create_directories(outputPath);
        SaveLoadUndo(); SixDirections(); PositionValidation(); StaleGeneration();
        CharacterArguments(); CharacterValidation(); LostContextWindowPath(); ForeignThreadDenied();
        ReloadTypedQueueAndTransition(); ReloadWithoutHeroAndStaleContext();
        ReloadContextAndWindowQueue(); ReloadClipboardAndInvalidPayloads();
        PrimaryTypedQueueAndLifetime();PrimaryScopeActorAndStaleGuards();
        ActorResetPreflightAndOutcomeOrder();ReplayTypedQueueAndInvocationLifetime();ReplaySelectionCopyAndLostContext();
        std::cout << "{\"passed\":true,\"assertions\":" << assertions << ",\"cases\":[";
        for (size_t i = 0; i < passedCases.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << '"' << passedCases[i] << '"';
        }
        std::cout << "],\"scoped_ticks\":" << simulated_dispatch::scopedTicks
            << ",\"dispatcher_boundary\":\"simulated_test_only\",\"reload_runtime_boundary\":\"simulated_guard_and_acceptance_only\",\"primary_runtime_boundary\":\"simulated_guard_and_acceptance_only\",\"actor_save_boundary\":\"simulated_reset_outcomes_only\",\"replay_boundary\":\"simulated_invoked_accepted_outcomes_only\",\"catalog\":\"actual_immutable_production_catalog\",\"os_clipboard_changed\":false,\"real_engine_context_proven\":false,\"game_loaded\":false,\"game_started\":false,\"game_tls_accessed\":false,\"hooks_installed\":false,\"windows_created\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"passed\":false,\"assertions\":" << assertions << ",\"error\":\"" << error.what()
            << "\",\"real_engine_context_proven\":false,\"game_loaded\":false,\"game_started\":false}\n";
        return 1;
    }
}

#endif
