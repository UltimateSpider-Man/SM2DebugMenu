// SPDX-License-Identifier: GPL-3.0-or-later
#include "reload_runtime.h"
#include "engine_dispatch.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <tuple>

namespace sm2debug {
namespace {
constexpr std::size_t kImageSize = 0xce54000;
constexpr std::size_t kMaximumCheckpoints = 4096;
constexpr std::size_t kMaximumName = 512;
using ReloadFunction = bool (*)(void*, std::uint32_t, bool);
using FrontendFunction = bool (*)(void*);
using UnloadFunction = bool (*)(void*);
using KernelPredicate = bool (*)(void*);
using CheckpointRecord = std::array<unsigned char, 72>;
struct ReloadImage {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    std::uintptr_t exceptionTable = 0;
    std::uint32_t exceptionCount = 0;
    bool Contains(std::uintptr_t address, std::size_t count) const {
        return base && address >= base && count <= size && address-base <= size-count;
    }
    std::uintptr_t At(std::uintptr_t rva, std::size_t count = 1) const {
        return rva <= size && count <= size-rva && base <= UINTPTR_MAX-rva ? base+rva : 0;
    }
};
struct ReloadRuntime {
    std::mutex mutex;
    bool initialized = false, supported = false, haveIdentity = false;
    bool awaitingTransition = false, transitionSeen = false;
    std::uint64_t serial = 0;
    ReloadImage image;
    ReloadToken identity;
    ReloadFunction reload = nullptr;
    FrontendFunction frontend = nullptr;
    UnloadFunction unload = nullptr;
    KernelPredicate kernelPredicate = nullptr;
    ReloadToken cacheIdentity;
    std::vector<CheckpointRecord> cachedRecords;
    std::shared_ptr<const std::vector<ReloadCheckpoint>> cachedCheckpoints;
    ULONGLONG namesReadAt = 0;
    std::wstring reason = L"Reload runtime has not initialized";
} reloadRuntime;

bool ReadBytes(std::uintptr_t address, void* destination, std::size_t size) {
    SIZE_T copied = 0;
    return address >= 0x10000 && size && size <= UINTPTR_MAX-address &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, size, &copied) && copied == size;
}
template<class T> bool Read(std::uintptr_t address, T& value) { return ReadBytes(address, &value, sizeof(value)); }
template<class T> bool ReadImage(const ReloadImage& image, std::uintptr_t address, T& value) {
    return image.Contains(address, sizeof(value)) && Read(address, value);
}
bool WritableStorage(std::uintptr_t address, std::size_t size) {
    if (address < 0x10000 || !size || size > UINTPTR_MAX-address) return false;
    const auto end = address+size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            ((memory.Protect & 0xff) != PAGE_READWRITE && (memory.Protect & 0xff) != PAGE_WRITECOPY)) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (address < start || address-start >= memory.RegionSize || memory.RegionSize > UINTPTR_MAX-start) return false;
        address = std::min(end, start+memory.RegionSize);
    }
    return true;
}
bool SameBytes(const ReloadImage& image, std::uintptr_t rva, const char* hexadecimal) {
    const std::size_t size = std::strlen(hexadecimal)/2;
    std::array<unsigned char, 128> bytes{};
    if (!size || size > bytes.size() || !image.At(rva, size) || !ReadBytes(image.At(rva), bytes.data(), size)) return false;
    auto nibble = [](char c) -> unsigned { return c >= '0' && c <= '9' ? c-'0' : c-'a'+10; };
    for (std::size_t index = 0; index < size; ++index)
        if (bytes[index] != ((nibble(hexadecimal[index*2]) << 4) | nibble(hexadecimal[index*2+1]))) return false;
    return true;
}
bool CodeHash(const ReloadImage& image, std::uintptr_t rva, std::size_t size, std::uint64_t expected) {
    std::array<unsigned char, 512> bytes{};
    if (!size || size > bytes.size() || !image.At(rva,size) || !ReadBytes(image.At(rva),bytes.data(),size)) return false;
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i])*1099511628211ull;
    return hash == expected;
}
bool ImageString(const ReloadImage& image, std::uintptr_t address, const char* expected) {
    const auto length = std::strlen(expected)+1;
    std::array<char, 96> value{};
    return length <= value.size() && image.Contains(address, length) && ReadBytes(address, value.data(), length) &&
        std::memcmp(value.data(), expected, length) == 0;
}
bool Rtti(const ReloadImage& image, std::uintptr_t vtableRva, std::uintptr_t colRva,
          std::uintptr_t typeRva, const char* name) {
    std::uintptr_t col = 0;
    std::array<std::uint32_t, 6> fields{};
    return ReadImage(image, image.At(vtableRva-8), col) && col == image.At(colRva) &&
        image.Contains(col, sizeof(fields)) && Read(col, fields) && fields[0] == 1 && fields[1] == 0 &&
        fields[3] == typeRva && fields[5] == colRva && ImageString(image, image.At(typeRva+16), name);
}
bool FunctionStart(const ReloadImage& image, std::uint32_t rva, std::uint32_t end, std::uint32_t unwind) {
    std::size_t first = 0, last = image.exceptionCount;
    while (first < last) {
        const auto middle = first+(last-first)/2;
        RUNTIME_FUNCTION entry{};
        if (!ReadImage(image, image.exceptionTable+middle*sizeof(entry), entry)) return false;
        if (entry.BeginAddress < rva) first = middle+1; else last = middle;
    }
    RUNTIME_FUNCTION entry{};
    return first < image.exceptionCount && ReadImage(image, image.exceptionTable+first*sizeof(entry), entry) &&
        entry.BeginAddress == rva && entry.EndAddress == end && entry.UnwindData == unwind;
}
bool CodePins(const ReloadImage& image) {
    return FunctionStart(image, 0x34cbc90, 0x34cbcc2, 0x964789c) &&
        FunctionStart(image, 0x14cd7f0, 0x14cd839, 0x92ce19c) &&
        FunctionStart(image, 0x28a5b60, 0x28a5bac, 0x94f4d6c) &&
        FunctionStart(image, 0x2d696c0, 0x2d69702, 0x956cde4) &&
        FunctionStart(image, 0x34cd270, 0x34ce690, 0x9647e18) &&
        SameBytes(image, 0x34cbc90, "48895c241048896c2418564883ec70410fb6e88bf2488bd9e863d8f8ff84c00f") &&
        SameBytes(image, 0x14cd7f0, "40534883ec20e8c5b8ff01488bc8488bd8e81ac9ff0184c07429488bcbe8bec9") &&
        SameBytes(image, 0x14cd816, "c6054910d10a00e86e0a8101488bcbc64070014883c4205be92de7ff01") &&
        SameBytes(image, 0x34c90c0, "488b05e1d5d508c3") &&
        SameBytes(image, 0x345d400, "488d0d2902dc08e9a40356ff") &&
        SameBytes(image, 0x2cde290, "488b0541f03d09c3") &&
        SameBytes(image, 0x29bd7b0, "8b118bcac1e91485c9742481e2ffff0f008bc248c1e00448030512147009") &&
        SameBytes(image, 0x9dd798, "33d2488bcbe8eee4ae02") &&
        SameBytes(image, 0x29b9e40, "488b4910e917bdeeff") &&
        // Native switch state 0 starts loading; state 22 produces ready=1
        // and performs normal loaded-level/checkpoint update work.
        SameBytes(image, 0x34ce628, "05d34c03") &&
        SameBytes(image, 0x34ce680, "0be04c03") &&
        SameBytes(image, 0x34cd33b, "41c7872806430001000000") &&
        SameBytes(image, 0x34cdff4, "4589af4806430041c7872806430016000000") &&
        SameBytes(image, 0x34ce00b, "4180bf65064300000f85ed00000041c6876506430001") &&
        SameBytes(image, 0x28a5b60, "40534883ec20488bd9e8921e4c0084c07508b0014883c4205bc3488d8ba8000000e83a3b4c004885c074110fb68021010000d0e824014883c4205bc38b4310c1e812f6d024014883c4205bc3") &&
        SameBytes(image, 0x2d67a00, "0fb60539d63d09c3") &&
        SameBytes(image, 0x2d696c0, "40534883ec208b1983fbff742d0fb7cbb8ff3f00006623c8e853cdffff488bc84885c0741533c0c1eb0e38991f010000480f44c14883c4205bc333c04883c4205bc3") &&
        SameBytes(image, 0x2d66430, "b8803e0000663bc8730f488b05df9637090fb7c9488b04c8c333c0c3");
}
bool FullReloadCodePins(const ReloadImage& image) {
    // Producer, same-level reuse decision and complete loaded-entry teardown.
    // These are chained unwind fragments of the retained native state machine.
    return FunctionStart(image, 0x34cbcc2, 0x34cbdd5, 0x96478ac) &&
        FunctionStart(image, 0x34cca50, 0x34cccf0, 0x9647b28) &&
        FunctionStart(image, 0x34cce1e, 0x34ccea2, 0x9647b5c) &&
        FunctionStart(image, 0x34ccea2, 0x34cd16f, 0x9647b6c) &&
        FunctionStart(image, 0x34cd270, 0x34ce690, 0x9647e18) &&
        SameBytes(image, 0x34cbc9f, "410fb6e8") &&
        SameBytes(image, 0x34cbdac, "4088ab69064300") &&
        SameBytes(image, 0x34ce307, "4588af68064300") &&
        SameBytes(image, 0x34ce317, "4538af690643007552") &&
        SameBytes(image, 0x34ce36a, "41c6876806430001") &&
        SameBytes(image, 0x34cccda, "4438af680643000f8437010000") &&
        SameBytes(image, 0x34cce4c, "4c8d05cda9ef048bd3488bcfe8b3aaffff") &&
        SameBytes(image, 0x34ccf79, "4438af680643007445") &&
        SameBytes(image, 0x34ccfc7, "488d8718000100488bcb4c89284c8968084c896810488d40404c8968d84c8968e04c8968e84c8968f04c8968f84883e90175d7") &&
        SameBytes(image, 0x34ce4d1, "498b57104885d2741c488d0d0f1fa406e8fa3349ff4d896f10") &&
        ImageString(image, image.At(0x83c7810), "Reload Level") &&
        ImageString(image, image.At(0x83c7820), "Unload Level");
}
bool LobbyCodePins(const ReloadImage& image) {
    // Original ExitLobby sets SaveSystem+0x70, then calls RequestUnload. The
    // retained Frontend wrapper additionally clears hero setup; do not use it.
    return FunctionStart(image,0x34cbf60,0x34cc041,0x9647894) &&
        FunctionStart(image,0x345e8e0,0x345e90e,0x96363c8) &&
        FunctionStart(image,0x345e90e,0x345e9ee,0x96363d0) &&
        FunctionStart(image,0x345e9ee,0x345e9f6,0x96363e8) &&
        CodeHash(image,0x34cbf60,225,0xbc276f32713c6eefull) &&
        CodeHash(image,0x345e8e0,278,0x258618dc58d87f9bull) &&
        SameBytes(image,0x34ca120,"4883ec28e8d732f9ff4885c0740e488b10488bc84883c42848ff62304883c428c3") &&
        SameBytes(image,0x34ca1d0,"0fb68165064300c3") &&
        SameBytes(image,0x3459510,"488b05193edc084885c07501c3837810090f94c0c3") &&
        SameBytes(image,0x2d694f0,"440fb6410c410fb6c04180c80124fe450fb6c084d20fb6c0440f44c04488410cc3");
}
bool NativeTypes(const ReloadImage& image) {
    std::uintptr_t prepare = 0, predicate = 0;
    return Rtti(image, 0x547d668, 0x8dfc0a0, 0xab46140, ".?AVGameLoadSystem@@") &&
        Rtti(image, 0x83c0ef0, 0x8fb7248, 0xabf7b58, ".?AVLoadStateObject@GameNet@@") &&
        Rtti(image, 0x54a1ca0, 0x8e18500, 0xab512a8, ".?AVSaveSystemGame@@") &&
        Rtti(image, 0x547d378, 0x8dfbeb0, 0xab46070, ".?AVExitToFrontendAction@@") &&
        ReadImage(image, image.At(0x547d668+0x98), prepare) && prepare == image.At(0x14cfa20) &&
        ReadImage(image, image.At(0x83c0ef0+0x30), predicate) && predicate == image.At(0x29b9e40);
}
bool HeaderIdentity(const IMAGE_NT_HEADERS64& nt, std::uintptr_t actualBase) {
    return nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt.FileHeader.TimeDateStamp == 0x679b5991 && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        nt.OptionalHeader.SizeOfImage == kImageSize &&
        (nt.OptionalHeader.ImageBase == 0x140000000 || nt.OptionalHeader.ImageBase == actualBase) &&
        nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION;
}
bool LoadImage(HMODULE module, ReloadImage& image, std::wstring& reason) {
    if (!module || module != GetModuleHandleW(nullptr)) { reason = L"Reload requires the main executable module"; return false; }
    image.base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!Read(image.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 ||
        !Read(image.base+dos.e_lfanew, nt) || !HeaderIdentity(nt, image.base)) {
        reason = L"Reload executable header validation failed"; return false;
    }
    image.size = nt.OptionalHeader.SizeOfImage;
    const auto& exceptions = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!exceptions.Size || exceptions.Size % sizeof(RUNTIME_FUNCTION) || exceptions.Size/sizeof(RUNTIME_FUNCTION) > 1000000 ||
        !image.At(exceptions.VirtualAddress, exceptions.Size)) {
        reason = L"Reload executable unwind directory validation failed"; return false;
    }
    image.exceptionTable = image.At(exceptions.VirtualAddress);
    image.exceptionCount = exceptions.Size/sizeof(RUNTIME_FUNCTION);
    if (!CodePins(image)) { reason = L"Reload native code or unwind signature validation failed"; return false; }
    if (!FullReloadCodePins(image)) { reason = L"Full Reload native teardown validation failed"; return false; }
    if (!NativeTypes(image)) { reason = L"Reload native type or vtable validation failed"; return false; }
    return true;
}
bool SameIdentity(const ReloadToken& a, const ReloadToken& b) noexcept {
    return std::tie(a.imageBase, a.manager, a.managerVtable, a.loadedLevel, a.mode, a.componentTable,
                    a.kernel, a.kernelOwner, a.checkpointRecords, a.saveSystem, a.saveSystemVtable,
                    a.levelAsset, a.kernelHandle, a.kernelGeneration, a.currentCheckpoint, a.managerState,
                    a.pendingInstances, a.pendingDeletions, a.modeValue, a.checkpointCount,
                    a.kernelSequence, a.managerReady, a.kernelState) ==
           std::tie(b.imageBase, b.manager, b.managerVtable, b.loadedLevel, b.mode, b.componentTable,
                    b.kernel, b.kernelOwner, b.checkpointRecords, b.saveSystem, b.saveSystemVtable,
                    b.levelAsset, b.kernelHandle, b.kernelGeneration, b.currentCheckpoint, b.managerState,
                    b.pendingInstances, b.pendingDeletions, b.modeValue, b.checkpointCount,
                    b.kernelSequence, b.managerReady, b.kernelState);
}
bool BaseSnapshot(const ReloadImage& image, ReloadToken& token, bool& frontendReady) {
    token.imageBase = image.base;
    std::int32_t componentCount = 0;
    std::uintptr_t kernelVtable = 0;
    if (!ReadImage(image, image.At(0xc2266a8), token.manager) || token.manager != image.At(0xb26b150) ||
        !Read(token.manager, token.managerVtable) || token.managerVtable != image.At(0x547d668) ||
        !Read(token.manager+0x10, token.loadedLevel) || token.loadedLevel < 0x10000 ||
        !Read(token.manager+0x430628, token.managerState) || token.managerState != 22 ||
        !Read(token.manager+0x199fc0, token.pendingInstances) || token.pendingInstances != 0 ||
        !Read(token.manager+0x319f68, token.pendingDeletions) || token.pendingDeletions != 0 ||
        !Read(token.manager+0x430665, token.managerReady) || token.managerReady != 1 ||
        !ReadImage(image, image.At(0xc21d330), token.mode) || token.mode < 0x10000 ||
        !Read(token.mode+0x10, token.modeValue) || token.modeValue != 9 ||
        !ReadImage(image, image.At(0xc21d630), token.kernelHandle) ||
        !ReadImage(image, image.At(0xc0bebe0), token.componentTable) || token.componentTable < 0x10000 ||
        !ReadImage(image, image.At(0xc0bec00), componentCount) || componentCount <= 0 || componentCount > 0x100000) return false;
    const auto index = token.kernelHandle & 0xfffff;
    token.kernelGeneration = token.kernelHandle >> 20;
    if (!token.kernelGeneration || index >= static_cast<std::uint32_t>(componentCount) ||
        token.componentTable > UINTPTR_MAX-static_cast<std::size_t>(index)*16-16) return false;
    const auto entry = token.componentTable+static_cast<std::size_t>(index)*16;
    std::uint32_t actualGeneration = 0;
    if (!Read(entry+8, actualGeneration) || actualGeneration != token.kernelGeneration ||
        !Read(entry, token.kernel) || token.kernel < 0x10000 || token.kernel > UINTPTR_MAX-0x6800 ||
        !Read(token.kernel, kernelVtable) || kernelVtable != image.At(0x83c0ef0) ||
        !Read(token.kernel+0x10, token.kernelOwner) || token.kernelOwner < 0x10000 ||
        !Read(token.kernel+0x58, token.levelAsset) || !token.levelAsset || static_cast<std::int32_t>(token.levelAsset >> 32) >= 0 ||
        !Read(token.kernel+0x62c0, token.currentCheckpoint) ||
        !Read(token.kernel+0x62f0, token.kernelSequence) ||
        !Read(token.kernel+0x6308, token.kernelState) || token.kernelState != 3 ||
        !Read(token.manager+0x41ff50, token.checkpointRecords) || token.checkpointRecords < 0x10000 ||
        !Read(token.manager+0x41ff58, token.checkpointCount) || token.checkpointCount <= 0 ||
        token.checkpointCount > static_cast<std::int32_t>(kMaximumCheckpoints)) return false;
    // These reads validate ownership memory without invoking Component::IsActive.
    std::array<unsigned char, 0x140> owner{};
    std::array<unsigned char, 8> loaded{};
    if (!ReadBytes(token.kernelOwner, owner.data(), owner.size()) || !ReadBytes(token.loadedLevel, loaded.data(), loaded.size())) return false;
    bool saveReady = ReadImage(image, image.At(0xc0bd2d8), token.saveSystem) && token.saveSystem >= 0x10000 &&
        token.saveSystem <= UINTPTR_MAX-0x71 && Read(token.saveSystem, token.saveSystemVtable) &&
        token.saveSystemVtable == image.At(0x54a1ca0);
    if (saveReady) {
        std::uint8_t saveFlag = 0;
        saveReady = Read(token.saveSystem+0x70, saveFlag) && saveFlag <= 1 && WritableStorage(token.saveSystem+0x70, 1);
    }
    frontendReady = saveReady && image.At(0xc1de866) && WritableStorage(image.At(0xc1de866), 1);
    if (!saveReady) { token.saveSystem = 0; token.saveSystemVtable = 0; }
    return true;
}
bool LobbyInputsValid(const ReloadImage& image, const ReloadToken& token) {
    if (!token.saveSystem || !WritableStorage(token.kernel,0x6800)) return false;
    // RequestStateUnloaded marks these embedded typed properties before its
    // ordinary state writes. Verify each virtual dirty-bit setter identity.
    for (const auto& property : std::array<std::pair<std::uintptr_t,std::uintptr_t>,5>{{
        {0x48,0x53bcd58},{0x6268,0x83c0ec0},{0x6298,0x52fa5e0},{0x62e0,0x530ef90},{0x62f8,0x52fa5e0}}}) {
        std::uintptr_t vtable = 0, setter = 0;
        if (!Read(token.kernel+property.first,vtable) || vtable != image.At(property.second) ||
            !ReadImage(image,vtable+0x20,setter) || setter != image.At(0x2d694f0)) return false;
    }
    // The manager's component handle can legally be null or stale. A matching
    // generation is used for an owner event, so bound that native lookup too.
    std::uint32_t handle = 0, generation = 0;
    std::int32_t count = 0;
    if (!Read(token.manager+0xc,handle)) return false;
    if (!(handle >> 20)) return true;
    const auto index = handle & 0xfffff;
    if (!ReadImage(image,image.At(0xc0bec00),count) || count <= 0 || count > 0x100000 ||
        index >= static_cast<std::uint32_t>(count) || token.componentTable > UINTPTR_MAX-index*16ull-16 ||
        !Read(token.componentTable+index*16ull+8,generation)) return false;
    if (generation != (handle >> 20)) return true;
    std::uintptr_t component = 0, owner = 0;
    std::array<unsigned char,16> ownerBytes{};
    if (!Read(token.componentTable+index*16ull,component)) return false;
    return !component || (component >= 0x10000 && component <= UINTPTR_MAX-0x18 &&
        Read(component+0x10,owner) && ReadBytes(owner,ownerBytes.data(),ownerBytes.size()));
}
// Guard every dynamic address traversed by the pinned, read-only component
// predicate before invoking it. The owner span is also checked by BaseSnapshot.
bool PredicateInputsReadable(const ReloadImage& image, const ReloadToken& token) {
    std::uint8_t replicationEnabled = 0;
    if (!ReadImage(image, image.At(0xc145040), replicationEnabled) || replicationEnabled > 1) return false;
    if (!replicationEnabled) return true;
    std::uint32_t handle = 0, ownerFlags = 0;
    if (!Read(token.kernelOwner+0xa8, handle) || !Read(token.kernelOwner+0x10, ownerFlags)) return false;
    (void)ownerFlags;
    const auto index = handle & 0x3fff;
    if (handle == 0xffffffff || index >= 0x3e80) return true;
    std::uintptr_t table = 0, replica = 0;
    if (!ReadImage(image, image.At(0xc0dfb20), table) || table < 0x10000 || table > UINTPTR_MAX-index*8-8 ||
        !Read(table+index*8, replica)) return false;
    if (!replica) return true;
    std::uint8_t generation = 0, activeFlags = 0;
    if (replica < 0x10000 || replica > UINTPTR_MAX-0x122 || !Read(replica+0x11f, generation)) return false;
    if (generation != static_cast<std::uint8_t>(handle >> 14)) return true;
    return Read(replica+0x121, activeFlags);
}
std::uint32_t NameCRC(const std::string& name) {
    if (name.empty()) return 0;
    std::uint32_t value = 0xedb88320;
    for (const unsigned char byte : name) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (value & 1 ? 0xedb88320 : 0);
    }
    return value;
}
bool ReadName(std::uintptr_t address, std::string& name) {
    name.clear();
    while (name.size() < kMaximumName) {
        if (address < 0x10000 || address > UINTPTR_MAX-64) return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (address < start || address-start >= memory.RegionSize) return false;
        const auto count = std::min<std::size_t>({64, kMaximumName-name.size(), memory.RegionSize-(address-start)});
        std::array<unsigned char, 64> bytes{};
        if (!count || !ReadBytes(address, bytes.data(), count)) return false;
        for (std::size_t index = 0; index < count; ++index) {
            if (!bytes[index]) return !name.empty();
            if (bytes[index] < 0x20 || bytes[index] > 0x7e) return false;
            name.push_back(static_cast<char>(bytes[index]));
        }
        address += count;
    }
    return false;
}
bool ReadCheckpointRecords(const ReloadToken& token, std::vector<CheckpointRecord>& raw) {
    const auto count = static_cast<std::size_t>(token.checkpointCount);
    if (!count || count > kMaximumCheckpoints || token.checkpointRecords > UINTPTR_MAX-count*72) return false;
    raw.resize(count);
    return ReadBytes(token.checkpointRecords, raw.data(), raw.size()*72);
}
bool ReadCheckpointNames(const std::vector<CheckpointRecord>& raw, std::vector<ReloadCheckpoint>& checkpoints) {
    std::set<std::uint32_t> keys;
    checkpoints.clear(); checkpoints.reserve(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index) {
        std::uintptr_t namePointer = 0;
        std::uint32_t key = 0;
        std::memcpy(&namePointer, raw[index].data()+0x20, sizeof(namePointer));
        std::memcpy(&key, raw[index].data()+0x28, sizeof(key));
        std::string name;
        if (!key || !keys.insert(key).second || !ReadName(namePointer, name) || NameCRC(name) != key) return false;
        checkpoints.push_back({key, static_cast<std::uint32_t>(index), std::wstring(name.begin(), name.end())});
    }
    return true;
}
ReloadState ReadLocked(ReloadRuntime& runtime, bool forceNames = false) {
    ReloadState result;
    result.supported = runtime.initialized && runtime.supported;
    result.status = runtime.reason;
    if (!result.supported) return result;
    bool frontendReady = false;
    ReloadToken before;
    if (!BaseSnapshot(runtime.image, before, frontendReady)) {
        if (runtime.haveIdentity) { runtime.haveIdentity = false; ++runtime.serial; }
        runtime.cachedCheckpoints.reset(); runtime.cachedRecords.clear();
        if (runtime.awaitingTransition) runtime.transitionSeen = true;
        result.status = L"Waiting for a stable loaded world";
        return result;
    }
    if (!runtime.haveIdentity || !SameIdentity(before, runtime.identity)) {
        if (runtime.awaitingTransition && runtime.haveIdentity) runtime.transitionSeen = true;
        ++runtime.serial; runtime.identity = before; runtime.haveIdentity = true;
    }
    if (runtime.awaitingTransition && !runtime.transitionSeen) {
        result.status = L"Native transition requested; waiting for the world to reload";
        return result;
    }
    if (runtime.awaitingTransition) {
        runtime.awaitingTransition = false; runtime.transitionSeen = false;
    }
    std::vector<CheckpointRecord> records;
    if (!ReadCheckpointRecords(before, records)) {
        runtime.cachedCheckpoints.reset(); runtime.cachedRecords.clear();
        result.status = L"Current-world checkpoint records could not be read";
        return result;
    }
    const auto now = GetTickCount64();
    auto checkpoints = runtime.cachedCheckpoints;
    if (forceNames || !checkpoints || !SameIdentity(before, runtime.cacheIdentity) ||
        records != runtime.cachedRecords || now-runtime.namesReadAt >= 1000) {
        auto fresh = std::make_shared<std::vector<ReloadCheckpoint>>();
        if (!ReadCheckpointNames(records, *fresh)) {
            runtime.cachedCheckpoints.reset(); runtime.cachedRecords.clear();
            result.status = L"Current-world checkpoint names or identifiers could not be validated";
            return result;
        }
        checkpoints = std::move(fresh);
    }
    if (!checkpoints) {
        result.status = L"Current-world checkpoint names or identifiers could not be validated";
        return result;
    }
    bool afterFrontendReady = false;
    ReloadToken after;
    if (!BaseSnapshot(runtime.image, after, afterFrontendReady) || !SameIdentity(before, after) ||
        frontendReady != afterFrontendReady) {
        runtime.haveIdentity = false; ++runtime.serial;
        result.status = L"World changed while reading its checkpoints";
        return result;
    }
    before.contextSerial = runtime.serial;
    if (checkpoints != runtime.cachedCheckpoints) runtime.namesReadAt = now;
    runtime.cacheIdentity = before; runtime.cachedRecords = std::move(records); runtime.cachedCheckpoints = checkpoints;
    result.token = before;
    result.ready = true; result.frontendReady = frontendReady;
    result.lobbyReady = runtime.unload && LobbyCodePins(runtime.image) && LobbyInputsValid(runtime.image,before);
    result.checkpoints = std::move(checkpoints);
    result.status = L"Current-world Reload is ready";
    return result;
}
} // namespace

