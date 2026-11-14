// Synthetic host only: reuses the real read-only Reload fixture world builder.
// Native action and callback boundaries are explicit substitutes. No game runs.
#define wmain UnusedReloadFixtureMain
#include "reload_runtime_fixture.cpp"
#undef wmain
#include "../src/actor_save_runtime.cpp"

namespace {
std::uintptr_t actorData=0, actorBase=0;
unsigned actorCalls=0, actorCases=0;
bool corruptAfter=false, throwAfter=false;
template<class T> T HostRead(std::uintptr_t p) { T value{};std::memcpy(&value,reinterpret_cast<void*>(p),sizeof(value));return value; }
template<class T> void HostWrite(std::uintptr_t p,T value) { std::memcpy(reinterpret_cast<void*>(p),&value,sizeof(value)); }
void FakeActorReset(void* block,bool keepCollected) {
    ++actorCalls;
    Require(reinterpret_cast<std::uintptr_t>(block)==actorBase+0xa196290,"exact native ActorSaveSystem block receiver");
    Require(keepCollected,"canonical reset always keeps collected items");
    for (const auto offset : {0xb0u,0x150u}) {
        const auto map=actorData+offset;
        const auto keys=HostRead<std::uintptr_t>(map),values=HostRead<std::uintptr_t>(map+8);
        const auto capacity=HostRead<std::uint32_t>(map+0x14);
        const auto tracked=HostRead<std::uintptr_t>(map+0x38);
        const auto trackedCapacity=HostRead<std::uint32_t>(map+0x44);
        if (tracked) for (std::size_t i=0;i<capacity;++i) {
            const auto uid=HostRead<std::uint64_t>(keys+i*8);
            if (!uid) continue;
            bool have=false;
            for (std::size_t j=0;j<trackedCapacity;++j) have|=HostRead<std::uint64_t>(tracked+j*8)==uid;
            if (!have) for (std::size_t j=0;j<trackedCapacity;++j) if (!HostRead<std::uint64_t>(tracked+j*8)) {
                HostWrite(tracked+j*8,uid);HostWrite(map+0x40,HostRead<std::uint32_t>(map+0x40)+1);break;
            }
        }
        std::memset(reinterpret_cast<void*>(keys),0,capacity*8);
        std::memset(reinterpret_cast<void*>(values),0,capacity);
        HostWrite(map+0x10,std::int32_t(0));
    }
    HostWrite(actorData+8,HostRead<std::uint64_t>(actorData+8)&~5ull);
    HostWrite(actorData+0x18,HostRead<std::uint64_t>(actorData+0x18)|5ull);
    if (corruptAfter) HostWrite(HostRead<std::uintptr_t>(actorData+0x108),std::uint8_t(2));
    if (throwAfter) throw std::runtime_error("synthetic native exception");
}
struct ActorHost : HostImage {
    std::uintptr_t container=0,rootKeys=0,rootValues=0,data=0;
    std::array<std::uintptr_t,3> buffers{},tracking{};
    explicit ActorHost(const wchar_t* path) : HostImage(path) {
        save=Allocate(0x4000);container=Allocate(0x1000);rootKeys=Allocate(0x1000);rootValues=Allocate(0x1000);data=Allocate(0x1000);
        for (std::size_t i=0;i<3;++i) {buffers[i]=Allocate(0x1000);tracking[i]=Allocate(0x1000);}
        CopyContracts();
    }
    void CopyContracts() {
        for (const auto pin : std::vector<std::pair<std::uintptr_t,std::size_t>>{
            {0x35d8a20,0x3e},{0x2d74720,0x59},{0x2cde3b0,0xce},{0x30942f0,0x79},
            {0x3097090,0x56},{0x3b0ce20,0x2dc},{0x3b0c860,0x2dc},{0x3b0d310,0x1e},{0x3b0d2d0,0x1e},
            {0x8597e88,0x190},{0xa88b210,0xb0},{0x83d7500,32}}) CopyFile(pin.first,pin.second);
        for (const auto rtti : std::vector<std::array<std::uintptr_t,3>>{
            {0x83d74c0,0x8fc7e90,0xabfe2f0},{0x8597f88,0x900ff20,0xac18ff0},{0x834fcd8,0x8f95fe8,0xabea878}}) {
            CopyFile(rtti[1],24);CopyFile(rtti[2],96);Put(rtti[0]-8,base+rtti[1]);
        }
        Put(0xa88b210,base+0x8597e88);Put(0xa88b210+0x60,base+0x8597eb0);
        Put(0xa88b210+0x68,base+0x8597ef8);Put(0xa88b210+0x80,base+0x8597f08);Put(0xa88b210+0x88,base+0x8597f18);
        for (std::size_t i=0;i<3;++i) {
            const auto name=HostRead<std::uintptr_t>(base+0x8597eb0+i*8)-0x140000000;
            Put(0x8597eb0+i*8,base+name);
        }
    }
    sm2debug::ActorSaveState ActorReady() {
        const auto world=Ready();const auto state=sm2debug::ReadActorSaveState(world);
        Require(state.supported && state.ready,"actor-save synthetic world admits valid populated maps");
        return state;
    }
    void ResetActor() {
        Reset();CopyContracts();
        actorCalls=0;corruptAfter=throwAfter=false;actorData=data;actorBase=base;
        std::memset(reinterpret_cast<void*>(data),0,0x1000);
        std::memset(reinterpret_cast<void*>(container),0,0x1000);
        std::memset(reinterpret_cast<void*>(rootKeys),0,0x1000);
        std::memset(reinterpret_cast<void*>(rootValues),0,0x1000);
        Put(0xa196290,base+0x83d74c0);Put(0xc23c6b8,base+0xa88b210);Put(0xc23c6c0,std::int32_t(0));
        Put(0xc23c6c8,base+0x83d7500);Put(0xc23c6d0,std::uint32_t(0x8c2d0f2d));Put(0xc23c6d4,std::int32_t(1));
        Object(save,0x98,std::int32_t(1));Object(save,0x30,container);Object(container,0,base+0x834fcd8);
        Object(container,0x28,rootKeys);Object(container,0x30,rootValues);
        Object(container,0x38,std::int32_t(1));Object(container,0x3c,std::int32_t(8));Object(container,0x44,std::uint32_t(80));
        const std::uint64_t product=std::uint64_t(0x8c2d0f2d)*0x9e3779b1u;
        const auto index=static_cast<std::uint32_t>((product>>32)^product)%8;
        Object(rootKeys,index*4,std::uint32_t(0x8c2d0f2d));Object(rootValues,index*80+0x38,data);Object(data,0,base+0x8597f88);
        Object(data,8,std::uint64_t(0x112));Object(data,0x18,std::uint64_t(0x200));
        const std::size_t offsets[]={0xb0,0x100,0x150};
        for (std::size_t i=0;i<3;++i) {
            std::memset(reinterpret_cast<void*>(buffers[i]),0,0x1000);
            std::memset(reinterpret_cast<void*>(tracking[i]),0,0x1000);
            const auto map=data+offsets[i];
            Object(map,0,buffers[i]);Object(map,8,buffers[i]+64);
            Object(map,0x10,std::int32_t(2));Object(map,0x14,std::int32_t(8));Object(map,0x20,std::uint32_t(1));
            Object(map,0x38,tracking[i]);Object(map,0x40,std::int32_t(1));Object(map,0x44,std::int32_t(16));
            Object(map,0x48,std::uintptr_t(0x12345678)); // Growth must never touch this deliberately invalid allocator.
            Object(buffers[i],0,std::uint64_t(0xfedcba9876543210ull+i));Object(buffers[i],24,std::uint64_t(0x123456789abcdef0ull+i));
            Object(buffers[i]+64,0,std::uint8_t(1));Object(tracking[i],0,std::uint64_t(0x4444000000000000ull+i));
        }
        auto& runtime=sm2debug::actorRuntime;
        runtime.initialized=runtime.supported=true;runtime.image={base};runtime.reset=FakeActorReset;runtime.applied=false;
        Require(sm2debug::ActorCode(runtime.image) && sm2debug::ActorTypes(runtime.image),"pins and RTTI copied from real read-only PE agree");
    }
};
}
int wmain(int argc,wchar_t** argv) {
    try {
        Require(argc==2,"pass exact retail executable for read-only byte copies");ActorHost host(argv[1]);std::wstring status;
        host.ResetActor();auto state=host.ActorReady();
        Require(!actorCalls,"read-only snapshot and polling perform zero native resets");
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Refused && !actorCalls,"no current callback denies every mutation");++actorCases;
        fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Applied && actorCalls==1,"canonical populated-map reset admitted exactly once");
        Require(HostRead<std::int32_t>(host.data+0xc0)==0 && HostRead<std::int32_t>(host.data+0x160)==0 && HostRead<std::int32_t>(host.data+0x110)==2,"deleted and flags cleared while collectibles remain");
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Refused && actorCalls==1,"captured request is consumed for this world");++actorCases;
        host.ResetActor();state=host.ActorReady();bool foreignDenied=false;
        std::thread foreign([&]{std::wstring text;foreignDenied=sm2debug::ExecuteActorSaveReset(state,text)==sm2debug::ActorSaveResetOutcome::Refused;});foreign.join();
        Require(foreignDenied && !actorCalls,"foreign thread cannot reuse callback readiness");++actorCases;
        for (unsigned invalid=0;invalid<28;++invalid) {
            host.ResetActor();state=host.ActorReady();const auto map=host.data+0xb0;
            switch(invalid) {
                case 0:host.Object(map,0x10,std::int32_t(-1));break;
                case 1:host.Object(map,0x10,std::int32_t(9));break;
                case 2:host.Object(map,0x14,std::int32_t(8193));break;
                case 3:host.Object(map,0x20,std::uint32_t(4));break;
                case 4:host.Object(map,0,std::uintptr_t(0));break;
                case 5:host.Object(map,8,host.buffers[0]);break;
                case 6:host.Object(map,0x10,std::int32_t(1));break;
                case 7:host.Object(host.buffers[0],24,HostRead<std::uint64_t>(host.buffers[0]));break;
                case 8:host.Object(map,0x44,std::int32_t(0));break;
                case 9:host.Object(map,0x40,std::int32_t(17));break;
                case 10:host.Object(host.tracking[0],8,HostRead<std::uint64_t>(host.tracking[0]));host.Object(map,0x40,std::int32_t(2));break;
                case 11:host.Object(map,0x44,std::int32_t(2));break;
                case 12:host.Object(host.save,0x30,std::uintptr_t(0));break;
                case 13:host.Object(host.container,0,host.base+0x834fce0);break;
                case 14:host.Object(host.container,0x44,std::uint32_t(88));break;
                case 15:host.Object(host.data,0,host.base+0x8597f90);break;
                case 16:host.Put(0xc23c6d0,std::uint32_t(1));break;
                case 17:host.Put(0xc23c6d4,std::int32_t(0));break;
                case 18:host.Put(0xa196290,host.base+0x83d74c8);break;
                case 19:host.Put(0xc23c6b8,host.base+0xa88b218);break;
                case 20:host.Object(host.buffers[0],32,std::uint64_t(9));host.Object(map,0x10,std::int32_t(3));break;
                case 21:host.Object(host.kernel,0x62f0,std::uint16_t(16));break;
                case 22:host.Put(0x35d8a20,std::uint8_t(0x90));break;
                case 23:host.Put(0x8597f18,std::uint32_t(0xb8));break;
                case 24:host.Object(host.save,0x98,std::int32_t(0));break;
                case 25:host.Object(host.buffers[0]+64,0,std::uint8_t(2));break;
                case 26:host.Object(map,0,host.buffers[1]);break;
                case 27:host.Object(state.token.entry,0x38,host.base+0xc279728);break;
            }
            fixtureScope=true;
            Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Refused && !actorCalls,"changed world/type/code/map fails before native reset");++actorCases;
        }
        host.ResetActor();state=host.ActorReady();DWORD old=0;
        Require(VirtualProtect(reinterpret_cast<void*>(host.buffers[0]),0x1000,PAGE_READONLY,&old)!=0,"protect synthetic mutable map read-only");fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Refused && !actorCalls,"non-writable primary arrays refuse");
        Require(VirtualProtect(reinterpret_cast<void*>(host.buffers[0]),0x1000,old,&old)!=0,"restore fixture map protection");++actorCases;
        host.ResetActor();
        for (const auto offset : {0xb0u,0x150u}) {
            const auto map=host.data+offset;
            std::memset(reinterpret_cast<void*>(HostRead<std::uintptr_t>(map)),0,64);
            std::memset(reinterpret_cast<void*>(HostRead<std::uintptr_t>(map+8)),0,8);
            host.Object(map,0x10,std::int32_t(0));
        }
        state=host.ActorReady();fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Applied && actorCalls==1,"already empty primary maps remain valid canonical cleanup inputs");++actorCases;
        host.ResetActor();
        for (const auto offset : {0xb0u,0x150u}) {
            host.Object(host.data+offset,0x38,std::uintptr_t(0));
            host.Object(host.data+offset,0x40,std::int32_t(0));host.Object(host.data+offset,0x44,std::int32_t(0));
        }
        state=host.ActorReady();fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Applied,"native explicit null-tracking path is admitted without allocator use");++actorCases;
        host.ResetActor();
        for (std::size_t i=0;i<7;++i) host.Object(host.tracking[1],i*8,std::uint64_t(0x8888000000000000ull+i));
        host.Object(host.data+0x100,0x40,std::int32_t(7));host.Object(host.data+0x100,0x44,std::int32_t(8));
        state=host.ActorReady();fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Applied,"kept collected map does not require unused tracking growth capacity");++actorCases;
        host.ResetActor();host.Object(host.buffers[0]+64,7,std::uint8_t(0xff));
        state=host.ActorReady();fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Applied,"unused value bytes do not falsely invalidate a Boolean map");++actorCases;
        host.ResetActor();
        for (std::size_t i=0;i<6;++i) host.Object(host.tracking[0],i*8,std::uint64_t(0x9999000000000000ull+i));
        host.Object(host.tracking[0],0,HostRead<std::uint64_t>(host.buffers[0]));
        host.Object(host.tracking[0],8,HostRead<std::uint64_t>(host.buffers[0]+24));
        host.Object(host.data+0xb0,0x40,std::int32_t(6));host.Object(host.data+0xb0,0x44,std::int32_t(8));
        state=host.ActorReady();fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Applied,"exact75percent tracking occupancy with duplicate incoming UIDs does not grow");++actorCases;
        host.ResetActor();state=host.ActorReady();
        for (std::size_t i=0;i<7;++i) host.Object(host.tracking[0],i*8,std::uint64_t(0x9999000000000000ull+i));
        host.Object(host.tracking[0],0,HostRead<std::uint64_t>(host.buffers[0]));
        host.Object(host.tracking[0],8,HostRead<std::uint64_t>(host.buffers[0]+24));
        host.Object(host.data+0xb0,0x40,std::int32_t(7));host.Object(host.data+0xb0,0x44,std::int32_t(8));fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Refused && !actorCalls,"above75percent tracking occupancy refuses even duplicate-only inputs because growth check happens first");++actorCases;
        host.ResetActor();state=host.ActorReady();fixtureScope=true;corruptAfter=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::AppliedButVerificationFailed && actorCalls==1,"post-call corruption truthfully reports mutation already invoked");++actorCases;
        host.ResetActor();state=host.ActorReady();fixtureScope=true;throwAfter=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::AppliedButVerificationFailed && actorCalls==1,"native exception truthfully reports irreversibility");++actorCases;
        host.ResetActor();state=host.ActorReady();const auto world=host.Ready();fixtureScope=true;
        Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Applied,"reset occurs before manual Reload");acceptNative=false;
        Require(!sm2debug::ExecuteReload(world,0,status) && reloadCalls==1 && actorCalls==1,"subsequent native Reload may refuse");
        Require(HostRead<std::int32_t>(host.data+0xc0)==0 && HostRead<std::int32_t>(host.data+0x160)==0,"refused Reload does not fabricate map rollback");++actorCases;
        host.ResetActor();state=host.ActorReady();fixtureScope=true;
        {std::lock_guard busy(sm2debug::actorRuntime.mutex);Require(sm2debug::ExecuteActorSaveReset(state,status)==sm2debug::ActorSaveResetOutcome::Refused && !actorCalls,"busy runtime refuses without blocking native call");}++actorCases;
        sm2debug::InitializeActorSaveRuntime(GetModuleHandleW(nullptr),true);
        Require(!sm2debug::ReadActorSaveState(world).supported,"actual non-game host cannot initialize production native bindings");++actorCases;
        std::cout<<"{\"passed\":true,\"cases\":"<<actorCases<<",\"assertions\":"<<assertions
                 <<",\"game_launched\":false,\"real_engine_context_proven\":false,\"real_native_calls\":false,\"boundary\":\"synthetic native reset and engine callback\"}\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<"\n";return 1;}
}
