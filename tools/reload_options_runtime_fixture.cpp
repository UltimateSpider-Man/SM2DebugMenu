// Synthetic current-process campaign ownership using the real Reload reader.
// No game, physical input, native engine call, or user save is touched.
#define wmain UnusedReloadFixtureMain
#include "reload_runtime_fixture.cpp"
#undef wmain
#include <atomic>
#include <functional>

namespace options_fixture_reads {
std::uintptr_t trigger = 0;
std::function<void()> mutation;
BOOL WINAPI Read(HANDLE process, LPCVOID source, LPVOID target, SIZE_T size, SIZE_T* copied) {
    const BOOL result = ::ReadProcessMemory(process, source, target, size, copied);
    if (reinterpret_cast<std::uintptr_t>(source) == trigger && mutation) {
        auto action = std::move(mutation); trigger = 0; mutation = {}; action();
    }
    return result;
}
}
#define ReadProcessMemory options_fixture_reads::Read
#include "../src/reload_options_runtime.cpp"
#undef ReadProcessMemory

namespace {
using namespace sm2debug;
unsigned optionsCases = 0;
struct OptionsHost : HostImage {
    std::uintptr_t container = 0, keys = 0, values = 0, data = 0, alternative = 0;
    std::size_t slot = 0;
    explicit OptionsHost(const wchar_t* path) : HostImage(path) {
        save = Allocate(0x4000); container = Allocate(0x1000); keys = Allocate(0x1000);
        values = Allocate(0x1000); data = Allocate(0x1000); alternative = Allocate(0x1000);
        const std::uint64_t product = std::uint64_t(kCampaignKey) * 0x9e3779b1u;
        slot = static_cast<std::uint32_t>((product >> 32) ^ product) % 8;
    }
    void Contracts() {
        for (const auto pin : std::vector<std::pair<std::uintptr_t, std::size_t>>{
            {0x1613060,0x17},{0x2638d98,0x28},{0x2d74720,0x59},{0x2cde3b0,0xce},
            {0x30942f0,0x79},{0x3097090,0x56},{0x161281f,0x68},{0x35d9870,8},
            {0x34bd04c,0x20},{0x34bd230,0x23},{0x34bd46a,0x25},
            {0x9ea95c0,0xb0},{0x5c380e0,32},{0x5c38b10,0x350},{0x549ec50,32},{0x54a1c18,16}})
            CopyFile(pin.first, pin.second);
        for (const auto rtti : std::vector<std::array<std::uintptr_t,3>>{
            {0x54a1ba0,0x8e18420,0xab51260},{0x54a1ca0,0x8e18500,0xab512a8},
            {0x834fcd8,0x8f95fe8,0xabea878},{0x5c39300,0x8f494e8,0xabca5e8}}) {
            CopyFile(rtti[1],24); CopyFile(rtti[2],96); Put(rtti[0]-8,base+rtti[1]);
        }
        Put(0x54a1ba0+0x18,base+0x16127f0);
        Put(0x9ea95c0,base+0x5c380e0); Put(0x9ea95c0+0x60,base+0x5c38b10);
        Put(0x9ea95c0+0x68,base+0x5c38d30); Put(0x9ea95c0+0x80,base+0x5c38d80);
        Put(0x9ea95c0+0x88,base+0x5c38de0); Put(0x5c38b10+18*8,base+0x549ec50);
        Put(0x5c38b10+6*8,base+0x5c38c58);
    }
    void ResetOptions() {
        options_fixture_reads::trigger = 0; options_fixture_reads::mutation = {};
        Reset(); Contracts();
        std::memset(reinterpret_cast<void*>(container),0,0x1000);
        std::memset(reinterpret_cast<void*>(keys),0,0x1000);
        std::memset(reinterpret_cast<void*>(values),0,0x1000);
        std::memset(reinterpret_cast<void*>(data),0x5a,0x2e0);
        Object(data,0,base+0x5c39300); Object(data,8,std::uint64_t(0x8000000000000011ull));
        Object(data,0x18,std::uint64_t(0x4000000000000022ull));
        Object(data,0xd4,std::uint8_t(1));
        Object(data,0x181,std::uint8_t(1)); Object(data,0x182,std::uint8_t(0)); Object(data,0x183,std::uint8_t(7));
        std::memcpy(reinterpret_cast<void*>(alternative),reinterpret_cast<void*>(data),0x2e0);
        Object(save,0x98,std::int32_t(1)); Object(save,0x30,container);
        Object(container,0,base+0x834fcd8); Object(container,0x28,keys); Object(container,0x30,values);
        Object(container,0x38,std::int32_t(1)); Object(container,0x3c,std::int32_t(8)); Object(container,0x44,std::uint32_t(80));
        Object(keys,slot*4,kCampaignKey); Object(values,slot*80+0x38,data);
        Put(0xc23c818,base+0xb6c2978); Put(0xb6c2978,base+0x54a1ba0);
        Put(0xb6c28e8,base+0x9ea95c0); Put(0xb6c28f0,std::int32_t(0));
        Put(0xb6c28f8,base+0x54a1c18); Put(0xb6c2900,kCampaignKey); Put(0xb6c2904,std::int32_t(1));
        std::lock_guard lock(optionsRuntime.mutex);
        optionsRuntime.image = {base}; optionsRuntime.supported = true;
        Require(OptionsCode(optionsRuntime.image) && OptionsTypes(optionsRuntime.image), "actual PE bytes/descriptor/RTTI satisfy NG+ contract");
    }
    ReloadOptionsState Capture() {
        auto result = ReadReloadOptionsState(Ready());
        Require(result.supported && result.ready, "valid owned campaign flag must be readable");
        return result;
    }
    std::array<unsigned char,0x2e0> Bytes() const {
        std::array<unsigned char,0x2e0> result{};
        std::memcpy(result.data(),reinterpret_cast<void*>(data),result.size()); return result;
    }
    template<class Mutation> void Refuses(Mutation mutate, const char* why) {
        ResetOptions(); const auto captured = Capture(); mutate(); const auto before = Bytes();
        fixtureScope = true; std::wstring status;
        Require(!ExecuteSetNewGamePlus(captured,true,status),why);
        Require(Bytes()==before,"refused NG+ request changed campaign bytes");
        Require(!reloadCalls && !frontendCalls && !predicateCalls,"NG+ request invoked native gameplay");
        ++optionsCases;
    }
    template<class Mutation> void RefusesLoadout(Mutation mutate, const char* why) {
        ResetOptions(); auto captured = Capture(); auto admitted = Ready(); fixtureScope = true;
        mutate(captured,admitted); const auto before = Bytes(); std::wstring status;
        {
            // The real preparation seam owns this mutex. Reacquiring it in
            // the adapter would deadlock, caught by the fixture time limit.
            std::lock_guard reloadLock(reloadRuntime.mutex);
            Require(!PrepareReloadLoadoutAfterAdmission(captured,admitted,status),why);
        }
        Require(Bytes()==before,"refused loadout preparation changed campaign bytes");
        Require(!reloadCalls && !frontendCalls && !predicateCalls,"loadout adapter invoked native gameplay");
        ++optionsCases;
    }
};
void LoadoutCases(OptionsHost& host) {
    std::wstring status;
    for (const bool wasApplied : {true,false}) {
        host.ResetOptions(); host.Object(host.data,0xd4,std::uint8_t(wasApplied));
        auto captured=host.Capture(); auto admitted=host.Ready(); auto expected=host.Bytes();
        expected[0xd4]=0;
        for (const auto offset : {8u,0x18u}) {
            std::uint64_t bits=0; std::memcpy(&bits,expected.data()+offset,8);
            bits|=0x40; std::memcpy(expected.data()+offset,&bits,8);
        }
        fixtureScope=true;
        {
            std::lock_guard reloadLock(reloadRuntime.mutex);
            Require(PrepareReloadLoadoutAfterAdmission(captured,admitted,status),"admitted loadout reset refused");
        }
        Require(host.Bytes()==expected,"loadout reset changed neighbors, NG+, counters or unrelated dirty flags");
        Require(!host.Capture().defaultLoadoutApplied,"loadout reset not reflected in passive state");
        Require(status.find(L"remains")!=std::wstring::npos,"reset persistence on native refusal was not described");
        Require(!reloadCalls && !frontendCalls && !predicateCalls,"loadout preparation invoked gameplay"); ++optionsCases;
    }
    host.ResetOptions(); auto captured=host.Capture(); auto admitted=host.Ready();
    host.Object(host.data,8,std::uint64_t(0x1234567887654321ull));
    host.Object(host.data,0x18,std::uint64_t(0xfedcba9876543210ull));
    auto expected=host.Bytes(); expected[0xd4]=0;
    for (const auto offset : {8u,0x18u}) {
        std::uint64_t bits=0; std::memcpy(&bits,expected.data()+offset,8);
        bits|=0x40; std::memcpy(expected.data()+offset,&bits,8);
    }
    fixtureScope=true;
    Require(PrepareReloadLoadoutAfterAdmission(captured,admitted,status) && host.Bytes()==expected,"late loadout dirty flags were overwritten");
    Require(!PrepareReloadLoadoutAfterAdmission(captured,admitted,status) && host.Bytes()==expected,"stale loadout value accepted twice"); ++optionsCases;
    host.RefusesLoadout([](auto&,auto&){fixtureScope=false;},"loadout reset bypassed engine callback");
    host.RefusesLoadout([](auto& c,auto&){c.ready=false;},"unready campaign capture admitted");
    host.RefusesLoadout([](auto& c,auto&){c.supported=false;},"unsupported campaign capture admitted");
    host.RefusesLoadout([](auto&,auto& a){a.ready=false;},"unready admitted world accepted");
    host.RefusesLoadout([](auto&,auto& a){a.supported=false;},"unsupported admitted world accepted");
    host.RefusesLoadout([](auto&,auto& a){++a.token.contextSerial;},"different admitted world serial accepted");
    host.RefusesLoadout([](auto&,auto& a){++a.token.kernelSequence;},"different admitted world identity accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Object(host.kernel,0x6308,std::uint8_t(1));},"world unloaded after admission was accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Object(host.kernel,0x62f0,std::uint16_t(16));},"world epoch changed after admission was accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Put(0xb26b150+0x199fc0,std::int32_t(1));},"world pending instances accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Object(host.table,7*16+8,std::uint32_t(4));},"component generation changed after admission was accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Put(0xc0bd2d8,std::uintptr_t(0));},"replaced save system admitted for loadout reset");
    host.RefusesLoadout([&](auto&,auto&){host.Object(host.save,0x98,std::int32_t(0));},"uninitialized campaign admitted for loadout reset");
    host.RefusesLoadout([&](auto&,auto&){host.Object(host.values,host.slot*80+0x38,host.alternative);},"replaced campaign admitted for loadout reset");
    host.RefusesLoadout([&](auto&,auto&){host.Object(host.data,0xd4,std::uint8_t(0));},"changed captured loadout flag accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Object(host.data,0xd4,std::uint8_t(2));},"non-Boolean loadout flag accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Put(0x5c38b10+6*8,host.base+0x549ec50);},"wrong loadout descriptor name accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Put(0x5c38d30+6,std::uint8_t(2));},"wrong loadout descriptor type accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Put(0x5c38d80+6*4,std::uint32_t(2));},"wrong loadout descriptor count accepted");
    host.RefusesLoadout([&](auto&,auto&){host.Put(0x5c38de0+6*4,std::uint32_t(0xd5));},"wrong loadout descriptor offset accepted");
    for (const auto rva : {0x34bd04cu,0x34bd230u,0x34bd46au})
        host.RefusesLoadout([&](auto&,auto&){host.Put(rva,std::uint8_t(0x90));},"changed loadout consumer proof accepted");
    host.RefusesLoadout([&](auto&,auto&){optionsRuntime.supported=false;},"unsupported loadout runtime accepted");
    host.RefusesLoadout([&](auto&,auto&){
        options_fixture_reads::trigger=host.data+0xd4;
        options_fixture_reads::mutation=[&]{host.Object(host.values,host.slot*80+0x38,host.alternative);};
    },"mid-read campaign replacement accepted for loadout reset");
    host.RefusesLoadout([&](auto&,auto&){
        options_fixture_reads::trigger=host.data+0xd4;
        options_fixture_reads::mutation=[&]{host.Object(host.kernel,0x62f0,std::uint16_t(16));};
    },"mid-read world change accepted for loadout reset");
    host.RefusesLoadout([&](auto&,auto&){
        options_fixture_reads::trigger=host.data+0xd4; options_fixture_reads::mutation=[]{fixtureScope=false;};
    },"mid-read revoked callback accepted for loadout reset");
    host.ResetOptions(); captured=host.Capture(); admitted=host.Ready(); expected=host.Bytes(); expected[0xd4]=0;
    options_fixture_reads::trigger=host.data+0xd4;
    options_fixture_reads::mutation=[&]{host.Object(host.data,0xd4,std::uint8_t(0));};
    fixtureScope=true;
    Require(!PrepareReloadLoadoutAfterAdmission(captured,admitted,status) && host.Bytes()==expected,
        "mid-read changed loadout value accepted or unrelated fields modified"); ++optionsCases;
    host.ResetOptions(); captured=host.Capture(); admitted=host.Ready(); auto before=host.Bytes(); fixtureScope=true;
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&]{std::lock_guard optionsLock(optionsRuntime.mutex);held=true;while(!release.load())std::this_thread::yield();});
    while(!held.load())std::this_thread::yield();
    const bool busyAccepted=PrepareReloadLoadoutAfterAdmission(captured,admitted,status);
    release=true;holder.join();
    Require(!busyAccepted,"busy options mutex did not refuse promptly");
    Require(host.Bytes()==before,"busy-mutex refusal changed loadout state"); ++optionsCases;
    DWORD previous=0;
    Require(VirtualProtect(reinterpret_cast<void*>(host.data),0x1000,PAGE_READONLY,&previous)!=0,"make loadout fixture save readonly");
    Require(!PrepareReloadLoadoutAfterAdmission(captured,admitted,status) && host.Bytes()==before,"readonly campaign accepted for loadout reset");
    DWORD ignored=0; Require(VirtualProtect(reinterpret_cast<void*>(host.data),0x1000,previous,&ignored)!=0,"restore loadout fixture protection"); ++optionsCases;
}
void LoadoutIntegrationCases(OptionsHost& host) {
    struct PreparationContext { ReloadOptionsState captured; unsigned calls=0; bool prepared=false; };
    const auto prepare=[](const ReloadState& admitted,void* opaque,std::wstring& status) {
        auto& context=*static_cast<PreparationContext*>(opaque); ++context.calls;
        context.prepared=PrepareReloadLoadoutAfterAdmission(context.captured,admitted,status);
        return context.prepared;
    };
    std::wstring status;
    for(const bool full : {false,true}) for(const bool accepted : {false,true}) {
        host.ResetOptions(); PreparationContext context{host.Capture()}; const auto world=host.Ready();
        auto expected=host.Bytes(); expected[0xd4]=0;
        for(const auto offset : {8u,0x18u}) {
            std::uint64_t bits=0;std::memcpy(&bits,expected.data()+offset,8);
            bits|=0x40;std::memcpy(expected.data()+offset,&bits,8);
        }
        fixtureScope=true;acceptNative=accepted;
        const bool result=full?ExecuteFullReload(world,0,status,prepare,&context):ExecuteReload(world,0,status,prepare,&context);
        Require(result==accepted && context.calls==1 && context.prepared,"admitted normal/full request did not use actual loadout preparation");
        Require(reloadCalls==1 && predicateCalls==1 && receivedFlag==full,"loadout preparation altered native Reload ABI or admission");
        Require(host.Bytes()==expected,"normal/full request failed to preserve exact prepared campaign state");
        if(!accepted) Require(!host.Capture().defaultLoadoutApplied,"native refusal rolled back original loadout reset semantics");
        ++optionsCases;
    }
    for(const bool full : {false,true}) {
        host.ResetOptions(); const auto world=host.Ready(); const auto before=host.Bytes(); fixtureScope=true;
        Require(full?ExecuteFullReload(world,0,status):ExecuteReload(world,0,status),"keep-loadout request without preparation refused");
        Require(host.Bytes()==before && reloadCalls==1,"omitted keep-loadout preparation modified campaign bytes");++optionsCases;
    }
    host.ResetOptions(); PreparationContext context{host.Capture()}; const auto world=host.Ready();
    host.Object(host.data,0xd4,std::uint8_t(0)); const auto before=host.Bytes(); fixtureScope=true;
    Require(!ExecuteReload(world,0,status,prepare,&context) && context.calls==1 && !context.prepared,
        "stale loadout capture did not cancel admitted request");
    Require(!reloadCalls && !reloadRuntime.awaitingTransition && host.Bytes()==before,
        "refused loadout preparation dispatched native request, latched transition or wrote save");++optionsCases;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc==2,"actual retail executable fixture input required");
        OptionsHost host(argv[1]); std::wstring status;
        InitializeReloadOptionsRuntime(GetModuleHandleW(nullptr),false);
        Require(!ReadReloadOptionsState({}).supported,"unsupported host admitted");
        InitializeReloadOptionsRuntime(GetModuleHandleW(nullptr),true);
        Require(!ReadReloadOptionsState({}).supported,"synthetic process admitted as retail"); ++optionsCases;
        host.ResetOptions(); auto captured = host.Capture(); const auto untouched = host.Bytes();
        for (int i=0;i<3;++i) Require(!host.Capture().newGamePlus,"passive snapshot changed NG+ flag");
        Require(host.Bytes()==untouched && !reloadCalls && !frontendCalls && !predicateCalls,"snapshot mutated save or invoked gameplay"); ++optionsCases;
        Require(!ExecuteSetNewGamePlus(captured,true,status) && host.Bytes()==untouched,"callback boundary was bypassed"); ++optionsCases;
        for (const bool enabled : {true,false}) {
            captured = host.Capture(); auto expected = host.Bytes();
            expected[0x182] = enabled ? 1 : 0;
            for (const auto offset : {8u,0x18u}) {
                std::uint64_t bits = 0; std::memcpy(&bits,expected.data()+offset,8);
                bits |= 0x40000; std::memcpy(expected.data()+offset,&bits,8);
            }
            fixtureScope = true;
            Require(ExecuteSetNewGamePlus(captured,enabled,status),"valid explicit NG+ change refused");
            Require(host.Bytes()==expected,"NG+ write changed neighbors, counters or unrelated dirty flags");
            Require(host.Capture().newGamePlus==enabled,"NG+ state did not reflect exact committed value"); ++optionsCases;
        }
        host.ResetOptions(); captured=host.Capture();
        host.Object(host.data,8,std::uint64_t(0x1234567887654321ull));
        host.Object(host.data,0x18,std::uint64_t(0xfedcba9876543210ull));
        auto expected=host.Bytes(); expected[0x182]=1;
        for (auto offset : {8u,0x18u}) { std::uint64_t bits=0;std::memcpy(&bits,expected.data()+offset,8);bits|=0x40000;std::memcpy(expected.data()+offset,&bits,8); }
        fixtureScope=true; Require(ExecuteSetNewGamePlus(captured,true,status) && host.Bytes()==expected,"late dirty bits were overwritten");
        const auto committed=host.Bytes(); Require(!ExecuteSetNewGamePlus(captured,true,status) && host.Bytes()==committed,"stale duplicate value was committed"); ++optionsCases;
        host.Refuses([&]{host.Object(host.kernel,0x6308,std::uint8_t(1));},"unloaded world admitted");
        host.Refuses([&]{host.Object(host.kernel,0x62f0,std::uint16_t(16));},"changed world epoch admitted");
        host.Refuses([&]{host.Put(0xc0bd2d8,std::uintptr_t(0));},"replaced save system admitted");
        host.Refuses([&]{host.Object(host.save,0x98,std::int32_t(0));},"uninitialized save admitted");
        host.Refuses([&]{host.Object(host.save,0x30,host.alternative);},"replaced shared container admitted");
        host.Refuses([&]{host.Put(0xc23c818,host.base+0xb6c2979);},"foreign GameSaveFile singleton admitted");
        host.Refuses([&]{host.Put(0xb6c2978,host.base+0x83d75c0);},"base-only SaveFile type admitted");
        host.Refuses([&]{host.Put(0xb6c28e8,host.base+0x9ea91d0);},"different save descriptor admitted");
        host.Refuses([&]{host.Put(0xb6c2904,std::int32_t(0));},"user-specific campaign scope admitted as shared");
        host.Refuses([&]{host.Put(0xb6c2900,std::uint32_t(0));},"unknown registered key admitted");
        host.Refuses([&]{host.Object(host.keys,host.slot*4,std::uint32_t(0));},"missing campaign entry admitted");
        host.Refuses([&]{host.Object(host.keys,((host.slot+1)%8)*4,kCampaignKey);host.Object(host.container,0x38,std::int32_t(2));},"duplicate campaign key admitted");
        host.Refuses([&]{host.Object(host.container,0x3c,std::int32_t(9000));},"unbounded save map admitted");
        host.Refuses([&]{host.Object(host.container,0x44,std::uint32_t(72));},"wrong native map stride admitted");
        host.Refuses([&]{host.Object(host.container,0x38,std::int32_t(2));},"inconsistent occupancy admitted");
        host.Refuses([&]{host.Object(host.values,host.slot*80+0x38,host.alternative);},"replaced live campaign admitted");
        host.Refuses([&]{host.Object(host.values,host.slot*80+0x38,host.base+0xb6c2978);},"image/default campaign storage admitted");
        host.Refuses([&]{host.Object(host.data,0,host.base+0x8597f88);},"wrong campaign native type admitted");
        host.Refuses([&]{host.Object(host.data,0x182,std::uint8_t(2));},"non-Boolean campaign flag admitted");
        host.Refuses([&]{host.Object(host.data,0x182,std::uint8_t(1));},"changed captured NG+ value admitted");
        host.Refuses([&]{host.Put(0x5c38de0+18*4,std::uint32_t(0x181));},"changed NG+ descriptor offset admitted");
        host.Refuses([&]{host.Put(0x2638d98,std::uint8_t(0x90));},"changed native setter proof admitted");
        host.Refuses([&]{optionsRuntime.supported=false;},"runtime unsupported verdict bypassed");
        host.ResetOptions();captured=host.Capture();auto before=host.Bytes();DWORD previous=0;
        Require(VirtualProtect(reinterpret_cast<void*>(host.data),0x1000,PAGE_READONLY,&previous)!=0,"make fixture save readonly");
        fixtureScope=true;Require(!ExecuteSetNewGamePlus(captured,true,status) && host.Bytes()==before,"readonly campaign was written");
        DWORD ignored=0;Require(VirtualProtect(reinterpret_cast<void*>(host.data),0x1000,previous,&ignored)!=0,"restore fixture protection");++optionsCases;
        host.Refuses([&]{options_fixture_reads::trigger=host.data+0x182;options_fixture_reads::mutation=[]{fixtureScope=false;};},"revoked callback admitted");
        host.Refuses([&]{options_fixture_reads::trigger=host.data+0x182;options_fixture_reads::mutation=[&]{host.Object(host.values,host.slot*80+0x38,host.alternative);};},"mid-snapshot campaign replacement admitted");
        host.ResetOptions();
        Require(host.Capture().defaultLoadoutApplied,"native loadout-applied flag was not captured");
        host.Object(host.data,0xd4,std::uint8_t(0));
        before=host.Bytes();
        Require(!host.Capture().defaultLoadoutApplied && host.Bytes()==before,"passive loadout capture changed campaign memory");
        host.Object(host.data,0xd4,std::uint8_t(2));
        Require(!ReadReloadOptionsState(host.Ready()).ready,"malformed loadout Boolean admitted"); ++optionsCases;
        for (const auto alias : {host.save+0x200,host.values+0x200}) {
            host.ResetOptions();
            std::memcpy(reinterpret_cast<void*>(alias),reinterpret_cast<void*>(host.data),0x2e0);
            host.Object(host.values,host.slot*80+0x38,alias);
            Require(!ReadReloadOptionsState(host.Ready()).ready,"fresh campaign range alias admitted"); ++optionsCases;
        }
        LoadoutCases(host);
        Require(!reloadCalls && !frontendCalls && !predicateCalls,"NG+ invoked another game action");
        LoadoutIntegrationCases(host);
        std::cout<<"{\"passed\":true,\"cases\":"<<optionsCases<<",\"assertions\":"<<assertions
            <<",\"game_launched\":false,\"real_engine_context_proven\":false,\"user_saves_touched\":false}\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
