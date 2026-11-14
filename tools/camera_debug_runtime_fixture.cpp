// Actual retail code pins plus owned synthetic camera/world memory only.
// No game process is accessed and no native gameplay function is invoked.
#define wmain UnusedCameraReloadFixtureMain
#include "reload_runtime_fixture.cpp"
#undef wmain
#include <atomic>
#include <functional>

namespace camera_fixture {
std::uintptr_t trigger=0;
std::function<void()> afterRead, beforeAtomic;
unsigned atomicCalls=0;
BOOL WINAPI Read(HANDLE process,LPCVOID source,LPVOID target,SIZE_T size,SIZE_T* copied) {
    const BOOL ok=::ReadProcessMemory(process,source,target,size,copied);
    if (reinterpret_cast<std::uintptr_t>(source)==trigger && afterRead) {
        auto action=std::move(afterRead);afterRead={};trigger=0;action();
    }
    return ok;
}
bool Compare(volatile std::uint32_t* address,std::uint32_t* expected,std::uint32_t desired,bool weak,int success,int failure) {
    ++atomicCalls;
    if (beforeAtomic) {auto action=std::move(beforeAtomic);beforeAtomic={};action();}
    return __atomic_compare_exchange_n(address,expected,desired,weak,success,failure);
}
}
#define ReadProcessMemory camera_fixture::Read
#define __atomic_compare_exchange_n camera_fixture::Compare
#include "../src/camera_debug_runtime.cpp"
#undef __atomic_compare_exchange_n
#undef ReadProcessMemory

