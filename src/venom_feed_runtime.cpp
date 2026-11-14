// SPDX-License-Identifier: GPL-3.0-or-later
#include "venom_feed_runtime.h"
#include "engine_dispatch.h"
#include "photo_mode_gate.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace sm2debug::venom_feed {
namespace {
constexpr std::uintptr_t kImageSize = 0xce54000;
constexpr std::uint64_t kPrivateGraph = 0xbc8adc33003a7584ull;
constexpr float kCloseDistance = 3.0f; // Explicit conservative policy, in native metres.
constexpr std::uint32_t kMaximumPoolEntries = 1u << 20;
struct Runtime {
    std::mutex mutex;
    Bindings bindings;
    bool supported = false;
    std::array<std::uintptr_t, 5> infos{};
} runtime;
struct Position { float x = 0, y = 0, z = 0; };
struct ComponentEntry { std::uintptr_t info = 0, component = 0; };

bool ReadBytes(std::uintptr_t address, void* output, std::size_t size) {
    SIZE_T copied = 0;
    return address >= 0x10000 && size && size <= UINTPTR_MAX - address &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size, &copied) && copied == size;
}
template<class T> bool Read(std::uintptr_t address, T& value) { return ReadBytes(address, &value, sizeof(value)); }
template<class T> bool At(std::uintptr_t address, std::uintptr_t offset, T& value) {
    return address && offset <= UINTPTR_MAX - address && Read(address + offset, value);
}
bool Contains(const Bindings& b, std::uintptr_t address, std::size_t size) {
    const auto base = reinterpret_cast<std::uintptr_t>(b.gameModule);
    return base && address >= base && size <= kImageSize && address - base <= kImageSize - size;
}
bool Bytes(const Bindings& b, std::uintptr_t rva, const char* hex) {
    const auto base = reinterpret_cast<std::uintptr_t>(b.gameModule);
    const auto size = std::strlen(hex) / 2;
    std::array<unsigned char, 128> buffer{};
    if (!size || size > buffer.size() || !Contains(b, base + rva, size) || !ReadBytes(base + rva, buffer.data(), size)) return false;
    const auto digit = [](char c) -> unsigned { return c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < size; ++i)
        if (buffer[i] != ((digit(hex[i * 2]) << 4) | digit(hex[i * 2 + 1]))) return false;
    return true;
}
bool NativeString(std::uintptr_t address, const char* expected) {
    std::array<char, 96> buffer{};
    const auto size = std::strlen(expected) + 1;
    return size <= buffer.size() && ReadBytes(address, buffer.data(), size) && std::memcmp(buffer.data(), expected, size) == 0;
}
bool Rtti(const Bindings& b, std::uintptr_t table, std::uintptr_t col, std::uintptr_t type, const char* name) {
    const auto base = reinterpret_cast<std::uintptr_t>(b.gameModule);
    std::uintptr_t actual = 0;
    std::array<std::uint32_t, 6> fields{};
    return Read(base + table - 8, actual) && actual == base + col && Read(actual, fields) &&
        fields[0] == 1 && fields[1] == 0 && fields[3] == type && fields[5] == col && NativeString(base + type + 16, name);
}
bool Pins(const Bindings& b) {
    return Bytes(b, 0x28a8970, "448b01418bd0c1ea1481e2ff070000742d4181e0ffff0f004b8d0c4048c1e10648030db9628109443b05ce628109") &&
        Bytes(b, 0x29bd7b0, "8b118bcac1e91485c9742481e2ffff0f008bc248c1e004480305121470093b152c147009") &&
        Bytes(b, 0xfbaf96, "488b473c488983c0000000488b473c488983c8000000") &&
        Bytes(b, 0xfbc3b0, "428b040141ffc14189004d8d4004453b0a7ced") &&
        Bytes(b, 0x106b33b, "8b81980000003bc2") && Bytes(b, 0x106b477, "89b798000000") &&
        Bytes(b, 0xfc4326, "803d4758a00800") &&
        Bytes(b, 0x3471080, "c5fa10a1b8000000c5fa1081d0000000c5fa5cd4c5fa1081a0000000c5f057c9c5ea5fd9c5fa5cd4c5ea5f0d084ce801c5e25ec1c3") &&
        Rtti(b, 0x541f698, 0x8dbd058, 0xab2db70, ".?AVHeroWAMUser@WAM@@") &&
        Rtti(b, 0x541fd30, 0x8dbd508, 0xab2dd60, ".?AVWAMFinisherStateTracker@WAM@@") &&
        Rtti(b, 0x5421220, 0x8dbf6d0, 0xab2ea60, ".?AVWAMTarget@WAM@@");
}
bool Image(const Bindings& b) {
    const auto base = reinterpret_cast<std::uintptr_t>(b.gameModule);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    return b.executableVerified && base && Read(base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew > 0 && dos.e_lfanew < 4096 && At(base, static_cast<std::uintptr_t>(dos.e_lfanew), nt) &&
        nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt.OptionalHeader.SizeOfImage == kImageSize && Contains(b, b.heroSystem, 0x158) &&
        Contains(b, b.componentRegistry, 0x20) && Contains(b, b.componentInfos, 8) && Pins(b);
}
std::uint32_t NameCrc(const char* text) {
    std::uint32_t result = 0xedb88320;
    for (; *text; ++text) {
        result ^= static_cast<unsigned char>(*text);
        for (unsigned bit = 0; bit < 8; ++bit) result = (result >> 1) ^ ((result & 1) ? 0xedb88320u : 0u);
    }
    return result;
}
std::uintptr_t FindInfo(const Bindings& b, const char* name) {
    std::uintptr_t keys = 0, values = 0, infos = 0;
    std::uint32_t count = 0, stride = 0;
    if (!Read(b.componentRegistry, keys) || !At(b.componentRegistry, 8, values) ||
        !At(b.componentRegistry, 0x14, count) || !count || count > 65536 ||
        !At(b.componentRegistry, 0x1c, stride) || stride < 4 || stride > 256 || (stride & 3) || !Read(b.componentInfos, infos)) return 0;
    const auto crc = NameCrc(name);
    const auto product = static_cast<std::uint64_t>(crc) * 0x9e3779b1u;
    std::uint32_t bucket = (static_cast<std::uint32_t>(product) ^ static_cast<std::uint32_t>(product >> 32)) % count;
    for (std::uint32_t visited = 0; visited < count; ++visited) {
        std::uint32_t key = 0, index = 0;
        std::uintptr_t info = 0, text = 0;
        if (!Read(keys + bucket * 4ull, key) || !key) return 0;
        if (key == crc) return Read(values + bucket * static_cast<std::uintptr_t>(stride), index) && index < 65536 &&
            Read(infos + index * sizeof(std::uintptr_t), info) && At(info, 0x60, text) && NativeString(text, name) ? info : 0;
        bucket = (bucket + 1) % count;
    }
    return 0;
}
std::uintptr_t Actor(const Bindings& b, std::uint32_t handle) {
    const auto base = reinterpret_cast<std::uintptr_t>(b.gameModule);
    const auto index = handle & 0xfffffu;
    const auto generation = static_cast<std::uint16_t>((handle >> 20) & 0x7ffu);
    std::uintptr_t pool = 0, after = 0;
    std::uint32_t count = 0, afterCount = 0;
    std::uint16_t actual = 0;
    if (!generation || !Read(base + 0xc0bec50, pool) || !Read(base + 0xc0bec6c, count) ||
        !pool || count > kMaximumPoolEntries || index >= count || index > (UINTPTR_MAX - pool) / 192) return 0;
    const auto actor = pool + index * 192ull;
    return At(actor, 8, actual) && actual == generation && Read(base + 0xc0bec50, after) && after == pool &&
        Read(base + 0xc0bec6c, afterCount) && afterCount == count ? actor : 0;
}
std::uintptr_t ComponentHandle(const Bindings& b, std::uint32_t handle) {
    const auto base = reinterpret_cast<std::uintptr_t>(b.gameModule);
    const auto index = handle & 0xfffffu, generation = handle >> 20;
    std::uintptr_t pool = 0, component = 0, afterPool = 0;
    std::uint32_t count = 0, actual = 0, afterCount = 0;
    if (!generation || !Read(base + 0xc0bebe0, pool) || !Read(base + 0xc0bec00, count) ||
        !pool || count > kMaximumPoolEntries || index >= count || index > (UINTPTR_MAX - pool) / 16) return 0;
    const auto slot = pool + index * 16ull;
    return Read(slot, component) && At(slot, 8, actual) && actual == generation &&
        Read(base + 0xc0bebe0, afterPool) && afterPool == pool && Read(base + 0xc0bec00, afterCount) && afterCount == count ? component : 0;
}
std::uintptr_t Component(const Bindings& b, std::uintptr_t actor, std::uint32_t handle, std::uintptr_t info) {
    if (!info || Actor(b, handle) != actor) return 0;
    std::uint16_t large = 0, count = 0;
    std::uintptr_t entries = 0, result = 0, owner = 0;
    if (!At(actor, 0x88, large)) return 0;
    if (large) {
        if (!At(actor, 0x80, entries) || !entries || large > 32768 || (large & (large - 1))) return 0;
        auto bucket = (info >> 4) & (large - 1);
        for (unsigned seen = 0; seen < large; ++seen) {
            ComponentEntry entry{};
            if (!Read(entries + bucket * sizeof(entry), entry)) return 0;
            if (!entry.info) break;
            if (entry.info == info) { result = entry.component; break; }
            bucket = (bucket + 1) & (large - 1);
        }
        std::uintptr_t after = 0;
        std::uint16_t afterCount = 0;
        if (!At(actor, 0x80, after) || after != entries || !At(actor, 0x88, afterCount) || afterCount != large) return 0;
    } else {
        unsigned char flags = 0;
        if (!At(actor, 0x68, entries) || !entries || !At(actor, 0x70, count) || !count || count > 4096 || !At(info, 0xe4, flags)) return 0;
        for (unsigned i = 0; i < count; ++i) {
            ComponentEntry entry{};
            if (!Read(entries + i * sizeof(entry), entry)) return 0;
            bool matches = entry.info == info;
            if (!matches && (flags & 0x40) && entry.info) {
                unsigned char bases = 0;
                if (!At(entry.info, 0xe6, bases)) return 0;
                for (unsigned j = 0; j < bases; ++j) {
                    std::uintptr_t candidate = 0;
                    if (!At(entry.info, 0x80 + j * sizeof(candidate), candidate)) return 0;
                    if (candidate == info) { matches = true; break; }
                }
            }
            if (matches) { result = entry.component; break; }
        }
        std::uintptr_t after = 0;
        std::uint16_t afterCount = 0;
        if (!At(actor, 0x68, after) || after != entries || !At(actor, 0x70, afterCount) || afterCount != count) return 0;
    }
    std::uint32_t componentHandle = 0;
    return result && At(result, 0x10, owner) && owner == actor && At(result, 0x1c, componentHandle) &&
        ComponentHandle(b, componentHandle) == result && Actor(b, handle) == actor ? result : 0;
}
bool CurrentHero(const Bindings& b, std::uintptr_t& actor, std::uint32_t& handle) {
    std::uintptr_t entry = 0, after = 0;
    std::uint32_t afterHandle = 0;
    return At(b.heroSystem, 0x150, entry) && At(entry, 4, handle) && (actor = Actor(b, handle)) &&
        At(b.heroSystem, 0x150, after) && after == entry && At(entry, 4, afterHandle) && afterHandle == handle;
}
bool GameFocused(HWND window) {
    DWORD process = 0;
    wchar_t name[32]{};
    return window && GetForegroundWindow() == window && GetWindowThreadProcessId(window, &process) &&
        process == GetCurrentProcessId() && GetClassNameW(window, name, 32) && std::wcscmp(name, L"GameNxApp") == 0;
}
bool PositionOf(std::uintptr_t actor, Position& position) {
    std::uintptr_t transform = 0, after = 0;
    return Read(actor, transform) && At(transform, 0x30, position) && Read(actor, after) && transform == after &&
        std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
        std::fabs(position.x) < 10000000 && std::fabs(position.y) < 10000000 && std::fabs(position.z) < 10000000;
}
bool SameIdentity(const State& a, const State& b) {
    return SameReloadContext(a.world, b.world) && a.heroActor == b.heroActor && a.heroHandle == b.heroHandle &&
        a.heroWamUser == b.heroWamUser && a.ownerComponentHandle == b.ownerComponentHandle &&
        a.characterManager == b.characterManager && a.characterIndex == b.characterIndex &&
        a.configuredGraph == b.configuredGraph && a.activeGraph == b.activeGraph && a.targetActor == b.targetActor &&
        a.targetHandle == b.targetHandle && a.targetWam == b.targetWam && a.targetFinisherTracker == b.targetFinisherTracker;
}
State ReadLocked(Runtime& rt, const ReloadState& supplied, const Environment& environment) {
    State state;
    state.supported = rt.supported;
    const auto fail = [&state](const wchar_t* reason) { state.ready = false; state.status = reason; return state; };
    if (!rt.supported) return fail(L"Venom feeding native metadata unavailable");
    if (!IsWithinVerifiedEngineCallback()) return fail(L"Venom feeding observation requires the verified engine callback");
    if (environment.menuVisible || !environment.pauseKnown || environment.paused || !GameFocused(environment.gameWindow))
        return fail(L"Venom feeding requires focused unpaused gameplay with the menu closed");
    const auto photo = photo_gate::Observe();
    if (!photo.stateKnown || photo.photoActive) return fail(L"Photo mode state is active or unknown");
    const auto fresh = ReadReloadState();
    if (!supplied.ready || !fresh.ready || !SameReloadContext(supplied.token, fresh.token)) return fail(L"The loaded world is unavailable or changed");
    state.world = fresh.token;
    const auto& b = rt.bindings;
    const auto base = reinterpret_cast<std::uintptr_t>(b.gameModule);
    if (fresh.token.imageBase != base || !Pins(b) || !CurrentHero(b, state.heroActor, state.heroHandle)) return fail(L"The current hero identity changed");
    constexpr std::array<const char*, 5> infoNames{{"HeroWAMUser", "HeroCharacterManager", "WAMTarget", "WAMFinisherStateTracker", "Health"}};
    for (std::size_t i = 0; i < infoNames.size(); ++i)
        if (FindInfo(b, infoNames[i]) != rt.infos[i]) return fail(L"Native component registration changed");
    state.characterManager = Component(b, state.heroActor, state.heroHandle, rt.infos[1]);
    std::uintptr_t config = 0, records = 0, text = 0;
    std::int32_t count = 0;
    if (!At(state.characterManager, 0x90, config) || !At(state.characterManager, 0x98, state.characterIndex) ||
        state.characterIndex != 14 || !At(config, 0x30, records) || !At(config, 0x38, count) || count < 15 || count > 128 ||
        !Read(records + 14ull * 0x2a0 + 8, text) || !NativeString(text, "Venom")) return fail(L"The active character is not stock Venom");
    state.heroWamUser = Component(b, state.heroActor, state.heroHandle, rt.infos[0]);
    std::uintptr_t table = 0;
    unsigned char blocked = 0;
    std::int32_t selected = 0;
    if (!Read(state.heroWamUser, table) || table != base + 0x541f698 ||
        !At(state.heroWamUser, 0x1c, state.ownerComponentHandle) ||
        !At(state.heroWamUser, 0xc0, state.configuredGraph) || !At(state.heroWamUser, 0xc8, state.activeGraph) ||
        state.configuredGraph != kPrivateGraph || state.activeGraph != kPrivateGraph ||
        !At(state.heroWamUser, 0x16c, blocked) || (blocked & 8) || !At(state.heroWamUser, 0x148, selected) || selected != 1 ||
        !At(state.heroWamUser, 0x140, state.targetHandle) || state.targetHandle == state.heroHandle)
        return fail(L"The private bite graph has no single native selected target");
    state.nativeSelectionReady = true;
    state.targetActor = Actor(b, state.targetHandle);
    state.targetWam = Component(b, state.targetActor, state.targetHandle, rt.infos[2]);
    state.targetFinisherTracker = Component(b, state.targetActor, state.targetHandle, rt.infos[3]);
    const auto health = Component(b, state.targetActor, state.targetHandle, rt.infos[4]);
    std::uint32_t wamHandle = 0, healthHandle = 0, flagsA = 0, flagsB = 0;
    unsigned char trackerFlags = 0;
    if (!Read(state.targetWam, table) || table != base + 0x5421220 ||
        !Read(state.targetFinisherTracker, table) || table != base + 0x541fd30 ||
        !At(state.targetFinisherTracker, 0x4c, wamHandle) || ComponentHandle(b, wamHandle) != state.targetWam ||
        !At(state.targetFinisherTracker, 0x50, healthHandle) || ComponentHandle(b, healthHandle) != health ||
        !At(state.targetFinisherTracker, 0x74, trackerFlags) || !(trackerFlags & 1) ||
        !At(state.targetWam, 0x108, flagsA) || !At(state.targetWam, 0x10c, flagsB) ||
        ((flagsA | flagsB) & (1u << 17)) || !((flagsA | flagsB) & (1u << 18)) ||
        !At(health, 0xa0, state.targetMaximumHealth) || !At(health, 0xd0, state.targetHealth) ||
        !std::isfinite(state.targetMaximumHealth) || !std::isfinite(state.targetHealth) ||
        state.targetMaximumHealth <= 0 || state.targetMaximumHealth > 10000000 ||
        state.targetHealth <= 0 || state.targetHealth > state.targetMaximumHealth)
        return fail(L"The selected actor has no live compatible finisher/health components");
    // Mirror only FC4320's proven scalar reads. FC4580 is deliberately NOT
    // called: its FC41B0 dependency acquires a target list lock. Native input
    // processing retains that full CanDo and synchronization check.
    unsigned char thresholdPolicy = 0;
    float absolute = 0, fraction = 0, minimum = 0;
    if (!Read(base + 0x99c9b74, thresholdPolicy) || thresholdPolicy > 1 ||
        !At(state.targetFinisherTracker, 0x68, absolute) || !At(state.targetFinisherTracker, 0x6c, fraction) ||
        !At(health, 0xb8, minimum) || !std::isfinite(absolute) || !std::isfinite(fraction) ||
        !std::isfinite(minimum) || minimum < 0 || minimum > state.targetMaximumHealth) return fail(L"Native finisher threshold inputs are invalid");
    bool overThreshold = false;
    if (thresholdPolicy) {
        if (absolute >= 0) overThreshold = state.targetHealth > absolute;
        else if (fraction >= 0 && fraction < 1) {
            const float percent = std::max(state.targetHealth - minimum, 0.0f) /
                std::max(state.targetMaximumHealth - minimum, 1.0f);
            overThreshold = percent > fraction;
        }
    }
    state.canKillKnown = true;
    state.canKill = !overThreshold || ((flagsA | flagsB) & (1u << 20));
    if (!state.canKill) return fail(L"The native finisher threshold does not permit a lethal bite");
    Position heroPosition{}, targetPosition{};
    if (!PositionOf(state.heroActor, heroPosition) || !PositionOf(state.targetActor, targetPosition)) return fail(L"Actor positions are unavailable");
    const double dx = static_cast<double>(heroPosition.x) - targetPosition.x;
    const double dy = static_cast<double>(heroPosition.y) - targetPosition.y;
    const double dz = static_cast<double>(heroPosition.z) - targetPosition.z;
    state.distance = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
    if (!std::isfinite(state.distance) || state.distance > kCloseDistance) return fail(L"The native selected target is beyond the 3 metre feeding limit");
    std::uintptr_t afterHero = 0;
    std::uint32_t afterHandle = 0, afterTarget = 0;
    std::int32_t afterSelected = 0, afterCharacter = -1;
    std::uint64_t afterGraph = 0, afterActiveGraph = 0;
    const auto afterWorld = ReadReloadState();
    const auto afterPhoto = photo_gate::Observe();
    if (!IsWithinVerifiedEngineCallback() || !CurrentHero(b, afterHero, afterHandle) || afterHero != state.heroActor || afterHandle != state.heroHandle ||
        Actor(b, state.targetHandle) != state.targetActor ||
        Component(b, state.heroActor, state.heroHandle, rt.infos[0]) != state.heroWamUser ||
        Component(b, state.heroActor, state.heroHandle, rt.infos[1]) != state.characterManager ||
        Component(b, state.targetActor, state.targetHandle, rt.infos[2]) != state.targetWam ||
        Component(b, state.targetActor, state.targetHandle, rt.infos[3]) != state.targetFinisherTracker ||
        Component(b, state.targetActor, state.targetHandle, rt.infos[4]) != health ||
        !At(state.characterManager, 0x98, afterCharacter) || afterCharacter != state.characterIndex ||
        !At(state.heroWamUser, 0x148, afterSelected) || afterSelected != 1 || !At(state.heroWamUser, 0x140, afterTarget) || afterTarget != state.targetHandle ||
        !At(state.heroWamUser, 0xc0, afterGraph) || afterGraph != kPrivateGraph || !At(state.heroWamUser, 0xc8, afterActiveGraph) || afterActiveGraph != kPrivateGraph ||
        !afterWorld.ready || !SameReloadContext(state.world, afterWorld.token) || !afterPhoto.stateKnown || afterPhoto.photoActive || !GameFocused(environment.gameWindow))
        return fail(L"Feeding admission changed during observation");
    state.observedAtMilliseconds = GetTickCount64();
    state.ready = true;
    state.status = L"Native finisher input may be requested for the selected nearby target";
    return state;
}
} // namespace

