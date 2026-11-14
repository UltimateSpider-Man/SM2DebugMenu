// Synthetic current-process memory and original PE pins. Catalog and engine
// predicate/invocation are explicit boundaries; no game/TLS/window is used.
#define wmain PrimaryFixtureMain
#include "primary_objective_fixture.cpp"
#undef wmain
#define ReadProcessMemory primary_fixture_reads::Read
#include "../src/replay_runtime.cpp"
#undef ReadProcessMemory

namespace {
constexpr std::uint32_t replayMission=0x86af55bf,replayObjective=0xa05308f5,replayCheckpoint=0x13572469;
constexpr std::uint64_t replayGraph=0x9876543210abcdefull;
constexpr char replayMissionName[]="FixtureMission",replayObjectiveName[]="FixtureObjective";
unsigned replayPreflightCalls=0,replayPredicateCalls=0,replayNativeCalls=0;
bool preflightAllowed=true,replayAllowed=true,nativeAccepted=true;
int replayRevokeAt=0;bool changeReplayTarget=false,changeReplayWorld=false;
std::uintptr_t replayNode=0,replayDefinition=0;
std::uint32_t replayReceivedObjective=0;
bool ReplayPreflightTrap(const ReloadState& captured,std::wstring& status,std::uint32_t key){
    ++replayPreflightCalls;Require(scope,"preflight outside fixture callback");Require(key==0,"replay must not send a second checkpoint request");
    if(replayRevokeAt==1)scope=false;
    status=L"Fixture read-only preflight";return preflightAllowed&&captured.ready;
}
bool ReplayPredicateTrap(void* node,bool allowCurrent){
    ++replayPredicateCalls;Require(scope,"predicate outside fixture callback");
    Require(reinterpret_cast<std::uintptr_t>(node)==replayNode&&allowCurrent,"replay predicate ABI mismatch");
    if(replayRevokeAt==2)scope=false;
    if(changeReplayTarget)*reinterpret_cast<std::int32_t*>(replayDefinition+0x1d0)=0;
    if(changeReplayWorld)++fixtureWorld.token.contextSerial;
    return replayAllowed;
}
bool ReplayNativeTrap(void* node,bool shouldReload,bool nativeArg2,bool nativeArg3,std::uint32_t objective){
    ++replayNativeCalls;Require(scope,"native replay outside fixture callback");
    Require(reinterpret_cast<std::uintptr_t>(node)==replayNode&&shouldReload&&nativeArg2&&!nativeArg3,"canonical replay ABI mismatch");
    replayReceivedObjective=objective;primary_fixture_reads::completed=true;return nativeAccepted;
}
const std::vector<ReloadMissionGraph>& FixtureCatalog(){
    static const std::vector<ReloadMissionGraph> data=[] {
        ReloadMissionObjective objective;objective.name=replayObjectiveName;objective.nameCrc=replayObjective;objective.checkpoint.name="FixtureCheckpoint";objective.checkpoint.nameCrc=replayCheckpoint;
        ReloadMission mission;mission.name=replayMissionName;mission.nameCrc=replayMission;mission.objectiveGraphAssetId=replayGraph;mission.objectives={objective};
        ReloadMissionGraph graph;graph.graphAssetId=replayGraph;graph.graphPath="fixture/main_missions";graph.missions={mission};return std::vector<ReloadMissionGraph>{graph};
    }();return data;
}
void VerifyReplayFixturePins(const ReplayImage& image){
    Require(ReplayBytes(image,0x15517a0,"48895c241048896c2418488974242057415641574883ec40488b01410fb6e90f"),"fixture copied byte pin 0x15517a0");
    Require(ReplayBytes(image,0x15516f0,"48895c2408574883ec200fb6fa488bd984d2752433d2488d0dc37a150ae84e3e"),"fixture copied byte pin 0x15516f0");
    Require(ReplayBytes(image,0x358eec0,"8b819000000083f804741384d2740c83f802740a83f8030f94c0c332c0c3b001c3"),"fixture copied byte pin 0x358eec0");
    Require(ReplayBytes(image,0x358eef0,"48895c24104889742418574883ec40488b010fb6f2410fb6d0410fb6f9488bd9"),"fixture copied byte pin 0x358eef0");
    Require(ReplayBytes(image,0x153ee10,"48895c240848896c24104889742418574883ec20410fb6f1410fb6f88bda488b"),"fixture copied byte pin 0x153ee10");
    Require(ReplayBytes(image,0x358f7f0,"488b4118488b889c000000488bc248890ac3"),"fixture copied byte pin 0x358f7f0");
    Require(ReplayBytes(image,0x3582410,"4883ec28488b124883c168e8209e1f004885c07409488b40284883c428c34883c428c3"),"fixture copied byte pin 0x3582410");
    Require(ReplayBytes(image,0x3582450,"4883ec28488bc24885d27419418bd0488bc8e8f99d1f004885c07409488b40284883c428c333c04883c428c3"),"fixture copied byte pin 0x3582450");
    Require(ReplayBytes(image,0x377c240,"4883c128e9a78191ff"),"fixture copied byte pin 0x377c240");
    Require(ReplayBytes(image,0x377c260,"4881c148010000e9848091ff"),"fixture copied byte pin 0x377c260");
    Require(ReplayBytes(image,0x15518a4,"4585f6743f488d542460488bcbe83adf0302"),"fixture copied byte pin 0x15518a4");
    Require(ReplayBytes(image,0x15518d1,"458bc6488bd0e8740b03024885c074078b784c85ff7574"),"fixture copied byte pin 0x15518d1");
    Require(ReplayBytes(image,0x15519c4,"488905397f150ac6052e7f150a01e8e976f701"),"fixture copied byte pin 0x15519c4");
    Require(ReplayBytes(image,0x15519e6,"4533c08bd7488bcbe89da2f701b001"),"fixture copied byte pin 0x15519e6");
    Require(ReplayFunction(image,0x15517a0,0x1551a12,0x92e1a1c),"fixture pdata 0x15517a0");
    Require(ReplayFunction(image,0x15516f0,0x1551796,0x92e1a10),"fixture pdata 0x15516f0");
    Require(ReplayFunction(image,0x358eef0,0x358f04d,0x966049c),"fixture pdata 0x358eef0");
    Require(ReplayFunction(image,0x153ee10,0x153eea6,0x92dce18),"fixture pdata 0x153ee10");
    Require(ReplayRtti(image,0x548a108,0x8e05428,0xab49d50,".?AVMissionNodeGame@@"),"fixture type 0x548a108");
    Require(ReplayRtti(image,0x86a3d48,0x901d610,0xac1dd20,".?AVObjectiveGraphBuiltGame@@"),"fixture type 0x86a3d48");
    Require(ReplayRtti(image,0x86a3ca8,0x901d588,0xac1dcf0,".?AVObjectiveNodeBuiltGame@@"),"fixture type 0x86a3ca8");
}
struct ReplayHost:Host {
    std::uintptr_t built=0,builtObjective=0;
    explicit ReplayHost(const wchar_t* path):Host(path,replayMission,replayObjective){
        for(const auto& pin:std::vector<std::pair<std::uintptr_t,std::size_t>>{{0x15517a0,32},{0x15516f0,32},{0x358eec0,33},{0x358eef0,32},{0x153ee10,32},
            {0x358f7f0,18},{0x3582410,35},{0x3582450,44},{0x377c240,9},{0x377c260,12},{0x15518a4,18},{0x15518d1,23},{0x15519c4,19},{0x15519e6,15}})Copy(pin.first,pin.second);
        for(const auto& type:std::vector<std::array<std::uintptr_t,3>>{{0x86a3d48,0x901d610,0xac1dd20},{0x86a3ca8,0x901d588,0xac1dcf0}}){Copy(type[1],24);Copy(type[2],96);Put(type[0]-8,base+type[1]);}
        Put(0x548a108+0x118,base+0x15516f0);Put(0x548a108+0x120,base+0x15517a0);
        const std::array<RUNTIME_FUNCTION,4> table{{{0x153ee10,0x153eea6,{0x92dce18}},{0x15516f0,0x1551796,{0x92e1a10}},
            {0x15517a0,0x1551a12,{0x92e1a1c}},{0x358eef0,0x358f04d,{0x966049c}}}};
        Commit(0x110000,sizeof(table));std::memcpy(reinterpret_cast<void*>(base+0x110000),table.data(),sizeof(table));
        built=Allocate(0x1000);builtObjective=Allocate(0x1000);ResetReplay();
    }
    void ResetReplay(){
        Host::Reset();replayPreflightCalls=replayPredicateCalls=replayNativeCalls=0;preflightAllowed=replayAllowed=nativeAccepted=true;
        replayRevokeAt=0;changeReplayTarget=changeReplayWorld=false;replayReceivedObjective=0;
        fixtureWorld.token.managerState=22;Put(0xb26b150+0x430628,std::int32_t(22));
        fixtureWorld.checkpoints=std::make_shared<const std::vector<ReloadCheckpoint>>(std::vector<ReloadCheckpoint>{{replayCheckpoint,0,L"FixtureCheckpoint"}});
        Set(mission,0x90,std::int32_t(2));Set(missionDefinition,0x78,reinterpret_cast<std::uintptr_t>(replayMissionName));
        Set(missionDefinition,0x9c,graphAsset);Set(missionDefinition,0x1d0,std::int32_t(1));
        Set(built,0,base+0x86a3d48);Set(builtObjective,0,base+0x86a3ca8);
        Set(builtObjective,0x28,reinterpret_cast<std::uintptr_t>(replayObjectiveName));Set(builtObjective,0x4c,replayCheckpoint);
        Map<std::uint64_t>(base+0xb6a91d0+0x90,{{graphAsset,built}},0x40);
        Map<std::uint32_t>(built+0x148,{{objectiveCRC,builtObjective}},0x40);
        replayNode=mission;replayDefinition=missionDefinition;
        std::lock_guard lock(replayRuntime.mutex);replayRuntime.image={base,base+0x110000,4};VerifyReplayFixturePins(replayRuntime.image);Require(ReplayPins(replayRuntime.image),"replay code/type/pdata pins match actual PE");
        replayRuntime.supported=true;replayRuntime.predicate=&ReplayPredicateTrap;replayRuntime.replay=&ReplayNativeTrap;
        replayRuntime.awaitingTransition=replayRuntime.transitionSeen=false;replayRuntime.invokedWorld={};++replayRuntime.epoch;
    }
    ReplaySelection Selection(bool explicitObjective=false){return {graphAsset,missionCRC,explicitObjective?objectiveCRC:0};}
    ReplayState ReadyReplay(bool explicitObjective=false){auto origin=Ready();auto result=ReadReplayState(origin,Selection(explicitObjective));Require(result.ready,"valid scoped replay target unreadable");return result;}
};
void ReplayNoNative(){Require(replayNativeCalls==0,"rejected replay reached native invocation");}
struct PreparationProbe { unsigned calls=0; bool allow=true, revoke=false, replaceWorld=false; };
bool PreparationTrap(const ReloadState& admitted,void* opaque,std::wstring& status){
    auto& probe=*static_cast<PreparationProbe*>(opaque);++probe.calls;
    Require(replayPreflightCalls==2&&replayPredicateCalls==1&&replayNativeCalls==0,"preparation ran before native replay admission");
    Require(SameReloadContext(admitted.token,fixtureWorld.token),"preparation received stale world");
    if(probe.revoke)scope=false;
    if(probe.replaceWorld)++fixtureWorld.token.contextSerial;
    status=L"Fixture preparation refused";return probe.allow;
}
}
namespace sm2debug {
const std::vector<ReloadMissionGraph>& ReloadMissionCatalog(){return FixtureCatalog();}
const ReloadMissionGraph* FindReloadMissionGraph(std::uint64_t graph){const auto& data=FixtureCatalog();return graph==replayGraph?&data[0]:nullptr;}
const ReloadMission* FindReloadMission(std::uint64_t graph,std::uint32_t mission){auto p=FindReloadMissionGraph(graph);return p&&mission==replayMission?&p->missions[0]:nullptr;}
const ReloadMissionObjective* FindReloadMissionObjective(std::uint64_t graph,std::uint32_t mission,std::uint32_t objective){auto p=FindReloadMission(graph,mission);return p&&objective==replayObjective?&p->objectives[0]:nullptr;}
bool CheckReloadExecutionContext(const ReloadState& captured,std::wstring& status,std::uint32_t key){return ReplayPreflightTrap(captured,status,key);}
}
int wmain(int argc,wchar_t** argv){
    try{
        (void)&PrimaryFixtureMain;assertions=cases=0;Require(argc==2,"fixture requires pinned executable path");
        InitializeReplayRuntime(GetModuleHandleW(nullptr),true);Require(!ReadReplayState({},{}).supported,"wrong host executable accepted for replay");++cases;
        ReplayHost host(argv[1]);auto request=host.ReadyReplay();Require(request.origin.token.missionGraphAssetId==host.graphAsset,"origin graph ID not captured");
        Require(replayPreflightCalls+replayPredicateCalls+replayNativeCalls==0,"replay snapshot invoked native preflight/predicate");++cases;
        std::wstring status;auto result=ExecuteReplay(request,status);Require(!result.invoked&&!result.accepted,"replay invoked outside callback");ReplayNoNative();++cases;
        for(const auto selection:std::vector<ReplaySelection>{{0,replayMission,0},{replayGraph+1,replayMission,0},{replayGraph,1,0},{replayGraph,replayMission,1}}){host.ResetReplay();Require(!ReadReplayState(host.Ready(),selection).ready,"uncataloged/cross-graph target accepted");}++cases;
        host.ResetReplay();host.Set(host.missionDefinition,0x78,reinterpret_cast<std::uintptr_t>("OtherMission"));Require(!ReadReplayState(host.Ready(),host.Selection()).ready,"CRC collision/wrong live mission name accepted");++cases;
        for(const auto state:{0,1,5}){host.ResetReplay();host.Set(host.mission,0x90,std::int32_t(state));Require(!ReadReplayState(host.Ready(),host.Selection()).ready,"unadmitted mission replay state accepted");}++cases;
        for(const auto type:{0,3}){host.ResetReplay();host.Set(host.missionDefinition,0x1d0,std::int32_t(type));Require(!ReadReplayState(host.Ready(),host.Selection()).ready,"unknown replay policy accepted");}++cases;
        host.ResetReplay();fixtureWorld.token.mode=fixtureWorld.token.kernel=0;fixtureWorld.token.modeValue=-1;fixtureWorld.token.kernelState=0;host.Put(0xc21d330,std::uintptr_t(0));
        Require(!ReadReplayState(host.Ready(),host.Selection()).ready,"replay admitted absent native mode/kernel");++cases;
        for(auto index:{std::uint32_t(0),std::uint32_t(0x80000000),std::uint32_t(0xffffffff),std::uint32_t(0x86af55bf)}){
            host.ResetReplay();auto cached=host.Allocate(0x1000);host.Set(cached,0,host.base+0x548ab78);host.Set(cached,0x108,index);
            host.Set(host.mission,0xb8,cached);host.Set(host.missionDefinition,0x1d0,std::int32_t(2));host.Put(0xba9b580,std::uint8_t(1));
            const auto unsupported=ReadReplayState(host.Ready(),host.Selection());
            Require(!unsupported.ready&&unsupported.status.find(L"policy")!=std::wstring::npos,"unproved type2 index policy accepted or unexplained");
            Require(replayPreflightCalls+replayPredicateCalls+replayNativeCalls==0,"type2 snapshot reached native predicate");
        }++cases;
        host.ResetReplay();request=host.ReadyReplay();host.Set(host.missionDefinition,0x1d0,std::int32_t(2));scope=true;
        result=ExecuteReplay(request,status);Require(!result.invoked&&replayPreflightCalls+replayPredicateCalls+replayNativeCalls==0,"queued policy change reached native predicate");++cases;
        host.ResetReplay();host.Set(host.builtObjective,0x4c,std::uint32_t(0));Require(!ReadReplayState(host.Ready(),host.Selection(true)).ready,"explicit objective silently fell back from zero checkpoint");++cases;
        host.ResetReplay();fixtureWorld.checkpoints=std::make_shared<const std::vector<ReloadCheckpoint>>();Require(!ReadReplayState(host.Ready(),host.Selection(true)).ready,"explicit objective invented an unloaded checkpoint key");++cases;
        host.ResetReplay();host.Set(host.builtObjective,0x28,reinterpret_cast<std::uintptr_t>("OtherObjective"));Require(!ReadReplayState(host.Ready(),host.Selection(true)).ready,"wrong live objective definition accepted");++cases;
        host.ResetReplay();request=host.ReadyReplay();++fixtureWorld.token.contextSerial;scope=true;result=ExecuteReplay(request,status);Require(!result.invoked&&replayPredicateCalls==0,"stale origin reached native predicate");++cases;
        host.ResetReplay();request=host.ReadyReplay();preflightAllowed=false;scope=true;result=ExecuteReplay(request,status);Require(!result.invoked&&replayPredicateCalls==0,"failed shared preflight reached replay predicate");++cases;
        for(int at:{1,2}){host.ResetReplay();request=host.ReadyReplay();replayRevokeAt=at;scope=true;result=ExecuteReplay(request,status);Require(!result.invoked,"revoked callback reached native replay");ReplayNoNative();}++cases;
        host.ResetReplay();request=host.ReadyReplay();replayAllowed=false;scope=true;result=ExecuteReplay(request,status);Require(!result.invoked,"native predicate refusal was ignored");++cases;
        host.ResetReplay();request=host.ReadyReplay();changeReplayTarget=true;scope=true;result=ExecuteReplay(request,status);Require(!result.invoked,"target changed by predicate reached native replay");++cases;
        host.ResetReplay();request=host.ReadyReplay();changeReplayWorld=true;scope=true;result=ExecuteReplay(request,status);Require(!result.invoked,"world changed by predicate reached native replay");++cases;
        for(int refusal:{0,1}){host.ResetReplay();request=host.ReadyReplay();scope=true;PreparationProbe probe;
            if(refusal==0)preflightAllowed=false;else replayAllowed=false;
            result=ExecuteReplay(request,status,PreparationTrap,&probe);
            Require(!result.invoked&&probe.calls==0,"failed native admission ran save preparation");++cases;}
        host.ResetReplay();request=host.ReadyReplay();scope=true;PreparationProbe refused;refused.allow=false;
        result=ExecuteReplay(request,status,PreparationTrap,&refused);
        Require(!result.invoked&&refused.calls==1&&replayNativeCalls==0&&!replayRuntime.awaitingTransition&&status==L"Fixture preparation refused",
            "preparation refusal invoked/rearmed/lost reason");++cases;
        for(int change:{0,1}){host.ResetReplay();request=host.ReadyReplay();scope=true;PreparationProbe probe;
            probe.revoke=change==0;probe.replaceWorld=change==1;
            result=ExecuteReplay(request,status,PreparationTrap,&probe);
            Require(!result.invoked&&probe.calls==1&&replayNativeCalls==0&&!replayRuntime.awaitingTransition,
                "world or callback changed by preparation reached native replay");++cases;}
        for(bool accepted:{false,true}){host.ResetReplay();request=host.ReadyReplay(true);scope=true;nativeAccepted=accepted;PreparationProbe probe;
            result=ExecuteReplay(request,status,PreparationTrap,&probe);
            Require(result.invoked&&result.accepted==accepted&&probe.calls==1&&replayNativeCalls==1&&replayRuntime.awaitingTransition,
                "prepared replay lost native result or called preparation twice");
            Require(primary_fixture_reads::afterComplete==0,"prepared replay read old world after invocation");++cases;}
        for(bool explicitObjective:{false,true}){host.ResetReplay();request=host.ReadyReplay(explicitObjective);scope=true;result=ExecuteReplay(request,status);
            Require(result.invoked&&result.accepted&&replayNativeCalls==1&&replayPreflightCalls==1&&replayPredicateCalls==1,"valid native replay sequence not invoked once");
            Require(replayReceivedObjective==(explicitObjective?replayObjective:0),"native replay objective CRC lost precision or canonical zero");
            Require(primary_fixture_reads::afterComplete==0,"old engine objects read after replay invocation");
            primary_fixture_reads::completed=false;auto duplicate=ExecuteReplay(request,status);Require(!duplicate.invoked&&replayNativeCalls==1,"queued duplicate replay invoked twice");
            Require(!ReadReplayState(host.Ready(),host.Selection(explicitObjective)).ready,"invoked replay republished ready before transition");}++cases;
        host.ResetReplay();request=host.ReadyReplay();nativeAccepted=false;scope=true;result=ExecuteReplay(request,status);Require(result.invoked&&!result.accepted,"native false result lost invoked/accepted distinction");
        primary_fixture_reads::completed=false;Require(!ExecuteReplay(request,status).invoked&&!ReadReplayState(host.Ready(),host.Selection()).ready,"native false bookkeeping could immediately replay twice");++cases;
        fixtureWorld.ready=false;Require(!ReadReplayState(request.origin,host.Selection()).ready,"invalid phase rearmed replay prematurely");fixtureWorld.ready=true;
        auto rearmed=ReadReplayState(host.Ready(),host.Selection());Require(rearmed.ready&&rearmed.requestEpoch!=request.requestEpoch,"observed transition did not rearm a new request epoch");
        Require(!ExecuteReplay(request,status).invoked,"old queued epoch invoked after rearm");++cases;
        host.ResetReplay();request=host.ReadyReplay();scope=true;Require(ExecuteReplay(request,status).invoked,"setup replay failed");primary_fixture_reads::completed=false;++fixtureWorld.token.contextSerial;
        rearmed=ReadReplayState(host.Ready(),host.Selection());Require(rearmed.ready&&!ExecuteReplay(request,status).invoked,"new context did not rearm or accepted old request");++cases;
        std::cout<<"{\"passed\":true,\"cases\":"<<cases<<",\"assertions\":"<<assertions<<",\"native_boundary\":\"fixture_traps_only\",\"catalog_boundary\":\"synthetic_typed_fixture_catalog\",\"snapshot_native_calls\":0,\"game_started\":false,\"game_tls_accessed\":false,\"hooks_installed\":false,\"windows_created\":false}\n";return 0;
    }catch(const std::exception& error){std::cout<<"{\"passed\":false,\"cases\":"<<cases<<",\"assertions\":"<<assertions<<",\"error\":\""<<error.what()<<"\"}\n";return 1;}
}
