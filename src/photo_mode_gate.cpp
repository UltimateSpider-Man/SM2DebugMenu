// SPDX-License-Identifier: GPL-3.0-or-later
#include "photo_mode_gate.h"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace sm2debug::photo_gate {
namespace {
constexpr std::uintptr_t imageSize = 0xce54000, objectRva = 0xb6ba560;
constexpr std::uintptr_t vtableRva = 0x549a498, slotRva = vtableRva + 0x40;
constexpr std::uintptr_t originalRva = 0x15a66b0;
using CanEnter = bool (*)(void*);
std::atomic<CanEnter> nativeCanEnter{nullptr};
std::atomic<bool (*)()> visible{nullptr};
std::atomic<std::uintptr_t> imageBase{0};
std::atomic<bool> installed{false}, restoredProtection{true};
std::atomic<std::uint64_t> blocked{0}, forwarded{0}, invalidObjects{0};
std::mutex setup;
struct Patch {
    void** slot = nullptr;
    void* original = nullptr;
    DWORD protection = 0;
    bool applied = false, protectionPending = false;
} patch;

bool ReadBytes(std::uintptr_t address, void* target, std::size_t size) {
    SIZE_T count = 0;
    return address >= 0x10000 && size && size <= UINTPTR_MAX-address &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), target, size, &count) && count == size;
}
template<class T> bool Read(std::uintptr_t address, T& value) { return ReadBytes(address, &value, sizeof(value)); }
bool ImageRegion(std::uintptr_t base, std::uintptr_t address, std::size_t size, bool executable, bool writable = false) {
    MEMORY_BASIC_INFORMATION region{};
    if (!base || address < base || size > imageSize || address-base > imageSize-size ||
        VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE || reinterpret_cast<std::uintptr_t>(region.AllocationBase) != base ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (start > address || region.RegionSize > UINTPTR_MAX-start || size > start+region.RegionSize-address) return false;
    const DWORD protection = region.Protect & 0xff;
    if (executable) return protection == PAGE_EXECUTE_READ;
    if (writable) return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY;
    return protection == PAGE_READONLY;
}
bool Bytes(std::uintptr_t base, std::uintptr_t rva, const char* hex) {
    const std::size_t size = std::strlen(hex)/2;
    std::array<unsigned char, 512> bytes{};
    if (!size || size > bytes.size() || !ImageRegion(base, base+rva, size, true) || !ReadBytes(base+rva, bytes.data(), size)) return false;
    auto digit = [](char c) -> unsigned { return c >= '0' && c <= '9' ? c-'0' : c-'a'+10; };
    for (std::size_t i = 0; i < size; ++i) if (bytes[i] != ((digit(hex[i*2]) << 4) | digit(hex[i*2+1]))) return false;
    return true;
}
bool Function(std::uintptr_t base, const IMAGE_NT_HEADERS64& nt, DWORD begin, DWORD end, DWORD unwind) {
    const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.Size || directory.Size % sizeof(RUNTIME_FUNCTION) || directory.VirtualAddress >= imageSize ||
        directory.Size > imageSize-directory.VirtualAddress) return false;
    const std::size_t count = directory.Size/sizeof(RUNTIME_FUNCTION);
    std::size_t first = 0, last = count;
    while (first < last) {
        const auto middle = first+(last-first)/2;
        RUNTIME_FUNCTION value{};
        if (!Read(base+directory.VirtualAddress+middle*sizeof(value), value)) return false;
        if (value.BeginAddress < begin) first = middle+1; else last = middle;
    }
    RUNTIME_FUNCTION value{};
    return first < count && Read(base+directory.VirtualAddress+first*sizeof(value), value) &&
        value.BeginAddress == begin && value.EndAddress == end && value.UnwindData == unwind;
}
bool Identity(std::uintptr_t base) {
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    std::uintptr_t singleton = 0, vt = 0, method = 0, locator = 0;
    std::array<std::uint32_t,6> col{};
    std::array<char,25> type{};
    constexpr char name[] = ".?AVPhotomodeSystem@@";
    if (!Read(base,dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0x40 || dos.e_lfanew > 0x1000 ||
        !Read(base+dos.e_lfanew,nt) || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.TimeDateStamp != 0x679b5991 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != imageSize || nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION ||
        (nt.OptionalHeader.ImageBase != 0x140000000 && nt.OptionalHeader.ImageBase != base) ||
        !ImageRegion(base,base+slotRva,sizeof(void*),false) || !ImageRegion(base,base+objectRva,0xf34,false,true) ||
        !Read(base+0xc237f90,singleton) || singleton != base+objectRva ||
        !Read(singleton,vt) || vt != base+vtableRva || !Read(base+slotRva,method) || method != base+originalRva ||
        !Read(base+vtableRva-8,locator) || locator != base+0x8e138b0 || !Read(locator,col) ||
        col[0] != 1 || col[1] || col[2] || col[3] != 0xab4f4c0 || col[4] != 0x8e138d8 || col[5] != 0x8e138b0 ||
        !ReadBytes(base+0xab4f4d0,type.data(),sizeof(name)) || std::memcmp(type.data(),name,sizeof(name))) return false;
    // Native Open359A640 calls this precise virtual before changing +34/+38.
    // Both the controller route15A6BD3 and pause-menu routeE9EBC9 call Open.
    // The native +46 lock is deliberately untouched: cinematics also own it.
    return Function(base,nt,0x15a66b0,0x15a67f9,0x92f1aec) &&
        Function(base,nt,0x359a640,0x359a674,0x9661668) &&
        Bytes(base,0x359a640,"40534883ec20488b01488bd9ff504084c074198b433885c0740a894334c7433800000000b0014883c4205bc332c04883c4205bc3") &&
        Bytes(base,0x15a66b0,"48895c24084889742418574883ec20488bd9e8c910ff0184c00f8418010000") &&
        Bytes(base,0x3597790,"80793c007408807946000f94c0c332c0c3") &&
        Bytes(base,0x3599490,"8b413883f80e740b83c0f683f8027603b001c332c0c3") &&
        Bytes(base,0x19b4c0,"4883ec28488d0d95f0510be830a04001488d0d793d0f054883c428e978c6ec04") &&
        Bytes(base,0x3596fb9,"48893dd00fca08");
}

bool Gate(void* object) {
    const auto base = imageBase.load(std::memory_order_acquire);
    // No pointer chasing or engine call occurs on a malformed/foreign object.
    if (!base || reinterpret_cast<std::uintptr_t>(object) != base+objectRva) { ++invalidObjects; return false; }
    const auto callback = visible.load(std::memory_order_acquire);
    if (callback && callback()) { ++blocked; return false; }
    const auto original = nativeCanEnter.load(std::memory_order_acquire);
    if (!original) return false;
    ++forwarded;
    // Preserve the original native thread, TLS role and restrictions. This
    // function is never called by a worker to ask the engine for permission.
    return original(object);
}
bool Apply() {
    DWORD old = 0;
    if (!VirtualProtect(patch.slot,sizeof(void*),PAGE_READWRITE,&old)) return false;
    patch.protection = old; patch.protectionPending = true; restoredProtection = false;
    patch.applied = InterlockedCompareExchangePointer(patch.slot,reinterpret_cast<void*>(&Gate),patch.original) == patch.original;
    DWORD ignored = 0;
    if (VirtualProtect(patch.slot,sizeof(void*),patch.protection,&ignored)) patch.protectionPending = false;
    restoredProtection = !patch.protectionPending;
    return patch.applied && !patch.protectionPending;
}
bool Restore() {
    if (!patch.slot) return true;
    if (patch.applied) {
        void* current = nullptr;
        if (!Read(reinterpret_cast<std::uintptr_t>(patch.slot),current)) return false;
        if (current == reinterpret_cast<void*>(&Gate)) {
            DWORD old = 0;
            if (!VirtualProtect(patch.slot,sizeof(void*),PAGE_READWRITE,&old)) return false;
            if (!patch.protectionPending) patch.protection = old;
            patch.protectionPending = true; restoredProtection = false;
            InterlockedCompareExchangePointer(patch.slot,patch.original,reinterpret_cast<void*>(&Gate));
        }
        // A foreign replacement is not ours to overwrite. The module remains
        // pinned, so a foreign hook that chains through Gate remains valid.
        patch.applied = false;
    }
    if (patch.protectionPending) {
        DWORD ignored = 0;
        if (VirtualProtect(patch.slot,sizeof(void*),patch.protection,&ignored)) patch.protectionPending = false;
    }
    restoredProtection = !patch.protectionPending;
    if (!patch.protectionPending) patch = {};
    return !patch.protectionPending;
}
}

