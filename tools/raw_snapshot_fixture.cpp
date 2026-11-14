// Synthetic, current-process-only memory fixtures. Never loads the game.
// Compile with MinGW-w64 C++17 and link user32, psapi, bcrypt.
#include "../src/backend.cpp"
// Existing snapshot/action cases model an open menu; performance cases toggle it.
namespace sm2debug { bool fixtureMenuVisible = true; bool MenuVisible() { return fixtureMenuVisible; } }
#include <iostream>
#include <stdexcept>

namespace {
using namespace sm2debug;
int assertions = 0, explicitSetterCalls = 0;
void Check(bool value, const char* reason) {
    ++assertions;
    if (!value) throw std::runtime_error(reason);
}
template<size_t N> struct alignas(64) Bytes {
    std::array<unsigned char, N> data{};
    uintptr_t address() { return reinterpret_cast<uintptr_t>(data.data()); }
    template<class T> void set(size_t offset, T value) {
        Check(offset <= N && sizeof(value) <= N - offset, "fixture write outside buffer");
        std::memcpy(data.data() + offset, &value, sizeof(value));
    }
};
void HealthTrap(void* component, float value) {
    ++explicitSetterCalls;
    std::memcpy(static_cast<unsigned char*>(component) + 0xd0, &value, sizeof(value));
}
void SwitchTrap(void*, uint32_t, int, void*) { ++explicitSetterCalls; }
void PositionTrap(void*, const Vec3*) { ++explicitSetterCalls; }

struct Fixture {
    Bytes<0x200> hero;
    Bytes<16> entry;
    Bytes<0x180> pool;
    Bytes<0x100> transform;
    Bytes<0x40> registry;
    Bytes<0x100> heroInfo, healthInfo, characterInfo;
    Bytes<0x250> health;
    Bytes<0x180> manager;
    Bytes<0x60> config;
    Bytes<0x540> characters;
    std::array<uint32_t, 17> keys{}, indices{};
    std::array<uintptr_t, 3> infos{};
    std::array<ComponentEntry, 2> small{};
    std::array<ComponentEntry, 8> large{};
    std::array<uintptr_t, 15> healthVtable{};
    std::array<unsigned char, 4> healthFlags{};
    uintptr_t poolPointer = 0, infosPointer = 0;
    uint32_t poolCount = 2;
    const uint32_t handle = (7u << 20) | 1u;
    uintptr_t actor = 0;