void Initialize(const Bindings& bindings) {
    try {
        std::lock_guard lock(runtime.mutex);
        runtime.supported = false;
        runtime.bindings = bindings;
        runtime.infos = {};
        if (!Image(bindings)) return;
        constexpr std::array<const char*, 5> names{{"HeroWAMUser", "HeroCharacterManager", "WAMTarget", "WAMFinisherStateTracker", "Health"}};
        for (std::size_t i = 0; i < names.size(); ++i) {
            runtime.infos[i] = FindInfo(bindings, names[i]);
            if (!runtime.infos[i]) return;
        }
        runtime.supported = true;
    } catch (...) {}
}
State ReadReadiness(const ReloadState& world, const Environment& environment) {
    try {
        std::unique_lock lock(runtime.mutex, std::try_to_lock);
        if (!lock) { State result; result.status = L"Feeding observation is busy"; return result; }
        return ReadLocked(runtime, world, environment);
    } catch (...) { State result; result.status = L"Feeding observation failed"; return result; }
}
bool Revalidate(const State& captured, const Environment& environment, std::wstring& status) {
    if (!IsWithinVerifiedEngineCallback()) { status = L"Feeding revalidation requires the verified engine callback"; return false; }
    const auto now = GetTickCount64();
    if (!captured.ready || !captured.observedAtMilliseconds || now < captured.observedAtMilliseconds || now - captured.observedAtMilliseconds > 250) {
        status = L"The captured feeding target expired"; return false;
    }
    const auto world = ReadReloadState();
    if (!world.ready || !SameReloadContext(captured.world, world.token)) { status = L"The feeding world changed"; return false; }
    const auto fresh = ReadReadiness(world, environment);
    if (!fresh.ready || !SameIdentity(captured, fresh)) { status = fresh.status; return false; }
    status = fresh.status;
    return true;
}
}
