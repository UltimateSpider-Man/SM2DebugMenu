// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera_debug_runtime.h"
#include "engine_dispatch.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace sm2debug {
namespace {
constexpr std::size_t kCameraImageSize = 0xce54000;
constexpr std::uintptr_t kCameraSystem = 0x9efee10, kCameraFlags = kCameraSystem + 0xf8;
struct CameraRuntime {
    std::mutex mutex;
    std::uintptr_t base = 0;
    bool supported = false, owned = false, initialEnabled = false, lastEnabled = false;
    CameraDebugToken owner;
} cameraRuntime;
bool CameraRead(std::uintptr_t address, void* value, std::size_t count) {
    SIZE_T copied = 0;
    return address >= 0x10000 && count && count <= UINTPTR_MAX-address &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), value, count, &copied) && copied == count;
}
template<class T> bool CameraRead(std::uintptr_t address, T& value) { return CameraRead(address, &value, sizeof(value)); }
bool CameraRange(std::uintptr_t address, std::size_t count, bool writable) {
    if (address < 0x10000 || !count || count > UINTPTR_MAX-address) return false;
    const auto end=address+count;
    while (address<end) {
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(reinterpret_cast<void*>(address),&m,sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD|PAGE_NOACCESS))) return false;
        const DWORD protection=m.Protect&0xff;
        if (writable ? (protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY) :
            (protection!=PAGE_READONLY && protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY)) return false;
        const auto first=reinterpret_cast<std::uintptr_t>(m.BaseAddress);
        if (first>address || m.RegionSize>UINTPTR_MAX-first || first+m.RegionSize<=address) return false;
        address=std::min(end,first+m.RegionSize);
    }
    return true;
}
bool CameraCode(std::uintptr_t base) {
    struct Pin {std::uintptr_t rva; std::size_t size; std::uint64_t hash;};
    // Singleton initialization/tick, layout initialization, native shake
    // application consumer and both retail game-setting writers of bit zero.
    static constexpr Pin pins[] = {
        {0x1529e99,0xc,0xb14b473de44b9046ull},{0x152a0e0,0xc,0xaddb3bded9770421ull},
        {0x2985aef,0x2d,0xfd8d72374d68f1a2ull},{0x2986064,0x12,0x6dfe4b8fc26b422full},
        {0x2987d67,0x34,0x032bd26bbfdfb6c7ull},{0x106678d,0x19,0x262c312718b849b9ull},
        {0x10675c4,0x23,0x980c50c6fd5b73bdull}
    };
    if (!base || base>UINTPTR_MAX-kCameraImageSize) return false;
    std::array<unsigned char,64> bytes{};
    for (const auto& pin:pins) {
        if (!CameraRead(base+pin.rva,bytes.data(),pin.size)) return false;
        std::uint64_t hash=14695981039346656037ull;
        for (std::size_t i=0;i<pin.size;++i) hash=(hash^bytes[i])*1099511628211ull;
        if (hash!=pin.hash) return false;
    }
    return true;
}
bool SameCameraIdentity(const CameraDebugToken& a,const CameraDebugToken& b) {
    return SameReloadContext(a.world,b.world) && a.cameraSystem==b.cameraSystem &&
        a.flagsAddress==b.flagsAddress && a.allocations==b.allocations && a.viewCount==b.viewCount;
}
CameraDebugState CameraReadLocked(const ReloadState& requested) {
    CameraDebugState result;
    result.supported=cameraRuntime.supported;
    result.status=L"Camera controls unavailable: no verified loaded world";
    const auto base=cameraRuntime.base;
    if (!result.supported || !requested.supported || !requested.ready || requested.token.imageBase!=base || !CameraCode(base)) return result;
    const auto current=ReadReloadState();
    if (!current.supported || !current.ready || !SameReloadContext(requested.token,current.token)) return result;
    auto& token=result.token;
    token.world=current.token; token.cameraSystem=base+kCameraSystem; token.flagsAddress=base+kCameraFlags;
    std::array<unsigned char,0x108> header{};
    if (!CameraRead(token.cameraSystem,header) || !CameraRange(token.flagsAddress,4,true)) return result;
    auto field=[&](std::size_t offset,auto& value) {std::memcpy(&value,header.data()+offset,sizeof(value));};
    field(0xb0,token.viewCount); field(0xf8,token.flags);
    std::uint32_t configuredViews=0;
    if (!CameraRead(base+0xc0b50f4,configuredViews) || !token.viewCount || token.viewCount>64 || token.viewCount!=configuredViews) return result;
    static constexpr std::size_t offsets[]={0,8,0x10,0x78,0xb8,0xc0,0xf0};
    const std::size_t sizes[]={token.viewCount*8u,0x5c00,0x5400,token.viewCount*8u,0x40,0x3b00,token.viewCount*16u};
    for (std::size_t i=0;i<token.allocations.size();++i) {
        auto& pointer=token.allocations[i]; field(offsets[i],pointer);
        if ((pointer&7) || !CameraRange(pointer,sizes[i],false) ||
            (pointer<base+kCameraImageSize && base<pointer+sizes[i])) return result;
        for (std::size_t j=0;j<i;++j)
            if (pointer<token.allocations[j]+sizes[j] && token.allocations[j]<pointer+sizes[i]) return result;
    }
    // Header identity must survive the allocation checks and world observation.
    std::array<unsigned char,0x108> again{};
    const auto finalWorld=ReadReloadState();
    if (!finalWorld.ready || !SameReloadContext(token.world,finalWorld.token) || !CameraRead(token.cameraSystem,again)) return result;
    for (std::size_t i=0;i<token.allocations.size();++i)
        if (std::memcmp(header.data()+offsets[i],again.data()+offsets[i],8)!=0) return result;
    if (std::memcmp(header.data()+0xb0,again.data()+0xb0,4)!=0 || std::memcmp(header.data()+0xf8,again.data()+0xf8,4)!=0) return result;
    result.ready=true; result.shakeEnabled=(token.flags&1)!=0;
    result.status=L"Camera shake uses the original runtime mask; game camera settings may replace it";
    return result;
}
ReloadState CameraWorld(const CameraDebugToken& token) {
    ReloadState world; world.supported=world.ready=true; world.token=token.world; return world;
}
void CameraRelease() {cameraRuntime.owned=false; cameraRuntime.owner={};}
bool CameraCompareWrite(const CameraDebugState& state,bool enabled) {
    auto expected=state.token.flags;
    const auto desired=(expected&~std::uint32_t(1))|(enabled?1u:0u);
    return __atomic_compare_exchange_n(reinterpret_cast<volatile std::uint32_t*>(state.token.flagsAddress),
        &expected,desired,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
}
}
void InitializeCameraDebugRuntime(HMODULE module,bool supported) {
    std::lock_guard lock(cameraRuntime.mutex);
    cameraRuntime.supported=false; cameraRuntime.base=0; CameraRelease();
    if (!supported || !module || module!=GetModuleHandleW(nullptr)) return;
    const auto base=reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!CameraRead(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x1000 ||
        !CameraRead(base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.TimeDateStamp!=0x679b5991 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage!=kCameraImageSize ||
        (nt.OptionalHeader.ImageBase!=0x140000000 && nt.OptionalHeader.ImageBase!=base) || !CameraCode(base)) return;
    cameraRuntime.base=base; cameraRuntime.supported=true;
}
CameraDebugState ReadCameraDebugState(const ReloadState& world) {
    std::lock_guard lock(cameraRuntime.mutex); return CameraReadLocked(world);
}
bool ExecuteSetCameraShake(const CameraDebugState& captured,bool enabled,std::wstring& status) {
    status=L"Camera shake refused: callback, world, camera allocations or value changed";
    if (!IsWithinVerifiedEngineCallback()) return false;
    std::unique_lock lock(cameraRuntime.mutex,std::try_to_lock);
    if (!lock.owns_lock() || !captured.supported || !captured.ready) return false;
    const auto fresh=CameraReadLocked(CameraWorld(captured.token));
    if (!fresh.ready || !SameCameraIdentity(captured.token,fresh.token) || fresh.shakeEnabled!=captured.shakeEnabled) return false;
    const auto finalState=CameraReadLocked(CameraWorld(fresh.token));
    if (!finalState.ready || !SameCameraIdentity(fresh.token,finalState.token) ||
        finalState.shakeEnabled!=captured.shakeEnabled || !IsWithinVerifiedEngineCallback()) return false;
    if (cameraRuntime.owned && (!SameCameraIdentity(cameraRuntime.owner,finalState.token) ||
        cameraRuntime.lastEnabled!=finalState.shakeEnabled)) CameraRelease();
    if (enabled==finalState.shakeEnabled) {status=L"Camera shake already has the requested value"; return true;}
    if (!CameraCompareWrite(finalState,enabled)) return false;
    if (!cameraRuntime.owned) {cameraRuntime.initialEnabled=finalState.shakeEnabled; cameraRuntime.owner=finalState.token;}
    cameraRuntime.lastEnabled=enabled; cameraRuntime.owned=true;
    status=enabled?L"Camera shake enabled":L"Camera shake disabled";
    return true;
}
bool ResetOwnedCameraDebugOverrides(std::wstring& status) {
    status=L"Camera restoration requires the verified engine callback";
    if (!IsWithinVerifiedEngineCallback()) return false;
    std::unique_lock lock(cameraRuntime.mutex,std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (!cameraRuntime.owned) {status=L"No camera override is owned";return true;}
    const auto fresh=CameraReadLocked(CameraWorld(cameraRuntime.owner));
    if (!fresh.ready || !SameCameraIdentity(cameraRuntime.owner,fresh.token) || fresh.shakeEnabled!=cameraRuntime.lastEnabled) {
        CameraRelease(); status=L"Camera override released; changed game state was preserved";return true;
    }
    const auto finalState=CameraReadLocked(CameraWorld(cameraRuntime.owner));
    if (!IsWithinVerifiedEngineCallback()) return false;
    if (!finalState.ready || !SameCameraIdentity(cameraRuntime.owner,finalState.token) || finalState.shakeEnabled!=cameraRuntime.lastEnabled) {
        CameraRelease();status=L"Camera override released; changed game state was preserved";return true;
    }
    if (!CameraCompareWrite(finalState,cameraRuntime.initialEnabled)) {status=L"Camera restoration refused: mask changed";return false;}
    CameraRelease();status=L"Owned camera shake bit restored; other camera flags preserved";return true;
}
}