bool Initialize(HMODULE owner, bool supported, bool (*menuVisible)()) {
    std::lock_guard lock(setup);
    if (installed) return true;
    if (!Restore() || !owner || !supported || !menuVisible) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!Identity(base)) return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(owner),&pinned) || pinned != owner) return false;
    nativeCanEnter.store(reinterpret_cast<CanEnter>(base+originalRva),std::memory_order_release);
    imageBase.store(base,std::memory_order_release);
    visible.store(menuVisible,std::memory_order_release);
    patch = {reinterpret_cast<void**>(base+slotRva),reinterpret_cast<void*>(base+originalRva),0,false,false};
    if (!Apply()) { visible = nullptr; Restore(); return false; }
    installed = true;
    return true;
}
void Shutdown() {
    std::lock_guard lock(setup);
    visible.store(nullptr,std::memory_order_release);
    Restore(); installed = false;
    // Keep original/base published for any already-in-flight/chained wrapper.
}
Diagnostics Observe() {
    Diagnostics result;
    result.installed = installed; result.protectionRestored = restoredProtection;
    result.blocked = blocked; result.forwarded = forwarded; result.invalidObjects = invalidObjects;
    const auto base = imageBase.load(std::memory_order_acquire);
    std::uintptr_t object = 0, vt = 0;
    std::uint32_t state = UINT32_MAX;
    std::uint8_t initialized = 0;
    if (base && Read(base+0xc237f90,object) && object == base+objectRva && Read(object,vt) && vt == base+vtableRva &&
        Read(object+0x3c,initialized) && initialized == 1 && Read(object+0x38,state) && state <= 14) {
        result.stateKnown = true; result.nativeState = state;
        // Exact native3599490 predicate;10..12 are suspended/inactive states.
        result.photoActive = state != 14 && (state < 10 || state > 12);
    }
    return result;
}
}
