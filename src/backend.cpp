// SPDX-License-Identifier: GPL-3.0-or-later
// Retail structure/signature references: hbgda/SM2ScriptTemplate; see SOURCES.md.
#include "debugmenu.h"
#include "supported_builds.h"
#include "reload_mission_catalog.h"
#include "engine_dispatch.h"
#include "game_input_capture.h"
#include "photo_mode_gate.h"
#include "overlay.h"
#include "flame_slot_contract.h"
#include "mj_weapon_switch_contract.h"
#include "flame_burn_runtime.h"
#include "flame_loop_runtime.h"
#include "flame_audio.h"

#include <bcrypt.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

namespace sm2debug {
namespace {
constexpr wchar_t kTickMessage[] = L"SM2DebugMenu.GameThread.Tick.v1";
struct Vec3 { float x, y, z; };
struct Command {
    Action action; float value; std::string setting{};
    uint32_t actorHandle = 0;
    std::wstring characterName{};
    ReloadState reload{};
    uint32_t checkpointKey = 0;
    uint16_t mjWeaponSelectedItem = 0;
    PrimaryObjectiveState primary{};
    bool resetActorSave = false;
    ActorSaveState actorSave{};
    ReplayState replay{};
    bool copyReplayMode = false;
    ReloadOptionsState reloadOptions{};
    bool newGamePlus = false;
    bool keepCurrentLoadout = true;
    CameraDebugState cameraDebug{};
    bool cameraShakeEnabled = false;
    bool applyMissionSelection = false;
    ReloadSelectionState missionSelection{};
    HeroWarpState heroWarp{};
    HeroWarpPosition warpPosition{};
};
struct LoadoutPreparation {
    const ReloadOptionsState* captured = nullptr;
    bool applied = false;
};
bool PrepareLoadout(const ReloadState& admitted, void* context, std::wstring& status) {
    auto* preparation = static_cast<LoadoutPreparation*>(context);
    if (!preparation || !preparation->captured) return false;
    preparation->applied = PrepareReloadLoadoutAfterAdmission(*preparation->captured, admitted, status);
    return preparation->applied;
}
void DescribeLoadoutPreparation(const LoadoutPreparation& preparation, bool accepted, std::wstring& status) {
    if (preparation.applied) status = L"Default loadout marked for reapplication. " + status +
        (accepted ? L"" : L" The loadout reset flag remains applied even though the transition was refused.");
}
struct MissionReloadPreparation {
    const ReloadSelectionState* missions = nullptr;
    LoadoutPreparation loadout;
    bool keepCurrentLoadout = true, missionsApplied = false;
};
bool PrepareMissionReload(const ReloadState& admitted,void* context,std::wstring& status) {
    auto* preparation=static_cast<MissionReloadPreparation*>(context);
    if(!preparation)return false;
    if(preparation->missions){
        if(!PrepareReloadSelectionAfterAdmission(*preparation->missions,admitted,status))return false;
        preparation->missionsApplied=true;
    }
    return preparation->keepCurrentLoadout||PrepareLoadout(admitted,&preparation->loadout,status);
}
void DescribeMissionReload(const MissionReloadPreparation& preparation,bool accepted,std::wstring& status) {
    DescribeLoadoutPreparation(preparation.loadout,accepted,status);
    if(preparation.missionsApplied)status=L"Mission overrides prepared. "+status+
        (accepted?L"":L" The prepared mission overrides remain applied even though the transition was refused.");
}
struct HealthEdit { uintptr_t component = 0; uint32_t handle = 0; float original = 0, applied = 0; };
struct Position { Vec3 value{}; uint32_t handle = 0; bool valid = false; };
struct HeroScaleEdit {
    uintptr_t actor = 0, transform = 0;
    uint32_t handle = 0;
    Vec3 original{};
    float applied = 1.0f;
};

struct Bindings {
    uintptr_t base = 0, size = 0, text = 0, textSize = 0;
    uintptr_t heroSystem = 0, registry = 0;
    uintptr_t actorPool = 0, actorCount = 0, componentInfos = 0;
    uintptr_t healthFlags = 0;
    using SwitchCharacter = void (*)(void*, uint32_t, int, void*);
    using SetPosition = void (*)(void*, const Vec3*);
    using SetBasis = void (*)(void*, const float*, Vec3*);
    SwitchCharacter switchCharacter = nullptr;
    SetPosition setPosition = nullptr;
    SetPosition setScale = nullptr;
    SetBasis setBasis = nullptr;
} g;

std::mutex stateMutex, commandMutex, logMutex, simulationMutex;
Snapshot state;
// Guarded by stateMutex; visibility changes revoke every displayed capability.
std::uint64_t menuSnapshotGeneration = 1, completedSnapshotGeneration = 0, settingsSnapshotGeneration = 0;
ReplaySelection desiredReplaySelection;
ReloadMissionSelection desiredReloadMissionSelection;
std::deque<Command> commands;
std::atomic<bool> tickPending{false}, stopping{false};
std::atomic<uint64_t> dispatchToken{0};
std::atomic<bool> dispatchRegistrationAttempted{false}, shutdownReset{false}, shutdownResetComplete{false};
std::atomic<bool> engineOwnsSimulation{false};
bool previewMode = false;
HMODULE selfModule = nullptr;
HWND targetWindow = nullptr;
WNDPROC previousProc = nullptr;
UINT tickMessage = 0;
uint64_t startedAt = 0;
std::filesystem::path outputPath;
HealthEdit healthEdit;
Position savedPosition, undoPosition;
HeroScaleEdit heroScaleEdit;
bool autoHeal = false;
std::atomic<bool> settingsRestorePending{false};
bool healthFlagsOriginal[4]{}, healthFlagsApplied[4]{}, healthFlagsEdited[4]{};
std::atomic<bool> changedWindow{false};
LONG_PTR oldStyle = 0, oldExStyle = 0;
WINDOWPLACEMENT oldPlacement{};

void Log(const std::string& line) {
    std::lock_guard lock(logMutex);
    std::ofstream file(outputPath / L"debugmenu.log", std::ios::app);
    if (file) file << GetTickCount64() << " " << line << "\n";
}
void Status(const std::wstring& text) {
    std::lock_guard lock(stateMutex);
    state.status = text;
}
bool EngineContextReady() noexcept {
    return IsWithinVerifiedEngineCallback();
}
bool EngineDispatchAvailable() noexcept {
    return EngineDispatchCallbacksAvailable();
}
const char* NativeActionName(Action action) {
    switch (action) {
    case Action::Heal: return "Heal";
    case Action::AutoHeal: return "AutoHeal";
    case Action::SetMaxHealth: return "SetMaxHealth";
    case Action::LoadPosition: return "LoadPosition";
    case Action::UndoTeleport: return "UndoTeleport";
    case Action::MoveXMinus: return "MoveXMinus";
    case Action::MoveXPlus: return "MoveXPlus";
    case Action::MoveYMinus: return "MoveYMinus";
    case Action::MoveYPlus: return "MoveYPlus";
    case Action::MoveZMinus: return "MoveZMinus";
    case Action::MoveZPlus: return "MoveZPlus";
    case Action::SwitchCharacter: return "SwitchCharacter";
    case Action::DebugSetting: return "DebugSetting";
    case Action::ReloadCheckpoint: return "ReloadCheckpoint";
    case Action::ExitFrontend: return "ExitFrontend";
    case Action::CompletePrimary: return "CompletePrimary";
    case Action::FullReload: return "FullReload";
    case Action::ReplayMission: return "ReplayMission";
    case Action::SetNewGamePlus: return "SetNewGamePlus";
    case Action::ExitLobby: return "ExitLobby";
    case Action::CameraShake: return "CameraShake";
    case Action::EquipMJFlamethrower: return "EquipMJFlamethrower";
    case Action::SwitchMJWeapon: return "SwitchMJWeapon";
    case Action::SetHeroScale: return "SetHeroScale";
    case Action::ResetHeroScale: return "ResetHeroScale";
    case Action::HeroWarp: return "HeroWarp";
    default: return nullptr;
    }
}
void RejectEngineOperation(const std::string& operation) {
    Status(L"Native engine actions are unavailable: no verified engine-thread dispatcher.");
    Log("Rejected native operation " + operation + ": no verified engine-thread dispatcher; no engine call made.");
}
void RejectNativeAction(Action action) {
    const char* name = NativeActionName(action);
    RejectEngineOperation(std::string(name ? name : "Unknown") + " (id=" + std::to_string(static_cast<int>(action)) + ")");
}
bool ReadBytes(uintptr_t address, void* value, SIZE_T length) {
    SIZE_T n = 0;
    return address >= 0x10000 && length && length <= std::numeric_limits<uintptr_t>::max() - address &&
        ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address), value, length, &n) && n == length;
}
template<class T> bool Read(uintptr_t address, T& value) { return ReadBytes(address, &value, sizeof(value)); }
uintptr_t Offset(uintptr_t address, size_t amount) {
    return address >= 0x10000 && amount <= std::numeric_limits<uintptr_t>::max() - address ? address + amount : 0;
}
uintptr_t Element(uintptr_t address, size_t index, size_t stride) {
    return stride && index <= std::numeric_limits<size_t>::max() / stride ? Offset(address, index * stride) : 0;
}
template<class T> bool ReadAt(uintptr_t address, size_t offset, T& value) { return Read(Offset(address, offset), value); }
template<class T> bool Write(uintptr_t address, const T& value) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) return false;
    SIZE_T n = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
        &value, sizeof(value), &n) && n == sizeof(value);
}
bool PlausibleHealth(float maximum, float current) {
    return std::isfinite(maximum) && std::isfinite(current) && maximum >= 1 &&
        maximum <= 1000000 && current >= 0 && current <= maximum * 1.01f;
}
bool PlausiblePosition(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
        std::fabs(p.x) < 100000 && std::fabs(p.y) < 100000 && std::fabs(p.z) < 100000;
}

// Insomniac's component-name CRC is seeded with the polynomial, with no final xor.
uint32_t NameCRC(const char* name) {
    uint32_t crc = 0xedb88320;
    while (*name) {
        crc ^= static_cast<unsigned char>(*name++);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc;
}
std::wstring Wide(const std::string& text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::vector<wchar_t> buffer(n);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer.data(), n);
    return buffer.data();
}
std::string ReadName(uintptr_t p) {
    std::string result;
    for (int i = 0; i < 128; ++i) {
        char c = 0;
        if (!ReadAt(p, static_cast<size_t>(i), c)) return {};
        if (!c) return result;
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) return {};
        result += c;
    }
    return {};
}

std::string FileSHA256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD objectBytes = 0, written = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &written, 0) >= 0;
    std::vector<unsigned char> object(objectBytes), digest(32), buffer(1024 * 1024);
    ok = ok && BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) >= 0;
    std::ifstream file(path, std::ios::binary);
    ok = ok && file.is_open();
    while (ok && file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = file.gcount();
        if (count && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0) ok = false;
    }
    if (file.bad()) ok = false;
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream out;
    for (unsigned char c : digest) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}

uintptr_t UniquePattern(const char* signature) {
    std::istringstream input(signature);
    std::vector<int> pattern;
    std::string word;
    while (input >> word) pattern.push_back(word.find('?') != std::string::npos ? -1 : std::stoi(word, nullptr, 16));
    if (pattern.empty() || pattern.size() > g.textSize) return 0;
    uintptr_t match = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(g.text);
    for (size_t i = 0; i <= g.textSize - pattern.size(); ++i) {
        if (pattern[0] >= 0 && bytes[i] != pattern[0]) continue;
        size_t j = 1;
        while (j < pattern.size() && (pattern[j] < 0 || bytes[i+j] == pattern[j])) ++j;
        if (j != pattern.size()) continue;
        if (match) return 0;
        match = g.text + i;
    }
    return match;
}
uintptr_t Relative(uintptr_t p, int displacementOffset, int instructionSize) {
    int32_t delta = 0;
    if (!p || !Read(p + displacementOffset, delta)) return 0;
    uintptr_t result = p + instructionSize + delta;
    return result >= g.base && result < g.base + g.size ? result : 0;
}
bool FunctionStart(uintptr_t address) {
    if (!address || address < g.text || address >= g.text + g.textSize) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g.base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* entries = reinterpret_cast<RUNTIME_FUNCTION*>(g.base + directory.VirtualAddress);
    size_t count = directory.Size / sizeof(RUNTIME_FUNCTION);
    for (size_t i = 0; i < count; ++i) if (g.base + entries[i].BeginAddress == address) return true;
    // These two independently disassembled leaf functions do not need x64
    // unwind records. BindGame still requires the known file hash and an exact,
    // unique live signature before accepting these specific entry RVAs.
    if (address == g.base + 0x28a8970 || address == g.base + 0x29bd860) return true;
    return false;
}
bool BindGame() {
    auto module = GetModuleHandleW(L"Spider-Man2.exe");
    if (!module || module != GetModuleHandleW(nullptr)) return false;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module, path, 32768)) return false;
    Log("Checking executable SHA256; no engine access before compatibility passes.");
    if (!SupportedExecutable::IsSupported(FileSHA256(path))) {
        Status(L"Unsupported executable hash. Gameplay controls are disabled.");
        Log("Unsupported executable hash.");
        return false;
    }
    g.base = reinterpret_cast<uintptr_t>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g.base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    g.size = nt->OptionalHeader.SizeOfImage;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sections[i].Name, ".text", 5) == 0) {
            g.text = g.base + sections[i].VirtualAddress;
            g.textSize = sections[i].Misc.VirtualSize;
        }
    }
    if (!g.text) return false;
    uintptr_t actor = UniquePattern("44 8B 01 41 8B D0 C1 EA 14 81 E2 FF 07 00 00 74 ?? 41 81 E0 FF FF 0F 00 4B 8D 0C ?? 48 C1 E1 06 48 03 0D ?? ?? ?? ?? 44 3B 05 ?? ?? ?? ?? 73 ?? 0F B7 41 ?? 3B D0 74 ?? 33 C9 48 8B C1");
    uintptr_t info = Relative(UniquePattern("E8 ?? ?? ?? ?? 49 8B 4E ?? 48 8D 54 24 ?? 48 83 C1 58"), 1, 5);
    uintptr_t small = UniquePattern("48 89 5C 24 ?? 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 0F 84 ?? ?? ?? ?? 66 83 79 ?? 00");
    uintptr_t large = UniquePattern("44 0F B7 49 ?? 4C 8B C2");
    uintptr_t character = UniquePattern("48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC 90 00 00 00 49 8B E9");
    uintptr_t positionSetter = UniquePattern("48 89 5C 24 08 57 48 83 EC 30 80 79 5F 00 48 8B FA C5 F8 29 74 24 20 48 8B D9 C5 F8 10 71 30 75 ?? 45 33 C0 48 8B D1 E8 ?? ?? ?? ?? 8B 07");
    g.heroSystem = Relative(UniquePattern("48 89 05 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? 48 89 35"), 3, 7);
    g.registry = Relative(UniquePattern("48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 8B 4E ?? 48 8D 54 24 ?? 48 83 C1 58"), 3, 7);
    const auto damageAccessor = UniquePattern("48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 39 0F B7 51 08 45 33 C0 8B 41 0C");
    if (FunctionStart(damageAccessor)) {
        const auto candidate = Relative(damageAccessor, 6, 11);
        unsigned char flags[4]{};
        if (ReadBytes(candidate, flags, sizeof(flags)) &&
            std::all_of(std::begin(flags), std::end(flags), [](unsigned char c) { return c <= 1; })) {
            g.healthFlags = candidate;
            for (int i = 0; i < 4; ++i) healthFlagsOriginal[i] = flags[i] != 0;
        }
    }
    // Metadata references are decoded from the verified lookup instructions;
    // automatic snapshots never call those engine lookup functions themselves.
    g.actorPool = Relative(actor + 0x20, 3, 7);
    g.actorCount = Relative(actor + 0x27, 3, 7);
    g.componentInfos = Relative(info + 0x10, 3, 7);
    if (!FunctionStart(actor) || !FunctionStart(info) || !FunctionStart(small) ||
        !FunctionStart(large) || !g.heroSystem || !g.registry) {
        Log("A required signature is missing, ambiguous, or not a function start.");
        return false;
    }
    if (!g.actorPool || !g.actorCount || !g.componentInfos) {
        Log("A read-only snapshot metadata reference is unavailable.");
        return false;
    }
    if (FunctionStart(character)) g.switchCharacter = reinterpret_cast<Bindings::SwitchCharacter>(character);
    if (FunctionStart(positionSetter)) g.setPosition = reinterpret_cast<Bindings::SetPosition>(positionSetter);
    // SceneObject::SetScale(Vec3), matched to the named 1.5 Engine export and
    // the retail actor/scene setter. Its native path updates scale, basis and
    // transform dirty state; writing only the visible matrix would bypass it.
    const auto scaleSetter = UniquePattern("48 8B C4 48 89 58 18 57 48 81 EC 80 00 00 00 80 79 5F 00 48 8B FA C5 F8 29 70 E8 48 8B D9 C5 F8 29 78 D8 C5 78 29 40 C8 C5 78 29 48 B8 C5 78 29 50 A8 C5 7B 10 12 C4 63 29 21 52 08 20");
    std::array<unsigned char,444> basisCode{};
    uint64_t basisDigest = 14695981039346656037ull;
    const bool basisRead = ReadBytes(g.base + 0x2cf64f0, basisCode.data(), basisCode.size());
    for (const auto byte : basisCode) basisDigest = (basisDigest ^ byte) * 1099511628211ull;
    if (scaleSetter == g.base + 0x2cf6af0 && FunctionStart(scaleSetter) && basisRead &&
        basisDigest == 0xf018636bb61deb95ull && FunctionStart(g.base + 0x2cf64f0)) {
        g.setScale = reinterpret_cast<Bindings::SetPosition>(scaleSetter);
        g.setBasis = reinterpret_cast<Bindings::SetBasis>(g.base + 0x2cf64f0);
    }
    Log("Core signatures verified against the supported retail build.");
    return true;
}