bool SameReloadContext(const ReloadToken& left, const ReloadToken& right) noexcept {
    return left.contextSerial && left.contextSerial == right.contextSerial && SameIdentity(left, right);
}
void InitializeReloadRuntime(HMODULE gameModule, bool supported) {
    try {
        std::lock_guard lock(reloadRuntime.mutex);
        ++reloadRuntime.serial;
        reloadRuntime.initialized = true; reloadRuntime.supported = false;
        reloadRuntime.haveIdentity = false; reloadRuntime.awaitingTransition = false; reloadRuntime.transitionSeen = false;
        reloadRuntime.cachedCheckpoints.reset(); reloadRuntime.cachedRecords.clear(); reloadRuntime.namesReadAt = 0;
        reloadRuntime.image = {}; reloadRuntime.reload = nullptr; reloadRuntime.frontend = nullptr; reloadRuntime.unload = nullptr; reloadRuntime.kernelPredicate = nullptr;
        reloadRuntime.reason = L"Reload unavailable for this executable or native binding";
        ReloadImage image;
        if (!supported || !LoadImage(gameModule, image, reloadRuntime.reason)) return;
        reloadRuntime.image = image;
        reloadRuntime.reload = reinterpret_cast<ReloadFunction>(image.At(0x34cbc90));
        reloadRuntime.frontend = reinterpret_cast<FrontendFunction>(image.At(0x14cd7f0));
        if (LobbyCodePins(image)) reloadRuntime.unload = reinterpret_cast<UnloadFunction>(image.At(0x34cbf60));
        reloadRuntime.kernelPredicate = reinterpret_cast<KernelPredicate>(image.At(0x29b9e40));
        reloadRuntime.supported = true; reloadRuntime.reason = L"Waiting for a stable loaded world";
    } catch (...) {}
}
bool ReloadTransitionPending() {
    std::unique_lock lock(reloadRuntime.mutex, std::try_to_lock);
    return !lock.owns_lock() || reloadRuntime.awaitingTransition;
}
ReloadState ReadReloadState() {
    try { std::lock_guard lock(reloadRuntime.mutex); return ReadLocked(reloadRuntime); }
    catch (...) { ReloadState state; state.status = L"Reload snapshot failed"; return state; }
}
namespace {
bool SameCheckpointSelection(const ReloadState& captured, const ReloadState& fresh, std::uint32_t key) {
    if (!key) return true;
    const auto member = [key](const ReloadState& state) -> const ReloadCheckpoint* {
        if (state.checkpoints) for (const auto& checkpoint : *state.checkpoints)
            if (checkpoint.key == key) return &checkpoint;
        return nullptr;
    };
    const auto previous = member(captured), current = member(fresh);
    return previous && current && previous->recordIndex == current->recordIndex && previous->name == current->name;
}
// Called with reloadRuntime.mutex held. Only the native, read-only active-kernel
// predicate is invoked; this never submits a request or marks a transition.
bool AdmitCheckpointRequest(const ReloadState& captured, std::uint32_t checkpointKey, bool forceFullUnload,
                            ReloadState& current, std::wstring& status) {
    if (!IsWithinVerifiedEngineCallback()) { status = L"Reload requires the verified engine callback"; return false; }
    if (!reloadRuntime.supported || !reloadRuntime.reload || !reloadRuntime.kernelPredicate) { status = L"Reload runtime unavailable"; return false; }
    current = ReadLocked(reloadRuntime, true);
    if (!captured.ready || !current.ready || !SameReloadContext(captured.token, current.token)) {
        status = L"Reload rejected: the captured world or checkpoint changed"; return false;
    }
    if (!SameCheckpointSelection(captured, current, checkpointKey)) {
        status = L"Reload rejected: the selected checkpoint record changed"; return false;
    }
    if (!CodePins(reloadRuntime.image) || (forceFullUnload && !FullReloadCodePins(reloadRuntime.image)) ||
        !NativeTypes(reloadRuntime.image) || !IsWithinVerifiedEngineCallback()) {
        status = L"Reload rejected: native code or callback context changed"; return false;
    }
    if (!PredicateInputsReadable(reloadRuntime.image, current.token) ||
        !reloadRuntime.kernelPredicate(reinterpret_cast<void*>(current.token.kernel))) {
        status = L"Reload rejected: the current level kernel is not active"; return false;
    }
    if (!IsWithinVerifiedEngineCallback()) { status = L"Reload callback context was revoked"; return false; }
    const auto afterPredicate = ReadLocked(reloadRuntime, true);
    if (!afterPredicate.ready || !SameReloadContext(current.token, afterPredicate.token) ||
        !SameCheckpointSelection(captured, afterPredicate, checkpointKey)) {
        status = L"Reload rejected: the world or checkpoint changed during native preflight"; return false;
    }
    current = afterPredicate;
    return true;
}
bool ExecuteCheckpointRequest(const ReloadState& captured, std::uint32_t checkpointKey, bool forceFullUnload, std::wstring& status,
                              ReloadPreparation preparation, void* preparationContext) {
    try {
        if (!IsWithinVerifiedEngineCallback()) { status = L"Reload requires the verified engine callback"; return false; }
        std::unique_lock lock(reloadRuntime.mutex, std::try_to_lock);
        if (!lock) { status = L"Reload runtime is busy"; return false; }
        ReloadState current;
        if (!AdmitCheckpointRequest(captured, checkpointKey, forceFullUnload, current, status)) return false;
        if (preparation && !preparation(current, preparationContext, status)) return false;
        if (!IsWithinVerifiedEngineCallback()) { status = L"Reload callback context was revoked after preparation"; return false; }
        const bool accepted = reloadRuntime.reload(reinterpret_cast<void*>(current.token.manager), checkpointKey, forceFullUnload);
        if (accepted) { reloadRuntime.awaitingTransition = true; reloadRuntime.transitionSeen = false; }
        status = accepted ? (forceFullUnload ? L"Native Full Reload requested with forced world unload" : L"Native checkpoint Reload requested") :
            (forceFullUnload ? L"The game rejected the Full Reload request" : L"The game rejected the Reload request");
        return accepted;
    } catch (...) { status = L"Reload request failed"; return false; }
}
} // namespace
bool CheckReloadExecutionContext(const ReloadState& captured, std::wstring& status, std::uint32_t checkpointKey) {
    try {
        if (!IsWithinVerifiedEngineCallback()) { status = L"Reload admission requires the verified engine callback"; return false; }
        std::unique_lock lock(reloadRuntime.mutex, std::try_to_lock);
        if (!lock) { status = L"Reload runtime is busy"; return false; }
        ReloadState current;
        if (!AdmitCheckpointRequest(captured, checkpointKey, false, current, status)) return false;
        status = L"Native Reload execution context verified";
        return true;
    } catch (...) { status = L"Reload execution admission failed"; return false; }
}
bool ExecuteReload(const ReloadState& captured, std::uint32_t checkpointKey, std::wstring& status,
                   ReloadPreparation preparation, void* preparationContext) {
    return ExecuteCheckpointRequest(captured, checkpointKey, false, status, preparation, preparationContext);
}
bool ExecuteFullReload(const ReloadState& captured, std::uint32_t checkpointKey, std::wstring& status,
                       ReloadPreparation preparation, void* preparationContext) {
    return ExecuteCheckpointRequest(captured, checkpointKey, true, status, preparation, preparationContext);
}
bool ExecuteExitFrontend(const ReloadState& captured, std::wstring& status) {
    try {
        if (!IsWithinVerifiedEngineCallback()) { status = L"Exit to Frontend requires the verified engine callback"; return false; }
        std::unique_lock lock(reloadRuntime.mutex, std::try_to_lock);
        if (!lock || !reloadRuntime.supported || !reloadRuntime.frontend || !reloadRuntime.kernelPredicate) { status = L"Frontend runtime unavailable"; return false; }
        const auto current = ReadLocked(reloadRuntime, true);
        if (!captured.ready || !captured.frontendReady || !current.ready || !current.frontendReady ||
            !SameReloadContext(captured.token, current.token)) {
            status = L"Exit to Frontend rejected: the captured world or save system changed"; return false;
        }
        if (!CodePins(reloadRuntime.image) || !NativeTypes(reloadRuntime.image) || !IsWithinVerifiedEngineCallback()) {
            status = L"Exit to Frontend rejected: native code or callback context changed"; return false;
        }
        if (!PredicateInputsReadable(reloadRuntime.image, current.token) ||
            !reloadRuntime.kernelPredicate(reinterpret_cast<void*>(current.token.kernel))) {
            status = L"Exit to Frontend rejected: the current level kernel is not active"; return false;
        }
        if (!IsWithinVerifiedEngineCallback()) { status = L"Frontend callback context was revoked"; return false; }
        const auto afterPredicate = ReadLocked(reloadRuntime, true);
        if (!afterPredicate.ready || !afterPredicate.frontendReady || !SameReloadContext(current.token, afterPredicate.token) ||
            !CodePins(reloadRuntime.image) || !NativeTypes(reloadRuntime.image) || !IsWithinVerifiedEngineCallback()) {
            status = L"Exit to Frontend rejected: the world or frontend routing changed during native preflight"; return false;
        }
        // Exact retained action body does not read its receiver. It performs
        // native frontend/save routing and returns the tail request's bool.
        const bool accepted = reloadRuntime.frontend(nullptr);
        if (accepted) { reloadRuntime.awaitingTransition = true; reloadRuntime.transitionSeen = false; }
        status = accepted ? L"Native Exit to Frontend requested" : L"The game rejected Exit to Frontend";
        return accepted;
    } catch (...) { status = L"Exit to Frontend request failed"; return false; }
}
bool ExecuteExitLobby(const ReloadState& captured, std::wstring& status) {
    try {
        if (!IsWithinVerifiedEngineCallback()) { status = L"Exit to Lobby requires the verified engine callback"; return false; }
        std::unique_lock lock(reloadRuntime.mutex,std::try_to_lock);
        if (!lock || !reloadRuntime.unload) { status = L"Lobby unload runtime unavailable"; return false; }
        ReloadState current;
        if (!captured.lobbyReady || !AdmitCheckpointRequest(captured,0,false,current,status) || !current.lobbyReady ||
            !LobbyCodePins(reloadRuntime.image) || !LobbyInputsValid(reloadRuntime.image,current.token) ||
            !IsWithinVerifiedEngineCallback()) {
            status = L"Exit to Lobby rejected: the world, save system or native unload binding changed"; return false;
        }
        // Source callback writes this Boolean even if native unload declines.
        // Keep the native destination routing intact; only Frontend resets it.
        *reinterpret_cast<volatile std::uint8_t*>(current.token.saveSystem+0x70) = 1;
        const bool accepted = reloadRuntime.unload(reinterpret_cast<void*>(current.token.manager));
        if (accepted) { reloadRuntime.awaitingTransition = true; reloadRuntime.transitionSeen = false; }
        status = accepted ? L"Native Exit to Lobby unload requested; destination follows game routing" :
            L"The game rejected Exit to Lobby; its native exit-save flag remains set";
        return accepted;
    } catch (...) { status = L"Exit to Lobby request failed"; return false; }
}
} // namespace sm2debug