namespace {
using namespace sm2debug;
unsigned cameraCases=0;
struct CameraHost : HostImage {
    std::array<std::uintptr_t,7> cameraAllocations{};
    static constexpr std::uintptr_t pinRvas[]={0x1529e99,0x152a0e0,0x2985aef,0x2986064,0x2987d67,0x106678d,0x10675c4};
    static constexpr std::size_t pinSizes[]={0xc,0xc,0x2d,0x12,0x34,0x19,0x23};
    explicit CameraHost(const wchar_t* path):HostImage(path) {
        const std::size_t sizes[]={0x1000,0x6000,0x6000,0x1000,0x1000,0x4000,0x1000};
        for (std::size_t i=0;i<7;++i) cameraAllocations[i]=Allocate(sizes[i]);
        Commit(kCameraSystem,0x108);
    }
    void ResetCamera(std::uint32_t flags=0xa5a51235) {
        Reset(); camera_fixture::trigger=0; camera_fixture::afterRead={};camera_fixture::beforeAtomic={};camera_fixture::atomicCalls=0;
        for (std::size_t i=0;i<7;++i) CopyFile(pinRvas[i],pinSizes[i]);
        std::memset(reinterpret_cast<void*>(base+kCameraSystem),0x3c,0x108);
        static constexpr std::size_t offsets[]={0,8,0x10,0x78,0xb8,0xc0,0xf0};
        for (std::size_t i=0;i<7;++i) Put(kCameraSystem+offsets[i],cameraAllocations[i]);
        Put(kCameraSystem+0xb0,std::uint32_t(1));Put(kCameraFlags,flags);Put(0xc0b50f4,std::uint32_t(1));
        std::lock_guard lock(cameraRuntime.mutex);
        cameraRuntime.base=base;cameraRuntime.supported=true;CameraRelease();
        Require(CameraCode(base),"all camera pins match actual retail executable bytes");
    }
    CameraDebugState Capture() {
        auto s=ReadCameraDebugState(Ready());Require(s.supported&&s.ready,"initialized camera state ready");return s;
    }
    std::array<unsigned char,0x108> Bytes() {
        std::array<unsigned char,0x108> b{};std::memcpy(b.data(),reinterpret_cast<void*>(base+kCameraSystem),b.size());return b;
    }
    std::uint32_t Flags() {return *reinterpret_cast<std::uint32_t*>(base+kCameraFlags);}
    void NoNative() {Require(!reloadCalls&&!frontendCalls&&!unloadCalls&&!predicateCalls,"camera control must not invoke gameplay calls");}
    template<class F> void Refuses(F mutate,const char* message) {
        ResetCamera();auto captured=Capture();fixtureScope=true;mutate(captured);const auto before=Bytes();std::wstring status;
        Require(!ExecuteSetCameraShake(captured,false,status),message);
        Require(Bytes()==before,"refusal modifies no camera header bytes");Require(camera_fixture::atomicCalls==0,"refusal attempts no atomic write");
        NoNative();++cameraCases;
    }
};
void BasicCases(CameraHost& h) {
    std::wstring status;
    for (const auto mask:{0u,1u,0x80000000u,0x80000001u,0xfffffffeu,0xffffffffu,0xa5a51235u}) {
        h.ResetCamera(mask);auto s=h.Capture();const auto before=h.Bytes();
        Require(s.shakeEnabled==((mask&1)!=0),"reader exposes exactly bit zero");
        Require(h.Bytes()==before&&!camera_fixture::atomicCalls&&!cameraRuntime.owned,"passive reads do not write/acquire ownership");
        fixtureScope=true;
        Require(ExecuteSetCameraShake(s,!s.shakeEnabled,status),"actual opposite bit change succeeds");
        Require(h.Flags()==(mask^1u),"only original bit zero toggled");
        auto expected=before;const auto flags=mask^1u;std::memcpy(expected.data()+0xf8,&flags,4);
        Require(h.Bytes()==expected,"entire native header preserved outside requested mask bit");
        Require(cameraRuntime.owned&&cameraRuntime.initialEnabled==s.shakeEnabled,"initial state owned after effective change");
        Require(ResetOwnedCameraDebugOverrides(status)&&h.Flags()==mask&&!cameraRuntime.owned,"reset restores original bit and releases ownership");
        h.NoNative();++cameraCases;
    }
    h.ResetCamera();auto s=h.Capture();fixtureScope=true;
    Require(ExecuteSetCameraShake(s,true,status)&&!camera_fixture::atomicCalls&&!cameraRuntime.owned,"same-value action does not acquire ownership or write");++cameraCases;
    Require(ExecuteSetCameraShake(s,false,status),"first change");
    Require(ExecuteSetCameraShake(h.Capture(),true,status),"second change");
    Require(ExecuteSetCameraShake(h.Capture(),false,status),"third change");
    Require(ResetOwnedCameraDebugOverrides(status)&&h.Flags()==0xa5a51235,"multiple edits restore original baseline");++cameraCases;
    h.ResetCamera();s=h.Capture();fixtureScope=true;
    Require(ExecuteSetCameraShake(s,false,status),"own mask before unrelated game update");
    h.Put(kCameraFlags,h.Flags()^0x200u);const auto changed=h.Flags();
    Require(ResetOwnedCameraDebugOverrides(status)&&h.Flags()==(changed|1u),"restoration preserves new unrelated game mask bits");++cameraCases;
    h.ResetCamera();s=h.Capture();fixtureScope=true;h.Put(kCameraFlags,s.token.flags^0x400u);
    Require(ExecuteSetCameraShake(s,false,status)&&h.Flags()==((s.token.flags^0x400u)&~1u),"action preserves unrelated bits changed after capture");++cameraCases;
}
void RefusalCases(CameraHost& h) {
    h.Refuses([](auto&){fixtureScope=false;},"outside verified callback");
    h.Refuses([](auto& s){s.ready=false;},"not-ready capture");
    h.Refuses([](auto& s){s.supported=false;},"unsupported capture");
    h.Refuses([](auto&){cameraRuntime.supported=false;},"unsupported runtime");
    h.Refuses([](auto& s){++s.token.cameraSystem;},"wrong singleton address");
    h.Refuses([](auto& s){s.token.flagsAddress+=4;},"wrong flags destination");
    h.Refuses([&](auto&){h.Put(kCameraFlags,h.Flags()&~1u);},"stale target bit");
    h.Refuses([&](auto&){h.Object(h.kernel,0x6308,std::uint8_t(1));},"world unloading");
    h.Refuses([&](auto&){h.Object(h.kernel,0x62f0,std::uint16_t(16));},"world sequence changed");
    h.Refuses([&](auto&){h.Put(kCameraSystem+0xb0,std::uint32_t(0));},"uninitialized view count");
    h.Refuses([&](auto&){h.Put(kCameraSystem+0xb0,std::uint32_t(65));h.Put(0xc0b50f4,std::uint32_t(65));},"view count exceeds native node pool");
    h.Refuses([&](auto&){h.Put(0xc0b50f4,std::uint32_t(2));},"view count differs from native initialization input");
    h.Refuses([&](auto&){h.Put(kCameraSystem+8,std::uintptr_t(0));},"null node pool");
    h.Refuses([&](auto&){h.Put(kCameraSystem+8,h.cameraAllocations[1]+1);},"misaligned node pool");
    h.Refuses([&](auto&){h.Put(kCameraSystem,h.base+kCameraSystem);},"camera allocation aliases executable globals");
    h.Refuses([&](auto&){h.Put(kCameraSystem+0x78,h.cameraAllocations[0]);},"camera allocations overlap");
    h.Refuses([&](auto&){h.Put(kCameraSystem+0x78,h.Allocate(0x1000));},"camera reinitialization invalidates captured allocations");
    for (const auto r:CameraHost::pinRvas)
        h.Refuses([&](auto&){*reinterpret_cast<unsigned char*>(h.base+r)^=1;},"changed native code pin");
    h.Refuses([&](auto&){camera_fixture::trigger=h.base+kCameraSystem;camera_fixture::afterRead=[] {fixtureScope=false;};},"callback revoked during fresh read");
    h.Refuses([&](auto&){camera_fixture::trigger=h.base+kCameraSystem;camera_fixture::afterRead=[&] {h.Object(h.kernel,0x62f0,std::uint16_t(16));};},"world changes during camera copy");
    h.ResetCamera();auto s=h.Capture();fixtureScope=true;const auto before=h.Bytes();std::wstring status;
    std::atomic<bool> locked{false},release{false};
    std::thread worker([&] {std::lock_guard lock(cameraRuntime.mutex);locked=true;while(!release) std::this_thread::yield();});
    while(!locked) std::this_thread::yield();
    const bool result=ExecuteSetCameraShake(s,false,status);release=true;worker.join();
    Require(!result&&h.Bytes()==before&&!camera_fixture::atomicCalls,"busy runtime refuses without blocking or writing");++cameraCases;
    h.ResetCamera();s=h.Capture();fixtureScope=true;
    camera_fixture::beforeAtomic=[&] {h.Put(kCameraFlags,h.Flags()^0x800u);};
    Require(!ExecuteSetCameraShake(s,false,status)&&h.Flags()==(s.token.flags^0x800u)&&!cameraRuntime.owned,"atomic race refuses without acquiring ownership");++cameraCases;
    h.ResetCamera();s=h.Capture();fixtureScope=true;DWORD previous=0;
    Require(VirtualProtect(reinterpret_cast<void*>(h.base+kCameraSystem),0x108,PAGE_READONLY,&previous)!=FALSE,"protect camera header read-only");
    const bool changed=ExecuteSetCameraShake(s,false,status);DWORD ignored=0;
    Require(VirtualProtect(reinterpret_cast<void*>(h.base+kCameraSystem),0x108,previous,&ignored)!=FALSE,"restore fixture page protection");
    Require(!changed&&!camera_fixture::atomicCalls,"read-only flags refuse");++cameraCases;
}
void OwnershipCases(CameraHost& h) {
    std::wstring status;
    for (unsigned variation=0;variation<5;++variation) {
        h.ResetCamera();fixtureScope=true;
        Require(ExecuteSetCameraShake(h.Capture(),false,status),"establish owned override");
        if (variation==0) h.Put(kCameraFlags,h.Flags()|1u);
        if (variation==1) h.Object(h.kernel,0x62f0,std::uint16_t(16));
        if (variation==2) h.Put(kCameraSystem+0x78,h.Allocate(0x1000));
        if (variation==3) h.Object(h.kernel,0x6308,std::uint8_t(1));
        if (variation==4) *reinterpret_cast<unsigned char*>(h.base+CameraHost::pinRvas[0])^=1;
        const auto before=h.Bytes();const auto calls=camera_fixture::atomicCalls;
        Require(ResetOwnedCameraDebugOverrides(status)&&!cameraRuntime.owned,"changed native state releases obsolete ownership");
        Require(h.Bytes()==before&&camera_fixture::atomicCalls==calls,"obsolete ownership never writes old-world or game-owned state");++cameraCases;
    }
    h.ResetCamera();fixtureScope=true;Require(ExecuteSetCameraShake(h.Capture(),false,status),"own for reset callback guard");
    fixtureScope=false;const auto before=h.Bytes();
    Require(!ResetOwnedCameraDebugOverrides(status)&&cameraRuntime.owned&&h.Bytes()==before,"reset outside callback retains ownership without mutation");++cameraCases;
    fixtureScope=true;camera_fixture::beforeAtomic=[&] {h.Put(kCameraFlags,h.Flags()^0x2000u);};
    Require(!ResetOwnedCameraDebugOverrides(status)&&cameraRuntime.owned,"restoration CAS race fails safely");
    Require(ResetOwnedCameraDebugOverrides(status)&&!cameraRuntime.owned&&(h.Flags()&0x2000u),"later reset preserves racing unrelated bit");++cameraCases;
    h.NoNative();
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        Require(argc==2,"actual executable path required");CameraHost host(argv[1]);
        BasicCases(host);RefusalCases(host);OwnershipCases(host);
        InitializeCameraDebugRuntime(nullptr,false);
        Require(!ReadCameraDebugState(host.Ready()).ready,"unsupported initialization disables reads");
        InitializeCameraDebugRuntime(reinterpret_cast<HMODULE>(host.base),true);
        Require(!ReadCameraDebugState(host.Ready()).ready,"foreign module initialization rejected");++cameraCases;
        std::cout<<"{\"passed\":true,\"cases\":"<<cameraCases<<",\"assertions\":"<<assertions<<",\"native_calls\":0,\"game_process_writes\":0}\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