uintptr_t CurrentActor(uint32_t& handle) {
    // Known-build actor handles: low 20 bits select a 0xc0-byte pool slot;
    // bits 20..30 are its generation. Checking a mapped address alone would not
    // establish actor identity, so verify the generation and current hero again.
    uintptr_t currentEntry = 0, vtable = 0, pool = 0;
    uint32_t candidateHandle = 0, count = 0;
    handle = 0;
    if (!g.actorPool || !g.actorCount || !Read(g.heroSystem, vtable) ||
        vtable < g.base || vtable - g.base >= g.size ||
        !ReadAt(g.heroSystem, 0x150, currentEntry) || !currentEntry ||
        !ReadAt(currentEntry, 4, candidateHandle) ||
        !Read(g.actorPool, pool) || !Read(g.actorCount, count) || count > (1u << 20)) return 0;
    const uint32_t index = candidateHandle & 0xfffff;
    const uint16_t generation = static_cast<uint16_t>((candidateHandle >> 20) & 0x7ff);
    if (!generation || index >= count) return 0;
    const uintptr_t actor = Element(pool, index, 0xc0);
    uint16_t actualGeneration = 0;
    uintptr_t transform = 0;
    if (!ReadAt(actor, 8, actualGeneration) || actualGeneration != generation ||
        !Read(actor, transform) || transform < 0x10000) return 0;
    uintptr_t afterEntry = 0, afterPool = 0;
    uint32_t afterHandle = 0, afterCount = 0;
    if (!ReadAt(g.heroSystem, 0x150, afterEntry) || afterEntry != currentEntry ||
        !ReadAt(currentEntry, 4, afterHandle) || afterHandle != candidateHandle ||
        !Read(g.actorPool, afterPool) || afterPool != pool ||
        !Read(g.actorCount, afterCount) || afterCount != count ||
        !ReadAt(actor, 8, actualGeneration) || actualGeneration != generation) return 0;
    handle = candidateHandle;
    return actor;
}
bool ActorStillCurrent(uintptr_t actor, uint32_t handle) {
    uint32_t currentHandle = 0;
    return actor && handle && CurrentActor(currentHandle) == actor && currentHandle == handle;
}
uintptr_t ComponentInfo(const char* name) {
    // The registry uses uint32 CRC keys and values indexing the global info
    // pointer array. Reproduce its bounded linear probe using guarded copies.
    // In particular, do not call the native small-container lookup: it can
    // update a thread-role-dependent descriptor/index cache inside the actor.
    constexpr uint32_t kMaxRegistrySlots = 65536;
    uintptr_t keys = 0, values = 0, infos = 0;
    uint32_t capacity = 0, stride = 0;
    if (!Read(g.registry, keys) || !ReadAt(g.registry, 8, values) ||
        !ReadAt(g.registry, 0x14, capacity) || !capacity || capacity > kMaxRegistrySlots ||
        !ReadAt(g.registry, 0x1c, stride) || stride < 4 || stride > 256 || (stride & 3) ||
        !Read(g.componentInfos, infos)) return 0;
    const uint32_t crc = NameCRC(name);
    if (!crc) return 0;
    const uint64_t product = static_cast<uint64_t>(crc) * 0x9e3779b1u;
    uint32_t bucket = (static_cast<uint32_t>(product) ^ static_cast<uint32_t>(product >> 32)) % capacity;
    uintptr_t descriptor = 0;
    for (uint32_t visited = 0; visited < capacity; ++visited) {
        uint32_t key = 0;
        if (!Read(Element(keys, bucket, sizeof(uint32_t)), key) || !key) return 0;
        if (key == crc) {
            uint32_t index = 0;
            if (!Read(Element(values, bucket, stride), index) || index >= kMaxRegistrySlots ||
                !Read(Element(infos, index, sizeof(uintptr_t)), descriptor)) return 0;
            break;
        }
        bucket = bucket + 1 == capacity ? 0 : bucket + 1;
    }
    uintptr_t namePointer = 0;
    uintptr_t afterKeys = 0, afterValues = 0, afterInfos = 0;
    uint32_t afterCapacity = 0, afterStride = 0;
    if (!descriptor || !ReadAt(descriptor, 0x60, namePointer) || ReadName(namePointer) != name ||
        !Read(g.registry, afterKeys) || afterKeys != keys ||
        !ReadAt(g.registry, 8, afterValues) || afterValues != values ||
        !ReadAt(g.registry, 0x14, afterCapacity) || afterCapacity != capacity ||
        !ReadAt(g.registry, 0x1c, afterStride) || afterStride != stride ||
        !Read(g.componentInfos, afterInfos) || afterInfos != infos) return 0;
    return descriptor;
}
struct ComponentEntry { uintptr_t info = 0, component = 0; };
static_assert(sizeof(ComponentEntry) == 16);
uintptr_t SmallComponent(uintptr_t actor, uintptr_t descriptor) {
    constexpr uint16_t kMaxComponents = 4096;
    const uintptr_t container = Offset(actor, 0x58);
    uintptr_t entries = 0;
    uint16_t count = 0;
    unsigned char flags = 0;
    if (!ReadAt(container, 0x10, entries) || !ReadAt(container, 0x18, count) ||
        !count || count > kMaxComponents || !ReadAt(descriptor, 0xe4, flags)) return 0;
    std::vector<ComponentEntry> copy(count);
    if (!ReadBytes(entries, copy.data(), copy.size() * sizeof(ComponentEntry))) return 0;
    uintptr_t result = 0;
    for (const auto& entry : copy) {
        bool matches = entry.info == descriptor;
        if (!matches && (flags & 0x40) && entry.info) {
            unsigned char baseCount = 0;
            std::array<uintptr_t, 255> bases{};
            if (!ReadAt(entry.info, 0xe6, baseCount) ||
                (baseCount && !ReadBytes(Offset(entry.info, 0x80), bases.data(), baseCount * sizeof(uintptr_t)))) return 0;
            for (unsigned i = 0; i < baseCount; ++i) if (bases[i] == descriptor) { matches = true; break; }
        }
        if (matches) { result = entry.component; break; }
    }
    uintptr_t afterEntries = 0;
    uint16_t afterCount = 0;
    return ReadAt(container, 0x10, afterEntries) && afterEntries == entries &&
        ReadAt(container, 0x18, afterCount) && afterCount == count ? result : 0;
}
uintptr_t LargeComponent(uintptr_t actor, uintptr_t descriptor) {
    // Large containers are power-of-two pointer-key hash tables. Copy the
    // bounded table, probe at most its capacity, and never touch its storage.
    const uintptr_t container = Offset(actor, 0x80);
    uintptr_t entries = 0;
    uint16_t capacity = 0;
    if (!Read(container, entries) || !ReadAt(container, 8, capacity) || !capacity ||
        capacity > 32768 || (capacity & (capacity - 1))) return 0;
    std::vector<ComponentEntry> copy(capacity);
    if (!ReadBytes(entries, copy.data(), copy.size() * sizeof(ComponentEntry))) return 0;
    const size_t mask = capacity - 1;
    size_t bucket = (descriptor >> 4) & mask;
    uintptr_t result = 0;
    for (size_t visited = 0; visited < capacity; ++visited) {
        const auto& entry = copy[bucket];
        if (entry.info == descriptor) { result = entry.component; break; }
        if (!entry.info) break;
        bucket = (bucket + 1) & mask;
    }
    uintptr_t afterEntries = 0;
    uint16_t afterCapacity = 0;
    return Read(container, afterEntries) && afterEntries == entries &&
        ReadAt(container, 8, afterCapacity) && afterCapacity == capacity ? result : 0;
}
uintptr_t Component(uintptr_t actor, uint32_t handle, const char* name) {
    if (!ActorStillCurrent(actor, handle)) return 0;
    const uintptr_t descriptor = ComponentInfo(name);
    if (!descriptor) return 0;
    unsigned char large = 0;
    if (!ReadAt(actor, 0x88, large)) return 0;
    const uintptr_t result = large ? LargeComponent(actor, descriptor) : SmallComponent(actor, descriptor);
    uintptr_t owner = 0;
    if (!ReadAt(result, 0x10, owner) || owner != actor || !ActorStillCurrent(actor, handle)) return 0;
    return result;
}
bool GetHealth(uintptr_t actor, uint32_t handle, uintptr_t& component, float& maximum, float& current) {
    component = Component(actor, handle, "HeroHealth");
    if (!component) component = Component(actor, handle, "Health");
    uintptr_t owner = 0;
    return component && ReadAt(component, 0xa0, maximum) && ReadAt(component, 0xd0, current) &&
        PlausibleHealth(maximum, current) && ReadAt(component, 0x10, owner) && owner == actor && ActorStillCurrent(actor, handle);
}
uintptr_t HealthSetter(uintptr_t component) {
    uintptr_t vtable = 0, getter = 0, setter = 0;
    if (!component || !Read(component, vtable) ||
        !Read(vtable + 13 * sizeof(uintptr_t), getter) ||
        !Read(vtable + 14 * sizeof(uintptr_t), setter) || getter != g.base + 0x43e020 ||
        (setter != g.base + 0xee5580 && setter != g.base + 0x34703d0)) return 0;
    return setter;
}
bool SetCurrentHealth(uintptr_t component, float value) {
    // Defense in depth: resets or periodic paths must obey the same caller
    // context requirement as explicit queued actions.
    if (!EngineContextReady()) { RejectEngineOperation("SetCurrentHealth"); return false; }
    const uintptr_t setter = HealthSetter(component);
    if (!setter || !std::isfinite(value)) return false;
    // These vtable methods were disassembled on this exact executable. The
    // setters clamp the value and publish the normal health-change event.
    reinterpret_cast<void (*)(void*, float)>(setter)(reinterpret_cast<void*>(component), value);
    return true;
}
bool GetPosition(uintptr_t actor, uint32_t handle, Vec3& position) {
    uintptr_t transform = 0, afterTransform = 0;
    return ActorStillCurrent(actor, handle) && Read(actor, transform) && ReadAt(transform, 0x30, position) &&
        PlausiblePosition(position) && Read(actor, afterTransform) && afterTransform == transform && ActorStillCurrent(actor, handle);
}
bool ValidScale(const Vec3& value) {
    const auto valid = [](float f) { return std::isfinite(f) && f >= 0.00001f && f <= 10000.0f; };
    return valid(value.x) && valid(value.y) && valid(value.z);
}
bool ReadHeroScale(uintptr_t actor, uint32_t handle, uintptr_t& transform, Vec3& scale) {
    uintptr_t after = 0;
    return g.setScale && g.setBasis && ActorStillCurrent(actor, handle) && Read(actor, transform) &&
        ReadAt(transform, 0x70, scale) && ValidScale(scale) &&
        Read(actor, after) && transform == after && ActorStillCurrent(actor, handle);
}
bool ScaleMatches(const Vec3& value, float uniform) {
    return value.x == uniform && value.y == uniform && value.z == uniform;
}
bool SetHeroScale(uintptr_t actor, uint32_t handle, float value) {
    if (!EngineContextReady() || !std::isfinite(value) || value < 0.1f || value > 10.0f) return false;
    uintptr_t transform = 0;
    Vec3 before{};
    if (!ReadHeroScale(actor, handle, transform, before)) return false;
    // A different writer or actor invalidates restoration ownership. Capture
    // that current value as the origin only after a new explicit user action.
    if (heroScaleEdit.actor != actor || heroScaleEdit.handle != handle ||
        heroScaleEdit.transform != transform || !ScaleMatches(before, heroScaleEdit.applied))
        heroScaleEdit = {actor, transform, handle, before, value};
    else heroScaleEdit.applied = value;
    const Vec3 desired{value, value, value};
    g.setScale(reinterpret_cast<void*>(transform), &desired);
    uintptr_t afterTransform = 0;
    Vec3 after{};
    if (!ReadHeroScale(actor, handle, afterTransform, after) || afterTransform != transform || !ScaleMatches(after, value)) {
        heroScaleEdit = {};
        return false;
    }
    return true;
}
bool RestoreHeroScale(uintptr_t actor, uint32_t handle) {
    if (!EngineContextReady()) return false;
    if (!heroScaleEdit.actor) return true;
    const auto owned = heroScaleEdit;
    uintptr_t transform = 0;
    Vec3 current{};
    if (owned.actor != actor || owned.handle != handle ||
        !ReadHeroScale(actor, handle, transform, current) || transform != owned.transform ||
        !ScaleMatches(current, owned.applied)) {
        heroScaleEdit = {};
        return false;
    }
    heroScaleEdit = {};
    g.setScale(reinterpret_cast<void*>(transform), &owned.original);
    return true;
}
void TickHeroScale() {
    if (!heroScaleEdit.actor || !EngineContextReady()) return;
    uint32_t handle = 0;
    const uintptr_t actor = CurrentActor(handle);
    uintptr_t transform = 0;
    Vec3 scale{};
    if (actor != heroScaleEdit.actor || handle != heroScaleEdit.handle ||
        !ReadHeroScale(actor, handle, transform, scale) || transform != heroScaleEdit.transform ||
        !ScaleMatches(scale, heroScaleEdit.applied)) {
        heroScaleEdit = {};
        return;
    }
    // Match the original per-frame Hero scale behavior while retaining the
    // current rotation. Avoid a setter call while the basis already matches.
    std::array<float, 12> basis{};
    if (!ReadBytes(transform, basis.data(), sizeof(basis))) { heroScaleEdit = {}; return; }
    bool needsApply = false;
    std::array<float,9> packed{};
    for (size_t row = 0; row < 3; ++row) {
        const auto i = row * 4;
        const float length = std::sqrt(basis[i]*basis[i] + basis[i+1]*basis[i+1] + basis[i+2]*basis[i+2]);
        if (!std::isfinite(length) || length < 0.00001f) { heroScaleEdit = {}; return; }
        needsApply |= std::fabs(length - heroScaleEdit.applied) > 0.0001f;
        const float ratio = heroScaleEdit.applied / length;
        for (size_t axis = 0; axis < 3; ++axis) packed[row*3+axis] = basis[i+axis] * ratio;
    }
    if (needsApply && ActorStillCurrent(actor, handle)) {
        // The source's Mat3 setter dirties changed basis even when the cached
        // scale is unchanged; SetScale alone would skip that notification.
        g.setBasis(reinterpret_cast<void*>(transform), packed.data(), reinterpret_cast<Vec3*>(transform + 0x70));
    }
}
std::vector<std::string> Characters(uintptr_t actor, uint32_t handle, uintptr_t& manager) {
    std::vector<std::string> names;
    manager = Component(actor, handle, "HeroCharacterManager");
    uintptr_t config = 0, entries = 0;
    int count = 0;
    if (!g.switchCharacter || !manager || !ReadAt(manager, 0x90, config) ||
        !ReadAt(config, 0x30, entries) || !ReadAt(config, 0x38, count) || count < 1 || count > 128) return names;
    for (int i = 0; i < count; ++i) {
        uintptr_t namePointer = 0;
        if (!ReadAt(Element(entries, static_cast<size_t>(i), 0x2a0), 8, namePointer)) return {};
        auto name = ReadName(namePointer);
        if (name.empty()) return {};
        names.push_back(name);
    }
    uintptr_t afterConfig = 0, afterEntries = 0, owner = 0;
    int afterCount = 0;
    if (!ReadAt(manager, 0x90, afterConfig) || afterConfig != config ||
        !ReadAt(config, 0x30, afterEntries) || afterEntries != entries ||
        !ReadAt(config, 0x38, afterCount) || afterCount != count ||
        !ReadAt(manager, 0x10, owner) || owner != actor || !ActorStillCurrent(actor, handle)) { manager = 0; return {}; }
    return names;
}
// The private loadout adds one weapon and equips it through the native system.
// It does not reset mission inventory or edit the character's original record.
bool FlameLoadoutCode() {
    struct Pin { uintptr_t rva; size_t size; uint64_t hash; };
    static constexpr Pin pins[] = {
#include "flame_loadout_pins.inc"
    };
    std::array<unsigned char,2048> bytes{};
    for (const auto& pin:pins) {
        if (pin.size>bytes.size() || !ReadBytes(g.base+pin.rva,bytes.data(),pin.size)) return false;
        uint64_t digest=14695981039346656037ull;
        for (size_t i=0;i<pin.size;++i) digest=(digest^bytes[i])*1099511628211ull;
        if (digest!=pin.hash) return false;
    }
    return true;
}
bool FlameLoadoutReady(uintptr_t actor,uint32_t handle,uintptr_t& system) {
    system=0;
    if (!g.base || !ActorStillCurrent(actor,handle) || !FlameLoadoutCode()) return false;
    uintptr_t manager=0;
    const auto names=Characters(actor,handle,manager);
    int index=-1;
    if (!manager || !ReadAt(manager,0x98,index) || index<0 || static_cast<size_t>(index)>=names.size()) return false;
    static constexpr const char* maryNames[]={"MaryJane","MaryJane_leather","MaryJane_longcoat","MaryJane_pullover",
        "MaryJane_buttonup","MaryJane_sweater_yellow","MaryJane_sweater_yellow_wet",
        "MaryJane_sweater_yellow_wetmud","MaryJane_leather_wet"};
    if (std::none_of(std::begin(maryNames),std::end(maryNames),[&](const char* n){return names[index]==n;})) return false;
    if (!Component(actor,handle,"ItemInventory")) return false;
    uintptr_t vtable=0,ready=0,apply=0,applyData=0,resources=0;
    return Read(g.base+0xc225310,system) && Read(system,vtable) && vtable==g.base+0x547c360 &&
        ReadAt(vtable,0x48,ready) && ready==g.base+0x14c7170 &&
        ReadAt(vtable,0x50,apply) && apply==g.base+0x14c72c0 &&
        ReadAt(vtable,0x58,applyData) && applyData==g.base+0x14c72b0 &&
        ReadAt(system,0x18,resources) && resources && ActorStillCurrent(actor,handle);
}
bool FlameDDLPointer(uintptr_t address,uintptr_t& pointer) {
    uint64_t encoded=0;
    if (!Read(address,encoded)) return false;
    if (encoded>>63) {
        // Native DDL getter 30989d0: tagged relative pointer, signed low32
        // displacement from this field, not from the object or image base.
        const auto delta=static_cast<int64_t>(static_cast<int32_t>(encoded));
        if (delta<0) {
            if (address<static_cast<uint64_t>(-delta)) return false;
            pointer=address-static_cast<uint64_t>(-delta);
        } else {
            if (static_cast<uint64_t>(delta)>UINTPTR_MAX-address) return false;
            pointer=address+static_cast<uint64_t>(delta);
        }
    } else pointer=static_cast<uintptr_t>(encoded);
    return pointer>=0x10000;
}
static constexpr uint64_t FlameTanksItemAsset=0x91ad3465cf372a7bull;
static constexpr uint64_t FlameTanksModelAsset=0xabebe979576066efull;
bool FlameLoadoutShape(uintptr_t loadout) {
    uintptr_t groups=0,items=0;
    uint32_t groupCount=0,itemCount=0,selection=0;
    unsigned char fillAmmo=1,postProcess=1;
    if (!(loadout && ReadAt(loadout,0x30,fillAmmo) && fillAmmo==0 &&
        FlameDDLPointer(Offset(loadout,0x38),groups) && ReadAt(loadout,0x40,groupCount) && groupCount==1 &&
        ReadAt(groups,0x30,postProcess) && postProcess==0 && ReadAt(groups,0x34,selection) && selection==0 &&
        FlameDDLPointer(Offset(groups,0x38),items) && ReadAt(groups,0x40,itemCount) && itemCount==2)) return false;
    // Native ItemLoadoutEntry is 0x78 bytes. Admit only this gun followed by its
    // backpack accessory, without progression, overrides or bulk equipment work.
    const uint64_t identities[]={flame_slot::ItemAsset,FlameTanksItemAsset};
    for (size_t i=0;i<std::size(identities);++i) {
        const auto item=Element(items,i,0x78);
        uint32_t quantity=0,skinCount=0,progressionBits=0;
        uint64_t identity=0,equipTarget=0;
        unsigned char equip=0;
        if (!(ReadAt(item,0x2c,identity) && identity==identities[i] &&
            ReadAt(item,0x38,quantity) && quantity==1 && ReadAt(item,0x3c,progressionBits) && progressionBits==0 &&
            ReadAt(item,0x40,equip) && equip==1 &&
            ReadAt(item,0x54,equipTarget) && equipTarget==0 && ReadAt(item,0x68,skinCount) && skinCount==0)) return false;
    }
    return true;
}
bool FlameSlotCode() {
    struct Pin { uintptr_t rva; size_t size; uint64_t hash; };
    static constexpr Pin pins[] = {
#include "flame_slot_pins.inc"
    };
    std::array<unsigned char,2048> bytes{};
    for (const auto& pin:pins) {
        if (pin.size>bytes.size() || !ReadBytes(g.base+pin.rva,bytes.data(),pin.size)) return false;
        uint64_t digest=14695981039346656037ull;
        for (size_t i=0;i<pin.size;++i) digest=(digest^bytes[i])*1099511628211ull;
        if (digest!=pin.hash) return false;
    }
    return true;
}
uintptr_t FlameComponentHandle(uint32_t handle) {
    // Component handles use all twelve generation bits (actor handles do not).
    uintptr_t pool=0,pointer=0,afterPool=0,afterPointer=0;
    uint32_t count=0,afterCount=0,generation=0,afterGeneration=0,self=0;
    const auto index=handle&0xfffff, expected=handle>>20;
    if (!expected || !Read(g.base+0xc0bebe0,pool) || !Read(g.base+0xc0bec00,count) ||
        count>(1u<<20) || index>=count) return 0;
    const auto entry=Element(pool,index,16);
    if (!Read(entry,pointer) || !ReadAt(entry,8,generation) || generation!=expected ||
        !ReadAt(pointer,0x1c,self) || self!=handle ||
        !Read(g.base+0xc0bebe0,afterPool) || afterPool!=pool ||
        !Read(g.base+0xc0bec00,afterCount) || afterCount!=count ||
        !Read(entry,afterPointer) || afterPointer!=pointer ||
        !ReadAt(entry,8,afterGeneration) || afterGeneration!=generation) return 0;
    return pointer;
}
struct FlameInventoryState {
    uintptr_t manager=0,inventory=0,item=0,config=0;
    uint32_t inventoryHandle=0,itemHandle=0;
    flame_slot::ItemContract contract;
    flame_slot::Slots slots;
};
bool FlameStageFailure(const char* stage) {
    Log(std::string("M2 flame: assignment guard failed at ")+stage+".");
    return false;
}
bool FlameTanksEquipped(uintptr_t actor,uint32_t handle) {
    if (!EngineContextReady() || !ActorStillCurrent(actor,handle)) return false;
    const auto manager=Component(actor,handle,"ItemInventory");
    uintptr_t vtable=0,lookup=0,identityGetter=0,owner=0;
    uint32_t inventoryHandle=0,itemHandle=0,afterHandle=0,category=0,quantity=0;
    uint64_t asset=0;
    if (!Read(manager,vtable) || vtable!=g.base+0x54792d8 ||
        !ReadAt(manager,0x2128,inventoryHandle)) return FlameStageFailure("tank inventory manager/handle");
    const auto inventory=FlameComponentHandle(inventoryHandle);
    if (!Read(inventory,vtable) || vtable!=g.base+0x547a7d8 ||
        !ReadAt(vtable,0x198,lookup) || lookup!=g.base+0x34c0460) return FlameStageFailure("tank vanity inventory type/lookup");
    const auto item=reinterpret_cast<uintptr_t (*)(void*,uint64_t)>(lookup)(
        reinterpret_cast<void*>(inventory),FlameTanksItemAsset);
    // VanityItem has its own embedded identity at +0x100. Check the tiny native
    // getter before the pinned base ItemConfig resolver invokes that virtual.
    static constexpr unsigned char expectedGetter[]={0x48,0x8d,0x81,0x00,0x01,0x00,0x00,0xc3};
    std::array<unsigned char,sizeof(expectedGetter)> bytes{};
    if (!Read(item,vtable) || vtable!=g.base+0x547bbb8 ||
        !ReadAt(vtable,0x120,identityGetter) || identityGetter!=g.base+0x14c5a40 ||
        !ReadBytes(identityGetter,bytes.data(),bytes.size()) ||
        !std::equal(bytes.begin(),bytes.end(),std::begin(expectedGetter)) ||
        !ReadAt(item,0x1c,itemHandle) || FlameComponentHandle(itemHandle)!=item ||
        !ReadAt(item,0xf8,owner) || owner!=inventory ||
        !ReadAt(item,0x88,category) || category!=7 ||
        !ReadAt(item,0x130,asset) || asset!=FlameTanksItemAsset ||
        !ReadAt(item,0x138,quantity) || quantity!=1) return FlameStageFailure("tank item type/identity/owner");
    const auto config=reinterpret_cast<uintptr_t (*)(void*)>(g.base+0x34b1350)(reinterpret_cast<void*>(item));
    uintptr_t name=0,models=0;
    uint32_t configCategory=0,partType=0,modelCount=0;
    unsigned char updateChildren=0;
    uint64_t modelAsset=0;
    const bool verified=FlameDDLPointer(Offset(config,0x30),name) && ReadName(name)=="MJFlamethrowerTanks" &&
        ReadAt(config,0x80,configCategory) && configCategory==21 &&
        ReadAt(config,0x8c,partType) && partType==74 &&
        ReadAt(config,0x95,updateChildren) && updateChildren==1 &&
        FlameDDLPointer(Offset(config,0x98),models) && ReadAt(config,0xa0,modelCount) && modelCount==1 &&
        // Model is an embedded AssetReferenceDef; its AssetPath holds the ID.
        ReadAt(models,0x3c,modelAsset) && modelAsset==FlameTanksModelAsset &&
        ReadAt(manager,0x2128,afterHandle) && afterHandle==inventoryHandle &&
        FlameComponentHandle(inventoryHandle)==inventory && FlameComponentHandle(itemHandle)==item &&
        ActorStillCurrent(actor,handle);
    return verified || FlameStageFailure("tank config/model/freshness");
}
bool ReadOwnedMJWeapon(uintptr_t actor,uint32_t handle,const mj_weapon_switch::Identity& identity,
                       FlameInventoryState& value,bool diagnostic=true) {
    value={};
    const auto fail=[&](const char* stage){return diagnostic ? FlameStageFailure(stage) : false;};
    if (!EngineContextReady() || !ActorStillCurrent(actor,handle) || !FlameSlotCode()) return fail("current actor/context/code");
    value.manager=Component(actor,handle,"ItemInventory");
    uintptr_t vtable=0,lookup=0,identityGetter=0,owner=0,pending=0;
    if (!Read(value.manager,vtable) || vtable!=g.base+0x54792d8 ||
        !ReadAt(value.manager,0x2118,value.inventoryHandle)) return fail("inventory manager/child handle");
    value.inventory=FlameComponentHandle(value.inventoryHandle);
    // This child belongs to the native persistent inventory actor, which can
    // differ from the current hero. The current manager's handle owns the link.
    if (!Read(value.inventory,vtable) || vtable!=g.base+0x5478588 ||
        !ReadAt(vtable,0x198,lookup) || lookup!=g.base+0x34c0460 ||
        !ReadAt(value.inventory,0x2390,pending) || pending) return fail("equipped handle/type or pending operation");
    value.item=reinterpret_cast<uintptr_t (*)(void*,uint64_t)>(lookup)(
        reinterpret_cast<void*>(value.inventory),identity.itemAsset);
    if (!Read(value.item,vtable) || vtable!=g.base+0x547afa8 ||
        !ReadAt(vtable,0x120,identityGetter) || identityGetter!=g.base+0x14bd040 ||
        !ReadAt(value.item,0x1c,value.itemHandle) || FlameComponentHandle(value.itemHandle)!=value.item ||
        !ReadAt(value.item,0xf8,owner) || owner!=value.inventory ||
        !ReadAt(value.item,0x88,value.contract.category) ||
        !ReadAt(value.item,0x178,value.contract.asset) ||
        !ReadAt(value.item,0x180,value.contract.quantity) ||
        !ReadAt(value.item,0x184,value.contract.id) || value.contract.asset!=identity.itemAsset ||
        value.contract.category!=3 || value.contract.quantity!=1 || !value.contract.id)
        return fail("MJ weapon item type/handle/identity/owner");
    // Both pinned getters only resolve preloaded typed assets. In particular,
    // the slot mutator assumes a non-null WeaponConfigGame, so check it first.
    const auto itemConfig=reinterpret_cast<uintptr_t (*)(void*)>(g.base+0x34b1350)(
        reinterpret_cast<void*>(value.item));
    value.config=reinterpret_cast<uintptr_t (*)(void*)>(g.base+0x14bd640)(
        reinterpret_cast<void*>(value.item));
    uintptr_t itemName=0,weaponName=0; uint64_t configAsset=0;
    if (!ReadAt(itemConfig,0x224,value.contract.weaponAsset) ||
        !FlameDDLPointer(Offset(itemConfig,0x30),itemName) || ReadName(itemName)!=identity.name ||
        !ReadAt(value.config,8,configAsset) || configAsset!=identity.weaponAsset ||
        !FlameDDLPointer(Offset(value.config,0x30),weaponName) || ReadName(weaponName)!=identity.name ||
        !ReadAt(value.config,0x118,value.contract.slotType) ||
        !ReadAt(value.config,0x11c,value.contract.anyHeldSlot) ||
        !ReadAt(value.config,0x120,value.contract.heldSlot) || !mj_weapon_switch::Admits(value.contract,identity) ||
        !ReadAt(value.inventory,0x21e0,value.slots.selected)) return fail("typed config/name/held slot");
    for (size_t i=0;i<value.slots.items.size();++i)
        if (!ReadAt(value.inventory,0x21f8+i*0x18,value.slots.items[i])) return fail("held-slot readback");
    uint32_t afterHandle=0;
    const bool stable=flame_slot::Valid(value.slots,value.contract.id) &&
        ReadAt(value.manager,0x2118,afterHandle) && afterHandle==value.inventoryHandle &&
        FlameComponentHandle(value.inventoryHandle)==value.inventory &&
        FlameComponentHandle(value.itemHandle)==value.item && ReadAt(value.inventory,0x2390,pending) && !pending &&
        ActorStillCurrent(actor,handle);
    return stable || fail("inventory capture freshness");
}
bool ReadFlameInventory(uintptr_t actor,uint32_t handle,FlameInventoryState& value) {
    return ReadOwnedMJWeapon(actor,handle,mj_weapon_switch::M2,value);
}
bool MJWeaponSelected(uintptr_t actor,uint32_t handle,const FlameInventoryState& inventory,
                      const mj_weapon_switch::Identity& identity) {
    const auto manager=Component(actor,handle,"HeroWeaponManager");
    uintptr_t vtable=0;
    uint32_t selected=0,count=0;
    if (!Read(manager,vtable) || (vtable!=g.base+0x5430c08 && vtable!=g.base+0x5430f38 &&
        vtable!=g.base+0x5431198) || !ReadAt(manager,0x6c,selected) || selected!=inventory.contract.id ||
        !ReadAt(manager,0x740,count) || count>48) return false;
    for (uint32_t i=0;i<count;++i) {
        const auto entry=Element(Offset(manager,0x200),i,0x1c);
        uint32_t id=0,componentHandle=0;
        if (!ReadAt(entry,0x14,id)) return false;
        if (id!=selected) continue;
        uintptr_t config=0,overrideWeapon=0;
        unsigned char flags=0;
        if (!ReadAt(entry,0x10,componentHandle)) return false;
        const auto weapon=FlameComponentHandle(componentHandle);
        return ReadAt(manager,0x8f1,flags) && (!(flags&0x10) ||
            (ReadAt(manager,0x8d0,overrideWeapon) && overrideWeapon==weapon)) &&
            Read(weapon,vtable) && vtable==g.base+identity.weaponVtable &&
            ReadAt(weapon,0x48,config) && config==inventory.config && ActorStillCurrent(actor,handle);
    }
    return false;
}
bool FlameWeaponSelected(uintptr_t actor,uint32_t handle,const FlameInventoryState& inventory) {
    return MJWeaponSelected(actor,handle,inventory,mj_weapon_switch::M2);
}
struct MJWeaponPair { FlameInventoryState m2,sonic; uint16_t selected=0; };
bool SameMJWeapon(const FlameInventoryState& a,const FlameInventoryState& b) {
    return a.manager==b.manager && a.inventory==b.inventory && a.item==b.item && a.config==b.config &&
        a.inventoryHandle==b.inventoryHandle && a.itemHandle==b.itemHandle && a.contract.id==b.contract.id;
}
bool ReadMJWeaponPair(uintptr_t actor,uint32_t handle,MJWeaponPair& value,bool diagnostic=true) {
    value={};
    if (!ReadOwnedMJWeapon(actor,handle,mj_weapon_switch::M2,value.m2,diagnostic) ||
        !ReadOwnedMJWeapon(actor,handle,mj_weapon_switch::Sonic,value.sonic,diagnostic) ||
        value.m2.manager!=value.sonic.manager || value.m2.inventory!=value.sonic.inventory ||
        value.m2.inventoryHandle!=value.sonic.inventoryHandle || value.m2.contract.id==value.sonic.contract.id ||
        value.m2.slots.items!=value.sonic.slots.items || value.m2.slots.selected!=value.sonic.slots.selected) return false;
    if (MJWeaponSelected(actor,handle,value.m2,mj_weapon_switch::M2)) value.selected=value.m2.contract.id;
    else if (MJWeaponSelected(actor,handle,value.sonic,mj_weapon_switch::Sonic)) value.selected=value.sonic.contract.id;
    return value.selected && mj_weapon_switch::Plan(value.m2.contract,value.sonic.contract,value.m2.slots,
        value.selected,value.selected)!=mj_weapon_switch::Target::None && ActorStillCurrent(actor,handle);
}

