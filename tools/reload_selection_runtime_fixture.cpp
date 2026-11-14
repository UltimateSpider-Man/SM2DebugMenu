// Production readers/writer over synthetic owned memory and actual retail PE
// code/type pins. Does not load or execute the game or touch any user save.
#define wmain UnusedSelectionReloadFixtureMain
#include "reload_runtime_fixture.cpp"
#undef wmain
#include <atomic>
#include <functional>
namespace selection_fixture_reads {
std::uintptr_t trigger=0; std::function<void()> mutation;
BOOL WINAPI Read(HANDLE process,LPCVOID source,LPVOID target,SIZE_T size,SIZE_T* copied) {
    const BOOL result=::ReadProcessMemory(process,source,target,size,copied);
    if (reinterpret_cast<std::uintptr_t>(source)==trigger && mutation) {
        auto action=std::move(mutation); trigger=0; mutation={}; action();
    }
    return result;
}
}
#define ReadProcessMemory selection_fixture_reads::Read
#include "../src/reload_selection_runtime.cpp"
#undef ReadProcessMemory
#include "../src/reload_mission_catalog.cpp"

namespace {
using namespace sm2debug;
unsigned selectionCases=0;
struct SelectionHost : HostImage {
    const ReloadMissionGraph& graphData=ReloadMissionCatalog().front();
    const ReloadMission& missionData=graphData.missions.front();
    const ReloadMission& secondMission=graphData.missions.at(1);
    const ReloadMissionObjective& objectiveData=missionData.objectives.front();
    const ReloadMissionObjective& secondObjective=missionData.objectives.at(1);
    std::uintptr_t system=0,graph=0,mission=0,mission2=0,objectives=0,objectives2=0,objective=0,objective2=0;
    std::uintptr_t missionKeys=0,missionValues=0,graphKeys=0,graphValues=0,objectiveOrder=0;
    explicit SelectionHost(const wchar_t* path):HostImage(path) {
        system=base+0xb6a91d0; Commit(0xb6a91d0,0x760);
        graph=Allocate(0x1000); mission=Allocate(0x1000); mission2=Allocate(0x1000);
        objectives=Allocate(0x1000);objectives2=Allocate(0x1000);objective=Allocate(0x1000);objective2=Allocate(0x1000);
    }
    void Contracts() {
        for (const auto& pin:selectionPins) CopyFile(pin.rva,pin.size);
        for (const auto& type:selectionTypes) {CopyFile(type.col,24);CopyFile(type.td,96);Put(type.vt-8,base+type.col);}
        for (const auto& pin:std::vector<std::pair<std::uintptr_t,std::size_t>>{
            {0xaa41f00,0x90},{0x86a3b00,0x90},{0xa78ac20,0x90},{0x83fbeb0,16}}) CopyFile(pin.first,pin.second);
        Put(0xaa41f00+0x60,base+0x86a3b10);Put(0xaa41f00+0x68,base+0x86a3b44);Put(0xaa41f00+0x88,base+0x86a3b60);
        Put(0xa78ac20+0x88,base+0x83fbeb0);
    }
    template<class Key> std::pair<std::uintptr_t,std::uintptr_t> Map(std::uintptr_t header,
        const std::vector<std::pair<Key,std::uintptr_t>>& rows) {
        const auto keys=Allocate(0x1000),values=Allocate(0x1000); const std::uint32_t capacity=8;
        std::memset(reinterpret_cast<void*>(header),0,40); Object(header,0,keys);Object(header,8,values);
        Object(header,0x10,static_cast<std::uint32_t>(rows.size()));Object(header,0x14,capacity);
        Object(header,sizeof(Key)==8?0x20:0x1c,std::uint32_t(64));
        for (const auto& row:rows) {
            const std::uint64_t product=static_cast<std::uint64_t>(row.first)*0x9e3779b1ull;
            auto slot=static_cast<std::uint32_t>((product>>32)^product)%capacity;
            while (*reinterpret_cast<Key*>(keys+slot*sizeof(Key))) slot=(slot+1)%capacity;
            Object(keys,slot*sizeof(Key),row.first);Object(values,slot*64+0x28,row.second);
        }
        return {keys,values};
    }
    std::uintptr_t Sorted(std::uintptr_t sorted,const std::vector<std::pair<std::uint32_t,const char*>>& names) {
        const auto array=Allocate(0x1000); Object(sorted,0,base+0x83fd248);
        Object(sorted,0x48,array);Object(sorted,0x50,static_cast<std::uint32_t>(names.size()));
        for (std::size_t i=0;i<names.size();++i) {
            Object(array,i*16,reinterpret_cast<std::uintptr_t>(names[i].second));Object(array,i*16+12,names[i].first);
        }
        return array;
    }
    void ResetSelection() {
        Reset();Contracts();selection_fixture_reads::trigger=0;selection_fixture_reads::mutation={};
        std::memset(reinterpret_cast<void*>(system),0,0x760);Put(0xc236a20,system);Object(system,0,base+0x5485118);
        Object(system,0x514,std::int32_t(-1));
        // Canaries prove unused records, opaque padding and neighboring fields
        // survive the native-equivalent bounded write.
        std::memset(reinterpret_cast<void*>(system+0x1d8),0x5a,0x310);
        Object(system,0x1d8,std::uint64_t(0));Object(system,0x4e0,std::int32_t(0));Object(system,0x4e4,std::uint16_t(0));
        Object(graph,0,base+0x86a3e88);Object(graph,0x28,graphData.graphAssetId);
        auto roots=Map<std::uint64_t>(system+0xe0,{{graphData.graphAssetId,graph}});graphKeys=roots.first;graphValues=roots.second;
        Map<std::uint64_t>(system+0x90,{{missionData.objectiveGraphAssetId,objectives},{secondMission.objectiveGraphAssetId,objectives2}});
        auto missions=Map<std::uint32_t>(graph+0xd8,{{missionData.nameCrc,mission},{secondMission.nameCrc,mission2}});
        missionKeys=missions.first;missionValues=missions.second;
        for (const auto& row:std::vector<std::pair<std::uintptr_t,const ReloadMission*>>{{mission,&missionData},{mission2,&secondMission}}) {
            Object(row.first,0,base+0x86a3de8);Object(row.first,0x28,reinterpret_cast<std::uintptr_t>(row.second->name));
            Object(row.first,0x34,row.second->nameCrc);Object(row.first,0x38,std::uint8_t(1));
            Object(row.first,0x4c,row.second->objectiveGraphAssetId);Object(row.first,0x6c,row.second->availableCheckpoint.nameCrc);
        }
        Object(objectives,0,base+0x86a3d48);Object(objectives2,0,base+0x86a3d48);
        Map<std::uint32_t>(objectives+0x148,{{objectiveData.nameCrc,objective},{secondObjective.nameCrc,objective2}});
        for (const auto& row:std::vector<std::pair<std::uintptr_t,const ReloadMissionObjective*>>{{objective,&objectiveData},{objective2,&secondObjective}}) {
            Object(row.first,0,base+0x86a3ca8);Object(row.first,0x28,reinterpret_cast<std::uintptr_t>(row.second->name));
            Object(row.first,0x34,row.second->nameCrc);Object(row.first,0x4c,row.second->checkpoint.nameCrc);
        }
        Sorted(graph+0x120,{{secondMission.nameCrc,secondMission.name},{missionData.nameCrc,missionData.name}});
        objectiveOrder=Sorted(objectives+0xe8,{{secondObjective.nameCrc,secondObjective.name},{objectiveData.nameCrc,objectiveData.name}});
        Sorted(objectives2+0xe8,{});
        selectionRuntime.image={base};selectionRuntime.supported=true;selectionRuntime.ownsPending=false;
        Require(SelectionCode(selectionRuntime.image),"actual PE selection code pins fail");
        Require(SelectionTypes(selectionRuntime.image),"actual PE selection RTTI/DDL pins fail");
    }
    ReloadMissionSelection Selection() const {return {graphData.graphAssetId,{{missionData.nameCrc,objectiveData.nameCrc,MissionStartState::Active}},false};}
    ReloadSelectionState Capture(const ReloadMissionSelection& selection) {
        auto result=ReadReloadSelectionState(Ready(),selection);Require(result.ready,"valid selected branch did not resolve");return result;
    }
    ReloadSelectionState Capture() {return Capture(Selection());}
    std::array<std::uint8_t,0x760> Bytes() const {std::array<std::uint8_t,0x760> out{};std::memcpy(out.data(),reinterpret_cast<void*>(system),out.size());return out;}
    template<class F> void Refuses(F mutate,const char* why) {
        ResetSelection();auto captured=Capture();auto admitted=Ready();fixtureScope=true;
        mutate(captured,admitted);const auto before=Bytes();std::wstring status;
        {std::lock_guard reloadLock(reloadRuntime.mutex);Require(!PrepareReloadSelectionAfterAdmission(captured,admitted,status),why);}
        if(Bytes()!=before)std::cerr<<why<<'\n';
        Require(Bytes()==before,"refused selection preparation wrote native memory");
        Require(!reloadCalls&&!frontendCalls&&!predicateCalls&&!unloadCalls,"selection helper invoked native gameplay");++selectionCases;
    }
};
void SelectionCases(SelectionHost& h) {
    std::wstring status;
    h.ResetSelection();const auto before=h.Bytes();auto ready=h.Capture();
    Require(h.Bytes()==before&&!reloadCalls&&!predicateCalls,"selection read wrote state or invoked native code");
    Require(ready.selection.graphId>UINT32_MAX && ready.selection.graphId==h.graphData.graphAssetId,"graph identifier was truncated");
    Require(ready.missionOrder==std::vector<std::uint32_t>{h.secondMission.nameCrc,h.missionData.nameCrc},"mission order was sorted or fabricated");
    Require(ready.branches.size()==1&&ready.branches[0].allowAvailable&&ready.branches[0].checkpoint==h.objectiveData.checkpoint.nameCrc,
        "branch capability/checkpoint metadata differs");
    Require(ready.branches[0].objectiveOrder==std::vector<std::uint32_t>{h.secondObjective.nameCrc,h.objectiveData.nameCrc},"objective order differs from actual native array");++selectionCases;
    auto empty=h.Selection();empty.branches={{}};auto edit=ReadReloadSelectionState(h.Ready(),empty);
    Require(!edit.ready&&edit.missionOrder==ready.missionOrder&&edit.branches.size()==1,"empty editor row prevents native mission browsing");++selectionCases;
    for (const auto state:{MissionStartState::Active,MissionStartState::Available,MissionStartState::Complete}) {
        h.ResetSelection();auto selection=h.Selection();selection.branches[0].state=state;
        if (state!=MissionStartState::Active)selection.branches[0].objectiveHash=0;
        auto captured=h.Capture(selection);auto admitted=h.Ready();auto expected=h.Bytes();fixtureScope=true;
        const std::int32_t count=1,reason=5;
        std::memcpy(expected.data()+0x514,&reason,4);std::memcpy(expected.data()+0x1d8,&selection.graphId,8);
        std::memcpy(expected.data()+0x1e0,&selection.branches[0].objectiveHash,4);
        std::memcpy(expected.data()+0x1e4,&selection.branches[0].missionHash,4);expected[0x1e8]=static_cast<std::uint8_t>(state);
        std::memcpy(expected.data()+0x4e0,&count,4);expected[0x4e4]=1;expected[0x4e5]=0;
        {std::lock_guard lock(reloadRuntime.mutex);Require(PrepareReloadSelectionAfterAdmission(captured,admitted,status),"admitted branch preparation refused");}
        Require(h.Bytes()==expected,"branch preparation changed unrelated system bytes, padding or unused records");
        Require(status.find(L"remain")!=std::wstring::npos,"native-refusal persistence not described");
        Require(!PrepareReloadSelectionAfterAdmission(captured,admitted,status)&&h.Bytes()==expected,"stale request could prepare twice");
        Require(!reloadCalls&&!predicateCalls,"branch preparation called native request");++selectionCases;
    }
    h.ResetSelection();auto selection=h.Selection();selection.branches[0].objectiveHash=0;
    Require(h.Capture(selection).ready,"native mission-default start rejected");++selectionCases;
    for(const auto state:{MissionStartState::Active,MissionStartState::Available,MissionStartState::Complete}) {
        h.ResetSelection();h.Map<std::uint64_t>(h.system+0x90,{{h.secondMission.objectiveGraphAssetId,h.objectives2}});
        selection=h.Selection();selection.branches[0].objectiveHash=0;selection.branches[0].state=state;
        auto missing=h.Capture(selection);auto world=h.Ready();fixtureScope=true;
        Require(missing.branches[0].allowAvailable&&missing.branches[0].objectiveOrder.empty(),
            "absent objective graph invented order or hid mission capability");
        Require(PrepareReloadSelectionAfterAdmission(missing,world,status),"zero-objective start incorrectly requires built objective graph");++selectionCases;
    }
    h.ResetSelection();h.Map<std::uint64_t>(h.system+0x90,{{h.secondMission.objectiveGraphAssetId,h.objectives2}});
    Require(!ReadReloadSelectionState(h.Ready(),h.Selection()).ready,"explicit objective admitted without built graph");++selectionCases;
    h.ResetSelection();selection=h.Selection();selection.branches[0].objectiveHash=0;
    h.Object(h.system,0x90+0x20,std::uint32_t(80));
    Require(!ReadReloadSelectionState(h.Ready(),selection).ready,"corrupt graph map confused with absent optional graph");++selectionCases;
    h.ResetSelection();selection=h.Selection();selection.branches[0].objectiveHash=0;
    auto misplaced=h.Map<std::uint64_t>(h.system+0x90,{{h.missionData.objectiveGraphAssetId,h.objectives}});
    const std::uint64_t product=h.missionData.objectiveGraphAssetId*0x9e3779b1ull;
    const auto slot=static_cast<std::uint32_t>((product>>32)^product)%8;
    h.Object(misplaced.first,slot*8,std::uint64_t(0));h.Object(misplaced.first,((slot+1)%8)*8,h.missionData.objectiveGraphAssetId);
    Require(!ReadReloadSelectionState(h.Ready(),selection).ready,"unreachable existing graph key confused with absent graph");++selectionCases;
    h.ResetSelection();selection=h.Selection();selection.branches[0].objectiveHash=0;selection.branches[0].state=MissionStartState::Available;
    h.Object(h.mission,0x38,std::uint8_t(0));
    const auto unavailable=ReadReloadSelectionState(h.Ready(),selection);
    Require(!unavailable.ready&&unavailable.branches.size()==1&&!unavailable.branches[0].allowAvailable,
        "AllowAvailable=false did not reject original unsupported start state");++selectionCases;
    h.ResetSelection();selection=h.Selection();selection.branches.push_back({});
    selection.branches.push_back({h.secondMission.nameCrc,0,MissionStartState::Complete});selection.branches.push_back({});
    auto aligned=h.Capture(selection);auto alignedWorld=h.Ready();fixtureScope=true;
    Require(aligned.branches.size()==4 && aligned.branches[1].objectiveOrder.empty() && aligned.branches[2].allowAvailable,
        "empty editor extras lost aligned metadata");
    Require(PrepareReloadSelectionAfterAdmission(aligned,alignedWorld,status),"blank extra rows prevented admitted reload");
    Require(*reinterpret_cast<std::int32_t*>(h.system+0x4e0)==2&&
        *reinterpret_cast<std::uint32_t*>(h.system+0x1e4+12)==h.secondMission.nameCrc&&
        *reinterpret_cast<std::uint8_t*>(h.system+0x1e8+12)==2,"RemoveEmpty did not compact and preserve branch order");++selectionCases;
    for (auto mutate:std::vector<std::function<void(ReloadMissionSelection&)>>{
        [](auto& x){x.graphId=0;},[](auto& x){x.graphId^=0x100000000ull;},[](auto& x){x.branches[0].missionHash=1;},
        [](auto& x){x.branches[0].objectiveHash=1;},[](auto& x){x.branches[0].state=static_cast<MissionStartState>(3);},
        [](auto& x){x.branches[0].state=MissionStartState::Available;},[](auto& x){x.branches[0].state=MissionStartState::Complete;},
        [](auto& x){x.branches.clear();},[](auto& x){x.branches.resize(65,x.branches[0]);},[](auto& x){x.replayMode=true;}}) {
        h.ResetSelection();selection=h.Selection();mutate(selection);auto result=ReadReloadSelectionState(h.Ready(),selection);
        Require(!result.ready,"invalid/incomplete or unproved replay selection enabled");++selectionCases;
    }
    h.ResetSelection();selection=h.Selection();selection.branches.resize(64,selection.branches[0]);
    auto full=h.Capture(selection);auto admitted=h.Ready();fixtureScope=true;
    Require(PrepareReloadSelectionAfterAdmission(full,admitted,status),"exact64 branch capacity rejected");
    Require(*reinterpret_cast<std::int32_t*>(h.system+0x4e0)==64,"native branch count differs");
    Require(*reinterpret_cast<std::uint32_t*>(h.system+0x1e0+63*12)==h.objectiveData.nameCrc,"last allowed branch lost");++selectionCases;
    h.Refuses([](auto&,auto&){fixtureScope=false;},"window context prepared native selection");
    h.Refuses([](auto& c,auto&){c.ready=false;},"unready capture prepared");
    h.Refuses([](auto& c,auto&){c.supported=false;},"unsupported capture prepared");
    h.Refuses([](auto&,auto& a){a.ready=false;},"unready admission prepared");
    h.Refuses([](auto&,auto& a){++a.token.contextSerial;},"changed world epoch prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.kernel,0x62f0,std::uint16_t(16));},"changed world sequence prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.kernel,0x6308,std::uint8_t(1));},"unloading world prepared");
    h.Refuses([&](auto&,auto&){h.Put(0xc236a20,std::uintptr_t(0));},"replaced objective system prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.system,0x1b1,std::uint8_t(1));},"deferred objective unload prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.system,0x1e8,std::uint8_t(2));},"changed inactive override bytes ignored");
    h.Refuses([&](auto&,auto&){h.Object(h.system,0x514,std::int32_t(5));},"pending native reason overwritten");
    h.Refuses([&](auto&,auto&){h.Object(h.graph,0x28,std::uint64_t(1));},"wrong graph identity prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.mission,0x28,reinterpret_cast<std::uintptr_t>("Wrong mission"));},"CRC collision/wrong mission name prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.mission,0x4c,h.secondMission.objectiveGraphAssetId);},"foreign objective graph prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.mission,0x38,std::uint8_t(2));},"non-Boolean capability prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.objective,0x4c,std::uint32_t(1));},"changed native checkpoint prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.objectiveOrder,12,std::uint32_t(1));},"changed native DFT state prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.graph,0xd8+0x14,std::uint32_t(16385));},"oversized hash table prepared");
    h.Refuses([&](auto&,auto&){h.Object(h.graph,0xd8+0x1c,std::uint32_t(80));},"wrong native table stride prepared");
    h.Refuses([&](auto&,auto&){h.Put(0x3589e70,std::uint8_t(0x90));},"changed retail setter code prepared");
    h.Refuses([&](auto&,auto&){h.Put(0x86a3b60+4,std::uint32_t(0x39));},"changed DDL field offset prepared");
    h.ResetSelection();auto raced=h.Capture();auto racedWorld=h.Ready();auto racedExpected=h.Bytes();fixtureScope=true;
    const std::uint64_t externalGraph=1;std::memcpy(racedExpected.data()+0x1d8,&externalGraph,8);
    selection_fixture_reads::trigger=h.system+0x514;
    selection_fixture_reads::mutation=[&]{h.Object(h.system,0x1d8,externalGraph);};
    Require(!PrepareReloadSelectionAfterAdmission(raced,racedWorld,status)&&h.Bytes()==racedExpected,
        "mid-read pending override mutation was admitted or overwritten");++selectionCases;
    h.Refuses([&](auto&,auto&){selection_fixture_reads::trigger=h.system+0x514;
        selection_fixture_reads::mutation=[&]{h.Object(h.kernel,0x62f0,std::uint16_t(16));};},"mid-read world change prepared");
    h.Refuses([&](auto&,auto&){selection_fixture_reads::trigger=h.system+0x514;
        selection_fixture_reads::mutation=[]{fixtureScope=false;};},"revoked callback prepared");
    h.ResetSelection();auto capture=h.Capture();admitted=h.Ready();auto bytes=h.Bytes();fixtureScope=true;
    std::atomic<bool> held{false},release{false};
    std::thread holder([&]{std::lock_guard lock(selectionRuntime.mutex);held=true;while(!release.load())std::this_thread::yield();});
    while(!held.load())std::this_thread::yield();
    const bool busy=PrepareReloadSelectionAfterAdmission(capture,admitted,status);release=true;holder.join();
    Require(!busy&&h.Bytes()==bytes,"busy selection mutex blocked or wrote");++selectionCases;
    h.ResetSelection();capture=h.Capture();admitted=h.Ready();bytes=h.Bytes();fixtureScope=true;
    DWORD previous=0;Require(VirtualProtect(reinterpret_cast<void*>(h.system&~std::uintptr_t(0xfff)),0x1000,PAGE_READONLY,&previous)!=0,"protect system fixture");
    Require(!PrepareReloadSelectionAfterAdmission(capture,admitted,status)&&h.Bytes()==bytes,"readonly objective system prepared");
    DWORD ignored=0;Require(VirtualProtect(reinterpret_cast<void*>(h.system&~std::uintptr_t(0xfff)),0x1000,previous,&ignored)!=0,"restore system fixture");++selectionCases;
}
void IntegrationCases(SelectionHost& h) {
    struct Context {ReloadSelectionState captured;unsigned calls=0;bool prepared=false;};
    const auto prepare=[](const ReloadState& admitted,void* opaque,std::wstring& status) {
        auto& context=*static_cast<Context*>(opaque);++context.calls;
        Require(predicateCalls==1&&!reloadCalls&&!reloadRuntime.awaitingTransition,"selection preparation order differs from final admission");
        context.prepared=PrepareReloadSelectionAfterAdmission(context.captured,admitted,status);return context.prepared;
    };
    for (const bool full:{false,true}) for (const bool accepts:{false,true}) {
        h.ResetSelection();Context context{h.Capture()};const auto world=h.Ready();fixtureScope=true;acceptNative=accepts;std::wstring status;
        const bool result=full?ExecuteFullReload(world,0,status,prepare,&context):ExecuteReload(world,0,status,prepare,&context);
        Require(result==accepts&&context.calls==1&&context.prepared&&reloadCalls==1&&receivedFlag==full,"normal/full preparation integration differs");
        Require(*reinterpret_cast<std::int32_t*>(h.system+0x514)==5&&*reinterpret_cast<std::int32_t*>(h.system+0x4e0)==1,
            "native refusal rolled back prepared selection");++selectionCases;
        if(!accepts){
            auto retry=ReadReloadSelectionState(h.Ready(),context.captured.selection);
            Require(retry.ready&&retry.token.priorEndReason==5,"owned pending overrides cannot be retried after native refusal");
            context={retry};acceptNative=true;predicateCalls=reloadCalls=0;
            Require(full?ExecuteFullReload(h.Ready(),0,status,prepare,&context):ExecuteReload(h.Ready(),0,status,prepare,&context),
                "fresh same-world retry of owned pending overrides failed");
            Require(context.prepared&&reloadCalls==1,"retry did not reach admitted native request");++selectionCases;
        }
    }
    h.ResetSelection();Context context{h.Capture()};const auto world=h.Ready();h.Object(h.system,0x4e0,std::int32_t(2));
    const auto before=h.Bytes();fixtureScope=true;std::wstring status;
    Require(!ExecuteReload(world,0,status,prepare,&context)&&context.calls==1&&!context.prepared&&!reloadCalls&&
        !reloadRuntime.awaitingTransition&&h.Bytes()==before,"stale pending request did not cancel native Reload cleanly");++selectionCases;
    for(unsigned mutation=0;mutation<4;++mutation){
        h.ResetSelection();auto captured=h.Capture();auto admitted=h.Ready();fixtureScope=true;
        Require(PrepareReloadSelectionAfterAdmission(captured,admitted,status),"owned retry fixture preparation failed");
        if(mutation==0)h.Object(h.system,0x1e8,std::uint8_t(2));
        if(mutation==1)h.Object(h.system,0x514,std::int32_t(7));
        if(mutation==2)h.Object(h.kernel,0x62f0,std::uint16_t(16));
        if(mutation==3)selectionRuntime.ownsPending=false;
        const auto changed=h.Bytes();const auto fresh=ReadReloadSelectionState(h.Ready(),captured.selection);
        Require(!fresh.ready&&!selectionRuntime.ownsPending&&h.Bytes()==changed,"changed/foreign/world-replaced pending overrides were adopted");++selectionCases;
    }
    h.ResetSelection();auto captured=h.Capture();auto admitted=h.Ready();fixtureScope=true;
    Require(PrepareReloadSelectionAfterAdmission(captured,admitted,status),"owned retry edit fixture preparation failed");
    auto edited=captured.selection;edited.branches[0].objectiveHash=0;edited.branches[0].state=MissionStartState::Complete;
    auto fresh=ReadReloadSelectionState(admitted,edited);Require(fresh.ready,"owned pending preparation cannot be explicitly edited");
    Require(PrepareReloadSelectionAfterAdmission(fresh,admitted,status)&&*reinterpret_cast<std::uint8_t*>(h.system+0x1e8)==2,
        "fresh explicit edit of owned pending request failed");++selectionCases;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        (void)&UnusedSelectionReloadFixtureMain;Require(argc==2,"actual retail PE argument required");
        InitializeReloadSelectionRuntime(GetModuleHandleW(nullptr),true);
        Require(!ReadReloadSelectionState({},{}).supported,"foreign executable enabled selection runtime");++selectionCases;
        SelectionHost host(argv[1]);SelectionCases(host);IntegrationCases(host);
        std::cout<<"{\"passed\":true,\"cases\":"<<selectionCases<<",\"assertions\":"<<assertions<<",\"game_started\":false,\"game_writes\":0,\"native_game_calls\":0}\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
