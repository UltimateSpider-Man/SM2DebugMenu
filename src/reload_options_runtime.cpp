// SPDX-License-Identifier: GPL-3.0-or-later
#include "reload_options_runtime.h"
#include "engine_dispatch.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace sm2debug {
namespace {
constexpr std::size_t kOptionsImageSize = 0xce54000, kOptionsMapLimit = 8192;
constexpr std::uint32_t kCampaignKey = 0x120a3cce; // Registered system name: SaveFile.
constexpr std::uint64_t kNewGamePlusMask = 0x40000;
constexpr std::uint64_t kDefaultLoadoutMask = 0x40;
constexpr std::uint64_t kOptionsFnv = 14695981039346656037ull;
struct OptionsImage {
    std::uintptr_t base = 0;
    bool Contains(std::uintptr_t p, std::size_t n = 1) const {
        return base && p >= base && n <= kOptionsImageSize && p - base <= kOptionsImageSize - n;
    }
    std::uintptr_t At(std::uintptr_t rva) const { return base && rva < kOptionsImageSize ? base + rva : 0; }
};
struct OptionsRuntime {
    std::mutex mutex;
    OptionsImage image;
    bool supported = false;
    std::array<std::uint32_t, kOptionsMapLimit> keys{}, checkKeys{};
} optionsRuntime;
bool OptionsRead(std::uintptr_t p, void* target, std::size_t n) {
    SIZE_T copied = 0;
    return p >= 0x10000 && n && n <= UINTPTR_MAX - p &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p), target, n, &copied) && copied == n;
}
template<class T> bool OptionsRead(std::uintptr_t p, T& target) { return OptionsRead(p, &target, sizeof(target)); }
bool OptionsWritable(std::uintptr_t p, std::size_t n) {
    if (p < 0x10000 || !n || n > UINTPTR_MAX - p) return false;
    const auto end = p + n;
    while (p < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            ((m.Protect & 0xff) != PAGE_READWRITE && (m.Protect & 0xff) != PAGE_WRITECOPY)) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
        if (start > p || m.RegionSize > UINTPTR_MAX - start || start + m.RegionSize <= p) return false;
        p = std::min(end, start + m.RegionSize);
    }
    return true;
}
void OptionsHash(std::uint64_t& hash, const void* source, std::size_t n) {
    const auto* bytes = static_cast<const unsigned char*>(source);
    for (std::size_t i = 0; i < n; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
}
bool OptionsString(const OptionsImage& image, std::uintptr_t p, const char* expected) {
    std::array<char, 96> text{};
    const auto n = std::strlen(expected) + 1;
    return n <= text.size() && image.Contains(p, n) && OptionsRead(p, text.data(), n) &&
        std::memcmp(text.data(), expected, n) == 0;
}
bool OptionsRtti(const OptionsImage& image, std::uintptr_t vt, std::uintptr_t col, std::uintptr_t td, const char* name) {
    std::uintptr_t pointer = 0;
    std::array<std::uint32_t, 6> record{};
    return OptionsRead(image.At(vt - 8), pointer) && pointer == image.At(col) && OptionsRead(pointer, record) &&
        record[0] == 1 && record[1] == 0 && record[3] == td && record[5] == col &&
        OptionsString(image, image.At(td + 16), name);
}
bool OptionsTypes(const OptionsImage& image) {
    if (!OptionsRtti(image, 0x54a1ba0, 0x8e18420, 0xab51260, ".?AVGameSaveFile@@") ||
        !OptionsRtti(image, 0x54a1ca0, 0x8e18500, 0xab512a8, ".?AVSaveSystemGame@@") ||
        !OptionsRtti(image, 0x834fcd8, 0x8f95fe8, 0xabea878, ".?AVSystemSaveDataContainer@@") ||
        !OptionsRtti(image, 0x5c39300, 0x8f494e8, 0xabca5e8, ".?AVGameCampaignSave@@")) return false;
    const auto d = image.At(0x9ea95c0);
    std::uintptr_t name = 0, names = 0, types = 0, counts = 0, offsets = 0, slot = 0;
    std::uint32_t type = 0, size = 0, count = 0, offset = 0;
    std::uint16_t fields = 0;
    std::uint8_t code = 0;
    return OptionsRead(image.At(0x54a1ba0 + 0x18), slot) && slot == image.At(0x16127f0) &&
        OptionsRead(d, name) && name == image.At(0x5c380e0) && OptionsString(image, name, "GameCampaignSave") &&
        OptionsRead(d + 8, type) && type == 0x5da4b89f && OptionsRead(d + 0x14, size) && size == 0x2e0 &&
        OptionsRead(d + 0x18, fields) && fields == 23 &&
        OptionsRead(d + 0x60, names) && names == image.At(0x5c38b10) &&
        OptionsRead(d + 0x68, types) && types == image.At(0x5c38d30) &&
        OptionsRead(d + 0x80, counts) && counts == image.At(0x5c38d80) &&
        OptionsRead(d + 0x88, offsets) && offsets == image.At(0x5c38de0) &&
        OptionsRead(names + 6 * 8, name) && name == image.At(0x5c38c58) && OptionsString(image, name, "DefaultLoadoutApplied") &&
        OptionsRead(types + 6, code) && code == 15 && OptionsRead(counts + 6 * 4, count) && count == 1 &&
        OptionsRead(offsets + 6 * 4, offset) && offset == 0xd4 &&
        OptionsRead(names + 18 * 8, name) && name == image.At(0x549ec50) && OptionsString(image, name, "NewGamePlus") &&
        OptionsRead(types + 18, code) && code == 15 && OptionsRead(counts + 18 * 4, count) && count == 1 &&
        OptionsRead(offsets + 18 * 4, offset) && offset == 0x182;
}
bool OptionsCode(const OptionsImage& image) {
    struct Pin { std::uintptr_t rva; std::size_t size; std::uint64_t hash; };
    // Exact retail resolver/getter and the generated native setter branch.
    // The original developer setter uses the same byte and two dirty masks.
    static constexpr Pin pins[] = {
        {0x1613060, 0x17, 0x03b9f6145023b6f1ull}, {0x2638d98, 0x28, 0xaa13039bee0be117ull},
        {0x2d74720, 0x59, 0x403a7f1428f9411bull}, {0x2cde3b0, 0xce, 0x97e45ec19337ffebull},
        {0x30942f0, 0x79, 0x4466ef20070fe189ull}, {0x3097090, 0x56, 0xd23befb488a0b84dull},
        {0x161281f, 0x68, 0xaf765d59d7100528ull}, {0x35d9870, 8, 0x4918b889d631574aull},
        // Current-campaign loadout reset, skip-if-already-applied consumer,
        // and the successful-application write-back use this exact field.
        {0x34bd04c, 0x20, 0xa0ff03668aeeb4e4ull}, {0x34bd230, 0x23, 0xf2c19b3f0a4c453full},
        {0x34bd46a, 0x25, 0xe560993d4eb755ddull}
    };
    std::array<unsigned char, 256> bytes{};
    for (const auto& pin : pins) {
        std::uint64_t hash = kOptionsFnv;
        if (pin.size > bytes.size() || !OptionsRead(image.At(pin.rva), bytes.data(), pin.size)) return false;
        OptionsHash(hash, bytes.data(), pin.size);
        if (hash != pin.hash) return false;
    }
    return true;
}
struct OptionsMap {
    std::uintptr_t keys = 0, values = 0;
    std::int32_t count = 0, capacity = 0;
    std::uint32_t unused = 0, stride = 0;
};
static_assert(sizeof(OptionsMap) == 32);
bool SameOptionsToken(const ReloadOptionsToken& a, const ReloadOptionsToken& b) {
    return SameReloadContext(a.world, b.world) && a.block == b.block && a.container == b.container &&
        a.entry == b.entry && a.campaign == b.campaign && a.keys == b.keys && a.values == b.values &&
        a.mapFingerprint == b.mapFingerprint;
}
bool OptionsDisjoint(std::uintptr_t p, std::size_t n, std::uintptr_t q, std::size_t m) {
    return p >= 0x10000 && q >= 0x10000 && n <= UINTPTR_MAX - p && m <= UINTPTR_MAX - q &&
        (p + n <= q || q + m <= p);
}
template<class T> bool OptionsMatches(std::uintptr_t p, const T& expected) {
    T value{};
    return OptionsRead(p, value) && value == expected;
}
// The supplied token has already passed native admission under the reload
// mutex. Recheck its complete memory identity without taking that mutex again.
// contextSerial is compared separately against the captured token.
bool OptionsAdmittedWorldMatches(const ReloadToken& t) {
    const auto& image = optionsRuntime.image;
    if (t.imageBase != image.base || !t.contextSerial || t.manager != image.At(0xb26b150) ||
        t.managerVtable != image.At(0x547d668) || t.managerState != 22 || t.managerReady != 1 ||
        t.pendingInstances || t.pendingDeletions || t.modeValue != 9 || t.kernelState != 3 ||
        t.loadedLevel < 0x10000 || t.mode < 0x10000 || t.mode > UINTPTR_MAX - 0x14 ||
        t.kernel < 0x10000 || t.kernel > UINTPTR_MAX - 0x6800 || t.kernelOwner < 0x10000 ||
        !t.levelAsset || static_cast<std::int32_t>(t.levelAsset >> 32) >= 0 ||
        t.checkpointRecords < 0x10000 || t.checkpointCount <= 0 || t.checkpointCount > 4096 ||
        t.saveSystem < 0x10000 || t.saveSystemVtable != image.At(0x54a1ca0) ||
        !OptionsMatches(image.At(0xc2266a8), t.manager) || !OptionsMatches(t.manager, t.managerVtable) ||
        !OptionsMatches(t.manager + 0x10, t.loadedLevel) || !OptionsMatches(t.manager + 0x430628, t.managerState) ||
        !OptionsMatches(t.manager + 0x199fc0, t.pendingInstances) || !OptionsMatches(t.manager + 0x319f68, t.pendingDeletions) ||
        !OptionsMatches(t.manager + 0x430665, t.managerReady) || !OptionsMatches(image.At(0xc21d330), t.mode) ||
        !OptionsMatches(t.mode + 0x10, t.modeValue) || !OptionsMatches(image.At(0xc21d630), t.kernelHandle) ||
        !OptionsMatches(image.At(0xc0bebe0), t.componentTable)) return false;
    const auto index = t.kernelHandle & 0xfffff;
    std::int32_t count = 0;
    if (!t.kernelGeneration || t.kernelGeneration != t.kernelHandle >> 20 ||
        !OptionsRead(image.At(0xc0bec00), count) || count <= 0 || count > 0x100000 ||
        index >= static_cast<std::uint32_t>(count) || t.componentTable < 0x10000 ||
        t.componentTable > UINTPTR_MAX - static_cast<std::size_t>(index) * 16 - 16) return false;
    const auto entry = t.componentTable + static_cast<std::size_t>(index) * 16;
    if (!OptionsMatches(entry, t.kernel) || !OptionsMatches(entry + 8, t.kernelGeneration) ||
        !OptionsMatches(t.kernel, image.At(0x83c0ef0)) || !OptionsMatches(t.kernel + 0x10, t.kernelOwner) ||
        !OptionsMatches(t.kernel + 0x58, t.levelAsset) || !OptionsMatches(t.kernel + 0x62c0, t.currentCheckpoint) ||
        !OptionsMatches(t.kernel + 0x62f0, t.kernelSequence) || !OptionsMatches(t.kernel + 0x6308, t.kernelState) ||
        !OptionsMatches(t.manager + 0x41ff50, t.checkpointRecords) ||
        !OptionsMatches(t.manager + 0x41ff58, t.checkpointCount) ||
        !OptionsMatches(image.At(0xc0bd2d8), t.saveSystem) || !OptionsMatches(t.saveSystem, t.saveSystemVtable)) return false;
    std::array<unsigned char, 0x140> owner{};
    std::array<unsigned char, 8> loaded{};
    return OptionsRead(t.kernelOwner, owner.data(), owner.size()) && OptionsRead(t.loadedLevel, loaded.data(), loaded.size());
}
ReloadOptionsState OptionsReadSuppliedLocked(const ReloadState& requested, const ReloadState& current) {
    ReloadOptionsState result;
    result.supported = optionsRuntime.supported;
    result.status = L"NG+ flag requires a validated current campaign save";
    if (!result.supported || !requested.supported || !requested.ready) return result;
    const auto& image = optionsRuntime.image;
    if (!current.supported || !current.ready || !SameReloadContext(requested.token, current.token) ||
        current.token.imageBase != image.base || !current.token.contextSerial || !current.token.saveSystem ||
        current.token.saveSystemVtable != image.At(0x54a1ca0)) return result;
    auto& token = result.token;
    token.world = current.token;
    token.block = image.At(0xb6c2978);
    std::uintptr_t pointer = 0;
    std::int32_t initialized = 0, kind = -1, scope = -1;
    std::uint32_t key = 0;
    const auto registration = image.At(0xb6c28e8);
    if (!OptionsRead(image.At(0xc0bd2d8), pointer) || pointer != token.world.saveSystem ||
        !OptionsRead(pointer, pointer) || pointer != image.At(0x54a1ca0) ||
        !OptionsRead(image.At(0xc23c818), pointer) || pointer != token.block ||
        !OptionsRead(token.block, pointer) || pointer != image.At(0x54a1ba0) ||
        !OptionsRead(registration, pointer) || pointer != image.At(0x9ea95c0) ||
        !OptionsRead(registration + 8, kind) || kind != 0 ||
        !OptionsRead(registration + 16, pointer) || pointer != image.At(0x54a1c18) ||
        !OptionsString(image, pointer, "SaveFile") ||
        !OptionsRead(registration + 24, key) || key != kCampaignKey ||
        !OptionsRead(registration + 28, scope) || scope != 1 ||
        !OptionsRead(token.world.saveSystem + 0x98, initialized) || initialized <= 0 ||
        !OptionsRead(token.world.saveSystem + 0x30, token.container) || image.Contains(token.container) ||
        !OptionsRead(token.container, pointer) || pointer != image.At(0x834fcd8)) return result;
    OptionsMap map{}, after{};
    if (!OptionsRead(token.container + 0x28, map) || map.count <= 0 || map.capacity <= map.count ||
        map.capacity > static_cast<std::int32_t>(kOptionsMapLimit) || map.stride != 80 ||
        !OptionsWritable(map.keys, static_cast<std::size_t>(map.capacity) * 4) ||
        !OptionsWritable(map.values, static_cast<std::size_t>(map.capacity) * 80) ||
        !OptionsRead(map.keys, optionsRuntime.keys.data(), static_cast<std::size_t>(map.capacity) * 4)) return result;
    const auto end = optionsRuntime.keys.begin() + map.capacity;
    if (std::count(optionsRuntime.keys.begin(), end, kCampaignKey) != 1 ||
        std::count_if(optionsRuntime.keys.begin(), end, [](auto k) { return k != 0; }) != map.count) return result;
    const std::uint64_t product = std::uint64_t(kCampaignKey) * 0x9e3779b1u;
    std::size_t index = static_cast<std::uint32_t>((product >> 32) ^ product) % static_cast<std::uint32_t>(map.capacity);
    std::size_t steps = 0;
    while (optionsRuntime.keys[index] && optionsRuntime.keys[index] != kCampaignKey && ++steps < static_cast<std::size_t>(map.capacity))
        index = (index + 1) % static_cast<std::size_t>(map.capacity);
    if (optionsRuntime.keys[index] != kCampaignKey) return result;
    token.keys = map.keys; token.values = map.values; token.entry = map.values + index * 80;
    if (!OptionsDisjoint(map.keys, static_cast<std::size_t>(map.capacity) * 4,
                         map.values, static_cast<std::size_t>(map.capacity) * 80) ||
        !OptionsRead(token.entry + 0x38, token.campaign) || image.Contains(token.campaign) ||
        (token.campaign & 7) || !OptionsWritable(token.campaign, 0x2e0) ||
        !OptionsRead(token.campaign, pointer) || pointer != image.At(0x5c39300) ||
        !OptionsDisjoint(token.campaign, 0x2e0, token.world.saveSystem, 0x3300) ||
        !OptionsDisjoint(token.campaign, 0x2e0, token.container, 0x50) ||
        !OptionsDisjoint(token.campaign, 0x2e0, map.keys, static_cast<std::size_t>(map.capacity) * 4) ||
        !OptionsDisjoint(token.campaign, 0x2e0, map.values, static_cast<std::size_t>(map.capacity) * 80)) return result;
    std::uint8_t value = 0, loadout = 0;
    if (!OptionsRead(token.campaign + 0x182, value) || value > 1 ||
        !OptionsRead(token.campaign + 0xd4, loadout) || loadout > 1 ||
        !OptionsRead(token.container + 0x28, after) || std::memcmp(&map, &after, sizeof(map)) ||
        !OptionsRead(map.keys, optionsRuntime.checkKeys.data(), static_cast<std::size_t>(map.capacity) * 4) ||
        !std::equal(optionsRuntime.keys.begin(), end, optionsRuntime.checkKeys.begin()) ||
        !OptionsRead(token.entry + 0x38, pointer) || pointer != token.campaign) return result;
    token.mapFingerprint = kOptionsFnv;
    OptionsHash(token.mapFingerprint, &map, sizeof(map));
    OptionsHash(token.mapFingerprint, optionsRuntime.keys.data(), static_cast<std::size_t>(map.capacity) * 4);
    result.newGamePlus = value != 0; result.defaultLoadoutApplied = loadout != 0; result.ready = true;
    result.status = L"Current save NG+ flag ready; changing it does not start a new playthrough";
    return result;
}
ReloadOptionsState OptionsReadLocked(const ReloadState& requested) {
    if (!optionsRuntime.supported || !requested.supported || !requested.ready)
        return OptionsReadSuppliedLocked(requested, {});
    return OptionsReadSuppliedLocked(requested, ReadReloadState());
}
}
void InitializeReloadOptionsRuntime(HMODULE module, bool supported) {
    std::lock_guard lock(optionsRuntime.mutex);
    optionsRuntime.supported = false; optionsRuntime.image = {};
    if (!supported || !module || module != GetModuleHandleW(nullptr)) return;
    const OptionsImage image{reinterpret_cast<std::uintptr_t>(module)};
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!OptionsRead(image.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 ||
        !OptionsRead(image.base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.TimeDateStamp != 0x679b5991 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage != kOptionsImageSize ||
        (nt.OptionalHeader.ImageBase != 0x140000000 && nt.OptionalHeader.ImageBase != image.base) ||
        !OptionsCode(image) || !OptionsTypes(image)) return;
    optionsRuntime.image = image; optionsRuntime.supported = true;
}
ReloadOptionsState ReadReloadOptionsState(const ReloadState& world) {
    std::lock_guard lock(optionsRuntime.mutex);
    return OptionsReadLocked(world);
}
bool ExecuteSetNewGamePlus(const ReloadOptionsState& captured, bool enabled, std::wstring& status) {
    status = L"NG+ flag change refused: callback, world or campaign save changed";
    if (!IsWithinVerifiedEngineCallback()) return false;
    std::unique_lock lock(optionsRuntime.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !optionsRuntime.supported || !captured.supported || !captured.ready ||
        !OptionsCode(optionsRuntime.image) || !OptionsTypes(optionsRuntime.image)) return false;
    const auto fresh = OptionsReadLocked(ReloadState{true, true, false, captured.token.world, nullptr, L""});
    if (!fresh.ready || !SameOptionsToken(captured.token, fresh.token) || fresh.newGamePlus != captured.newGamePlus) return false;
    const auto finalWorld = ReadReloadState();
    if (!finalWorld.supported || !finalWorld.ready || !SameReloadContext(fresh.token.world, finalWorld.token)) return false;
    const auto finalState = OptionsReadLocked(finalWorld);
    if (!finalState.ready || !SameOptionsToken(fresh.token, finalState.token) ||
        finalState.newGamePlus != captured.newGamePlus || !IsWithinVerifiedEngineCallback()) return false;
    // Only this user-requested action writes. Atomic OR preserves dirty flags
    // added by another subsystem; the byte CAS rejects a changed selection.
    auto previous = static_cast<unsigned char>(captured.newGamePlus ? 1 : 0);
    if (!__atomic_compare_exchange_n(reinterpret_cast<volatile unsigned char*>(fresh.token.campaign + 0x182),
        &previous, static_cast<unsigned char>(enabled ? 1 : 0), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return false;
    InterlockedOr64(reinterpret_cast<volatile LONG64*>(fresh.token.campaign + 8), static_cast<LONG64>(kNewGamePlusMask));
    InterlockedOr64(reinterpret_cast<volatile LONG64*>(fresh.token.campaign + 0x18), static_cast<LONG64>(kNewGamePlusMask));
    status = enabled ? L"Current save NG+ flag enabled; no new playthrough started" :
                       L"Current save NG+ flag disabled; no new playthrough started";
    return true;
}
bool PrepareReloadLoadoutAfterAdmission(const ReloadOptionsState& captured,
                                       const ReloadState& admitted, std::wstring& status) {
    status = L"Loadout reset refused: callback, admitted world or campaign save changed";
    if (!IsWithinVerifiedEngineCallback()) return false;
    std::unique_lock lock(optionsRuntime.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !optionsRuntime.supported || !captured.supported || !captured.ready ||
        !admitted.supported || !admitted.ready || !SameReloadContext(captured.token.world, admitted.token) ||
        !OptionsCode(optionsRuntime.image) || !OptionsTypes(optionsRuntime.image) ||
        !OptionsAdmittedWorldMatches(admitted.token)) return false;
    const ReloadState requested{true, true, false, captured.token.world, nullptr, L""};
    const auto fresh = OptionsReadSuppliedLocked(requested, admitted);
    if (!fresh.ready || !SameOptionsToken(captured.token, fresh.token) ||
        fresh.defaultLoadoutApplied != captured.defaultLoadoutApplied) return false;
    const auto finalState = OptionsReadSuppliedLocked(requested, admitted);
    if (!finalState.ready || !SameOptionsToken(fresh.token, finalState.token) ||
        finalState.defaultLoadoutApplied != captured.defaultLoadoutApplied ||
        !OptionsAdmittedWorldMatches(admitted.token) || !IsWithinVerifiedEngineCallback()) return false;
    auto previous = static_cast<unsigned char>(captured.defaultLoadoutApplied ? 1 : 0);
    if (!__atomic_compare_exchange_n(reinterpret_cast<volatile unsigned char*>(fresh.token.campaign + 0xd4),
        &previous, static_cast<unsigned char>(0), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return false;
    InterlockedOr64(reinterpret_cast<volatile LONG64*>(fresh.token.campaign + 8), static_cast<LONG64>(kDefaultLoadoutMask));
    InterlockedOr64(reinterpret_cast<volatile LONG64*>(fresh.token.campaign + 0x18), static_cast<LONG64>(kDefaultLoadoutMask));
    status = L"Default loadout reset prepared; this reset remains if the native reload request is refused";
    return true;
}
}