// Only the verified engine callback owns these borrowed native identities.
// Retired guns remain under observation until their generation is destroyed;
// this also catches an effect whose asynchronous load finishes after release.
struct FlameLoopBinding {
    uintptr_t actor=0,manager=0,entry=0,weapon=0,config=0,gun=0,conduit=0;
    uint32_t actorHandle=0,managerHandle=0,weaponHandle=0,gunHandle=0,conduitHandle=0,itemId=0;
    ReloadToken world{};
    flame_loop::Gate gate{};
    bool retired=false;
};
std::array<FlameLoopBinding,4> flameLoops{};
bool flameLoopCodeReady=false;
std::atomic<bool> flameBurnReady{false},flameBurnDrained{true};
ReloadToken flameBurnWorld{};
uintptr_t flameBurnActor=0;
uint32_t flameBurnActorHandle=0;
uint64_t flameBurnWorldSerial=0;
bool flameBurnWorldLive=false;
std::atomic<uint64_t> flameGateEpoch{0};
unsigned flameDiscoveryAttempts=0;
constexpr uint64_t FlameLoopAsset=0x885fedab012ac7f4ull;
bool FlameLoopCode() {
    // native-loop-control-contract.json: complete Kill function, including its
    // chained unwind fragments; the scene resolver is reproduced read-only.
    std::array<unsigned char,368> bytes{};
    if (!g.base || !ReadBytes(g.base+0x2ddaf70,bytes.data(),bytes.size())) return false;
    uint64_t digest=14695981039346656037ull;
    for (auto byte:bytes) digest=(digest^byte)*1099511628211ull;
    return digest==0x336bd25c138f0f4bull;
}
bool FlameActorIdentity(uintptr_t actor,uint32_t handle) {
    uintptr_t pool=0; uint32_t count=0,index=0; uint16_t generation=0;
    return handle && (handle>>20) && Read(g.actorPool,pool) && Read(g.actorCount,count) &&
        count<=(1u<<20) && (handle&0xfffff)<count && Element(pool,handle&0xfffff,0xc0)==actor &&
        ReadAt(actor,8,generation) && generation==((handle>>20)&0x7ff) &&
        ReadAt(actor,0xc,index) && (index&0xfffff)==(handle&0xfffff);
}
bool FlameLoopOwner(const FlameLoopBinding& binding) {
    uintptr_t vtable=0,owner=0,config=0; uint32_t user=0; uint64_t asset=0;
    return binding.weapon && FlameActorIdentity(binding.gun,binding.gunHandle) &&
        FlameComponentHandle(binding.weaponHandle)==binding.weapon &&
        Read(binding.weapon,vtable) && vtable==g.base+0x5323378 &&
        ReadAt(binding.weapon,0x10,owner) && owner==binding.gun &&
        ReadAt(binding.weapon,0x48,config) && config==binding.config &&
        ReadAt(config,8,asset) && asset==flame_slot::WeaponAsset &&
        ReadAt(binding.weapon,0x5b0,user) && user==binding.actorHandle &&
        FlameComponentHandle(binding.conduitHandle)==binding.conduit &&
        Read(binding.conduit,vtable) && vtable==g.base+0x5d2f0e8 &&
        ReadAt(binding.conduit,0x10,owner) && owner==binding.gun;
}
bool FlameLoopWorld(const ReloadToken& token) {
    uintptr_t manager=0,level=0,mode=0,vtable=0; uint32_t kernelHandle=0;
    int stateValue=0,pending=0; unsigned char ready=0; uint16_t sequence=0; uint64_t asset=0;
    return Read(g.base+0xc2266a8,manager) && manager==token.manager &&
        Read(manager,vtable) && vtable==token.managerVtable &&
        ReadAt(manager,0x10,level) && level==token.loadedLevel &&
        ReadAt(manager,0x430628,stateValue) && stateValue==22 &&
        ReadAt(manager,0x199fc0,pending) && pending==0 &&
        ReadAt(manager,0x319f68,pending) && pending==0 &&
        ReadAt(manager,0x430665,ready) && ready==1 &&
        Read(g.base+0xc21d330,mode) && mode==token.mode &&
        ReadAt(mode,0x10,stateValue) && stateValue==9 &&
        Read(g.base+0xc21d630,kernelHandle) && kernelHandle==token.kernelHandle &&
        FlameComponentHandle(kernelHandle)==token.kernel &&
        ReadAt(token.kernel,0x58,asset) && asset==token.levelAsset &&
        ReadAt(token.kernel,0x62f0,sequence) && sequence==token.kernelSequence &&
        ReadAt(token.kernel,0x6308,ready) && ready==3;
}
bool FlameLoopSelected(const FlameLoopBinding& binding) {
    uintptr_t vtable=0,owner=0,overrideWeapon=0; uint32_t selected=0,id=0,weaponHandle=0;
    unsigned char flags=0;
    return ActorStillCurrent(binding.actor,binding.actorHandle) &&
        FlameComponentHandle(binding.managerHandle)==binding.manager &&
        Read(binding.manager,vtable) && (vtable==g.base+0x5430c08 || vtable==g.base+0x5430f38 || vtable==g.base+0x5431198) &&
        ReadAt(binding.manager,0x10,owner) && owner==binding.actor &&
        ReadAt(binding.manager,0x6c,selected) && selected==binding.itemId &&
        ReadAt(binding.entry,0x14,id) && id==selected &&
        ReadAt(binding.entry,0x10,weaponHandle) && weaponHandle==binding.weaponHandle &&
        ReadAt(binding.manager,0x8f1,flags) && (!(flags&0x10) ||
            (ReadAt(binding.manager,0x8d0,overrideWeapon) && overrideWeapon==binding.weapon));
}
uintptr_t BurnActor(uint32_t handle) {
    uintptr_t pool=0,afterPool=0; uint32_t count=0,afterCount=0;
    if (!handle || (handle&0x80000000u) || !Read(g.actorPool,pool) || !Read(g.actorCount,count) ||
        count>(1u<<20) || (handle&0xfffff)>=count) return 0;
    const auto actor=Element(pool,handle&0xfffff,0xc0);
    if (!FlameActorIdentity(actor,handle) || !Read(g.actorPool,afterPool) || afterPool!=pool ||
        !Read(g.actorCount,afterCount) || afterCount!=count) return 0;
    return actor;
}
uintptr_t BurnFind(uintptr_t actor,uintptr_t descriptor) {
    // A verified empty lookup is0; unreadable storage is explicitly different.
    // Only the four exact native component types used by this feature qualify.
    constexpr auto invalid=~uintptr_t{0};
    if (descriptor!=g.base+0xb87ca20 && descriptor!=g.base+0xb877d70 &&
        descriptor!=g.base+0xb6a1e80 && descriptor!=g.base+0xad6ec60) return invalid;
    // Native150b98d tests the complete capacity WORD. Reading one byte would
    // misclassify capacities256 and512 as the small-container layout.
    uint16_t generation=0,large=0,afterLarge=0; uint32_t index=0;
    if (!ReadAt(actor,8,generation) || !generation || generation>0x7ff ||
        !ReadAt(actor,0xc,index) || !ReadAt(actor,0x88,large)) return invalid;
    const auto handle=(index&0xfffff)|(static_cast<uint32_t>(generation)<<20);
    if (BurnActor(handle)!=actor) return invalid;
    uintptr_t entries=0,afterEntries=0,result=0; uint16_t count=0,afterCount=0;
    const auto container=Offset(actor,large ? 0x80 : 0x58);
    const size_t entriesOffset=large ? 0 : 0x10,countOffset=large ? 8 : 0x18;
    if (!ReadAt(container,entriesOffset,entries) || !ReadAt(container,countOffset,count) ||
        (large && (!count || count>32768 || (count&(count-1)))) || (!large && count>4096) ||
        (count && !entries)) return invalid;
    size_t bucket=large ? (descriptor>>4)&(count-1) : 0;
    for (size_t visited=0;visited<count;++visited) {
        ComponentEntry entry{};
        if (!Read(Element(entries,bucket,sizeof(ComponentEntry)),entry)) return invalid;
        if (entry.info==descriptor) {result=entry.component;break;}
        if (large && !entry.info) break;
        bucket=large ? (bucket+1)&(count-1) : bucket+1;
    }
    uintptr_t owner=0;
    if (!ReadAt(container,entriesOffset,afterEntries) || entries!=afterEntries ||
        !ReadAt(container,countOffset,afterCount) || count!=afterCount ||
        !ReadAt(actor,0x88,afterLarge) || afterLarge!=large || BurnActor(handle)!=actor ||
        (result && (!ReadAt(result,0x10,owner) || owner!=actor))) return invalid;
    return result;
}
bool BurnSourceValid(const flame_burn::Source& source) {
    if (!EngineContextReady() || !flameBurnWorldLive || source.world!=flameBurnWorldSerial ||
        source.actor!=flameBurnActor || source.actorHandle!=flameBurnActorHandle ||
        !FlameLoopWorld(flameBurnWorld) || !ActorStillCurrent(source.actor,source.actorHandle)) return false;
    for (const auto& binding:flameLoops) {
        if (!binding.retired && binding.actor==source.actor && binding.actorHandle==source.actorHandle &&
            binding.weapon==source.weapon && binding.weaponHandle==source.weaponHandle &&
            binding.config==source.config && binding.itemId==source.item &&
            FlameLoopOwner(binding) && FlameLoopWorld(binding.world)) return true;
    }
    return false;
}
void TickFlameBurn() {
    if (!flameBurnReady.load(std::memory_order_acquire)) return;
    if (shutdownReset.load() || stopping.load()) {
        flame_burn::RequestStop();
        flameBurnDrained.store(flame_burn::DrainOnEngineThread(),std::memory_order_release);
        return;
    }
    if (flameBurnWorldLive && (!FlameLoopWorld(flameBurnWorld) ||
        !ActorStillCurrent(flameBurnActor,flameBurnActorHandle))) {
        flameBurnWorldLive=false;
        ++flameBurnWorldSerial;
    }
    flame_burn::Source source{};
    for (const auto& binding:flameLoops) {
        if (!binding.weapon || binding.retired || !FlameLoopOwner(binding) ||
            !ActorStillCurrent(binding.actor,binding.actorHandle) || !FlameLoopWorld(binding.world) ||
            !binding.itemId || binding.itemId>65535) continue;
        if (!flameBurnWorldLive) {
            flameBurnWorld=binding.world;flameBurnActor=binding.actor;
            flameBurnActorHandle=binding.actorHandle;flameBurnWorldLive=true;
            if (!++flameBurnWorldSerial) ++flameBurnWorldSerial;
        }
        source={flameBurnWorldSerial,binding.actor,binding.weapon,binding.config,
            binding.actorHandle,binding.weaponHandle,0,static_cast<uint16_t>(binding.itemId)};
        // WeaponGame::GetNameHash, not the unrelated mapped Fire input hash.
        if (!ReadAt(binding.weapon,0x610,source.nameHash) || !source.nameHash) source={};
        break;
    }
    // A committed M2 projectile may hit after holstering. Exact damage-event
    // source checks distinguish it from subsequent Sonic shots and native DoT.
    flame_burn::EngineTick(source.world ? &source : nullptr,flameBurnWorldLive ? flameBurnWorldSerial : 0);
    const auto observed=flame_burn::Status();
    flameBurnDrained.store(!observed.owned && !observed.inFlight,std::memory_order_release);
    static uint64_t nextLog=0;
    static flame_burn::Counters previous{};
    static const char* previousFailure=nullptr;
    const auto now=GetTickCount64();
    if (now<nextLog) return;
    nextLog=now+1000;
    const auto failure=flame_burn::LastFailure();
    if (observed.accepted!=previous.accepted || observed.applied!=previous.applied ||
        observed.dropped!=previous.dropped || observed.owned!=previous.owned || failure!=previousFailure) {
        Log("M2 burn: accepted="+std::to_string(observed.accepted)+" applied="+std::to_string(observed.applied)+
            " dropped="+std::to_string(observed.dropped)+" created="+std::to_string(observed.created)+
            " retired="+std::to_string(observed.retired)+" owned="+std::to_string(observed.owned)+
            " status="+failure);
        previous=observed;previousFailure=failure;
    }
}
uintptr_t FlameScene(uint32_t handle) {
    uintptr_t pool=0,scene=0,afterPool=0,afterScene=0;
    uint32_t count=0,afterCount=0; unsigned char generation=0,type=0,afterGeneration=0;
    const auto index=handle&0xffffff; const auto expected=handle>>24;
    if (!expected || !Read(g.base+0xc0bdbc8,pool) || !Read(g.base+0xc0bdbe4,count) ||
        count>(1u<<24) || index>=count) return 0;
    const auto entry=Element(pool,index,16);
    if (!Read(entry,scene) || !ReadAt(entry,8,generation) || generation!=expected ||
        (scene>=pool && scene<Element(pool,count,16)) ||
        !ReadAt(scene,0x5f,type) || type!=5 ||
        !Read(g.base+0xc0bdbc8,afterPool) || afterPool!=pool ||
        !Read(g.base+0xc0bdbe4,afterCount) || afterCount!=count ||
        !Read(entry,afterScene) || afterScene!=scene ||
        !ReadAt(entry,8,afterGeneration) || afterGeneration!=generation) return 0;
    return scene;
}
unsigned StopFlameLoop(const FlameLoopBinding& binding) {
    unsigned killed=0;
    if (!EngineContextReady() || !flameLoopCodeReady || !FlameLoopOwner(binding)) return killed;
    uintptr_t entries=0; uint32_t count=0;
    if (!ReadAt(binding.conduit,0x240,entries) || !ReadAt(binding.conduit,0x248,count) || count>64) return killed;
    for (uint32_t i=0;i<count;++i) {
        const auto slot=Element(entries,i,24); uint32_t handle=0,afterHandle=0,afterCount=0;
        uintptr_t resource=0,afterResource=0,afterEntries=0; uint64_t asset=0,flags=0;
        if (!ReadAt(slot,8,handle)) return killed;
        const auto scene=FlameScene(handle);
        if (!scene || !ReadAt(scene,0x128,resource) || !ReadAt(resource,8,asset) || asset!=FlameLoopAsset ||
            !ReadAt(scene,0x198,flags) || (flags&0x200000000ull)) continue;
        // Revalidate the owning gun, list slot and scene generation immediately
        // before calling the same native destructor request as kVFX Start=-1.
        if (!FlameLoopOwner(binding) || !ReadAt(binding.conduit,0x240,afterEntries) || afterEntries!=entries ||
            !ReadAt(binding.conduit,0x248,afterCount) || afterCount!=count ||
            !ReadAt(slot,8,afterHandle) || afterHandle!=handle || FlameScene(handle)!=scene ||
            !ReadAt(scene,0x128,afterResource) || afterResource!=resource ||
            !ReadAt(resource,8,asset) || asset!=FlameLoopAsset || !FlameLoopCode()) return killed;
        reinterpret_cast<void (*)(void*)>(g.base+0x2ddaf70)(reinterpret_cast<void*>(scene));
        ++killed;
    }
    return killed;
}
FlameLoopBinding* FlameLoopDestination(const FlameLoopBinding& binding) {
    // Reuse a retired identity before any free slot, otherwise its old monitor
    // could keep killing the newly rearmed copy of the very same native gun.
    for (auto& cached:flameLoops)
        if (cached.weapon==binding.weapon && cached.weaponHandle==binding.weaponHandle) return &cached;
    for (auto& cached:flameLoops)
        if (!cached.weapon || !FlameLoopOwner(cached)) return &cached;
    return nullptr;
}
bool ArmFlameLoop(uintptr_t actor,uint32_t handle,const ReloadState& world,const FlameInventoryState& inventory) {
    if (!EngineContextReady() || !flameLoopCodeReady || !world.ready || !FlameLoopCode() ||
        !FlameWeaponSelected(actor,handle,inventory)) return false;
    FlameLoopBinding binding{};
    binding.actor=actor; binding.actorHandle=handle; binding.config=inventory.config;
    binding.itemId=inventory.contract.id; binding.world=world.token;
    binding.manager=Component(actor,handle,"HeroWeaponManager");
    uint32_t count=0;
    if (!ReadAt(binding.manager,0x1c,binding.managerHandle) || !ReadAt(binding.manager,0x740,count) || count>48) return false;
    for (uint32_t i=0;i<count;++i) {
        const auto entry=Element(Offset(binding.manager,0x200),i,0x1c); uint32_t id=0;
        if (!ReadAt(entry,0x14,id)) return false;
        if (id!=binding.itemId) continue;
        binding.entry=entry;
        if (!ReadAt(entry,0x10,binding.weaponHandle)) return false;
        binding.weapon=FlameComponentHandle(binding.weaponHandle);
        break;
    }
    uint16_t generation=0; uint32_t index=0; unsigned char large=0;
    if (!binding.weapon || !ReadAt(binding.weapon,0x10,binding.gun) ||
        !ReadAt(binding.gun,8,generation) || !ReadAt(binding.gun,0xc,index)) return false;
    binding.gunHandle=((generation&0x7ffu)<<20)|(index&0xfffff);
    if (!FlameActorIdentity(binding.gun,binding.gunHandle) || !ReadAt(binding.gun,0x88,large)) return false;
    binding.conduit=large ? LargeComponent(binding.gun,g.base+0xbae5410) : SmallComponent(binding.gun,g.base+0xbae5410);
    if (!ReadAt(binding.conduit,0x1c,binding.conduitHandle) || !FlameLoopOwner(binding) ||
        !FlameLoopSelected(binding) || !FlameLoopWorld(binding.world)) return false;
    for (auto& cached:flameLoops) {
        if (cached.weapon==binding.weapon && cached.weaponHandle==binding.weaponHandle &&
            cached.actorHandle==handle && !cached.retired) return true;
    }
    if (auto* cached=FlameLoopDestination(binding)) {
        uint32_t frame=0; double time=0;
        if (!ReadAt(binding.weapon,0x1ac,frame) || !ReadAt(binding.weapon,0x198,time) || !std::isfinite(time)) return false;
        binding.gate.Observe(false,frame,time,GetTickCount64()); *cached=binding;
        Log("M2 loop: exact selected weapon armed, handle="+std::to_string(binding.weaponHandle));
        return true;
    }
    Log("M2 loop: monitor capacity occupied by live native guns; new assignment not armed.");
    return false;
}
bool ReserveFlameLoopSlot(uintptr_t actor,uint32_t handle) {
    for (auto& cached:flameLoops) {
        if (!cached.weapon || !FlameLoopOwner(cached)) {
            cached={}; cached.actor=actor; cached.actorHandle=handle;
            return true;
        }
        uint32_t id=0,component=0;
        if (cached.actor==actor && cached.actorHandle==handle &&
            ReadAt(cached.entry,0x14,id) && id==cached.itemId &&
            ReadAt(cached.entry,0x10,component) && component==cached.weaponHandle) return true;
    }
    return false;
}
void DiscoverFlameLoop(uintptr_t actor,uint32_t handle,bool visible) {
    // Cache the manager once per hero. Its cheap selected-slot observation
    // covers arbitrarily late weapon streaming, without full hidden snapshots.
    static uint32_t prior=0;
    static uintptr_t manager=0; static uint32_t managerHandle=0;
    if (prior!=handle) { prior=handle; manager=0; managerHandle=0; flameDiscoveryAttempts=30; }
    if (!actor || !handle || !EngineContextReady() || !flameLoopCodeReady || shutdownReset) return;
    uintptr_t owner=0,vtable=0;
    if (!manager || FlameComponentHandle(managerHandle)!=manager) {
        manager=0; managerHandle=0;
        if (!visible && !flameDiscoveryAttempts) return;
        if (flameDiscoveryAttempts) --flameDiscoveryAttempts;
        manager=Component(actor,handle,"HeroWeaponManager");
        if (!ReadAt(manager,0x1c,managerHandle)) { manager=0; return; }
    }
    uint32_t selected=0,count=0;
    if (!Read(manager,vtable) || (vtable!=g.base+0x5430c08 && vtable!=g.base+0x5430f38 && vtable!=g.base+0x5431198) ||
        !ReadAt(manager,0x10,owner) || owner!=actor || !ReadAt(manager,0x6c,selected) ||
        !ReadAt(manager,0x740,count) || count>48) return;
    bool candidate=false; uintptr_t candidateWeapon=0; uint32_t candidateHandle=0;
    for (uint32_t i=0;i<count;++i) {
        const auto entry=Element(Offset(manager,0x200),i,0x1c); uint32_t id=0,component=0;
        uintptr_t weapon=0,vtable=0,config=0; uint64_t asset=0;
        if (!ReadAt(entry,0x14,id) || id!=selected || !ReadAt(entry,0x10,component)) continue;
        weapon=FlameComponentHandle(component);
        candidate=Read(weapon,vtable) && vtable==g.base+0x5323378 && ReadAt(weapon,0x48,config) &&
            ReadAt(config,8,asset) && asset==flame_slot::WeaponAsset;
        candidateWeapon=weapon; candidateHandle=component;
        break;
    }
    if (!candidate) return;
    for (const auto& cached:flameLoops)
        if (cached.weapon==candidateWeapon && cached.weaponHandle==candidateHandle && !cached.retired) return;
    uintptr_t system=0; FlameInventoryState inventory;
    if (!FlameLoadoutReady(actor,handle,system) || !ReadFlameInventory(actor,handle,inventory)) return;
    const auto world=ReadReloadState();
    (void)ArmFlameLoop(actor,handle,world,inventory);
}
void TickFlameLoops() {
    bool active=false; unsigned killed=0;
    static uint64_t priorEpoch=0;
    const auto epoch=flameGateEpoch.load(); const bool visibilityChanged=epoch!=priorEpoch; priorEpoch=epoch;
    const bool permitted=!stopping.load() && !shutdownReset.load() && !MenuVisible() &&
        !visibilityChanged && targetWindow && GetForegroundWindow()==targetWindow;
    const auto now=GetTickCount64();
    for (auto& binding:flameLoops) {
        if (!binding.weapon) continue;
        if (!FlameLoopOwner(binding)) { binding={}; flameDiscoveryAttempts=30; continue; }
        uint32_t frame=0; double time=0;
        const bool sameWorld=FlameLoopWorld(binding.world) && ActorStillCurrent(binding.actor,binding.actorHandle);
        if (!sameWorld && !binding.retired) { binding.retired=true; flameDiscoveryAttempts=30; }
        const bool selected=!binding.retired && sameWorld && FlameLoopSelected(binding);
        const bool sampled=ReadAt(binding.weapon,0x1ac,frame) && ReadAt(binding.weapon,0x198,time);
        const bool wasActive=binding.gate.active;
        const bool playing=binding.gate.Observe(permitted && selected && sampled,frame,time,now);
        if (!playing) killed+=StopFlameLoop(binding);
        if (playing!=wasActive) Log(std::string("M2 loop: ")+(playing?"active":"stopped")+
            " native_shot_frame="+std::to_string(frame));
        active|=playing;
    }
    if (killed) Log("M2 loop: private_fx_kill_count="+std::to_string(killed));
    flame_audio::Update(active);
    static const char* lastAudio=nullptr; static unsigned lastError=0;
    const auto audio=flame_audio::Status(); const auto error=flame_audio::LastError();
    if (audio!=lastAudio || error!=lastError) {
        lastAudio=audio; lastError=error;
        Log(std::string("M2 audio: ")+audio+" error="+std::to_string(error));
    }
}
bool AssignFlameHeldSlot(uintptr_t actor,uint32_t handle,const ReloadState& admitted,std::wstring& status) {
    status=L"M2 inventory request completed, but held-slot assignment could not be verified.";
    if (!EngineContextReady() || !FlameSlotCode()) return FlameStageFailure("slot code/context");
    FlameInventoryState before;
    if (!ReadFlameInventory(actor,handle,before)) return false;
    uintptr_t system=0;
    const auto currentWorld=ReadReloadState();
    if (!currentWorld.ready || !SameReloadContext(admitted.token,currentWorld.token) ||
        !FlameLoadoutReady(actor,handle,system)) return FlameStageFailure("world/character freshness");
    FlameInventoryState final;
    if (!ReadFlameInventory(actor,handle,final) || final.manager!=before.manager ||
        final.inventory!=before.inventory || final.item!=before.item || final.config!=before.config ||
        final.inventoryHandle!=before.inventoryHandle || final.itemHandle!=before.itemHandle ||
        final.contract.id!=before.contract.id || final.slots.items!=before.slots.items ||
        final.slots.selected!=before.slots.selected || !EngineContextReady()) return FlameStageFailure("final assignment freshness");
    // Weapon AutoEquip tries the configured slot with replacement and selection
    // disabled. Explicitly replace/select this one slot when Sonic occupies it.
    // PostProcessEquip would clear all eleven slots and is avoided.
    flame_slot::Options options;
    const bool accepted=reinterpret_cast<bool (*)(void*,void*,const flame_slot::Options*)>(g.base+0x14aeda0)(
        reinterpret_cast<void*>(before.inventory),reinterpret_cast<void*>(before.item),&options);
    FlameInventoryState after;
    const auto afterWorld=ReadReloadState();
    const bool observed=afterWorld.ready && SameReloadContext(admitted.token,afterWorld.token) &&
        ReadFlameInventory(actor,handle,after) && after.manager==before.manager && after.inventory==before.inventory &&
        after.item==before.item && after.config==before.config && after.contract.id==before.contract.id &&
        after.inventoryHandle==before.inventoryHandle && after.itemHandle==before.itemHandle;
    const bool assigned=observed && flame_slot::Preserved(before.slots,after.slots,before.contract.id);
    const bool selected=assigned && FlameWeaponSelected(actor,handle,after);
    Log("M2 flame: targeted slot assignment native_return="+std::to_string(accepted)+
        " item_id="+std::to_string(before.contract.id)+" held_slot=2 replaced_item_id="+
        std::to_string(before.slots.items[flame_slot::HeldSlot])+" assigned="+std::to_string(assigned)+
        " selected_weapon_verified="+std::to_string(selected));
    if (!assigned) {
        status=L"M2 is owned, but native held-slot assignment was not confirmed; mission scripts may restrict equipment.";
        return false;
    }
    status=selected ? L"M2 flamethrower assigned and selected. Aim and hold Fire." :
        L"M2 flamethrower assigned to the gun slot; held weapon not yet confirmed.";
    return true;
}
bool EquipFlamethrower(uintptr_t actor,uint32_t handle,const ReloadState& captured,std::wstring& status) {
    status=L"M2 flamethrower requires the original Mary Jane and a ready loaded mission or world.";
    if (!EngineContextReady() || !captured.ready) return false;
    if (!flameLoopCodeReady || !FlameLoopCode()) {
        status=L"M2 flame loop cleanup is unavailable on this build; equipment request refused.";
        return false;
    }
    if (!ReserveFlameLoopSlot(actor,handle)) {
        status=L"M2 flame cleanup monitors are occupied by live guns; equipment request refused.";
        return false;
    }
    const auto world=ReadReloadState();
    uintptr_t system=0;
    if (!world.ready || !SameReloadContext(world.token,captured.token) || !FlameLoadoutReady(actor,handle,system) ||
        !FlameSlotCode()) return false;
    static constexpr char path[]="configs/hero/mj_flame/mj_flame_loadout.config";
    // Native typed lookup only resolves preloaded registered assets; it does
    // not load or grant them. Reject missing/changed contents before applying.
    auto resolve=reinterpret_cast<uintptr_t (*)(void*,const char*)>(g.base+0x34bdde0);
    const auto loadout=resolve(reinterpret_cast<void*>(system),path);
    if (!FlameLoadoutShape(loadout)) {
        status=L"M2 flamethrower package is missing, not ready, or has unexpected loadout contents.";
        return false;
    }
    uintptr_t currentSystem=0;
    const auto finalWorld=ReadReloadState();
    if (!EngineContextReady() || !finalWorld.ready || !SameReloadContext(world.token,finalWorld.token) ||
        !FlameLoadoutReady(actor,handle,currentSystem) || currentSystem!=system || !FlameLoadoutShape(loadout)) return false;
    reinterpret_cast<void (*)(void*,void*,const char*,uint32_t)>(g.base+0x14c72c0)(
        reinterpret_cast<void*>(system),reinterpret_cast<void*>(actor),path,0);
    Log("M2 flame: native additive gun/backpack request dispatched; checking held-slot and accessory inventory.");
    const bool assigned=AssignFlameHeldSlot(actor,handle,finalWorld,status);
    const auto afterWorld=ReadReloadState();
    const bool tanks=afterWorld.ready && SameReloadContext(finalWorld.token,afterWorld.token) &&
        FlameTanksEquipped(actor,handle);
    Log("M2 flame: tank accessory equipped_inventory_verified="+std::to_string(tanks)+
        "; visible model requires native skin streaming.");
    status+=tanks ? L" Tank backpack equipped." : L" Tank backpack equip not yet confirmed.";
    if (assigned) {
        flameDiscoveryAttempts=30;
        FlameInventoryState inventory;
        const bool armed=afterWorld.ready && SameReloadContext(finalWorld.token,afterWorld.token) &&
            ReadFlameInventory(actor,handle,inventory) && ArmFlameLoop(actor,handle,afterWorld,inventory);
        if (!armed) status+=L" Flame feedback is waiting for the native held weapon to finish loading.";
    }
    return assigned;
}
bool SwitchMJWeapon(uintptr_t actor,uint32_t handle,const ReloadState& captured,
                    uint16_t capturedItem,std::wstring& status) {
    status=L"Switching requires Mary Jane, a ready world, and owned M2 and Sonic Shotgun weapons.";
    if (!EngineContextReady() || !captured.ready || !capturedItem || !flameLoopCodeReady ||
        !FlameLoopCode() || !FlameSlotCode()) return false;
    uintptr_t system=0;
    const auto world=ReadReloadState();
    if (!world.ready || !SameReloadContext(captured.token,world.token) ||
        !FlameLoadoutReady(actor,handle,system)) return false;
    MJWeaponPair before;
    if (!ReadMJWeaponPair(actor,handle,before)) return false;
    const auto target=mj_weapon_switch::Plan(before.m2.contract,before.sonic.contract,
        before.m2.slots,before.selected,capturedItem);
    if (target==mj_weapon_switch::Target::None) {
        status=L"The selected weapon changed after this command was chosen; switch discarded.";
        return false;
    }
    const bool toM2=target==mj_weapon_switch::Target::M2;
    if (toM2 && !ReserveFlameLoopSlot(actor,handle)) {
        status=L"M2 flame cleanup monitors are occupied; switch refused.";return false;
    }
    // Stop this hero's private feedback before selecting another held weapon.
    // Keep the identity monitor alive to catch late asynchronous FX creation.
    unsigned killed=0;
    for (auto& binding:flameLoops) {
        if (binding.actor!=actor || binding.actorHandle!=handle) continue;
        binding.gate.Observe(false,0,0,GetTickCount64());
        killed+=StopFlameLoop(binding);
    }
    flame_audio::Update(false);
    // Cleanup enters the native FX system, so recapture the world and both
    // items after it, immediately before the native slot mutator.
    MJWeaponPair final;
    const auto finalWorld=ReadReloadState();
    if (!finalWorld.ready || !SameReloadContext(world.token,finalWorld.token) ||
        !ReadMJWeaponPair(actor,handle,final) || !SameMJWeapon(before.m2,final.m2) ||
        !SameMJWeapon(before.sonic,final.sonic) || before.m2.slots.items!=final.m2.slots.items ||
        before.m2.slots.selected!=final.m2.slots.selected ||
        mj_weapon_switch::Plan(final.m2.contract,final.sonic.contract,final.m2.slots,final.selected,capturedItem)!=target ||
        !ActorStillCurrent(actor,handle) || !EngineContextReady()) {
        status=L"Weapon or world changed before assignment; switch discarded.";return false;
    }
    const auto& destination=toM2 ? final.m2 : final.sonic;
    flame_slot::Options options;
    // Pinned native replacement/select changes only the existing held slot.
    // There is deliberately no loadout application, grant or inventory clear.
    const bool accepted=reinterpret_cast<bool (*)(void*,void*,const flame_slot::Options*)>(g.base+0x14aeda0)(
        reinterpret_cast<void*>(destination.inventory),reinterpret_cast<void*>(destination.item),&options);
    FlameInventoryState afterM2,afterSonic;
    const auto afterWorld=ReadReloadState();
    const bool observed=afterWorld.ready && SameReloadContext(world.token,afterWorld.token) &&
        ReadOwnedMJWeapon(actor,handle,mj_weapon_switch::M2,afterM2) &&
        ReadOwnedMJWeapon(actor,handle,mj_weapon_switch::Sonic,afterSonic) &&
        SameMJWeapon(before.m2,afterM2) && SameMJWeapon(before.sonic,afterSonic) &&
        afterM2.slots.items==afterSonic.slots.items && afterM2.slots.selected==afterSonic.slots.selected;
    const bool assigned=observed && mj_weapon_switch::Preserved(before.m2.contract,before.sonic.contract,
        afterM2.contract,afterSonic.contract,before.m2.slots,afterM2.slots,target);
    const bool selected=assigned && MJWeaponSelected(actor,handle,toM2 ? afterM2 : afterSonic,
        toM2 ? mj_weapon_switch::M2 : mj_weapon_switch::Sonic);
    Log("MJ weapon switch: target="+std::string(toM2 ? "M2" : "SonicShotgun")+
        " from_item="+std::to_string(before.selected)+" to_item="+std::to_string(destination.contract.id)+
        " native_return="+std::to_string(accepted)+" inventory_preserved="+std::to_string(assigned)+
        " selected_weapon_verified="+std::to_string(selected)+" private_fx_kill_count="+std::to_string(killed));
    if (!assigned) {
        status=L"Native weapon switch was not confirmed; mission scripts may restrict equipment.";return false;
    }
    status=selected ? (toM2 ? L"Switched to M2 flamethrower." : L"Switched to Sonic Shotgun.") :
        L"Weapon assigned to the gun slot; waiting for the native held weapon.";
    if (toM2) {
        flameDiscoveryAttempts=30;
        if (!selected || !ArmFlameLoop(actor,handle,afterWorld,afterM2))
            status+=L" Flame feedback is waiting for the native held weapon to finish loading.";
    }
    return true;
}
void RestoreHealth(uintptr_t component, uint32_t handle) {
    autoHeal = false;
    if (healthEdit.component && !EngineContextReady()) {
        // Restoring a component maximum may need its native health setter to
        // clamp/publish current health. Global flags and windows restore below
        // independently; do not enter that setter from the window thread.
        RejectEngineOperation("RestoreHealth");
        return;
    }
    float currentMaximum = 0, health = 0;
    if (healthEdit.component == component && healthEdit.handle == handle &&
        Read(component + 0xa0, currentMaximum) && Read(component + 0xd0, health) &&
        PlausibleHealth(currentMaximum, health) && std::fabs(currentMaximum - healthEdit.applied) < .01f) {
        // Only restore a value still owned by this mod; preserve later edits by others.
        if (Write(component + 0xa0, healthEdit.original))
            SetCurrentHealth(component, std::min(health, healthEdit.original));
    }
    healthEdit = {};
}
void RestoreHealthFlags() {
    for (int i = 0; i < 4; ++i) {
        unsigned char current = 0;
        if (healthFlagsEdited[i] && Read(g.healthFlags + i, current) &&
            (current != 0) == healthFlagsApplied[i]) {
            const unsigned char original = healthFlagsOriginal[i] ? 1 : 0;
            Write(g.healthFlags + i, original);
        }
        healthFlagsEdited[i] = false;
    }
}
void SetHealthFlag(int index, bool value) {
    unsigned char current = 0;
    if (!g.healthFlags || index < 0 || index >= 4 || !Read(g.healthFlags + index, current) || current > 1) {
        Status(L"Health setting is unavailable on this build."); return;
    }
    if (!healthFlagsEdited[index]) healthFlagsOriginal[index] = current != 0;
    const unsigned char requested = value ? 1 : 0;
    if (Write(g.healthFlags + index, requested)) {
        healthFlagsEdited[index] = true;
        healthFlagsApplied[index] = value;
        Status(L"Engine health setting updated.");
    } else Status(L"Health setting write rejected.");
}
void RestoreWindow() {
    if (!changedWindow || !IsWindow(targetWindow)) return;
    SetWindowLongPtrW(targetWindow, GWL_STYLE, oldStyle);
    SetWindowLongPtrW(targetWindow, GWL_EXSTYLE, oldExStyle);
    SetWindowPlacement(targetWindow, &oldPlacement);
    SetWindowPos(targetWindow, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    changedWindow = false;
}
void Borderless() {
    if (!IsWindow(targetWindow) || changedWindow) return;
    oldStyle = GetWindowLongPtrW(targetWindow, GWL_STYLE);
    oldExStyle = GetWindowLongPtrW(targetWindow, GWL_EXSTYLE);
    oldPlacement.length = sizeof(oldPlacement);
    if (!GetWindowPlacement(targetWindow, &oldPlacement)) return;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    SetWindowLongPtrW(targetWindow, GWL_STYLE, (oldStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    SetWindowPos(targetWindow, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top,
        monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    changedWindow = true;
}

void ExportDiagnostics() {
    const auto s = GetSnapshot();
    const auto capture = input_capture::Observe();
    const auto photo = photo_gate::Observe();
    const auto path = outputPath / L"diagnostics.json";
    std::ofstream f(path, std::ios::trunc);
    if (!f) { Status(L"Could not write diagnostics.json."); return; }
    f << std::boolalpha << std::setprecision(7)
      << "{\n  \"menu_version\": \"2.10.0\",\n  \"preview\": " << previewMode
      << ",\n  \"input_capture_installed\": " << capture.installed
      << ",\n  \"input_capture_imports\": " << capture.patchedImports
      << ",\n  \"input_capture_sony_sdk_installed\": " << capture.sonySdkInstalled
      << ",\n  \"input_capture_sony_reports\": " << capture.sonyReports
      << ",\n  \"input_capture_raw_packets\": " << capture.rawPackets
      << ",\n  \"input_capture_xinput_states\": " << capture.xinputStates
      << ",\n  \"input_capture_keyboard_queries\": " << capture.keyboardQueries
      << ",\n  \"input_capture_window_messages\": " << capture.windowMessages
      << ",\n  \"input_capture_unhandled_reports\": " << capture.unsupportedReports
      << ",\n  \"photo_entry_gate_installed\": " << photo.installed
      << ",\n  \"photo_entry_gate_protection_restored\": " << photo.protectionRestored
      << ",\n  \"photo_entry_gate_blocked\": " << photo.blocked
      << ",\n  \"photo_entry_gate_forwarded\": " << photo.forwarded
      << ",\n  \"photo_state_known\": " << photo.stateKnown
      << ",\n  \"photo_active\": " << photo.photoActive
      << ",\n  \"photo_native_state\": " << photo.nativeState
      << ",\n  \"supported_build\": " << s.supported
      << ",\n  \"hero_available\": " << s.heroReady
      << ",\n  \"health_available\": " << s.healthReady
      << ",\n  \"teleport_available\": " << s.teleportReady
      << ",\n  \"mj_flamethrower_equip_available\": " << s.mjFlamethrowerReady
      << ",\n  \"mj_weapon_switch_available\": " << s.mjWeaponSwitchReady
      << ",\n  \"reload_available\": " << s.reload.ready
      << ",\n  \"exit_frontend_available\": " << s.reload.frontendReady
      << ",\n  \"primary_objective_completion_available\": " << s.primary.ready
      << ",\n  \"reload_checkpoint_choices\": " << (s.reload.checkpoints ? s.reload.checkpoints->size() : 0)
      << ",\n  \"current_checkpoint_key\": " << s.reload.token.currentCheckpoint
      << ",\n  \"auto_refill\": " << s.autoHeal
      << ",\n  \"heroes_do_not_take_damage\": " << s.heroNoDamage
      << ",\n  \"heroes_do_not_die\": " << s.heroNoDeath
      << ",\n  \"enemy_bots_do_not_die\": " << s.enemyNoDeath
      << ",\n  \"enemy_bots_do_not_take_damage\": " << s.enemyNoDamage
      << ",\n  \"health\": " << s.health << ",\n  \"max_health\": " << s.maxHealth
      << ",\n  \"position\": [" << s.x << ", " << s.y << ", " << s.z << "]"
      << ",\n  \"working_set_mib\": " << s.memoryMiB
      << ",\n  \"process_cpu_percent\": " << s.cpuPercent
      << ",\n  \"client_size\": [" << s.clientWidth << ", " << s.clientHeight << "]"
      << ",\n  \"uptime_seconds\": " << s.uptimeSeconds << "\n}\n";
    f.close();
    Status(f ? L"Wrote SM2DebugMenu\\diagnostics.json." : L"Diagnostics write failed.");
}

bool WindowAction(Action action) {
    return action == Action::Borderless || action == Action::RestoreWindow || action == Action::ExportDiagnostics ||
           action == Action::CopyReloadData;
}
bool TakeCommand(Command& command, bool windowOnly) {
    std::lock_guard lock(commandMutex);
    const bool engine = EngineContextReady();
    auto found = std::find_if(commands.begin(), commands.end(), [&](const Command& queued) {
        if (windowOnly) return WindowAction(queued.action);
        return !engine || !WindowAction(queued.action);
    });
    if (found == commands.end()) return false;
    command = std::move(*found); commands.erase(found); return true;
}
std::wstring ReloadClipboardText(const ReloadState& captured, uint32_t key) {
    if (!captured.supported || !captured.ready) return {};
    std::wstring name = L"Current checkpoint";
    if (key) {
        if (!captured.checkpoints) return {};
        const auto found = std::find_if(captured.checkpoints->begin(), captured.checkpoints->end(),
            [key](const ReloadCheckpoint& item) { return item.key == key; });
        if (found == captured.checkpoints->end()) return {};
        name = found->name;
    }
    std::wostringstream text;
    text << L"SM2 Debug Menu - native reload request\r\n"
         << L"Build: retail 1.0130.0.0\r\n"
         << L"Checkpoint: " << name << L"\r\n"
         << L"Selection: " << (key ? L"Named checkpoint in the loaded level" : L"Current checkpoint") << L"\r\n"
         << std::hex << std::setfill(L'0')
         << L"Request key: 0x" << std::setw(8) << key << L"\r\n"
         << L"Current checkpoint key: 0x" << std::setw(8) << captured.token.currentCheckpoint << L"\r\n"
         << L"Level asset: 0x" << std::setw(16) << captured.token.levelAsset << L"\r\n"
         << L"Captured selection only; loading state is rechecked when Reload is selected.\r\n";
    return text.str();
}
void CopyReloadData(const Command& command) {
    auto text = ReloadClipboardText(command.reload, command.checkpointKey);
    if (command.copyReplayMode) {
        const auto& selection = command.replay.selection;
        const auto* graph = FindReloadMissionGraph(selection.graphId);
        const auto* mission = FindReloadMission(selection.graphId, selection.missionHash);
        const auto* objective = FindReloadMissionObjective(selection.graphId, selection.missionHash, selection.objectiveHash);
        if (text.empty() || !graph || !mission || (selection.objectiveHash && !objective)) {
            Status(L"Replay selection is unavailable; clipboard unchanged."); return;
        }
        std::wostringstream replay;
        replay << L"SM2 Debug Menu - replay selection\r\nBuild: retail 1.0130.0.0\r\n"
               << L"Replay Mode: On\r\nGraph: " << graph->graphPath << L"\r\nMission: " << mission->name
               << L"\r\nObjective: " << (objective ? objective->name : "Mission default") << L"\r\n"
               << std::hex << std::setfill(L'0') << L"Graph asset: 0x" << std::setw(16) << selection.graphId
               << L"\r\nMission key: 0x" << std::setw(8) << selection.missionHash
               << L"\r\nObjective key: 0x" << std::setw(8) << selection.objectiveHash
               << L"\r\nCheckpoint: resolved by native replay\r\nCaptured selection only; target eligibility is rechecked before execution.\r\n";
        text = replay.str();
    }
    if (text.empty() || text.size() > 16384 || !IsWindow(targetWindow)) {
        Status(L"Reload data is unavailable; clipboard unchanged."); return;
    }
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { Status(L"Could not allocate clipboard data."); return; }
    void* contents = GlobalLock(memory);
    if (!contents) { GlobalFree(memory); Status(L"Could not prepare clipboard data."); return; }
    std::memcpy(contents, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(targetWindow)) {
        GlobalFree(memory); Status(L"Clipboard is busy; try again."); return;
    }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!copied) GlobalFree(memory); // Ownership transfers only on success.
    Status(copied ? L"Reload selection copied to the clipboard." : L"Could not copy reload data.");
}
void RunWindowCommand(const Command& command) {
    switch (command.action) {
    case Action::Borderless: Borderless(); Status(L"Borderless window applied."); break;
    case Action::RestoreWindow: RestoreWindow(); Status(L"Original window placement restored."); break;
    case Action::ExportDiagnostics: ExportDiagnostics(); break;
    case Action::CopyReloadData: CopyReloadData(command); break;
    default: break;
    }
}
bool RestoreMenuRuntimeOverrides() {
    const bool settingsRestored=RestoreDebugSettings();
    std::wstring cameraStatus;
    const bool cameraRestored=ResetOwnedCameraDebugOverrides(cameraStatus);
    return settingsRestored&&cameraRestored;
}
void RunCommands(uintptr_t actor, uint32_t handle, uintptr_t health, float maximum, float current) {
    std::deque<Command> pending;
    Command next;
    if (TakeCommand(next, false)) pending.push_back(std::move(next));
    for (const auto& c : pending) {
        // Native commands require the actual verified callback scope. A
        // published capability or window thread identity cannot authorize them.
        if (actor && !ActorStillCurrent(actor, handle)) { actor = health = 0; handle = 0; maximum = current = 0; }
        // These operations invoke native setters, or prepare data for those
        // setters. Reject before any mutation when no verified dispatcher is
        // available. Disabling auto-refill remains safe in every context.
        if (NativeActionName(c.action) && !(c.action == Action::AutoHeal && c.value == 0) && !EngineContextReady()) {
            if (c.action == Action::AutoHeal) autoHeal = false;
            RejectNativeAction(c.action);
            continue;
        }
        const bool actorBound = (NativeActionName(c.action) && c.action != Action::DebugSetting &&
                                 c.action != Action::ReloadCheckpoint && c.action != Action::ExitFrontend &&
                                 c.action != Action::CompletePrimary && c.action != Action::FullReload && c.action != Action::ReplayMission &&
                                 c.action != Action::SetNewGamePlus && c.action != Action::ExitLobby && c.action != Action::CameraShake &&
                                 !(c.action == Action::AutoHeal && c.value == 0)) || c.action == Action::SavePosition;
        if (actorBound && (!c.actorHandle || c.actorHandle != handle)) {
            Status(L"Hero changed after this command was selected; command discarded."); continue;
        }
        switch (c.action) {
        case Action::HeroWarp: {
            std::wstring result;
            const bool handled = ExecuteHeroWarp(c.heroWarp, c.warpPosition, result);
            Status(result);
            Log(handled ? "HeroWarp: native event handled." : "HeroWarp: request not handled; see status.");
            // Synchronous event handlers can change the hero/world even if no
            // final handler accepts the event. Never reuse this tick's pointers.
            InvalidateMenuSnapshot();
            if (handled) QueueMenuInput(MenuInput::Close);
            return;
        }
        case Action::SetHeroScale:
            Status(SetHeroScale(actor, handle, c.value) ? L"Hero scale applied. Reset scale restores this hero's original scale." :
                L"Hero scale request rejected; current actor, scale or engine context is unavailable.");
            break;
        case Action::ResetHeroScale:
            Status(RestoreHeroScale(actor, handle) ? L"Owned hero scale restored." :
                L"Hero scale changed elsewhere; no earlier scale was written.");
            break;
        case Action::EquipMJFlamethrower: {
            std::wstring result;
            const bool accepted=EquipFlamethrower(actor,handle,c.reload,result);
            Status(result);
            Log(accepted?"M2 flame: held-slot assignment confirmed.":"M2 flame: held-slot assignment not confirmed; see status.");
            break;
        }
        case Action::SwitchMJWeapon: {
            std::wstring result;
            const bool accepted=SwitchMJWeapon(actor,handle,c.reload,c.mjWeaponSelectedItem,result);
            Status(result);
            Log(accepted ? "MJ weapon switch: held slot confirmed." : "MJ weapon switch: request not confirmed; see status.");
            break;
        }
        case Action::CameraShake: {
            std::wstring result;
            const bool accepted=ExecuteSetCameraShake(c.cameraDebug,c.cameraShakeEnabled,result);
            Status(result);Log(accepted?"CameraShake: runtime flag updated.":"CameraShake: request rejected.");
            break;
        }
        case Action::SetNewGamePlus: {
            std::wstring result;
            const bool accepted = ExecuteSetNewGamePlus(c.reloadOptions, c.newGamePlus, result);
            Status(result);
            Log(accepted ? "SetNewGamePlus: current save flag updated." : "SetNewGamePlus: request rejected.");
            break;
        }
        case Action::ReplayMission: {
            std::wstring result;
            LoadoutPreparation preparation{&c.reloadOptions};
            const auto outcome = ExecuteReplay(c.replay, result,
                c.keepCurrentLoadout ? nullptr : PrepareLoadout, &preparation);
            DescribeLoadoutPreparation(preparation, outcome.accepted, result);
            Status(result);
            Log(outcome.accepted ? "ReplayMission: native request accepted." :
                (outcome.invoked ? "ReplayMission: invoked but not accepted; instance records discarded." : "ReplayMission: request rejected before invocation."));
            if (outcome.invoked) {
                QueueMenuInput(MenuInput::Close);
                autoHeal = false; healthEdit = {}; heroScaleEdit = {}; savedPosition = {}; undoPosition = {};
                std::lock_guard lock(stateMutex);
                state.autoHeal = state.savedPosition = state.undoPosition = false;
                state.heroReady = state.healthReady = state.teleportReady = state.heroScaleReady = state.heroScaleOwned = false;
                state.actorHandle = 0; state.characterNames.clear(); state.primary = {}; state.reload = {};
                state.actorSave = {}; state.replay = {}; state.reloadOptions = {}; state.cameraDebug = {}; state.reloadSelection = {};
            }
            return;
        }
        case Action::CompletePrimary: {
            std::wstring result;
            const bool accepted = ExecuteCompletePrimary(c.primary, result);
            Status(result);
            Log(accepted ? "CompletePrimary: native completion requested." : "CompletePrimary: request rejected.");
            if (accepted) {
                // Objective scripts can transition levels or replace the hero.
                // Discard instance records without reading old engine objects.
                autoHeal = false; healthEdit = {}; heroScaleEdit = {}; savedPosition = {}; undoPosition = {};
                std::lock_guard lock(stateMutex);
                state.autoHeal = state.savedPosition = state.undoPosition = false;
                state.heroReady = state.healthReady = state.teleportReady = state.heroScaleReady = state.heroScaleOwned = false;
                state.actorHandle = 0; state.characterNames.clear(); state.primary = {}; state.reload = {}; state.actorSave = {}; state.replay = {}; state.reloadOptions = {}; state.cameraDebug = {}; state.reloadSelection = {};
            }
            return;
        }
        case Action::ReloadCheckpoint: case Action::ExitFrontend: case Action::FullReload: case Action::ExitLobby: {
            std::wstring result;
            bool resetApplied = false;
            if (c.resetActorSave) {
                if ((c.action != Action::ReloadCheckpoint && c.action != Action::FullReload) ||
                    !SameReloadContext(c.reload.token, c.actorSave.token.world)) {
                    Status(L"Actor-save reset no longer belongs to this Reload request.");
                    return;
                }
                // Refuse known-stale checkpoints and native transition guards
                // before the persistent reset. Reload still validates again.
                if (!CheckReloadExecutionContext(c.reload, result, c.checkpointKey)) {
                    Status(result);
                    return;
                }
                const auto outcome = ExecuteActorSaveReset(c.actorSave, result);
                if (outcome != ActorSaveResetOutcome::Refused) {
                    std::lock_guard lock(stateMutex);
                    state.actorSave = {};
                }
                if (outcome != ActorSaveResetOutcome::Applied) {
                    Status(result);
                    return;
                }
                resetApplied = true;
            }
            MissionReloadPreparation preparation{c.applyMissionSelection?&c.missionSelection:nullptr,{&c.reloadOptions},c.keepCurrentLoadout};
            const auto prepare = c.keepCurrentLoadout&&!c.applyMissionSelection ? nullptr : PrepareMissionReload;
            const bool accepted = c.action == Action::ReloadCheckpoint ?
                ExecuteReload(c.reload, c.checkpointKey, result, prepare, &preparation) : (c.action == Action::FullReload ?
                ExecuteFullReload(c.reload, c.checkpointKey, result, prepare, &preparation) : (c.action == Action::ExitLobby ?
                ExecuteExitLobby(c.reload, result) : ExecuteExitFrontend(c.reload, result)));
            DescribeMissionReload(preparation, accepted, result);
            if (resetApplied) result = L"Actor save reset applied; collected items kept. " + result +
                (accepted ? L"" : L" The reset remains applied even though Reload was refused.");
            Status(result);
            Log(std::string(NativeActionName(c.action)) + (accepted ? ": native request accepted." : ": request rejected."));
            if (accepted) {
                QueueMenuInput(MenuInput::Close);
                // A native transition can destroy the actor immediately. Drop
                // per-instance state without touching its memory after the call.
                autoHeal = false; healthEdit = {}; heroScaleEdit = {}; savedPosition = {}; undoPosition = {};
                std::lock_guard lock(stateMutex);
                state.autoHeal = state.savedPosition = state.undoPosition = false;
                state.heroReady = state.healthReady = state.teleportReady = state.heroScaleReady = state.heroScaleOwned = false;
                state.actorHandle = 0; state.characterNames.clear(); state.reload = {}; state.primary = {}; state.actorSave = {}; state.replay = {}; state.reloadOptions = {}; state.cameraDebug = {}; state.reloadSelection = {};
            }
            return;
        }
        case Action::DebugSetting:
            Status(ApplyDebugSetting(c.setting, static_cast<int>(c.value)) ?
                L"Engine debug setting updated." : L"Setting change rejected; engine value preserved.");
            break;
        case Action::HeroNoDamage: SetHealthFlag(0, c.value != 0); break;
        case Action::HeroNoDeath: SetHealthFlag(1, c.value != 0); break;
        case Action::EnemyNoDeath: SetHealthFlag(2, c.value != 0); break;
        case Action::EnemyNoDamage: SetHealthFlag(3, c.value != 0); break;
        case Action::Heal:
            if (health && current > 0 && SetCurrentHealth(health, maximum)) Status(L"Hero health refilled.");
            else Status(L"A living hero with a valid Health component is required.");
            break;
        case Action::AutoHeal:
            if (c.value == 0 || health) { autoHeal = c.value != 0; Status(autoHeal ? L"Auto-refill on (does not prevent instant death)." : L"Auto-refill off."); }
            break;
        case Action::SetMaxHealth:
            if (health && HealthSetter(health) && std::isfinite(c.value) && c.value >= 1 && c.value <= 100000) {
                if (healthEdit.component != health || healthEdit.handle != handle)
                    healthEdit = {health, handle, maximum, maximum};
                float clamped = std::min(current, c.value);
                if (Write(health + 0xa0, c.value)) {
                    healthEdit.applied = c.value;
                    if (SetCurrentHealth(health, clamped)) Status(L"Maximum health changed for this hero instance.");
                    else { RestoreHealth(health, handle); Status(L"Health setter refused the change; maximum restored."); }
                } else Status(L"Health write rejected.");
            }
            break;
        case Action::ResetHealth:
            RestoreHealth(health, handle);
            RestoreHealthFlags();
            Status(L"Health flags restored; auto-refill off.");
            break;
        case Action::SavePosition: {
            Vec3 p{};
            if (actor && GetPosition(actor, handle, p)) { savedPosition = {p, handle, true}; Status(L"Position saved for this hero."); }
            break;
        }
        case Action::LoadPosition: case Action::UndoTeleport:
        case Action::MoveXMinus: case Action::MoveXPlus: case Action::MoveYMinus:
        case Action::MoveYPlus: case Action::MoveZMinus: case Action::MoveZPlus: {
            Vec3 before{}, desired{};
            uintptr_t transform = 0;
            if (!g.setPosition || !actor || !Read(actor, transform) || !GetPosition(actor, handle, before)) {
                Status(L"Position setter or current hero is unavailable."); break;
            }
            desired = before;
            if (c.action == Action::LoadPosition || c.action == Action::UndoTeleport) {
                const auto& source = c.action == Action::LoadPosition ? savedPosition : undoPosition;
                if (!source.valid || source.handle != handle) {
                    Status(L"No position is stored for this hero instance."); break;
                }
                desired = source.value;
            } else {
                if (!std::isfinite(c.value) || c.value < .1f || c.value > 1000) break;
                { std::lock_guard lock(stateMutex); state.moveStep = c.value; }
                switch (c.action) {
                case Action::MoveXMinus: desired.x -= c.value; break;
                case Action::MoveXPlus: desired.x += c.value; break;
                case Action::MoveYMinus: desired.y -= c.value; break;
                case Action::MoveYPlus: desired.y += c.value; break;
                case Action::MoveZMinus: desired.z -= c.value; break;
                case Action::MoveZPlus: desired.z += c.value; break;
                default: break;
                }
            }
            if (!PlausiblePosition(desired)) { Status(L"Position outside supported coordinates."); break; }
            undoPosition = {before, handle, true};
            if (!EngineContextReady()) { RejectNativeAction(c.action); break; }
            g.setPosition(reinterpret_cast<void*>(transform), &desired);
            Status(L"Position change requested through the engine transform setter.");
            break;
        }
        case Action::SwitchCharacter: {
            uintptr_t manager = 0;
            const auto names = Characters(actor, handle, manager);
            if (!std::isfinite(c.value) || c.value < 0 || c.value >= names.size()) {
                Status(L"Character selection is not available for the current hero."); break;
            }
            const auto index = static_cast<int>(c.value);
            if (c.characterName.empty() || Wide(names[index]) != c.characterName) {
                Status(L"Character list changed after selection; command discarded."); break;
            }
            RestoreHealth(health, handle);
            RestoreHeroScale(actor, handle);
            savedPosition = {}; undoPosition = {};
            if (!EngineContextReady()) { RejectNativeAction(c.action); break; }
            g.switchCharacter(reinterpret_cast<void*>(manager), NameCRC(names[index].c_str()), 4, nullptr);
            Status(L"Character switch requested; waiting for the new hero.");
            // The old actor may have been destroyed. Do not read it again this tick.
            return;
        }
        case Action::Borderless: case Action::RestoreWindow: case Action::ExportDiagnostics: case Action::CopyReloadData:
            RunWindowCommand(c); break;
        case Action::ResetAll:
            if (EngineContextReady() && flameBurnReady.load()) {
                flameBurnWorldLive=false;++flameBurnWorldSerial;
                flame_burn::EngineTick(nullptr,0);
            }
            RestoreHealth(health, handle); RestoreHealthFlags(); RestoreHeroScale(actor, handle);
            settingsRestorePending = !RestoreMenuRuntimeOverrides(); savedPosition = {}; undoPosition = {};
            Status(L"Menu overrides reset; saved positions cleared.");
            if (shutdownReset.load() && !settingsRestorePending) shutdownResetComplete.store(true);
            break;
        }
        // Each later command obtains a fresh object snapshot on a later tick.
        // Character switches and reload/exit requests may invalidate actors.
    }
}

void GameTick() {
    if (stopping) return;
    std::unique_lock simulationLock(simulationMutex, std::try_to_lock);
    if (!simulationLock.owns_lock()) return;
    // Once registered, the engine callback exclusively owns simulation state.
    // Window messages continue to service HWND and diagnostic commands only.
    if ((engineOwnsSimulation.load() || EngineDispatchAvailable()) && !EngineContextReady()) {
        return;
    }
    if (settingsRestorePending && EngineContextReady()) {
        settingsRestorePending = !RestoreMenuRuntimeOverrides();
        if (shutdownReset.load() && !settingsRestorePending) shutdownResetComplete.store(true);
        if (settingsRestorePending) return;
    }
    const bool menuVisible = MenuVisible();
    bool pendingCommand = false;
    { std::lock_guard lock(commandMutex);
      pendingCommand = std::any_of(commands.begin(), commands.end(), [](const Command& command) {
          return !WindowAction(command.action) || !EngineContextReady();
      }); }
    const bool refreshMenu = menuVisible || pendingCommand;
    bool supported = false;
    std::uint64_t snapshotGeneration = 0;
    { std::lock_guard lock(stateMutex); supported = state.supported; snapshotGeneration = menuSnapshotGeneration; }
    uint32_t handle = 0;
    uintptr_t actor = supported ? CurrentActor(handle) : 0;
    uintptr_t health = 0;
    float maximum = 0, current = 0;
    // The default snapshot path below performs guarded memory copies only.
    // It neither calls engine getters nor updates the actor lookup caches.
    bool hasHealth = actor && GetHealth(actor, handle, health, maximum, current) && HealthSetter(health);
    if (healthEdit.component && (healthEdit.component != health || healthEdit.handle != handle)) healthEdit = {};
    // Do not carry automatic writes into a loading screen or a different hero.
    static uint32_t priorHandle = 0;
    if (priorHandle && priorHandle != handle) autoHeal = false;
    priorHandle = handle;
    if (!hasHealth || !EngineContextReady()) autoHeal = false;
    if (autoHeal && current > 0 && current < maximum && ActorStillCurrent(actor, handle)) SetCurrentHealth(health, maximum);
    Vec3 p{};
    bool position = actor && GetPosition(actor, handle, p);
    uintptr_t scaleTransform = 0;
    Vec3 scale{};
    const bool scaleReady = refreshMenu && actor && ReadHeroScale(actor, handle, scaleTransform, scale);
    uintptr_t manager = 0;
    auto names = refreshMenu && actor ? Characters(actor, handle, manager) : std::vector<std::string>{};
    if (actor && !ActorStillCurrent(actor, handle)) {
        actor = health = 0; handle = 0; hasHealth = position = autoHeal = false;
        maximum = current = 0; p = {}; names.clear();
    }
    DiscoverFlameLoop(actor,handle,menuVisible);
    ReloadState reload;
    PrimaryObjectiveState primary;
    ActorSaveState actorSave;
    ReloadOptionsState reloadOptions;
    CameraDebugState cameraDebug;
    ReloadSelectionState reloadSelection;
    HeroWarpState heroWarp;
    ReplayState replay;
    bool flameReady = false,weaponSwitchReady=false;
    uint16_t weaponSelectedItem=0;
    if (refreshMenu) {
        reload = ReadReloadState();
        heroWarp = ReadHeroWarpState(reload, actor ? handle : 0);
        primary = ReadPrimaryObjectiveState(reload);
        actorSave = ReadActorSaveState(reload);
        reloadOptions = ReadReloadOptionsState(reload);
        cameraDebug = ReadCameraDebugState(reload);
        uintptr_t flameSystem = 0;
        flameReady = reload.ready && FlameLoadoutReady(actor, handle, flameSystem);
        MJWeaponPair weapons;
        weaponSwitchReady=flameReady && flameLoopCodeReady && FlameSlotCode() &&
            ReadMJWeaponPair(actor,handle,weapons,false);
        if (weaponSwitchReady) weaponSelectedItem=weapons.selected;
        ReplaySelection replaySelection;
        ReloadMissionSelection missionSelection;
        { std::lock_guard lock(stateMutex); replaySelection = desiredReplaySelection; missionSelection = desiredReloadMissionSelection; }
        reloadSelection = ReadReloadSelectionState(reload, missionSelection);
        replay = ReadReplayState(primary, replaySelection);
        static std::wstring lastReloadStatus;
        if (reload.status != lastReloadStatus) {
            lastReloadStatus = reload.status;
            Log("Reload runtime: " + std::string(reload.status.begin(), reload.status.end()) +
                " [checkpoints=" + std::to_string(reload.checkpoints ? reload.checkpoints->size() : 0) + "]");
        }
    } else {
        // A native reload/replay may have closed the menu. Continue observing
        // its unload/reload latch, including return to the same world, without
        // rebuilding actor-save, settings, camera or primary menu snapshots.
        if (ReloadTransitionPending()) (void)ReadReloadState();
        if (ReplayTransitionPending()) {
            PrimaryObjectiveState origin;
            ReplaySelection selection;
            { std::lock_guard lock(stateMutex); origin = state.primary; selection = desiredReplaySelection; }
            (void)ReadReplayState(origin, selection);
        }
    }
    {
        std::lock_guard lock(stateMutex);
        state.heroReady = actor != 0;
        if (refreshMenu) {
            state.reload = std::move(reload);
            state.primary = std::move(primary);
            state.actorSave = std::move(actorSave);
            state.replay = std::move(replay);
            state.reloadOptions = std::move(reloadOptions);
            state.cameraDebug = std::move(cameraDebug);
            state.reloadSelection = std::move(reloadSelection);
            state.heroWarp = std::move(heroWarp);
            state.mjFlamethrowerReady = flameReady;
            state.mjWeaponSwitchReady = weaponSwitchReady;
            state.mjWeaponSelectedItem = weaponSelectedItem;
            state.heroScaleReady = scaleReady && actor != 0;
            state.heroScaleOwned = scaleReady && actor == heroScaleEdit.actor && handle == heroScaleEdit.handle &&
                scaleTransform == heroScaleEdit.transform && ScaleMatches(scale, heroScaleEdit.applied);
            state.heroScale = scaleReady ? scale.x : 1.0f;
            if (menuVisible && MenuVisible() && snapshotGeneration == menuSnapshotGeneration)
                completedSnapshotGeneration = snapshotGeneration;
        }
        state.actorHandle = actor ? handle : 0;
        state.healthReady = hasHealth;
        state.teleportReady = g.setPosition && position;
        state.engineActionsReady = EngineDispatchAvailable();
        state.autoHeal = autoHeal;
        unsigned char flags[4]{};
        state.healthFlagsReady = g.healthFlags && ReadBytes(g.healthFlags, flags, sizeof(flags)) &&
            std::all_of(std::begin(flags), std::end(flags), [](unsigned char c) { return c <= 1; });
        if (state.healthFlagsReady) {
            state.heroNoDamage = flags[0] != 0; state.heroNoDeath = flags[1] != 0;
            state.enemyNoDeath = flags[2] != 0; state.enemyNoDamage = flags[3] != 0;
        }
        state.health = hasHealth ? current : 0;
        state.maxHealth = hasHealth ? maximum : 0;
        state.x = position ? p.x : 0; state.y = position ? p.y : 0; state.z = position ? p.z : 0;
        state.savedPosition = savedPosition.valid && savedPosition.handle == handle;
        state.undoPosition = undoPosition.valid && undoPosition.handle == handle;
        state.borderless = changedWindow;
        if (refreshMenu) {
            state.characterNames.clear();
            for (const auto& name : names) state.characterNames.push_back(Wide(name));
        }
    }
    RunCommands(actor, handle, hasHealth ? health : 0, maximum, current);
}

void EngineFrame() noexcept {
    if (!IsWithinVerifiedEngineCallback()) return;
    engineOwnsSimulation.store(true);
    try {
        {
            std::unique_lock loopLock(simulationMutex,std::try_to_lock);
            if (loopLock.owns_lock()) { TickHeroScale(); TickFlameLoops(); TickFlameBurn(); }
        }
        static uint64_t nextTick = 0;
        const auto now = GetTickCount64();
        if (now < nextTick && !shutdownReset.load() && !settingsRestorePending.load()) return;
        nextTick = now + 100;
        GameTick();
    } catch (...) {
        auto token = dispatchToken.load();
        if (!token) token = GetEngineDispatchObservation().callbackToken;
        ReportEngineDispatchCallbackFailure(token, 1);
    }
}

LRESULT CALLBACK GameWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CHAR && MenuVisible() && wParam <= 0xffff)
        QueueMenuText(static_cast<wchar_t>(wParam));
    LRESULT captured = 0;
    if (input_capture::HandleWindowMessage(window, message, wParam, lParam, previousProc, captured)) return captured;
    if (message == tickMessage && wParam == reinterpret_cast<WPARAM>(selfModule)) {
        tickPending = false;
        if (engineOwnsSimulation.load() || EngineDispatchAvailable()) {
            Command command;
            if (TakeCommand(command, true)) RunWindowCommand(command);
        } else GameTick();
        return 0;
    }
    return previousProc ? CallWindowProcW(previousProc, window, message, wParam, lParam)
                        : DefWindowProcW(window, message, wParam, lParam);
}
struct WindowChoice { HWND window = nullptr; long long area = 0; };
BOOL CALLBACK FindWindowCallback(HWND window, LPARAM param) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    wchar_t cls[128]{};
    GetClassNameW(window, cls, 128);
    if (std::wstring(cls) == L"SM2DebugMenuWindow") return TRUE;
    RECT r{};
    if (!GetClientRect(window, &r) || r.right < 640 || r.bottom < 360) return TRUE;
    auto* choice = reinterpret_cast<WindowChoice*>(param);
    const long long area = static_cast<long long>(r.right) * r.bottom;
    if (area > choice->area) { choice->area = area; choice->window = window; }
    return TRUE;
}
void UpdatePerformance() {
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    const bool hasMemory = GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
    FILETIME created{}, exited{}, kernel{}, user{};
    static uint64_t previousTime = 0, previousCpu = 0;
    const uint64_t now = GetTickCount64();
    double cpu = 0;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const uint64_t total = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
            (static_cast<uint64_t>(user.dwHighDateTime) << 32) + user.dwLowDateTime;
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        if (previousTime && now > previousTime)
            cpu = 100.0 * (total - previousCpu) / (10000.0 * (now - previousTime) * std::max<DWORD>(1, system.dwNumberOfProcessors));
        previousTime = now; previousCpu = total;
    }
    RECT client{};
    if (targetWindow) GetClientRect(targetWindow, &client);
    std::lock_guard lock(stateMutex);
    state.memoryMiB = hasMemory ? memory.WorkingSetSize / (1024.0 * 1024.0) : 0;
    state.cpuPercent = std::clamp(cpu, 0.0, 100.0);
    state.uptimeSeconds = static_cast<unsigned long>((now - startedAt) / 1000);
    state.clientWidth = client.right; state.clientHeight = client.bottom;
    state.gameWindow = targetWindow;
}
} // namespace