    Fixture() {
        g = {};
        g.base = reinterpret_cast<uintptr_t>(&HealthTrap) - 0xee5580;
        g.size = 0x10000000;
        g.heroSystem = hero.address();
        poolPointer = pool.address();
        actor = poolPointer + 0xc0;
        g.actorPool = reinterpret_cast<uintptr_t>(&poolPointer);
        g.actorCount = reinterpret_cast<uintptr_t>(&poolCount);
        infos = {heroInfo.address(), healthInfo.address(), characterInfo.address()};
        infosPointer = reinterpret_cast<uintptr_t>(infos.data());
        g.componentInfos = reinterpret_cast<uintptr_t>(&infosPointer);
        g.registry = registry.address();
        g.healthFlags = reinterpret_cast<uintptr_t>(healthFlags.data());
        g.switchCharacter = &SwitchTrap;
        g.setPosition = &PositionTrap;
        hero.set(0, g.base + 0x100);
        hero.set(0x150, entry.address());
        entry.set(4, handle);
        pool.set(0xc0, transform.address());
        pool.set(0xc8, uint16_t(7));
        transform.set(0x30, Vec3{12.5f, -42.0f, 300.0f});
        registry.set(0, reinterpret_cast<uintptr_t>(keys.data()));
        registry.set(8, reinterpret_cast<uintptr_t>(indices.data()));
        registry.set(0x14, uint32_t(17));
        registry.set(0x1c, uint32_t(4));
        // CRC constants and bucket positions independently calculated with
        // Python zlib using the documented seed, rather than calling NameCRC.
        keys[7] = 0x07479b47; indices[7] = 0;
        keys[14] = 0xf0a26d74; indices[14] = 1;
        keys[12] = 0x22216f8c; indices[12] = 2;
        heroInfo.set(0x60, reinterpret_cast<uintptr_t>("HeroHealth"));
        healthInfo.set(0x60, reinterpret_cast<uintptr_t>("Health"));
        characterInfo.set(0x60, reinterpret_cast<uintptr_t>("HeroCharacterManager"));
        healthInfo.set(0xe4, static_cast<unsigned char>(0x40));
        heroInfo.set(0xe6, static_cast<unsigned char>(1));
        heroInfo.set(0x80, healthInfo.address());
        small = {{{heroInfo.address(), health.address()}, {characterInfo.address(), manager.address()}}};
        SetSmall();
        pool.set(0xc0 + 0x78, uintptr_t(0xfeedbeef12345678ull));
        pool.set(0xc0 + 0x76, uint16_t(0xa55a));
        healthVtable[13] = g.base + 0x43e020;
        healthVtable[14] = reinterpret_cast<uintptr_t>(&HealthTrap);
        health.set(0, reinterpret_cast<uintptr_t>(healthVtable.data()));
        health.set(0x10, actor);
        health.set(0xa0, 100.0f);
        health.set(0xd0, 75.0f);
        manager.set(0x10, actor);
        manager.set(0x90, config.address());
        config.set(0x30, characters.address());
        config.set(0x38, int(2));
        characters.set(8, reinterpret_cast<uintptr_t>("Peter"));
        characters.set(0x2a0 + 8, reinterpret_cast<uintptr_t>("Miles"));
        state = {};
        state.supported = true;
        commands.clear();
        autoHeal = false;
        healthEdit = {}; savedPosition = {}; undoPosition = {};
        std::fill(std::begin(healthFlagsEdited), std::end(healthFlagsEdited), false);
        stopping = false;
        previewMode = false;
    }
    void SetSmall() {
        pool.set(0xc0 + 0x68, reinterpret_cast<uintptr_t>(small.data()));
        pool.set(0xc0 + 0x70, uint16_t(2));
        pool.set(0xc0 + 0x88, static_cast<unsigned char>(0));
    }
    void SetLarge() {
        large = {};
        for (const auto& value : small) {
            size_t slot = (value.info >> 4) & 7;
            while (large[slot].info) slot = (slot + 1) & 7;
            large[slot] = value;
        }
        pool.set(0xc0 + 0x80, reinterpret_cast<uintptr_t>(large.data()));
        pool.set(0xc0 + 0x88, uint16_t(8));
    }
};

void Exercise() {
    Fixture f;
    Check(NameCRC("HeroHealth") == 0x07479b47 && NameCRC("Health") == 0xf0a26d74 &&
        NameCRC("HeroCharacterManager") == 0x22216f8c, "component CRC differs from independent constants");
    uint32_t handle = 0;
    Check(CurrentActor(handle) == f.actor && handle == f.handle, "current actor slot was not resolved");
    f.pool.set(0xc8, uint16_t(6));
    Check(!CurrentActor(handle) && !handle, "stale generation accepted");
    f.pool.set(0xc8, uint16_t(7));
    f.entry.set(4, uint32_t((7u << 20) | 2u));
    Check(!CurrentActor(handle), "out-of-range actor index accepted");
    f.entry.set(4, uint32_t(1));
    Check(!CurrentActor(handle), "zero actor generation accepted");
    f.entry.set(4, f.handle);
    f.poolCount = 0xffffffff;
    Check(!CurrentActor(handle), "oversized actor pool accepted");
    f.poolCount = 2;
    f.poolPointer = std::numeric_limits<uintptr_t>::max() - 16;
    Check(!CurrentActor(handle), "overflowed actor address accepted");
    f.poolPointer = f.pool.address();
    Check(ComponentInfo("HeroHealth") == f.heroInfo.address(), "registry did not resolve health metadata");
    Check(ComponentInfo("HeroCharacterManager") == f.characterInfo.address(), "registry did not resolve character metadata");
    Check(!ComponentInfo("MissingComponent"), "missing registry key accepted");
    f.registry.set(0x14, uint32_t(0));
    Check(!ComponentInfo("HeroHealth"), "zero registry capacity accepted");
    f.registry.set(0x14, uint32_t(65537));
    Check(!ComponentInfo("HeroHealth"), "oversized registry capacity accepted");
    f.registry.set(0x14, uint32_t(17));
    f.registry.set(0x1c, uint32_t(0));
    Check(!ComponentInfo("HeroHealth"), "zero registry stride accepted");
    f.registry.set(0x1c, uint32_t(4));
    f.registry.set(0, std::numeric_limits<uintptr_t>::max() - 1);
    Check(!ComponentInfo("HeroHealth"), "overflowing registry pointer accepted");
    f.registry.set(0, reinterpret_cast<uintptr_t>(f.keys.data()));
    const auto keys = f.keys;
    f.keys.fill(1);
    Check(!ComponentInfo("HeroHealth"), "full unmatched registry table failed to terminate");
    f.keys = keys;
    Check(Component(f.actor, f.handle, "HeroHealth") == f.health.address(), "small component lookup failed");
    Check(Component(f.actor, f.handle, "Health") == f.health.address(), "inherited small component lookup failed");
    f.pool.set(0xc0 + 0x70, uint16_t(4097));
    Check(!Component(f.actor, f.handle, "HeroHealth"), "oversized small component list accepted");
    f.SetSmall();
    f.pool.set(0xc0 + 0x68, std::numeric_limits<uintptr_t>::max() - 8);
    Check(!Component(f.actor, f.handle, "HeroHealth"), "overflowing small component buffer accepted");
    f.SetSmall();
    f.health.set(0x10, f.actor + 0xc0);
    Check(!Component(f.actor, f.handle, "HeroHealth"), "component with another owner accepted");
    f.health.set(0x10, f.actor);
    f.SetLarge();
    Check(Component(f.actor, f.handle, "HeroHealth") == f.health.address(), "large component lookup failed");
    Check(Component(f.actor, f.handle, "HeroCharacterManager") == f.manager.address(), "large manager lookup failed");
    f.pool.set(0xc0 + 0x88, uint16_t(3));
    Check(!Component(f.actor, f.handle, "HeroHealth"), "non-power-of-two large table accepted");
    f.SetLarge();
    const auto savedLarge = f.large;
    for (size_t i = 0; i < f.large.size(); ++i) f.large[i] = {0x10000 + i * 16, 0};
    Check(!Component(f.actor, f.handle, "HeroHealth"), "full unmatched large table failed to terminate");
    f.large = savedLarge;
    f.pool.set(0xc0 + 0x80, std::numeric_limits<uintptr_t>::max() - 8);
    Check(!Component(f.actor, f.handle, "HeroHealth"), "overflowing large component buffer accepted");
    f.SetSmall();
    void* protectedPage = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    Check(protectedPage != nullptr, "cannot allocate inaccessible fixture page");
    f.pool.set(0xc0 + 0x68, reinterpret_cast<uintptr_t>(protectedPage));
    const bool noAccessRejected = !Component(f.actor, f.handle, "HeroHealth");
    f.SetSmall();
    VirtualFree(protectedPage, 0, MEM_RELEASE);
    Check(noAccessRejected, "inaccessible component buffer accepted");
    f.config.set(0x38, int(129));
    uintptr_t manager = 0;
    Check(Characters(f.actor, f.handle, manager).empty(), "oversized character list accepted");
    f.config.set(0x38, int(2));

    const auto actorBefore = f.pool.data;
    const auto healthBefore = f.health.data;
    const auto registryBefore = f.registry.data;
    for (int i = 0; i < 100; ++i) GameTick();
    const auto snapshot = GetSnapshot();
    Check(snapshot.heroReady && snapshot.healthReady && snapshot.health == 75 && snapshot.maxHealth == 100,
        "read-only snapshot did not report health");
    Check(snapshot.x == 12.5f && snapshot.y == -42 && snapshot.z == 300, "read-only snapshot did not report position");
    Check(snapshot.characterNames == std::vector<std::wstring>{L"Peter", L"Miles"}, "read-only snapshot did not report character names");
    Check(f.pool.data == actorBefore && f.health.data == healthBefore && f.registry.data == registryBefore,
        "default snapshot modified engine-owned fixture bytes or actor caches");
    Check(explicitSetterCalls == 0, "default snapshot called an engine mutation");

    // The synthetic process has no registered game TLS role or safe-point
    // dispatcher. A valid manager and a bound switch stub must not suffice.
    SubmitAction(Action::SwitchCharacter, 1);
    GameTick();
    Check(explicitSetterCalls == 0, "character switch called native code on an unregistered thread");
    Check(!GetSnapshot().engineActionsReady, "unregistered fixture advertised native engine actions");
    SubmitAction(Action::AutoHeal, 1);
    GameTick();
    GameTick();
    Check(!autoHeal && explicitSetterCalls == 0, "auto-refill called native code without a verified dispatcher");
    SubmitAction(Action::AutoHeal, 0);
    GameTick();
    Check(!autoHeal && explicitSetterCalls == 0, "auto-refill failed to stop safely");
    f.health.set(0xd0, 75.0f);
    SubmitAction(Action::Heal);
    GameTick();
    Check(explicitSetterCalls == 0, "Heal called native code without a verified dispatcher");
    const auto transformBefore = f.transform.data;
    const auto managerBefore = f.manager.data;
    SubmitAction(Action::SavePosition);
    GameTick();
    Check(savedPosition.valid && savedPosition.handle == f.handle && savedPosition.value.x == 12.5f,
        "read-only save-position action was disabled");
    undoPosition = {{20, 30, 40}, f.handle, true};
    const std::array<Action, 12> nativeActions{{Action::Heal, Action::AutoHeal, Action::SetMaxHealth,
        Action::LoadPosition, Action::UndoTeleport, Action::MoveXMinus, Action::MoveXPlus,
        Action::MoveYMinus, Action::MoveYPlus, Action::MoveZMinus, Action::MoveZPlus, Action::SwitchCharacter}};
    for (Action action : nativeActions) {
        SubmitAction(action, action == Action::SetMaxHealth ? 200.0f : 1.0f);
        GameTick();
        Check(explicitSetterCalls == 0, "queued native action entered engine code without a dispatcher");
        Check(GetSnapshot().status.find(L"no verified engine-thread dispatcher") != std::wstring::npos,
            "rejected native action did not provide a diagnostic reason");
    }
    Check(f.health.data == healthBefore && f.transform.data == transformBefore && f.manager.data == managerBefore,
        "blocked native action modified engine-owned fixture memory");
    Check(!SetCurrentHealth(f.health.address(), 100) && explicitSetterCalls == 0,
        "setter-level guard allowed direct native entry");
    autoHeal = true;
    GameTick();
    Check(!autoHeal && explicitSetterCalls == 0, "periodic auto-refill bypassed the context guard");
    const std::array<Action, 4> flagActions{{Action::HeroNoDamage, Action::HeroNoDeath, Action::EnemyNoDeath, Action::EnemyNoDamage}};
    for (Action action : flagActions) { SubmitAction(action, 1); GameTick(); }
    Check(f.healthFlags == std::array<unsigned char, 4>{{1, 1, 1, 1}}, "safe health flag controls stopped working");
    SubmitAction(Action::ResetHealth); GameTick();
    Check(f.healthFlags == std::array<unsigned char, 4>{} && explicitSetterCalls == 0,
        "ResetHealth failed to restore owned global flags without native entry");
    SubmitAction(Action::HeroNoDamage, 1); GameTick();
    healthEdit = {f.health.address(), f.handle, 80, 100};
    SubmitAction(Action::ResetAll); GameTick();
    Check(f.healthFlags == std::array<unsigned char, 4>{} && !savedPosition.valid && !undoPosition.valid &&
        explicitSetterCalls == 0 && f.health.data == healthBefore,
        "ResetAll did not safely restore flags/positions or entered an unavailable native setter");
    Check(GetSnapshot().healthReady && !GetSnapshot().engineActionsReady,
        "readable health and unavailable native-action capability were conflated");
    Check(healthEdit.component == f.health.address(), "refused native restoration discarded its ownership record");
    commands.clear();
    SubmitAction(Action::SwitchCharacter, 1);
    Check(commands.size() == 1 && commands.front().actorHandle == f.handle &&
        commands.front().characterName == L"Miles", "queued switch lost hero generation or exact selected character");
    commands.clear();
    SubmitAction(Action::Heal);
    // No real HWND exists in this fixture. Populate the ordinary window queue
    // directly rather than taking SubmitAction's no-window immediate export.
    commands.push_back({Action::ExportDiagnostics, 0});
    const auto queuedBefore = commands.size();
    engineOwnsSimulation.store(true);
    GameTick();
    Check(commands.size() == queuedBefore && explicitSetterCalls == 0,
        "readiness loss transferred gameplay ownership back to the window path");
    const auto exported = outputPath / L"diagnostics.json";
    std::filesystem::remove(exported);
    {
        std::lock_guard busyEngine(simulationMutex);
        GameWindowProc(nullptr, tickMessage, reinterpret_cast<WPARAM>(selfModule), 0);
    }
    Check(std::filesystem::exists(exported), "window diagnostic command stalled behind engine simulation mutex");
    Check(commands.size() == 1 && commands.front().action == Action::Heal && explicitSetterCalls == 0,
        "window dispatcher consumed a gameplay command during readiness loss");
    engineOwnsSimulation.store(false);
    commands.clear();
    SubmitDebugSetting("Health.DisplayHealthValues", 0);
    GameTick();
    Check(explicitSetterCalls == 0 && GetSnapshot().status.find(L"no verified engine-thread dispatcher") != std::wstring::npos,
        "generic retained setting bypassed the execution-context guard");
}
} // namespace

int main() {
    try {
        outputPath = std::filesystem::current_path() / L"engine-action-fixture";
        std::filesystem::create_directories(outputPath);
        Exercise();
        std::cout << "{\"passed\":true,\"assertions\":" << assertions
            << ",\"snapshot_ticks\":100,\"default_engine_mutations\":0,\"explicit_fixture_setter_calls\":" << explicitSetterCalls
            << ",\"native_actions_blocked\":12,\"unregistered_thread\":true,\"game_loaded\":false,\"game_started\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"passed\":false,\"assertions\":" << assertions << ",\"error\":\"" << error.what()
            << "\",\"game_loaded\":false,\"game_started\":false}\n";
        return 1;
    }
}
