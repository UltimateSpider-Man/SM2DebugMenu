// SPDX-License-Identifier: GPL-3.0-or-later
// Isolated synthetic host. The retail PE supplies pinned bytes/RTTI only.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../src/reload_runtime.h"
#include "../src/photo_mode_gate.h"
namespace {
bool callback = true, focused = true, knownPhoto = true, activePhoto = false, changeWorld = false;
DWORD fixtureThread = 0;
unsigned worldReads = 0;
HWND fixtureWindow = reinterpret_cast<HWND>(0x1234);
sm2debug::ReloadState fixtureWorld;
HWND FixtureForeground() { return focused ? fixtureWindow : nullptr; }
DWORD FixtureWindowProcess(HWND window, LPDWORD pid) { if (pid) *pid = GetCurrentProcessId(); return window == fixtureWindow ? 1 : 0; }
int FixtureClass(HWND window, LPWSTR buffer, int length) {
    if (window != fixtureWindow || length < 10) return 0;
    std::wcscpy(buffer, L"GameNxApp"); return 9;
}
}
namespace sm2debug {
bool IsWithinVerifiedEngineCallback() noexcept { return callback && GetCurrentThreadId() == fixtureThread; }
ReloadState ReadReloadState() {
    auto result = fixtureWorld;
    if (changeWorld && ++worldReads >= 2) ++result.token.contextSerial;
    return result;
}
bool SameReloadContext(const ReloadToken& a, const ReloadToken& b) noexcept {
    return a.imageBase == b.imageBase && a.manager == b.manager && a.loadedLevel == b.loadedLevel &&
        a.contextSerial == b.contextSerial && a.currentCheckpoint == b.currentCheckpoint && a.kernelHandle == b.kernelHandle;
}
namespace photo_gate {
Diagnostics Observe() { Diagnostics result; result.stateKnown = knownPhoto; result.photoActive = activePhoto; return result; }
}
}
#define GetForegroundWindow FixtureForeground
#define GetWindowThreadProcessId FixtureWindowProcess
#define GetClassNameW FixtureClass
#include "../src/venom_feed_runtime.cpp"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef GetClassNameW

