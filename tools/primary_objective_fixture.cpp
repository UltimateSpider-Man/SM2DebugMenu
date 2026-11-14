// Current-process synthetic memory only. Native lookup/state/resolver functions
// are explicit trap boundaries; no game module, engine TLS or hooks are used.
#include <windows.h>
namespace primary_fixture_reads {
bool completed = false;
unsigned afterComplete = 0;
BOOL WINAPI Read(HANDLE process,LPCVOID source,LPVOID target,SIZE_T size,SIZE_T* copied) {
    if (completed) ++afterComplete;
    return ::ReadProcessMemory(process,source,target,size,copied);
}
}
#define ReadProcessMemory primary_fixture_reads::Read
#include "../src/primary_objective_runtime.cpp"
#undef ReadProcessMemory
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace sm2debug;
unsigned assertions=0, cases=0, lookupCalls=0,stateCalls=0,graphCalls=0,nodeCalls=0,completeCalls=0;
thread_local bool scope=false;
int revokeAt=0; bool wrongHandle=false, wrongState=false, mutatePrimary=false, mutateCode=false;
ReloadState fixtureWorld;
PrimaryHandle fixtureHandle;
std::uintptr_t fixtureObjective=0,fixtureGraph=0,fixtureSystem=0,fixtureImage=0;
std::uint32_t receivedMission=0,receivedObjective=0;
void Require(bool value,const char* message) { ++assertions; if (!value) throw std::runtime_error(message); }
PrimaryHandle* TrapLookup(void* manager,PrimaryHandle* output,std::uint32_t mission,std::uint32_t objective) {
    ++lookupCalls; Require(scope,"lookup outside fixture scope"); Require(reinterpret_cast<std::uintptr_t>(manager)==fixtureImage+0xb6a91d0,"lookup received wrong ObjectiveSystem");
    receivedMission=mission;receivedObjective=objective;*output=fixtureHandle;if(wrongHandle)++output->node;
    if(revokeAt==1)scope=false;
    return output;
}
int TrapState(void*,const PrimaryHandle* handle) {
    ++stateCalls;Require(scope,"state predicate outside fixture scope");Require(std::memcmp(handle,&fixtureHandle,40)==0,"state received changed native handle");
    if(revokeAt==2)scope=false;
    if(mutateCode)*reinterpret_cast<unsigned char*>(fixtureImage+0x1555540)^=1;
    return wrongState?2:1;
}
void* TrapGraph(const PrimaryHandle*) { ++graphCalls;Require(scope,"graph resolver outside fixture scope");if(revokeAt==3)scope=false;return reinterpret_cast<void*>(fixtureGraph); }
void* TrapNode(void* graph,std::uint64_t id) {
    ++nodeCalls;Require(scope,"node resolver outside fixture scope");Require(reinterpret_cast<std::uintptr_t>(graph)==fixtureGraph&&id==fixtureHandle.node,"node resolver lost exact graph/node ID");
    if(revokeAt==4)scope=false;
    if(mutatePrimary)++*reinterpret_cast<std::uint32_t*>(fixtureSystem+0x64);
    return reinterpret_cast<void*>(fixtureObjective);
}
void TrapComplete(void* node) {
    ++completeCalls;Require(scope,"completion outside fixture scope");Require(reinterpret_cast<std::uintptr_t>(node)==fixtureObjective,"completion received wrong node");
    *reinterpret_cast<std::int32_t*>(fixtureObjective+0x110)=2;
    primary_fixture_reads::completed=true;
}
struct Host {
    struct Section { std::uint32_t rva,raw,size; };
    std::vector<Section> sections;std::ifstream executable;std::vector<void*> allocations;
    std::uintptr_t base=0,save=0,container=0,block=0,system=0,mission=0,objective=0,graphs=0;
    std::uintptr_t missionDefinition=0,objectiveDefinition=0,resource=0,missionIndex=0,graphIndex=0,objectiveIndex=0;
    std::uintptr_t worldManager=0,level=0,mode=0,kernel=0;
    std::uintptr_t saveMapKeys=0,saveMapValues=0,savedGraphsKeys=0,savedGraphsValues=0;
    const std::uint32_t missionCRC,objectiveCRC;
    const std::uint64_t graphAsset=0x9876543210abcdefull,missionNodeId=0x8000000012345678ull,objectiveNodeId=0x8000000087654321ull;
    explicit Host(const wchar_t* path,std::uint32_t missionValue=0xdeadbeefu,std::uint32_t objectiveValue=0xfedcba99u)
        :executable(std::filesystem::path(path),std::ios::binary),missionCRC(missionValue),objectiveCRC(objectiveValue) {
        Require(executable.good(),"open original executable read-only");
        IMAGE_DOS_HEADER dos{};executable.read(reinterpret_cast<char*>(&dos),sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};executable.seekg(dos.e_lfanew);executable.read(reinterpret_cast<char*>(&nt),sizeof(nt));
        executable.seekg(dos.e_lfanew+24+nt.FileHeader.SizeOfOptionalHeader);
        for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){IMAGE_SECTION_HEADER section{};executable.read(reinterpret_cast<char*>(&section),sizeof(section));sections.push_back({section.VirtualAddress,section.PointerToRawData,section.SizeOfRawData});}
        base=reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr,kPrimaryImageSize,MEM_RESERVE,PAGE_NOACCESS));Require(base!=0,"reserve synthetic image");
        save=Allocate(0x1000);container=Allocate(0x1000);block=Allocate(0x1000);system=Allocate(0x1000);
        mission=Allocate(0x1000);objective=Allocate(0x1000);graphs=Allocate(0x1000);
        missionDefinition=Allocate(0x1000);objectiveDefinition=Allocate(0x1000);resource=Allocate(0x1000);
        missionIndex=Allocate(0x1000);graphIndex=Allocate(0x2000);objectiveIndex=Allocate(0x1000);
        level=Allocate(0x1000);mode=Allocate(0x1000);kernel=Allocate(0x7000);worldManager=base+0xb26b150;
        for(const auto& pin:std::vector<std::pair<std::uintptr_t,std::size_t>>{{0x3583a20,32},{0x3583d90,32},{0x2bc3cc0,42},{0x2bc4fe0,27},{0x2bc53d0,40},{0x2bc5ae0,5},{0x1555540,14},{0x3591750,32},{0x1555330,7},{0x15514c0,7},{0x3583e50,27}})Copy(pin.first,pin.second);
        for(const auto& type:std::vector<std::array<std::uintptr_t,3>>{{0x5485118,0x8e014e0,0xab48120},{0x54a1ca0,0x8e18500,0xab512a8},{0x85979a8,0x900fe70,0xac18fc0},{0x548a108,0x8e05428,0xab49d50},{0x548ab78,0x8e059a0,0xab49f00}}){Copy(type[1],24);Copy(type[2],96);Put(type[0]-8,base+type[1]);}
        for(const auto& slot:std::vector<std::array<std::uintptr_t,2>>{{0x5485118+0x18,0x1539470},{0x548a108+0x38,0x15514c0},{0x548ab78+0x38,0x1555330},{0x548ab78+0xe8,0x1555540},{0x548ab78+0x108,0x1555970},{0x548ab78+0xa0,0x3590520},{0x548ab78+0x50,0x3591890},{0x548ab78+0x100,0x15555f0}})Put(slot[0],base+slot[1]);
        const std::array<RUNTIME_FUNCTION,4> pdata{{{0x2bc3cc0,0x2bc3cea,{0x953dbb8}},{0x3583a20,0x3583b62,{0x965f0e0}},{0x3583d90,0x3583e4b,{0x965f13c}},{0x3591750,0x35917e7,{0x9660920}}}};
        Commit(0x100000,sizeof(pdata));std::memcpy(reinterpret_cast<void*>(base+0x100000),pdata.data(),sizeof(pdata));
        Reset();
    }
    ~Host(){for(auto pointer:allocations)VirtualFree(pointer,0,MEM_RELEASE);if(base)VirtualFree(reinterpret_cast<void*>(base),0,MEM_RELEASE);}
    std::uintptr_t Allocate(std::size_t size){auto p=VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);Require(p!=nullptr,"allocate fixture object");allocations.push_back(p);return reinterpret_cast<std::uintptr_t>(p);}
    void Commit(std::uintptr_t rva,std::size_t size){const auto first=rva&~std::uintptr_t(0xfff),end=(rva+size+0xfff)&~std::uintptr_t(0xfff);Require(VirtualAlloc(reinterpret_cast<void*>(base+first),end-first,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"commit synthetic image page");}
    template<class T>void Put(std::uintptr_t rva,T value){Commit(rva,sizeof(value));std::memcpy(reinterpret_cast<void*>(base+rva),&value,sizeof(value));}
    template<class T>void Set(std::uintptr_t object,std::size_t offset,T value){std::memcpy(reinterpret_cast<void*>(object+offset),&value,sizeof(value));}
    void Copy(std::uintptr_t rva,std::size_t size){for(const auto& section:sections)if(rva>=section.rva&&size<=section.size&&rva-section.rva<=section.size-size){Commit(rva,size);executable.clear();executable.seekg(section.raw+rva-section.rva);executable.read(reinterpret_cast<char*>(base+rva),size);Require(executable.gcount()==static_cast<std::streamsize>(size),"copy original PE bytes");return;}throw std::runtime_error("file RVA not backed");}
    template<class Key>std::uintptr_t Map(std::uintptr_t header,const std::vector<std::pair<Key,std::uintptr_t>>& values,std::uint32_t stride,bool pointers=false){
        auto keys=Allocate(0x1000),data=Allocate(0x1000);const std::uint32_t capacity=7;
        Set(header,0,keys);Set(header,8,data);Set(header,0x14,capacity);Set(header,sizeof(Key)==8?0x20:0x1c,stride);
        for(const auto& pair:values){auto product=static_cast<std::uint64_t>(pair.first)*0x9e3779b1ull;auto slot=static_cast<std::uint32_t>((product>>32)^static_cast<std::uint32_t>(product))%capacity;
            while(*reinterpret_cast<Key*>(keys+slot*sizeof(Key)))slot=(slot+1)%capacity;
            Set(keys,slot*sizeof(Key),pair.first);Set(data,slot*stride+(pointers?0:0x28),pair.second);
        }return data;
    }
    std::uintptr_t MapEntry(std::uintptr_t header,std::uint64_t key,bool wide){PrimaryMap map;Require(wide?PrimaryMapFind(header,key,8,map):PrimaryMapFind(header,static_cast<std::uint32_t>(key),8,map),"fixture map entry exists");return map.entry;}
    void Reset(){
        primary_fixture_reads::completed=false;primary_fixture_reads::afterComplete=0;lookupCalls=stateCalls=graphCalls=nodeCalls=completeCalls=0;
        receivedMission=receivedObjective=0;scope=false;revokeAt=0;wrongHandle=wrongState=mutatePrimary=mutateCode=false;
        Copy(0x1555540,14);
        fixtureWorld={};fixtureWorld.supported=fixtureWorld.ready=fixtureWorld.frontendReady=true;auto& world=fixtureWorld.token;
        world.imageBase=base;world.contextSerial=41;world.manager=worldManager;world.managerVtable=base+0x547d668;world.loadedLevel=level;
        world.managerState=world.pendingInstances=world.pendingDeletions=0;world.managerReady=1;world.mode=mode;world.modeValue=9;
        world.kernel=kernel;world.kernelState=3;world.kernelSequence=5;world.levelAsset=graphAsset;world.saveSystem=save;world.saveSystemVtable=base+0x54a1ca0;
        Put(0xc2266a8,worldManager);Put(0xb26b150,world.managerVtable);Put(0xb26b150+0x10,level);
        Put(0xb26b150+0x430628,std::int32_t(0));Put(0xb26b150+0x199fc0,std::int32_t(0));Put(0xb26b150+0x319f68,std::int32_t(0));Put(0xb26b150+0x430665,std::uint8_t(1));
        Put(0xc21d330,mode);Set(mode,0x10,std::int32_t(9));Set(kernel,0x58,graphAsset);Set(kernel,0x62f0,std::uint16_t(5));Set(kernel,0x6308,std::uint8_t(3));
        Put(0xc0bd2d8,save);Set(save,0,base+0x54a1ca0);Set(save,0x98,std::uint32_t(1));Set(save,0x30,container);
        Put(0xb6a9170,base+0xa88ad60);Put(0xb6a9178,std::uint32_t(0));Put(0xb6a9180,base+0x54850b0);Put(0xb6a9188,kObjectiveSystemCRC);Put(0xb6a918c,std::uint32_t(1));
        Put(0xc236a20,base+0xb6a91d0);Commit(0xb6a91d0,0x760);Put(0xb6a91d0,base+0x5485118);Put(0xb6a91d0+0x1b1,std::uint8_t(0));Put(0xb6a9900,std::uint8_t(0));
        Map<std::uint32_t>(container+0x28,{{kObjectiveSystemCRC,0}},0x80);const auto saveEntry=MapEntry(container+0x28,kObjectiveSystemCRC,false);Set(saveEntry,0x38,block);
        PrimaryRead(container+0x28,saveMapKeys);PrimaryRead(container+0x30,saveMapValues);
        Set(block,0,base+0x85979a8);Set(block,0xb0,system);Set(system,0x60,missionCRC);Set(system,0x64,objectiveCRC);
        Set(mission,0,base+0x548a108);Set(mission,0x88,missionCRC);Set(mission,0x18,missionDefinition);
        Set(objective,0,base+0x548ab78);Set(objective,0x108,missionCRC);Set(objective,0x10c,objectiveCRC);Set(objective,0x110,std::int32_t(1));Set(objective,0x18,objectiveDefinition);
        const auto missionGraph=graphs,objectiveGraph=graphs+0x138;
        Set(missionGraph,0,resource);Set(missionGraph,8,std::uint32_t(1u<<20));Set(objectiveGraph,0,resource);Set(objectiveGraph,8,std::uint32_t((1u<<20)|1u));Set(resource,8,graphAsset);
        Set(mission,8,missionGraph);Set(objective,8,objectiveGraph);Set(missionDefinition,8,missionNodeId);Set(objectiveDefinition,8,objectiveNodeId);
        Put(0x9f47170+0x18,std::uint32_t(2));Put(0x9f47170+0x28,graphs);
        auto missionWrapper=Allocate(0x1000),objectiveWrapper=Allocate(0x1000);Set(missionWrapper,8,mission);Set(objectiveWrapper,8,objective);
        Map<std::uint64_t>(missionGraph+0x48,{{missionNodeId,missionWrapper}},16,true);Map<std::uint64_t>(objectiveGraph+0x48,{{objectiveNodeId,objectiveWrapper}},16,true);
        Set(missionIndex,0,missionCRC);Set(missionIndex,4+8,std::int32_t(-1));Set(missionIndex,4+0xc,static_cast<std::uint32_t>(graphAsset));Set(missionIndex,4+0x10,static_cast<std::uint32_t>(graphAsset>>32));Set(missionIndex,4+0x18,mission);
        Put(0xb6a91d0+0x20,missionIndex);Put(0xb6a91d0+0x28,std::uint16_t(1));Put(0xb6a91d0+0x2a,std::uint16_t(1));
        Set(graphIndex,0,graphAsset);Put(0xb6a91d0+0x160,graphIndex);Put(0xb6a91d0+0x168,std::uint16_t(1));Put(0xb6a91d0+0x16a,std::uint16_t(1));
        Set(objectiveIndex,0,objectiveCRC);Set(objectiveIndex,4,objective);Set(graphIndex,8+0xb0,objectiveIndex);Set(graphIndex,8+0xb8,std::uint16_t(1));Set(graphIndex,8+0xba,std::uint16_t(1));
        Map<std::uint64_t>(block+0xc0,{{graphAsset,0}},0x90);const auto savedGraph=MapEntry(block+0xc0,graphAsset,true);
        auto missionData=Allocate(0x1000),objectiveData=Allocate(0x1000);Set(missionData,0x30,std::int32_t(2));Set(objectiveData,0x30,std::int32_t(1));
        Map<std::uint32_t>(savedGraph+0x30,{{missionCRC,missionData},{objectiveCRC,objectiveData}},0x38);
        fixtureHandle={};fixtureHandle.graph=(1u<<20)|1u;fixtureHandle.node=objectiveNodeId;fixtureHandle.missionGraph=1u<<20;fixtureHandle.missionNode=missionNodeId;fixtureHandle.mission=missionCRC;
        fixtureObjective=objective;fixtureGraph=objectiveGraph;fixtureSystem=system;fixtureImage=base;
        std::lock_guard lock(primaryRuntime.mutex);primaryRuntime.image={base,base+0x100000,4};
        Require(PrimaryCodePins(primaryRuntime.image),"production code/pdata pins match original PE");Require(PrimaryTypes(primaryRuntime.image),"production RTTI/vtable pins match original PE");
        primaryRuntime.supported=true;primaryRuntime.reason=L"Fixture waiting";primaryRuntime.lookup=&TrapLookup;primaryRuntime.state=&TrapState;primaryRuntime.graph=&TrapGraph;primaryRuntime.node=&TrapNode;primaryRuntime.complete=&TrapComplete;
    }
    PrimaryObjectiveState Ready(){auto result=ReadPrimaryObjectiveState(fixtureWorld);Require(result.ready,"independent raw snapshot must find active primary");return result;}
};
void NoComplete(){Require(completeCalls==0,"rejected request reached native mutation trap");}
}
namespace sm2debug {
bool IsWithinVerifiedEngineCallback() noexcept{return scope;}
ReloadState ReadReloadState(){return fixtureWorld;}
bool SameReloadContext(const ReloadToken& a,const ReloadToken& b) noexcept {
    return a.contextSerial && a.contextSerial==b.contextSerial && a.imageBase==b.imageBase && a.manager==b.manager &&
        a.loadedLevel==b.loadedLevel && a.kernel==b.kernel && a.kernelSequence==b.kernelSequence && a.saveSystem==b.saveSystem;
}
}
int wmain(int argc,wchar_t** argv){
    try {
        Require(argc==2,"fixture requires original executable path");
        InitializePrimaryObjectiveRuntime(GetModuleHandleW(nullptr),true);Require(!ReadPrimaryObjectiveState({}).supported,"wrong executable process accepted");++cases;
        Host host(argv[1]);auto captured=host.Ready();Require(captured.token.missionHash==host.missionCRC&&captured.token.objectiveHash==host.objectiveCRC,"snapshot lost high uint32 primary hashes");Require(lookupCalls+stateCalls+graphCalls+nodeCalls+completeCalls==0,"snapshot called a native trap");++cases;
        std::wstring status;Require(!ExecuteCompletePrimary(captured,status),"outside callback accepted");NoComplete();++cases;
        for(auto offset:{std::uintptr_t(0xb6a9170),std::uintptr_t(0xb6a9178),std::uintptr_t(0xb6a9188),std::uintptr_t(0xb6a918c)}){host.Reset();host.Put(offset,std::uint64_t(123));Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"invalid save registration accepted");}++cases;
        for(auto capacity:{std::uint32_t(0),std::uint32_t(16385)}){host.Reset();host.Set(host.container+0x28,0x14,capacity);Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"invalid save-map capacity accepted");}host.Reset();host.Set(host.container+0x28,0x1c,std::uint32_t(8));Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"short save-map stride accepted");++cases;
        host.Reset();for(unsigned i=0;i<7;++i)host.Set(host.saveMapKeys,i*4,kObjectiveSystemCRC);Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"map without empty slot accepted");++cases;
        for(auto offset:{std::size_t(0x60),std::size_t(0x64)}){host.Reset();host.Set(host.system,offset,std::uint32_t(0));Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"missing primary hash accepted");}++cases;
        host.Reset();host.Set(host.block,0,std::uintptr_t(0));Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"foreign saveblock accepted");host.Reset();host.Set(host.block,0xb0,host.base+0x9ea7a00);Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"default-instance SystemData accepted");++cases;
        for(auto state:{std::int32_t(0),std::int32_t(2),std::int32_t(3)}){host.Reset();host.Set(host.objective,0x110,state);Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"inactive runtime objective accepted");}++cases;
        host.Reset();auto savedGraph=host.MapEntry(host.block+0xc0,host.graphAsset,true);auto savedEntry=host.MapEntry(savedGraph+0x30,host.objectiveCRC,false);std::uintptr_t savedData=0;PrimaryRead(savedEntry+0x28,savedData);host.Set(savedData,0x30,std::int32_t(2));Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"completed saved objective accepted");++cases;
        host.Reset();host.Set(host.objective,0,host.base+0x548aa50);Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"unsupported shared objective type accepted");
        host.Reset();captured=host.Ready();host.Set(host.graphs+0x138,8,std::uint32_t((2u<<20)|1u));scope=true;
        Require(!ExecuteCompletePrimary(captured,status),"queued recycled graph generation accepted");Require(lookupCalls==0,"recycled graph reached native lookup");++cases;
        host.Reset();captured=host.Ready();host.Set(host.system,0x64,std::uint32_t(1));scope=true;Require(!ExecuteCompletePrimary(captured,status),"changed current primary accepted");Require(lookupCalls==0,"stale primary reached native lookup");NoComplete();++cases;
        host.Reset();captured=host.Ready();host.Set(host.objective,0x108,std::uint32_t(0x12345678));
        Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"objective assigned to another mission remained ready");scope=true;
        Require(!ExecuteCompletePrimary(captured,status)&&lookupCalls==0,"foreign mission owner reached native lookup");NoComplete();++cases;
        host.Reset();captured=host.Ready();++fixtureWorld.token.contextSerial;scope=true;Require(!ExecuteCompletePrimary(captured,status),"stale world context accepted");Require(lookupCalls==0,"stale world reached native lookup");NoComplete();++cases;
        host.Reset();captured=host.Ready();wrongHandle=true;scope=true;Require(!ExecuteCompletePrimary(captured,status),"different native lookup handle accepted");NoComplete();++cases;
        host.Reset();captured=host.Ready();wrongState=true;scope=true;Require(!ExecuteCompletePrimary(captured,status),"native inactive state accepted");NoComplete();++cases;
        for(int at:{1,2,3,4}){host.Reset();captured=host.Ready();revokeAt=at;scope=true;Require(!ExecuteCompletePrimary(captured,status),"revoked callback reached completion");NoComplete();}++cases;
        host.Reset();captured=host.Ready();mutatePrimary=true;scope=true;Require(!ExecuteCompletePrimary(captured,status),"primary changed by resolver reached completion");NoComplete();++cases;
        host.Reset();captured=host.Ready();mutateCode=true;scope=true;Require(!ExecuteCompletePrimary(captured,status),"changed code pin reached completion");NoComplete();++cases;
        host.Reset();fixtureWorld.token.mode=fixtureWorld.token.kernel=0;fixtureWorld.token.modeValue=-1;fixtureWorld.token.kernelState=0;
        fixtureWorld.token.managerState=22;host.Put(0xc21d330,std::uintptr_t(0));host.Put(0xb26b150+0x430628,std::int32_t(22));
        Require(ReadPrimaryObjectiveState(fixtureWorld).ready,"upstream-ready offline world acquired invented online prerequisites");
        Require(lookupCalls+stateCalls+graphCalls+nodeCalls+completeCalls==0,"offline snapshot called a native trap");++cases;
        host.Reset();captured=host.Ready();scope=true;Require(ExecuteCompletePrimary(captured,status),"valid current primary native algorithm rejected");Require(lookupCalls==1&&stateCalls==1&&graphCalls==1&&nodeCalls==1&&completeCalls==1,"native sequence count differs");Require(receivedMission==host.missionCRC&&receivedObjective==host.objectiveCRC,"native lookup lost exact high uint32 hashes");Require(primary_fixture_reads::afterComplete==0,"old object read after native completion");++cases;
        primary_fixture_reads::completed=false;Require(!ReadPrimaryObjectiveState(fixtureWorld).ready,"completed primary remained ready");Require(!ExecuteCompletePrimary(captured,status)&&completeCalls==1,"completed primary repeated mutation");++cases;
        host.Reset();captured=host.Ready();std::atomic<bool> held{false},release{false};std::thread holder([&]{std::lock_guard lock(primaryRuntime.mutex);held=true;while(!release.load())Sleep(1);});while(!held.load())Sleep(1);scope=true;const auto busy=ReadPrimaryObjectiveState(fixtureWorld);const bool accepted=ExecuteCompletePrimary(captured,status);release=true;holder.join();Require(!busy.ready&&!accepted,"contended runtime stalled or accepted mutation");NoComplete();++cases;
        std::cout<<"{\"passed\":true,\"cases\":"<<cases<<",\"assertions\":"<<assertions<<",\"native_boundary\":\"fixture_traps_only\",\"snapshot_native_calls\":0,\"game_started\":false,\"game_tls_accessed\":false,\"hooks_installed\":false,\"windows_created\":false}\n";
        return 0;
    }catch(const std::exception& error){std::cout<<"{\"passed\":false,\"cases\":"<<cases<<",\"assertions\":"<<assertions<<",\"error\":\""<<error.what()<<"\"}\n";return 1;}
}
