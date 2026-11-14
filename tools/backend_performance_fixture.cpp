// Runs the production backend with the existing guarded synthetic actor and
// runtime boundary doubles. Work counts measure calls, not game frame rate.
#ifndef EXISTING_HERO_CASES
#define BACKEND_PERFORMANCE_FIXTURE
#endif
#define SnapshotDebugSettings CountedSnapshotDebugSettings
#include "hero_actions_fixture.cpp"
#undef SnapshotDebugSettings
#include <chrono>

namespace sm2debug {
std::vector<DebugSettingValue> SnapshotDebugSettings();
std::vector<DebugSettingValue> CountedSnapshotDebugSettings() {
    ++snapshot_reads::settings;
    return SnapshotDebugSettings();
}
void InitializeReloadOptionsRuntime(HMODULE, bool) {}
ReloadOptionsState ReadReloadOptionsState(const ReloadState& world) {
    ++snapshot_reads::options;
    ReloadOptionsState value; value.supported=value.ready=world.ready; return value;
}
bool ExecuteSetNewGamePlus(const ReloadOptionsState&,bool,std::wstring&) { return false; }
bool PrepareReloadLoadoutAfterAdmission(const ReloadOptionsState&,const ReloadState&,std::wstring&) { return false; }
void InitializeCameraDebugRuntime(HMODULE,bool) {}
CameraDebugState ReadCameraDebugState(const ReloadState& world) {
    ++snapshot_reads::camera;
    CameraDebugState value; value.supported=value.ready=world.ready; return value;
}
bool ExecuteSetCameraShake(const CameraDebugState&,bool,std::wstring&) { return false; }
bool ResetOwnedCameraDebugOverrides(std::wstring&) { return true; }
void InitializeReloadSelectionRuntime(HMODULE,bool) {}
ReloadSelectionState ReadReloadSelectionState(const ReloadState&,const ReloadMissionSelection&) {
    ++snapshot_reads::selection; return {};
}
bool PrepareReloadSelectionAfterAdmission(const ReloadSelectionState&,const ReloadState&,std::wstring&) { return false; }
void QueueMenuInput(MenuInput) {}
// The older action fixture predates the optional preparation arguments.
bool ExecuteReload(const ReloadState& captured,uint32_t key,std::wstring& status,ReloadPreparation prepare,void*) {
    return !prepare&&FixtureReload(Action::ReloadCheckpoint,captured,key,status);
}
bool ExecuteFullReload(const ReloadState& captured,uint32_t key,std::wstring& status,ReloadPreparation prepare,void*) {
    return !prepare&&FixtureReload(Action::FullReload,captured,key,status);
}
bool ExecuteExitLobby(const ReloadState& captured,std::wstring& status) {
    return FixtureReload(Action::ExitLobby,captured,0,status);
}
ReplayResult ExecuteReplay(const ReplayState& captured,std::wstring& status,ReloadPreparation prepare,void*) {
    const auto legacy=static_cast<ReplayResult(*)(const ReplayState&,std::wstring&)>(&ExecuteReplay);
    return prepare?ReplayResult{}:legacy(captured,status);
}
}