void InitializeBackend(HMODULE module, bool preview) {
    // Construct immutable catalog vectors before the first engine callback.
    (void)ReloadMissionCatalog();
    selfModule = module; previewMode = preview; startedAt = GetTickCount64();
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, 32768);
    auto directory = std::filesystem::path(path).parent_path();
    if (directory.filename() == L"scripts") outputPath = directory.parent_path() / L"SM2DebugMenu";
    else if (directory.filename() == L"dist") outputPath = directory.parent_path();
    else outputPath = directory;
    std::error_code error;
    std::filesystem::create_directories(outputPath, error);
    {
        std::lock_guard lock(stateMutex);
        state.status = preview ? L"Preview only: no game objects are accessed." : L"Initializing retail bindings...";
        state.compatibility = preview ? L"UI preview / no game attached" : L"Checking supported retail build 1.0130.0.0";
        state.engineActionsReady = false;
    }
    if (!preview) {
        const bool supported = BindGame();
        {
            std::lock_guard lock(stateMutex);
            state.supported = supported;
            state.compatibility = supported ? L"Retail 1.0130.0.0 / signatures verified" : L"Unsupported build or bindings / gameplay disabled";
            if (supported) state.status = L"Press PS or F10 in the game to open the debug overlay.";
        }
        if (supported) {
            flameLoopCodeReady=FlameLoopCode();
            if (flameLoopCodeReady) {
                const bool audio=flame_audio::Initialize(module,101);
                Log(std::string("M2 loop cleanup verified; audio worker ")+(audio?"started":"unavailable")+".");
            } else Log("M2 loop cleanup signature refused; M2 equipment action disabled.");
            InitializeDebugSettings(module);
            InitializeReloadRuntime(GetModuleHandleW(nullptr), true);
            InitializePrimaryObjectiveRuntime(GetModuleHandleW(nullptr), true);
            InitializeActorSaveRuntime(GetModuleHandleW(nullptr), true);
            InitializeReplayRuntime(GetModuleHandleW(nullptr), true);
            InitializeReloadOptionsRuntime(GetModuleHandleW(nullptr), true);
            InitializeCameraDebugRuntime(GetModuleHandleW(nullptr), true);
            InitializeReloadSelectionRuntime(GetModuleHandleW(nullptr), true);
            InitializeHeroWarpRuntime(GetModuleHandleW(nullptr), true);
            InitializeEngineDispatch(module, nullptr);
        }
    }
    tickMessage = RegisterWindowMessageW(kTickMessage);
    Log(preview ? "Preview started; gameplay disabled." : "Debug menu initialized.");
}