namespace {
using sm2debug::venom_feed::Bindings;
using sm2debug::venom_feed::Environment;
using sm2debug::venom_feed::State;
unsigned checks = 0, cases = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class T> void Put(std::uintptr_t address, const T& value) { std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value)); }
void Text(std::uintptr_t address, const char* value) { std::memcpy(reinterpret_cast<void*>(address), value, std::strlen(value) + 1); }
struct Pe {
    std::vector<unsigned char> data;
    IMAGE_NT_HEADERS64 nt{};
    std::vector<IMAGE_SECTION_HEADER> sections;
    explicit Pe(const wchar_t* path) {
        std::ifstream input(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(input), {});
        Check(data.size() > 0x1000, "Read original PE");
        IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, data.data(), sizeof(dos));
        std::memcpy(&nt, data.data() + dos.e_lfanew, sizeof(nt));
        const auto start = static_cast<std::size_t>(dos.e_lfanew) + 24 + nt.FileHeader.SizeOfOptionalHeader;
        sections.resize(nt.FileHeader.NumberOfSections);
        std::memcpy(sections.data(), data.data() + start, sections.size() * sizeof(sections[0]));
        Check(nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 && nt.OptionalHeader.SizeOfImage == 0xce54000, "Original PE shape");
    }
    const unsigned char* At(std::uint32_t rva, std::size_t bytes) const {
        for (const auto& section : sections) if (rva >= section.VirtualAddress &&
            rva - section.VirtualAddress <= section.SizeOfRawData && bytes <= section.SizeOfRawData - (rva - section.VirtualAddress))
            return data.data() + section.PointerToRawData + rva - section.VirtualAddress;
        throw std::runtime_error("Fixture PE RVA is not file-backed");
    }
};
struct Fixture {
    std::uintptr_t image = 0;
    std::array<unsigned char, 384> actors{};
    std::array<unsigned char, 256> heroTransform{}, targetTransform{};
    std::array<unsigned char, 512> heroUser{}, character{}, targetWam{}, tracker{}, health{}, config{};
    std::array<unsigned char, 34 * 0x2a0> records{};
    std::array<unsigned char, 16> current{};
    std::array<unsigned char, 5 * 16> componentSlots{};
    std::array<sm2debug::venom_feed::ComponentEntry, 2> heroEntries{};
    std::array<sm2debug::venom_feed::ComponentEntry, 3> targetEntries{};
    Bindings bindings;
    Environment environment;
    const std::uint32_t heroHandle = 0x100000, targetHandle = 0x100001;
    std::uintptr_t hero() { return reinterpret_cast<std::uintptr_t>(actors.data()); }
    std::uintptr_t target() { return hero() + 192; }
    template<std::size_t N> static std::uintptr_t Address(std::array<unsigned char, N>& a) { return reinterpret_cast<std::uintptr_t>(a.data()); }
    explicit Fixture(const Pe& pe) {
        image = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr, 0xce54000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(image != 0, "Allocate fixture-only image");
        std::memcpy(reinterpret_cast<void*>(image), pe.data.data(), 4096);
        for (const auto address : {0x28a8970u, 0x29bd7b0u, 0xfbaf96u, 0xfbc3b0u, 0x106b33bu, 0x106b477u, 0xfc4326u, 0x3471080u})
            std::memcpy(reinterpret_cast<void*>(image + address), pe.At(address, 128), 128);
        for (const auto row : {std::array<std::uint32_t, 3>{0x541f698, 0x8dbd058, 0xab2db70},
                {0x541fd30, 0x8dbd508, 0xab2dd60}, {0x5421220, 0x8dbf6d0, 0xab2ea60}}) {
            Put(image + row[0] - 8, image + row[1]);
            std::memcpy(reinterpret_cast<void*>(image + row[1]), pe.At(row[1], 24), 24);
            std::memcpy(reinterpret_cast<void*>(image + row[2]), pe.At(row[2], 96), 96);
        }
        bindings = {reinterpret_cast<HMODULE>(image), true, image + 0xb000000, image + 0xb001000, image + 0xb001100};
        constexpr std::array<const char*, 5> names{{"HeroWAMUser", "HeroCharacterManager", "WAMTarget", "WAMFinisherStateTracker", "Health"}};
        const auto keys = image + 0xb002000, values = image + 0xb002100, infos = image + 0xb002200;
        Put(bindings.componentRegistry, keys); Put(bindings.componentRegistry + 8, values);
        Put<std::uint32_t>(bindings.componentRegistry + 0x14, 16); Put<std::uint32_t>(bindings.componentRegistry + 0x1c, 4);
        Put(bindings.componentInfos, infos);
        std::array<std::uintptr_t, 5> pointers{};
        for (std::size_t i = 0; i < names.size(); ++i) {
            pointers[i] = image + 0xb010000 + i * 0x1000;
            const auto text = image + 0xb020000 + i * 128;
            Text(text, names[i]); Put(pointers[i] + 0x60, text); Put(infos + i * 8, pointers[i]);
            const auto crc = sm2debug::venom_feed::NameCrc(names[i]);
            const auto product = static_cast<std::uint64_t>(crc) * 0x9e3779b1u;
            auto bucket = (static_cast<std::uint32_t>(product) ^ static_cast<std::uint32_t>(product >> 32)) % 16;
            std::uint32_t old = 0;
            while (std::memcpy(&old, reinterpret_cast<void*>(keys + bucket * 4), 4), old) bucket = (bucket + 1) % 16;
            Put(keys + bucket * 4, crc); Put(values + bucket * 4, static_cast<std::uint32_t>(i));
        }
        const std::array<std::uintptr_t, 5> components{{Address(heroUser), Address(character), Address(targetWam), Address(tracker), Address(health)}};
        for (std::size_t i = 0; i < components.size(); ++i) {
            Put(components[i] + 0x10, i < 2 ? hero() : target());
            Put(components[i] + 0x1c, static_cast<std::uint32_t>(0x100000 + i));
            Put(Address(componentSlots) + i * 16, components[i]); Put<std::uint32_t>(Address(componentSlots) + i * 16 + 8, 1);
        }
        Put(image + 0xc0bebe0, Address(componentSlots)); Put<std::uint32_t>(image + 0xc0bec00, 5);
        Put(image + 0xc0bec50, hero()); Put<std::uint32_t>(image + 0xc0bec6c, 2);
        Put<std::uint16_t>(hero() + 8, 1); Put<std::uint16_t>(target() + 8, 1);
        Put(hero(), Address(heroTransform)); Put(target(), Address(targetTransform));
        Put<sm2debug::venom_feed::Position>(Address(heroTransform) + 0x30, {0, 0, 0});
        Put<sm2debug::venom_feed::Position>(Address(targetTransform) + 0x30, {2, 0, 0});
        heroEntries = {{{pointers[0], Address(heroUser)}, {pointers[1], Address(character)}}};
        targetEntries = {{{pointers[2], Address(targetWam)}, {pointers[3], Address(tracker)}, {pointers[4], Address(health)}}};
        Put(hero() + 0x68, reinterpret_cast<std::uintptr_t>(heroEntries.data())); Put<std::uint16_t>(hero() + 0x70, 2);
        Put(target() + 0x68, reinterpret_cast<std::uintptr_t>(targetEntries.data())); Put<std::uint16_t>(target() + 0x70, 3);
        Put(bindings.heroSystem + 0x150, Address(current)); Put(Address(current) + 4, heroHandle);
        Put(Address(character) + 0x90, Address(config)); Put<std::int32_t>(Address(character) + 0x98, 14);
        Put(Address(config) + 0x30, Address(records)); Put<std::int32_t>(Address(config) + 0x38, 34);
        Put(Address(records) + 14 * 0x2a0 + 8, reinterpret_cast<std::uintptr_t>("Venom"));
        Put(Address(records) + 33 * 0x2a0 + 8, reinterpret_cast<std::uintptr_t>("Carnage"));
        Put(Address(heroUser), image + 0x541f698);
        Put<std::uint64_t>(Address(heroUser) + 0xc0, 0xbc8adc33003a7584ull);
        Put<std::uint64_t>(Address(heroUser) + 0xc8, 0xbc8adc33003a7584ull);
        Put(Address(heroUser) + 0x140, targetHandle); Put<std::int32_t>(Address(heroUser) + 0x148, 1);
        Put(Address(targetWam), image + 0x5421220); Put<std::uint32_t>(Address(targetWam) + 0x108, 1u << 18);
        Put(Address(tracker), image + 0x541fd30); Put<std::uint32_t>(Address(tracker) + 0x4c, 0x100002);
        Put<std::uint32_t>(Address(tracker) + 0x50, 0x100004); Put<unsigned char>(Address(tracker) + 0x74, 1);
        Put<float>(Address(tracker) + 0x68, -1); Put<float>(Address(tracker) + 0x6c, -1);
        Put<float>(Address(health) + 0xa0, 100); Put<float>(Address(health) + 0xd0, 50);
        Put<unsigned char>(image + 0x99c9b74, 1);
        fixtureWorld = {}; fixtureWorld.supported = fixtureWorld.ready = true;
        fixtureWorld.token.imageBase = image; fixtureWorld.token.manager = image + 0xb040000;
        fixtureWorld.token.loadedLevel = image + 0xb050000; fixtureWorld.token.contextSerial = 7;
        callback = focused = knownPhoto = true; activePhoto = changeWorld = false; worldReads = 0;
        environment = {fixtureWindow, false, true, false};
        sm2debug::venom_feed::Initialize(bindings);
        Check(sm2debug::venom_feed::Pins(bindings), "Actual original PE pin bytes in isolated image");
        Check(sm2debug::venom_feed::runtime.supported, "Production initialization accepts exact synthetic metadata");
    }
    ~Fixture() { if (image) VirtualFree(reinterpret_cast<void*>(image), 0, MEM_RELEASE); }
    State Read() { return sm2debug::venom_feed::ReadReadiness(fixtureWorld, environment); }
    template<class T, class F> void Changed(std::uintptr_t address, T replacement, F test) {
        T old{}; std::memcpy(&old, reinterpret_cast<void*>(address), sizeof(old)); Put(address, replacement); test(); Put(address, old);
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "Expected original retail PE path"); fixtureThread = GetCurrentThreadId();
        const Pe pe(argv[1]); Fixture f(pe); ++cases;
        const auto originalActors = f.actors;
        const auto originalHero = f.heroUser, originalTracker = f.tracker, originalHealth = f.health;
        auto state = f.Read(); Check(state.ready && state.nativeSelectionReady && state.canKillKnown && state.canKill, "Valid callback snapshot ready");
        Check(!state.canDoPredicateEvaluated && state.heroHandle == f.heroHandle && state.targetHandle == f.targetHandle && state.distance == 2, "No native predicates and exact target identity");
        Check(f.actors == originalActors && f.heroUser == originalHero && f.tracker == originalTracker && f.health == originalHealth, "Read-only observation preserves native sentinel buffers");
        std::wstring status; Check(sm2debug::venom_feed::Revalidate(state, f.environment, status), "Fresh exact snapshot revalidates");
        callback = false; Check(!f.Read().ready, "Refuse worker/no verified callback"); callback = true; ++cases;
        f.environment.menuVisible = true; Check(!f.Read().ready, "Refuse visible menu"); f.environment.menuVisible = false; ++cases;
        f.environment.pauseKnown = false; Check(!f.Read().ready, "Refuse unknown pause"); f.environment.pauseKnown = true;
        f.environment.paused = true; Check(!f.Read().ready, "Refuse paused game"); f.environment.paused = false; ++cases;
        knownPhoto = false; Check(!f.Read().ready, "Refuse unknown photo state"); knownPhoto = true;
        activePhoto = true; Check(!f.Read().ready, "Refuse active photo mode"); activePhoto = false; ++cases;
        focused = false; Check(!f.Read().ready, "Refuse game focus loss"); focused = true; ++cases;
        fixtureWorld.ready = false; Check(!f.Read().ready, "Refuse world transition"); fixtureWorld.ready = true;
        changeWorld = true; worldReads = 0; Check(!f.Read().ready, "Refuse world change during snapshot"); changeWorld = false; ++cases;
        f.Changed<std::int32_t>(Fixture::Address(f.character) + 0x98, 33, [&] { Check(!f.Read().ready, "Refuse Carnage sharing native kVenom family"); }); ++cases;
        f.Changed<std::uintptr_t>(Fixture::Address(f.records) + 14 * 0x2a0 + 8, reinterpret_cast<std::uintptr_t>("Carnage"), [&] { Check(!f.Read().ready, "Refuse changed index14 name"); }); ++cases;
        for (const auto offset : {0xc0u, 0xc8u}) {
            f.Changed<std::uint64_t>(Fixture::Address(f.heroUser) + offset, 0x87a772b0dcf6f0abull, [&] { Check(!f.Read().ready, "Refuse stock or temporarily overridden WAM graph"); });
        }
        ++cases;
        f.Changed<std::uint16_t>(f.target() + 8, 2, [&] { Check(!f.Read().ready, "Refuse stale target actor generation"); });
        f.Changed<std::uint16_t>(f.hero() + 8, 2, [&] { Check(!f.Read().ready, "Refuse stale hero actor generation"); }); ++cases;
        f.Changed<std::uint32_t>(Fixture::Address(f.componentSlots) + 3 * 16 + 8, 2, [&] { Check(!f.Read().ready, "Refuse recycled target component generation"); }); ++cases;
        f.Changed<std::uintptr_t>(Fixture::Address(f.tracker) + 0x10, f.hero(), [&] { Check(!f.Read().ready, "Refuse foreign tracker owner"); }); ++cases;
        f.Changed<std::uintptr_t>(Fixture::Address(f.targetWam), f.image + 0x541fd30, [&] { Check(!f.Read().ready, "Refuse foreign WAM vtable"); }); ++cases;
        for (const auto count : {0, 2, -1, 100}) {
            f.Changed<std::int32_t>(Fixture::Address(f.heroUser) + 0x148, count, [&] { Check(!f.Read().ready, "Require exactly one native selected target"); });
        }
        ++cases;
        f.Changed<unsigned char>(Fixture::Address(f.heroUser) + 0x16c, 8, [&] { Check(!f.Read().ready, "Refuse native blocked finisher flag"); }); ++cases;
        f.Changed<std::uint32_t>(Fixture::Address(f.heroUser) + 0x140, f.heroHandle, [&] { Check(!f.Read().ready, "Refuse self-target"); }); ++cases;
        f.Changed<std::uint32_t>(Fixture::Address(f.tracker) + 0x50, 0x100002, [&] { Check(!f.Read().ready, "Refuse tracker Health reference mismatch"); }); ++cases;
        f.Changed<unsigned char>(Fixture::Address(f.tracker) + 0x74, 0, [&] { Check(!f.Read().ready, "Refuse target not finisher enabled"); });
        f.Changed<std::uint32_t>(Fixture::Address(f.targetWam) + 0x108, (1u << 17) | (1u << 18), [&] { Check(!f.Read().ready, "Refuse native immune target"); }); ++cases;
        f.Changed<float>(Fixture::Address(f.health) + 0xd0, 0, [&] { Check(!f.Read().ready, "Refuse dead victim"); });
        f.Changed<float>(Fixture::Address(f.health) + 0xd0, 101, [&] { Check(!f.Read().ready, "Refuse inconsistent health"); }); ++cases;
        f.Changed<float>(Fixture::Address(f.tracker) + 0x68, 25, [&] { const auto s = f.Read(); Check(!s.ready && s.canKillKnown && !s.canKill, "Absolute native kill threshold refuses"); }); ++cases;
        f.Changed<float>(Fixture::Address(f.tracker) + 0x6c, .4f, [&] {
            Check(!f.Read().ready, "Fractional native kill threshold refuses");
            f.Changed<float>(Fixture::Address(f.health) + 0xb8, 20, [&] { Check(f.Read().ready, "Native percent uses Health+B8 floor"); });
            f.Changed<std::uint32_t>(Fixture::Address(f.targetWam) + 0x108, (1u << 18) | (1u << 20), [&] { Check(f.Read().ready, "Native force-lethal state overrides scalar threshold"); });
            f.Changed<unsigned char>(f.image + 0x99c9b74, 0, [&] { Check(f.Read().ready, "Native threshold policy disabled"); });
        }); ++cases;
        f.Changed<sm2debug::venom_feed::Position>(Fixture::Address(f.targetTransform) + 0x30, {3.001f, 0, 0}, [&] { Check(!f.Read().ready, "Refuse target beyond explicit close range"); });
        f.Changed<sm2debug::venom_feed::Position>(Fixture::Address(f.targetTransform) + 0x30, {3, 0, 0}, [&] { Check(f.Read().ready, "Accept exact3m boundary"); }); ++cases;
        f.Changed<unsigned char>(f.image + 0x28a8970, 0, [&] { Check(!f.Read().ready, "Refuse changed native code pin"); }); ++cases;
        f.Changed<char>(f.image + 0xb020000, 'X', [&] { Check(!f.Read().ready, "Refuse changed cached component registration name"); }); ++cases;
        {
            std::array<sm2debug::venom_feed::ComponentEntry, 8> large{};
            for (const auto entry : f.targetEntries) {
                auto bucket = (entry.info >> 4) & 7;
                while (large[bucket].info) bucket = (bucket + 1) & 7;
                large[bucket] = entry;
            }
            f.Changed<std::uintptr_t>(f.target() + 0x80, reinterpret_cast<std::uintptr_t>(large.data()), [&] {
                f.Changed<std::uint16_t>(f.target() + 0x88, 8, [&] { Check(f.Read().ready, "Native large actor component table admitted"); });
                f.Changed<std::uint16_t>(f.target() + 0x88, 3, [&] { Check(!f.Read().ready, "Refuse non-power-of-two native component table"); });
            });
        }
        ++cases;
        auto stale = state; stale.observedAtMilliseconds = GetTickCount64() > 500 ? GetTickCount64() - 500 : 0;
        Check(!sm2debug::venom_feed::Revalidate(stale, f.environment, status), "Refuse expired immutable snapshot");
        f.Changed<std::uint64_t>(Fixture::Address(f.heroUser) + 0xc8, 1, [&] { Check(!sm2debug::venom_feed::Revalidate(state, f.environment, status), "Revalidation rejects graph change"); }); ++cases;
        Check(f.Read().ready, "All fixture corruption restored and valid path remains ready");
        auto unverified = f.bindings; unverified.executableVerified = false;
        sm2debug::venom_feed::Initialize(unverified); Check(!f.Read().supported && !f.Read().ready, "Refuse absent upstream exact executable verdict");
        sm2debug::venom_feed::Initialize(f.bindings); Check(f.Read().ready, "Verified reinitialization restores observation"); ++cases;
        std::cout << "{\"passed\":true,\"cases\":" << cases << ",\"assertions\":" << checks
                  << ",\"native_calls\":false,\"game_process_access\":false,\"writes\":\"fixture-owned memory only\"}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