#ifndef EXISTING_HERO_CASES
namespace {
void ResetReads() {
    using namespace snapshot_reads;
    reload=primary=actor=replay=options=camera=selection=settings=0;
}
unsigned FullReads() {
    using namespace snapshot_reads;
    return reload+primary+actor+replay+options+camera+selection+settings;
}
void SetVisible(bool value) {
    fixtureMenuVisible=value;
#ifdef AFTER_GATING
    InvalidateMenuSnapshot();
#endif
}
}
int main(int argc,char**argv) {
    try {
#ifdef EXISTING_RAW_CASES
        return RawSnapshotFixtureMain();
#endif
        (void)&Exercise;
        // Keep the existing action cases compiled in this same translation unit.
        (void)&SaveLoadUndo;(void)&SixDirections;(void)&PositionValidation;(void)&StaleGeneration;
        (void)&CharacterArguments;(void)&CharacterValidation;(void)&LostContextWindowPath;(void)&ForeignThreadDenied;
        (void)&ReloadTypedQueueAndTransition;(void)&ReloadWithoutHeroAndStaleContext;
        (void)&ReloadContextAndWindowQueue;(void)&ReloadClipboardAndInvalidPayloads;
        (void)&PrimaryTypedQueueAndLifetime;(void)&PrimaryScopeActorAndStaleGuards;
        (void)&ActorResetPreflightAndOutcomeOrder;(void)&ReplayTypedQueueAndInvocationLifetime;(void)&ReplaySelectionCopyAndLostContext;
        outputPath=argc>1?std::filesystem::path(argv[1]):std::filesystem::current_path();
        Fixture fixture;Start(fixture);
        simulated_reload::current=ReadyReloadState();simulated_primary::current=ReadyPrimaryState();
        simulated_actor_save::current=ReadyActorSaveState();simulated_replay::current=ReadyReplayState();
        ScopedTick();
        constexpr unsigned ticks=1000;
        SetVisible(false);ResetReads();
        const auto started=std::chrono::steady_clock::now();
        for(unsigned i=0;i<ticks;++i){ScopedTick();TickBackend();}
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
        const auto hiddenReads=FullReads();
        std::cout<<"{\"phase\":\"hidden\",\"ticks\":"<<ticks<<",\"menu_snapshot_calls\":"<<hiddenReads
                 <<",\"reload\":"<<snapshot_reads::reload<<",\"settings\":"<<snapshot_reads::settings
                 <<",\"elapsed_us\":"<<elapsed<<"}\n";
        Check(hiddenReads==0,"hidden idle backend still refreshes expensive menu snapshots");
        Check(!GetSnapshot().reload.ready&&!GetSnapshot().primary.ready&&!GetSnapshot().cameraDebug.ready,
              "hidden snapshot retained interactive readiness");
        SetVisible(true);
        Check(!GetSnapshot().reload.ready,"opening exposed stale ready world before the next engine tick");
        ResetReads();ScopedTick();TickBackend();
        Check(snapshot_reads::reload==1&&snapshot_reads::primary==1&&snapshot_reads::actor==1&&snapshot_reads::replay==1&&
              snapshot_reads::options==1&&snapshot_reads::camera==1&&snapshot_reads::settings==1,"visible snapshot lost a reader");
        Check(GetSnapshot().reload.ready&&GetSnapshot().primary.ready,"visible tick failed to publish current readiness");
        SetVisible(false);SetVisible(true);
        Check(!GetSnapshot().reload.ready,"rapid reopen reused a stale ready snapshot");
        snapshot_reads::onReload=[] {
            std::thread notifier([] { SetVisible(false); SetVisible(true); });
            notifier.join();
        };
        ScopedTick();
        Check(!GetSnapshot().reload.ready,"in-flight snapshot was published across a close/reopen generation");
        ScopedTick();
        Check(GetSnapshot().reload.ready,"fresh generation did not recover after racing notification");
        SubmitAction(Action::Heal);SetVisible(false);ResetReads();ScopedTick();
        Check(snapshot_reads::reload==1&&snapshot_reads::actor==1&&commands.empty()&&explicitSetterCalls==1,
              "queued action did not refresh and execute while hidden");
        Check(!GetSnapshot().reload.ready,"hidden queued command republished interactive readiness");
        ResetReads();autoHeal=true;fixture.health.set(0xd0,40.0f);ScopedTick();
        Check(FullReads()==0&&explicitSetterCalls==2,"hidden autoheal stopped or refreshed menu-only state");
        fixture.pool.set(0xc8,uint16_t(6));ScopedTick();
        Check(!autoHeal&&explicitSetterCalls==2,"changed actor generation retained autoheal");
        fixture.pool.set(0xc8,uint16_t(7));
        simulated_reload::pending=true;ResetReads();ScopedTick();
        Check(snapshot_reads::reload==1&&FullReads()==1,"pending reload did not receive isolated transition observation");
        simulated_reload::pending=false;simulated_replay::pending=true;ResetReads();ScopedTick();
        Check(snapshot_reads::replay==1&&FullReads()==1,"pending replay did not receive isolated transition observation");
        simulated_replay::pending=false;ResetReads();ScopedTick();
        Check(FullReads()==0,"completed transition retained hidden polling");
        settingsRestorePending=true;shutdownReset=true;shutdownResetComplete=false;ResetReads();ScopedTick();
        Check(!settingsRestorePending&&shutdownResetComplete&&FullReads()==0,"hidden restore/shutdown was skipped");
        shutdownReset=false;
        std::cout<<"{\"passed\":true,\"assertions\":"<<assertions<<",\"hidden_snapshot_calls\":0,\"ticks\":"<<ticks
                 <<",\"game_launched\":false,\"timing_is_synthetic_not_fps\":true}\n";
        return 0;
    } catch(const std::exception& error) {
        std::cout<<"{\"passed\":false,\"error\":\""<<error.what()<<"\"}\n";return 1;
    }
}
#endif