void TickBackend() {
    static bool burnInitializationAttempted=false;
    if (!previewMode && !burnInitializationAttempted && g.base && flameLoopCodeReady &&
        GetEngineDispatchObservation().contextReady) {
        burnInitializationAttempted=true;
        const flame_burn::Host host{ReadBytes,BurnActor,FlameComponentHandle,BurnFind,BurnSourceValid,EngineContextReady};
        const bool ready=flame_burn::Initialize(g.base,g.size,host);
        flameBurnReady.store(ready,std::memory_order_release);
        Log(std::string("M2 accepted-hit burn ")+(ready ? "callback ready." : "unavailable: ")+
            (ready ? "" : flame_burn::LastFailure()));
    }
    if (MenuVisible() || shutdownReset.load() || stopping.load() || !targetWindow || GetForegroundWindow()!=targetWindow)
        flame_audio::Update(false);
    if (!previewMode && !dispatchRegistrationAttempted.load() && GetEngineDispatchObservation().contextReady) {
        dispatchRegistrationAttempted.store(true);
        uint64_t token = 0;
        if (RegisterEngineDispatchCallback(selfModule, EngineFrame, &token)) {
            engineOwnsSimulation.store(true);
            dispatchToken.store(token);
            Log("Verified main-engine callback registered; native operations are confined to its scope.");
        } else Log("Engine callback registration refused; native operations remain unavailable.");
    }
    if (!previewMode && !targetWindow && tickMessage) {
        WindowChoice choice;
        EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&choice));
        if (choice.window) {
            targetWindow = choice.window;
            previousProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(targetWindow, GWLP_WNDPROC));
            SetLastError(0);
            auto oldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(targetWindow, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(GameWindowProc)));
            if (!oldProc) {
                Log("Could not install game window dispatch."); targetWindow = nullptr;
            } else { previousProc = oldProc; Log("Game window dispatch ready; snapshots use guarded copies, user actions use the window thread."); }
        }
    }
    if (targetWindow && !IsWindow(targetWindow)) {
        targetWindow = nullptr; previousProc = nullptr; tickPending = false;
        std::lock_guard lock(stateMutex);
        state.heroReady = state.healthReady = false;
        state.status = L"Waiting for the game window.";
    }
    if (!previewMode && targetWindow) {
        static std::uint64_t nextInputCaptureAttempt = 0;
        const auto now = GetTickCount64();
        const auto capture = input_capture::Observe();
        if ((!capture.installed || !capture.sonySdkInstalled) && now >= nextInputCaptureAttempt) {
            nextInputCaptureAttempt = now + 1000;
            input_capture::Initialize(selfModule, GetSnapshot().supported, &MenuVisible);
            if (!capture.sonySdkInstalled && input_capture::Observe().sonySdkInstalled)
                Log("Sony SDK input capture installed; Debug/Dev visibility blocks hero controls.");
        }
    }
    if (!previewMode && targetWindow && GetEngineDispatchObservation().contextReady) {
        // The script worker can precede a native system's constructor. Retry
        // only after a real window and verified native frame have appeared;
        // Initialize still refuses foreign slots and rechecks every code pin.
        static bool photoGateReady = false;
        static unsigned photoGateAttempts = 0;
        static std::uint64_t nextPhotoGateAttempt = 0;
        const auto now = GetTickCount64();
        if (!photoGateReady && now >= nextPhotoGateAttempt && photoGateAttempts < 120) {
            nextPhotoGateAttempt = now + 1000;
            ++photoGateAttempts;
            photoGateReady = photo_gate::Initialize(selfModule, GetSnapshot().supported, &MenuVisible);
            if (photoGateReady) Log("Native photo-mode entry gate verified and installed; Debug/Dev visibility blocks Open.");
            else if (photoGateAttempts == 120) Log("Native photo-mode entry gate remains unavailable after bounded startup retries.");
        }
    }
    UpdatePerformance();
    if (!previewMode) {
        // Settings writes/restoration revalidate themselves on the engine
        // callback. Hidden display refreshes have no execution responsibility.
        const bool visible = MenuVisible();
        std::uint64_t generation = 0;
        { std::lock_guard lock(stateMutex); generation = menuSnapshotGeneration; }
        auto settings = visible ? SnapshotDebugSettings() : std::vector<DebugSettingValue>{};
        std::lock_guard lock(stateMutex);
        if (visible) {
            state.settings = std::move(settings);
            if (MenuVisible() && generation == menuSnapshotGeneration) settingsSnapshotGeneration = generation;
        }
        state.engineActionsReady = EngineDispatchAvailable();
        state.borderless = changedWindow.load();
    }
    if (targetWindow && !tickPending.exchange(true)) {
        if (!PostMessageW(targetWindow, tickMessage, reinterpret_cast<WPARAM>(selfModule), 0)) tickPending = false;
    }
    if (previewMode) {
        std::deque<Command> local;
        { std::lock_guard lock(commandMutex); local.swap(commands); }
        for (const auto& c : local) {
            if (c.action == Action::ExportDiagnostics) ExportDiagnostics();
            else if (c.action == Action::ResetAll) Status(L"Preview reset. No game state was changed.");
            else Status(L"Gameplay controls are disabled in preview mode.");
        }
    }
}
void InvalidateMenuSnapshot() {
    flameGateEpoch.fetch_add(1);
    flame_audio::Update(false);
    std::lock_guard lock(stateMutex);
    if (!++menuSnapshotGeneration) ++menuSnapshotGeneration;
}
Snapshot GetSnapshot() {
    std::lock_guard lock(stateMutex);
    auto result = state;
    if (!previewMode && (!MenuVisible() || completedSnapshotGeneration != menuSnapshotGeneration)) {
        result.heroReady = result.healthReady = result.teleportReady = result.healthFlagsReady = false;
        result.heroScaleReady = result.heroScaleOwned = false;
        result.heroWarp.ready = false;
        result.mjFlamethrowerReady = false;
        result.mjWeaponSwitchReady = false;
        result.mjWeaponSelectedItem = 0;
        result.reload.ready = result.reload.frontendReady = result.reload.lobbyReady = false;
        result.primary.ready = result.actorSave.ready = result.replay.ready = false;
        result.reloadOptions.ready = result.cameraDebug.ready = result.reloadSelection.ready = false;
    }
    if (!previewMode && (!MenuVisible() || settingsSnapshotGeneration != menuSnapshotGeneration))
        for (auto& setting : result.settings) setting.available = false;
    return result;
}
void SubmitAction(Action action, float value) {
    if (stopping || shutdownReset.load()) return;
    if (action == Action::ReloadCheckpoint || action == Action::ExitFrontend || action == Action::CopyReloadData ||
        action == Action::CompletePrimary || action == Action::FullReload || action == Action::ReplayMission ||
        action == Action::SetNewGamePlus || action == Action::ExitLobby || action == Action::CameraShake || action == Action::HeroWarp) return;
    if (action == Action::ExportDiagnostics && !targetWindow) { ExportDiagnostics(); return; }
    const auto snapshot = GetSnapshot();
    if ((action == Action::SetHeroScale || action == Action::ResetHeroScale) &&
        (!snapshot.heroReady || !snapshot.heroScaleReady || !snapshot.engineActionsReady || previewMode)) return;
    if (action == Action::SetHeroScale && (!std::isfinite(value) || value < 0.1f || value > 10.0f)) return;
    if (action==Action::SwitchMJWeapon && !snapshot.mjWeaponSwitchReady) return;
    std::wstring choice;
    if (action == Action::SwitchCharacter && std::isfinite(value) && value >= 0 && value < snapshot.characterNames.size())
        choice = snapshot.characterNames[static_cast<size_t>(value)];
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{action, value, {}, snapshot.actorHandle, std::move(choice)};
        if (action==Action::EquipMJFlamethrower || action==Action::SwitchMJWeapon) command.reload=snapshot.reload;
        if (action==Action::SwitchMJWeapon) command.mjWeaponSelectedItem=snapshot.mjWeaponSelectedItem;
        commands.push_back(std::move(command));
        if (action == Action::ResetAll) commands.push_back({Action::RestoreWindow, 0});
    }
}
void SubmitHeroWarp(const HeroWarpState& captured, const HeroWarpPosition& position) {
    if (stopping || shutdownReset.load() || previewMode || !captured.ready || !captured.supported ||
        !captured.actorHandle || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        std::fabs(position.x) >= 100000.0f || std::fabs(position.y) >= 100000.0f || std::fabs(position.z) >= 100000.0f) return;
    const auto snapshot = GetSnapshot();
    if (!snapshot.engineActionsReady || !snapshot.heroReady || snapshot.actorHandle != captured.actorHandle ||
        !SameHeroWarpContext(captured, snapshot.heroWarp)) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{Action::HeroWarp, 0};
        command.actorHandle = captured.actorHandle;
        command.heroWarp = captured;
        command.warpPosition = position;
        commands.push_back(std::move(command));
    }
}
void SubmitDebugSetting(const std::string& id, int direction) {
    if (stopping || shutdownReset.load() || previewMode || id.empty() || id.size() > 192 || direction < -1 || direction > 1) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 32) commands.push_back({Action::DebugSetting, static_cast<float>(direction), id});
}
void SubmitReloadAction(Action action, const ReloadState& captured, uint32_t checkpointKey) {
    if (stopping || shutdownReset.load() || previewMode || !captured.ready ||
        (action != Action::ReloadCheckpoint && action != Action::ExitFrontend && action != Action::ExitLobby &&
         action != Action::CopyReloadData && action != Action::FullReload)) return;
    if (action == Action::ExitFrontend && !captured.frontendReady) return;
    if (action == Action::ExitLobby && !captured.lobbyReady) return;
    if (checkpointKey && (!captured.checkpoints || std::none_of(captured.checkpoints->begin(), captured.checkpoints->end(),
            [checkpointKey](const ReloadCheckpoint& item) { return item.key == checkpointKey; }))) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{action, 0};
        command.reload = captured;
        command.checkpointKey = checkpointKey;
        commands.push_back(std::move(command));
    }
}
void SubmitSetNewGamePlus(const ReloadOptionsState& captured, bool enabled) {
    if (stopping || shutdownReset.load() || previewMode || !captured.supported || !captured.ready) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{Action::SetNewGamePlus, 0};
        command.reloadOptions = captured;
        command.newGamePlus = enabled;
        commands.push_back(std::move(command));
    }
}
void SubmitCompletePrimary(const PrimaryObjectiveState& captured) {
    if (stopping || shutdownReset.load() || previewMode || !captured.supported || !captured.ready ||
        !captured.token.missionHash || !captured.token.objectiveHash) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{Action::CompletePrimary, 0};
        command.primary = captured;
        commands.push_back(std::move(command));
    }
}
void SubmitReloadWithActorReset(Action action, const ReloadState& captured,
    uint32_t checkpointKey, const ActorSaveState& reset) {
    if (stopping || shutdownReset.load() || previewMode || !captured.supported || !captured.ready ||
        !reset.supported || !reset.ready || !SameReloadContext(captured.token, reset.token.world) ||
        (action != Action::ReloadCheckpoint && action != Action::FullReload)) return;
    if (checkpointKey && (!captured.checkpoints || std::none_of(captured.checkpoints->begin(), captured.checkpoints->end(),
        [checkpointKey](const ReloadCheckpoint& item) { return item.key == checkpointKey; }))) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{action, 0};
        command.reload = captured; command.checkpointKey = checkpointKey;
        command.resetActorSave = true; command.actorSave = reset;
        commands.push_back(std::move(command));
    }
}
void SubmitReplaySelection(const ReplaySelection& selection) {
    if (stopping || shutdownReset.load() || previewMode) return;
    if (selection.graphId && (!FindReloadMission(selection.graphId, selection.missionHash) ||
        (selection.objectiveHash && !FindReloadMissionObjective(selection.graphId, selection.missionHash, selection.objectiveHash)))) return;
    if (!selection.graphId && (selection.missionHash || selection.objectiveHash)) return;
    std::lock_guard lock(stateMutex);
    desiredReplaySelection = selection;
}
void SubmitReplayAction(const ReplayState& captured) {
    if (stopping || shutdownReset.load() || previewMode || !captured.supported || !captured.ready ||
        !captured.origin.ready || !captured.selection.graphId || !captured.selection.missionHash) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{Action::ReplayMission, 0};
        command.replay = captured;
        commands.push_back(std::move(command));
    }
}
void SubmitReloadWithPolicy(Action action, const ReloadState& captured, uint32_t checkpointKey,
    const ReloadPolicy& policy) {
    if (stopping || shutdownReset.load() || previewMode || !captured.supported || !captured.ready ||
        (action != Action::ReloadCheckpoint && action != Action::FullReload)) return;
    if (!policy.keepCurrentLoadout && (!policy.campaign.supported || !policy.campaign.ready ||
        !SameReloadContext(captured.token, policy.campaign.token.world))) return;
    if (policy.resetActorSave && (!policy.actors.supported || !policy.actors.ready ||
        !SameReloadContext(captured.token, policy.actors.token.world))) return;
    if(policy.applyMissionSelection&&(!policy.missions.supported||!policy.missions.ready||
        policy.missions.selection.replayMode||!SameReloadContext(captured.token,policy.missions.token.world)))return;
    if (checkpointKey && (!captured.checkpoints || std::none_of(captured.checkpoints->begin(), captured.checkpoints->end(),
        [checkpointKey](const ReloadCheckpoint& item) { return item.key == checkpointKey; }))) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{action, 0}; command.reload = captured; command.checkpointKey = checkpointKey;
        command.keepCurrentLoadout = policy.keepCurrentLoadout; command.reloadOptions = policy.campaign;
        command.resetActorSave = policy.resetActorSave; command.actorSave = policy.actors;
        command.applyMissionSelection=policy.applyMissionSelection;command.missionSelection=policy.missions;
        commands.push_back(std::move(command));
    }
}
void SubmitReplayWithPolicy(const ReplayState& captured, const ReloadPolicy& policy) {
    if (stopping || shutdownReset.load() || previewMode || !captured.supported || !captured.ready ||
        !captured.origin.ready || !captured.selection.graphId || !captured.selection.missionHash || policy.resetActorSave || policy.applyMissionSelection) return;
    if (!policy.keepCurrentLoadout && (!policy.campaign.supported || !policy.campaign.ready ||
        !SameReloadContext(captured.origin.world, policy.campaign.token.world))) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{Action::ReplayMission, 0}; command.replay = captured;
        command.keepCurrentLoadout = policy.keepCurrentLoadout; command.reloadOptions = policy.campaign;
        commands.push_back(std::move(command));
    }
}
void SubmitReplayDataCopy(const ReloadState& world, const ReplaySelection& selection) {
    if (stopping || shutdownReset.load() || previewMode || !world.supported || !world.ready ||
        !FindReloadMission(selection.graphId, selection.missionHash) ||
        (selection.objectiveHash && !FindReloadMissionObjective(selection.graphId, selection.missionHash, selection.objectiveHash))) return;
    std::lock_guard lock(commandMutex);
    if (commands.size() < 30) {
        Command command{Action::CopyReloadData, 0};
        command.reload = world; command.replay.selection = selection; command.copyReplayMode = true;
        commands.push_back(std::move(command));
    }
}
void SubmitCameraShake(const CameraDebugState& captured,bool enabled) {
    if(stopping||shutdownReset.load()||previewMode||!captured.supported||!captured.ready)return;
    std::lock_guard lock(commandMutex);
    if(commands.size()<30){Command command{Action::CameraShake,0};command.cameraDebug=captured;
        command.cameraShakeEnabled=enabled;commands.push_back(std::move(command));}
}
void SubmitReloadMissionSelection(const ReloadMissionSelection& selection) {
    if(stopping||shutdownReset.load()||previewMode||selection.branches.size()>64)return;
    if(!selection.graphId){if(!selection.branches.empty())return;}
    else if(!FindReloadMissionGraph(selection.graphId))return;
    for(const auto& branch:selection.branches){
        if(static_cast<unsigned>(branch.state)>2)return;
        if(!branch.missionHash){if(branch.objectiveHash)return;continue;}
        if(!FindReloadMission(selection.graphId,branch.missionHash)||
           (branch.objectiveHash&&!FindReloadMissionObjective(selection.graphId,branch.missionHash,branch.objectiveHash)))return;
    }
    std::lock_guard lock(stateMutex);desiredReloadMissionSelection=selection;
}
void ShutdownBackend() {
    shutdownReset.store(true);
    flame_burn::RequestStop();
    flame_audio::Update(false);
    photo_gate::Shutdown();
    input_capture::Shutdown();
    shutdownResetComplete.store(false);
    {
        std::lock_guard lock(commandMutex);
        commands.clear();
        commands.push_front({Action::ResetAll, 0});
        commands.push_back({Action::RestoreWindow, 0});
    }
    if (targetWindow && previousProc && IsWindow(targetWindow)) {
        // Reset must run before any queued gameplay operation, and must be the
        // only operation serviced by the synchronous shutdown tick.
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(targetWindow, tickMessage, reinterpret_cast<WPARAM>(selfModule), 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored);
        const auto windowDeadline = GetTickCount64() + 2000;
        while (changedWindow.load() && IsWindow(targetWindow) && GetTickCount64() < windowDeadline &&
            reinterpret_cast<WNDPROC>(GetWindowLongPtrW(targetWindow, GWLP_WNDPROC)) == GameWindowProc) {
            if (!SendMessageTimeoutW(targetWindow, tickMessage, reinterpret_cast<WPARAM>(selfModule), 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &ignored)) break;
            Sleep(5);
        }
        if (changedWindow.load()) Log("Original window restoration did not complete before dispatcher shutdown.");
        if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(targetWindow, GWLP_WNDPROC)) == GameWindowProc)
            SetWindowLongPtrW(targetWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousProc));
    }
    if (dispatchToken.load()) {
        const auto deadline = GetTickCount64() + 2000;
        while ((!shutdownResetComplete.load() || !flameBurnDrained.load()) && GetTickCount64() < deadline) Sleep(5);
        if (!shutdownResetComplete.load()) Log("Engine reset did not finish before shutdown; no window-thread native fallback was attempted.");
        if (!flameBurnDrained.load()) Log("M2 burn cleanup remains actor-owned; callback detached and module pinned. No off-thread component removal attempted.");
        UnregisterEngineDispatchCallback(dispatchToken.load(), 2000);
        dispatchToken.store(0);
    }
    stopping = true;
    if (!flame_burn::Quiescent(1000)) Log("M2 burn shutdown incomplete; its pinned module and adapter remain mapped until process exit.");
    if (!flame_audio::Shutdown(1000)) Log("M2 audio worker shutdown pending; worker retains its module reference.");
    if (!previewMode) ShutdownEngineDispatch();
    Log("Menu shut down.");
}
std::wstring OutputDirectory() { return outputPath.wstring(); }

} // namespace sm2debug
