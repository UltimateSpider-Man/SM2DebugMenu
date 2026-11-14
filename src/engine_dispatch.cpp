// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic-only data hook. No native commands, executable writes or TLS writes.
#include "engine_dispatch.h"
#include "supported_builds.h"
#include <bcrypt.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace sm2debug {
namespace {
namespace fs = std::filesystem;
constexpr DWORD kTimestamp = 0x679b5991, kImageSize = 0xce54000;
constexpr uintptr_t kObject = 0xac3b7c0, kVtable = 0x52f4e58, kMethod = 0x373b20;
constexpr uintptr_t kCaller = 0x2af6a3a, kFrameBegin = 0x2af6620, kFrameEnd = 0x2af76f5;
constexpr uintptr_t kTlsIndex = 0xc349cf0, kMainRole = 0xa018c98, kMainArena = 0xbd539a8;
constexpr uint64_t kRequiredFrames = 30;
enum Refusal : uint32_t {
    None, CallbackNotAuthorized, WrongExecutable, WrongFileHash, WrongImage,
    WrongSections, WrongCode, WrongUnwind, WrongTlsDirectory, WrongObjectVtable,
    WrongVtableContents, WrongVtableMemory, PinFailed, WorkerFailed,
    ProtectionFailed, ConcurrentSlotChange, ProtectionRestoreFailed, SlotChangedAfterInstall, PrivateScopeUnavailable
};
struct Fault { Refusal reason; };
void Require(bool value, Refusal reason) { if (!value) throw Fault{reason}; }
bool ReadBytes(uintptr_t address, void* output, size_t bytes) noexcept {
    SIZE_T read = 0;
    return address >= 0x10000 && bytes && bytes <= UINTPTR_MAX - address &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), output, bytes, &read) && read == bytes;
}
template<class T> bool Read(uintptr_t address, T& output) noexcept { return ReadBytes(address, &output, sizeof(output)); }
template<class T> uintptr_t Address(T function) noexcept {
    uintptr_t address = 0; static_assert(sizeof(address) == sizeof(function));
    std::memcpy(&address, &function, sizeof(address)); return address;
}
using Original = void (*)(void*);
struct Context {
    uintptr_t object = 0, caller = 0, teb = 0, tlsArray = 0, tlsBlock = 0, arena = 0;
    uint32_t thread = 0, index = 0, mainRole = 0, role = 0;
    int32_t arenaIndex = -1, arenaSlot = -1;
};
struct AtomicContext {
    std::atomic<uintptr_t> object{0}, caller{0}, teb{0}, tlsArray{0}, tlsBlock{0}, arena{0};
    std::atomic<uint32_t> thread{0}, index{0}, mainRole{0}, role{0};
    std::atomic<int32_t> arenaIndex{-1}, arenaSlot{-1};
};
struct Registration {
    HMODULE owner;
    EngineDispatchCallback callback;
    uint64_t token;
};
struct CallbackRegistry {
    std::mutex lifecycle;
    std::vector<std::unique_ptr<Registration>> resident;
    std::atomic<const Registration*> published{nullptr};
    std::atomic<uint64_t> token{0}, started{0}, completed{0};
    std::atomic<uint32_t> inFlight{0}, failure{0};
    std::atomic<bool> retiring{false};
    uint64_t nextToken = 1;
};
bool RegistryExecutionAllowed(const CallbackRegistry& registry) noexcept {
    return registry.published.load() && !registry.retiring.load() && !registry.failure.load();
}
bool CallbackOwnedBy(HMODULE expectedOwner, HMODULE owner, uintptr_t callback, bool pin) noexcept {
    if (!owner || owner != expectedOwner || !callback) return false;
    HMODULE actualOwner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(callback), &actualOwner) || actualOwner != owner) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void*>(callback), &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || memory.AllocationBase != owner ||
        (memory.Protect != PAGE_EXECUTE_READ && memory.Protect != PAGE_EXECUTE)) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(owner);
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!Read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 ||
        !Read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.NumberOfSections > 64) return false;
    bool executable = false;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        const uintptr_t at = base + dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader + i * sizeof(section);
        if (!Read(at, section)) return false;
        if (callback >= base + section.VirtualAddress && callback - base - section.VirtualAddress < section.Misc.VirtualSize)
            executable = (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && !(section.Characteristics & IMAGE_SCN_MEM_WRITE);
    }
    if (!executable) return false;
    if (!pin) return true;
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(callback), &pinned) && pinned == owner;
}
uint64_t PublishRegistration(CallbackRegistry& registry, HMODULE owner, EngineDispatchCallback callback) {
    std::lock_guard lock(registry.lifecycle);
    if (!callback || registry.failure.load() || registry.published.load() || registry.inFlight.load() || registry.token.load() || registry.retiring.load()) return 0;
    if (!registry.nextToken) return 0;
    const uint64_t token = registry.nextToken++;
    auto record = std::make_unique<Registration>(Registration{owner, callback, token});
    const auto* pointer = record.get(); registry.resident.push_back(std::move(record));
    registry.token.store(token); registry.published.store(pointer);
    return token;
}
bool RevokeRegistration(CallbackRegistry& registry, uint64_t token, DWORD timeoutMs, bool mayWait) noexcept {
    try {
        std::unique_lock<std::mutex> lock;
        if (mayWait) lock = std::unique_lock<std::mutex>(registry.lifecycle);
        if (!token || registry.token.load() != token) return false;
        const Registration* pointer = registry.published.load();
        if (pointer && pointer->token != token) return false;
        if (pointer) {
            if (!registry.published.compare_exchange_strong(pointer, nullptr)) return false;
            registry.retiring.store(true);
        }
        if (!mayWait) return false;
        const auto started = GetTickCount64();
        const DWORD bounded = timeoutMs > 2000 ? 2000 : timeoutMs;
        do {
            if (!registry.inFlight.load()) {
                if (registry.token.load() != token || registry.published.load()) return false;
                registry.token.store(0); registry.retiring.store(false); return true;
            }
            if (GetTickCount64() - started >= bounded) return false;
            Sleep(1);
        } while (true);
    } catch (...) { return false; }
}
void FailRegistration(CallbackRegistry& registry, uint64_t token, uint32_t reason) noexcept {
    if (!token || registry.token.load() != token) return;
    const Registration* pointer = registry.published.load();
    if (pointer && pointer->token == token && registry.published.compare_exchange_strong(pointer, nullptr)) {
        registry.failure.store(reason ? reason : 1); registry.retiring.store(true);
    }
}
template<class Eligible> bool InvokeRegistration(CallbackRegistry& registry, Eligible&& eligible) noexcept {
    struct Flight {
        CallbackRegistry& registry;
        explicit Flight(CallbackRegistry& registry) : registry(registry) { registry.inFlight.fetch_add(1); }
        ~Flight() { registry.inFlight.fetch_sub(1); }
    } flight(registry);
    // These atomics intentionally use the default sequentially-consistent
    // order: retirement can safely distinguish readers that acquired an old
    // record from future readers that must observe the cleared publication.
    const auto* record = registry.published.load();
    if (!record || !RegistryExecutionAllowed(registry) || !eligible()) return false;
    registry.started.fetch_add(1);
    record->callback();
    registry.completed.fetch_add(1);
    return true;
}
struct State {
    std::atomic<EngineDispatchStatus> status{EngineDispatchStatus::NotStarted};
    std::atomic<uint32_t> reason{None}, error{0};
    std::atomic<bool> configured{false}, stopping{false}, verified{false}, pinned{false}, installed{false}, protectionPending{false}, ready{false};
    std::atomic<uint32_t> engineThread{0};
    std::atomic<uintptr_t> activeScope{0};
    std::atomic<bool> shutdownRequested{false};
    CallbackRegistry callbacks;
    std::atomic<uint64_t> calls{0}, valid{0}, rejected{0}, consecutive{0}, wrongObject{0}, wrongCaller{0}, wrongRole{0}, wrongThread{0}, unreadable{0}, dropped{0};
    std::atomic_flag publishing = ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> sequence{0};
    AtomicContext observed;
    uintptr_t base = 0, object = 0, table = 0, slot = 0, original = 0, replacement = 0;
    DWORD pageProtection = 0;
    HMODULE owner = nullptr;
    HANDLE stopEvent = nullptr, worker = nullptr;
    fs::path output;
    std::mutex lifecycle, reportMutex;
};
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uintptr_t>::is_always_lock_free,
              "Frame observations require lock-free x64 atomics");
// Callback state is intentionally never destroyed, including after rollback.
// The callback module is pinned before its address is written to a vtable.
State& Data() { static State* const value = new State; return *value; }
struct CallbackScope { bool active = false; uintptr_t object = 0, caller = 0; uint32_t thread = 0; };
struct LastErrorGuard {
    DWORD value = GetLastError();
    ~LastErrorGuard() { SetLastError(value); }
};
bool OnCurrentStack(uintptr_t address, size_t bytes) noexcept {
    // NT_TIB describes this thread/fiber's current stack. The dispatcher has
    // exactly one verified engine thread; no private TLS slot is needed to
    // distinguish its live stack scope from a caller on another thread.
    const auto base = static_cast<uintptr_t>(__readgsqword(0x08));
    const auto limit = static_cast<uintptr_t>(__readgsqword(0x10));
    return limit && limit < base && address >= limit && address < base && bytes <= base - address;
}
struct ScopeGuard {
    State& state;
    CallbackScope current;
    bool active = false;
    ScopeGuard(State& state, const Context& context) noexcept : state(state),
        current{true, context.object, context.caller, context.thread} {
        const auto thread = GetCurrentThreadId();
        uintptr_t empty = 0;
        if (state.configured.load(std::memory_order_acquire) && thread == context.thread &&
            thread == state.engineThread.load(std::memory_order_acquire) &&
            OnCurrentStack(reinterpret_cast<uintptr_t>(&current), sizeof(current)))
            active = state.activeScope.compare_exchange_strong(empty, reinterpret_cast<uintptr_t>(&current),
                std::memory_order_acq_rel);
    }
    ~ScopeGuard() {
        if (!active) return;
        uintptr_t owned = reinterpret_cast<uintptr_t>(&current);
        if (!state.activeScope.compare_exchange_strong(owned, 0, std::memory_order_acq_rel)) {
            state.reason.store(PrivateScopeUnavailable);
            state.stopping.store(true); state.ready.store(false, std::memory_order_release);
            state.status.store(EngineDispatchStatus::Refused);
        }
    }
};
bool OwnedScopeActive(State& state, CallbackScope* copy = nullptr) noexcept {
    if (!state.configured.load(std::memory_order_acquire)) return false;
    const auto thread = GetCurrentThreadId();
    if (!state.engineThread.load() || thread != state.engineThread.load()) return false;
    const auto known = state.activeScope.load(std::memory_order_acquire);
    if (!OnCurrentStack(known, sizeof(CallbackScope))) return false;
    CallbackScope active;
    if (!Read(known, active) || !active.active || active.thread != thread || active.object != state.object || active.caller != state.base + kCaller) return false;
    if (copy) *copy = active;
    return true;
}
bool BindingStillOwned(State& state) noexcept {
    uintptr_t slot = 0, table = 0;
    std::array<unsigned char, 3> method{};
    return state.configured.load(std::memory_order_acquire) && Read(state.slot, slot) && slot == state.replacement &&
        Read(state.object, table) && table == state.table && ReadBytes(state.original, method.data(), method.size()) &&
        method[0] == 0xc2 && method[1] == 0 && method[2] == 0;
}
bool ReadContext(State& state, Context& context) noexcept {
    context.thread = GetCurrentThreadId();
    context.teb = static_cast<uintptr_t>(__readgsqword(0x30));
    context.tlsArray = static_cast<uintptr_t>(__readgsqword(0x58));
    if (!Read(state.base + kTlsIndex, context.index) || context.index >= 4096 || !context.tlsArray ||
        !Read(context.tlsArray + static_cast<uintptr_t>(context.index) * sizeof(void*), context.tlsBlock) || !context.tlsBlock ||
        !Read(state.base + kMainRole, context.mainRole) || !Read(context.tlsBlock + 0xb0, context.role) ||
        !Read(context.tlsBlock + 0x20, context.arenaIndex) || !Read(context.tlsBlock + 0x24, context.arenaSlot)) return false;
    if (context.mainRole && context.role == context.mainRole) context.arena = state.base + kMainArena;
    else if (context.arenaIndex >= 0 && context.arenaIndex < 2 && context.arenaSlot >= 0 && context.arenaSlot < 16)
        context.arena = state.base + 0xbd24df8 + static_cast<uintptr_t>(context.arenaIndex) * 0x5a8 + static_cast<uintptr_t>(context.arenaSlot) * 0x50;
    uint32_t used = 0, marker = 0;
    return context.arena && Read(context.arena + 0x0c, used) && Read(context.arena + 0x28, marker);
}
void Publish(State& state, const Context& context) noexcept {
    if (state.publishing.test_and_set(std::memory_order_acquire)) { state.dropped.fetch_add(1); return; }
    state.sequence.fetch_add(1, std::memory_order_acq_rel);
#define STORE(field, value) state.observed.field.store(context.value, std::memory_order_relaxed)
    STORE(object, object); STORE(caller, caller); STORE(teb, teb); STORE(tlsArray, tlsArray); STORE(tlsBlock, tlsBlock); STORE(arena, arena);
    STORE(thread, thread); STORE(index, index); STORE(mainRole, mainRole); STORE(role, role); STORE(arenaIndex, arenaIndex); STORE(arenaSlot, arenaSlot);
#undef STORE
    state.sequence.fetch_add(1, std::memory_order_release);
    state.publishing.clear(std::memory_order_release);
}
__attribute__((noinline)) void FrameHook(void* object) noexcept {
    LastErrorGuard lastError;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    auto& state = Data();
    if (!state.configured.load(std::memory_order_acquire)) return;
    // Preserve the original verified no-op method and its ordinary Win64 ABI.
    reinterpret_cast<Original>(state.original)(object);
    const auto phase = state.status.load(std::memory_order_acquire);
    if (!state.installed.load(std::memory_order_acquire) || state.stopping.load(std::memory_order_acquire) ||
        state.protectionPending.load(std::memory_order_acquire) ||
        (phase != EngineDispatchStatus::Observing && phase != EngineDispatchStatus::ContextObserved)) return;
    state.calls.fetch_add(1, std::memory_order_relaxed);
    Context context; context.object = reinterpret_cast<uintptr_t>(object); context.caller = caller;
    bool good = true;
    if (state.activeScope.load(std::memory_order_acquire) && GetCurrentThreadId() == state.engineThread.load()) {
        state.dropped.fetch_add(1); good = false;
    }
    if (context.object != state.object) { state.wrongObject.fetch_add(1); good = false; }
    if (caller != state.base + kCaller) { state.wrongCaller.fetch_add(1); good = false; }
    if (!ReadContext(state, context)) { state.unreadable.fetch_add(1); good = false; }
    if (!context.mainRole || context.role != context.mainRole) { state.wrongRole.fetch_add(1); good = false; }
    if (good) {
        uint32_t expected = 0;
        state.engineThread.compare_exchange_strong(expected, context.thread, std::memory_order_acq_rel);
        if (state.engineThread.load(std::memory_order_acquire) != context.thread) { state.wrongThread.fetch_add(1); good = false; }
    }
    Publish(state, context);
    if (!good || !state.installed.load(std::memory_order_acquire)) {
        state.rejected.fetch_add(1); state.consecutive.store(0); state.ready.store(false);
        FailRegistration(state.callbacks, state.callbacks.token.load(), 0x10001);
        return;
    }
    ScopeGuard guard(state, context);
    if (!guard.active) {
        state.rejected.fetch_add(1); state.consecutive.store(0); state.ready.store(false); state.stopping.store(true);
        state.reason.store(PrivateScopeUnavailable); state.status.store(EngineDispatchStatus::Refused); return;
    }
    state.valid.fetch_add(1, std::memory_order_relaxed);
    const auto count = state.consecutive.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (count >= kRequiredFrames && !state.stopping.load(std::memory_order_acquire)) {
        state.ready.store(true, std::memory_order_release);
        state.status.store(EngineDispatchStatus::ContextObserved, std::memory_order_release);
    }
    // A null registration remains observation-only. Callback publication can
    // occur only through the separately gated API after context observation.
    InvokeRegistration(state.callbacks, [&]() noexcept {
        const bool eligible = IsWithinVerifiedEngineCallback();
        if (!eligible) FailRegistration(state.callbacks, state.callbacks.token.load(), 0x10001);
        return eligible;
    });
}
std::string Hash(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD bytes = 0, read = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes), &read, 0) >= 0;
    std::vector<unsigned char> object(bytes), buffer(1024 * 1024); std::array<unsigned char, 32> digest{};
    ok = ok && BCryptCreateHash(algorithm, &hash, object.data(), bytes, nullptr, 0, 0) >= 0;
    std::ifstream input(path, std::ios::binary); ok = ok && bool(input);
    while (ok && input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        if (input.gcount() && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(input.gcount()), 0) < 0) ok = false;
    }
    ok = ok && !input.bad() && BCryptFinishHash(hash, digest.data(), digest.size(), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream result;
    for (unsigned c : digest) result << std::hex << std::setw(2) << std::setfill('0') << c;
    return result.str();
}
fs::path ModulePath(HMODULE module) {
    wchar_t path[32768]{}; const DWORD length = GetModuleFileNameW(module, path, 32768);
    Require(length && length < 32768, WrongExecutable); return fs::path(std::wstring(path, length));
}
void Validate(State& state) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const auto path = ModulePath(module);
    Require(CompareStringOrdinal(path.filename().c_str(), -1, L"Spider-Man2.exe", -1, TRUE) == CSTR_EQUAL &&
        module == GetModuleHandleW(L"Spider-Man2.exe"), WrongExecutable);
    Require(SupportedExecutable::IsSupported(Hash(path)), WrongFileHash);
    state.base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos{}; Require(Read(state.base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 0x1000, WrongImage);
    IMAGE_NT_HEADERS64 nt{}; Require(Read(state.base + dos.e_lfanew, nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 && nt.FileHeader.TimeDateStamp == kTimestamp &&
        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && nt.OptionalHeader.SizeOfImage == kImageSize &&
        nt.FileHeader.NumberOfSections > 0 && nt.FileHeader.NumberOfSections <= 32, WrongImage);
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    const uintptr_t table = state.base + dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader;
    Require(ReadBytes(table, sections.data(), sections.size() * sizeof(sections[0])), WrongSections);
    auto inSection = [&](uintptr_t rva, size_t size, const char* name, DWORD required, DWORD forbidden) {
        for (const auto& s : sections) if (std::strncmp(reinterpret_cast<const char*>(s.Name), name, 8) == 0)
            return rva >= s.VirtualAddress && size <= s.Misc.VirtualSize && rva - s.VirtualAddress <= s.Misc.VirtualSize - size &&
                (s.Characteristics & required) == required && !(s.Characteristics & forbidden);
        return false;
    };
    Require(inSection(kVtable - 8, 48, ".rdata", IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE) &&
        inSection(kObject, 8, ".data", IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) &&
        inSection(kMainRole, 4, ".data", IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) &&
        inSection(kMainArena, 0x30, ".data", IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) &&
        inSection(kMethod, 16, ".text", IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE) &&
        inSection(kCaller - 9, 16, ".text", IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE), WrongSections);
    const std::array<unsigned char, 16> method{{0xc2,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}};
    const std::array<unsigned char, 16> call{{0x48,0x8b,0x07,0x48,0x8b,0xcf,0xff,0x50,0x20,0x48,0x8d,0x0d,0xff,0x68,0x5c,0x09}};
    const std::array<unsigned char, 37> checker{{0x8b,0x0d,0x7a,0x61,0x2c,0x09,0x65,0x48,0x8b,0x04,0x25,0x58,0,0,0,0xba,0xb0,0,0,0,0x48,0x8b,0x04,0xc8,0x8b,0x0c,0x02,0x39,0x0d,0x07,0x51,0xf9,0x06,0x0f,0x94,0xc0,0xc3}};
    std::array<unsigned char, 37> live{};
    Require(ReadBytes(state.base + kMethod, live.data(), method.size()) && std::memcmp(live.data(), method.data(), method.size()) == 0 &&
        ReadBytes(state.base + kCaller - 9, live.data(), call.size()) && std::memcmp(live.data(), call.data(), call.size()) == 0 &&
        ReadBytes(state.base + 0x3083b70, live.data(), checker.size()) && live == checker, WrongCode);
    const auto& unwind = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    Require(unwind.Size && unwind.Size <= 16 * 1024 * 1024 && unwind.Size % sizeof(RUNTIME_FUNCTION) == 0 &&
        unwind.VirtualAddress < kImageSize && unwind.Size <= kImageSize - unwind.VirtualAddress, WrongUnwind);
    std::vector<RUNTIME_FUNCTION> functions(unwind.Size / sizeof(RUNTIME_FUNCTION));
    Require(ReadBytes(state.base + unwind.VirtualAddress, functions.data(), unwind.Size), WrongUnwind);
    bool frame = false, leaf = true;
    for (const auto& f : functions) {
        if (f.BeginAddress == kFrameBegin && f.EndAddress == kFrameEnd && f.BeginAddress <= kCaller && kCaller < f.EndAddress) frame = true;
        if (f.BeginAddress <= kMethod && kMethod < f.EndAddress) leaf = false;
    }
    Require(frame && leaf, WrongUnwind);
    const auto& tls = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    IMAGE_TLS_DIRECTORY64 tlsDirectory{};
    Require(tls.Size >= sizeof(tlsDirectory) && tls.VirtualAddress < kImageSize &&
        sizeof(tlsDirectory) <= kImageSize - tls.VirtualAddress && Read(state.base + tls.VirtualAddress, tlsDirectory) &&
        tlsDirectory.AddressOfIndex == state.base + kTlsIndex, WrongTlsDirectory);
    state.object = state.base + kObject; state.table = state.base + kVtable; state.slot = state.table + 0x20;
    state.original = state.base + kMethod; state.replacement = Address(&FrameHook);
    uintptr_t vptr = 0; Require(Read(state.object, vptr) && vptr == state.table, WrongObjectVtable);
    const std::array<uintptr_t, 5> expected{{state.base + 0x373c80, state.base + 0x373cb0, state.base + 0x3507910, state.base + 0x3507a00, state.original}};
    std::array<uintptr_t, 5> actual{};
    Require(ReadBytes(state.table, actual.data(), sizeof(actual)) && actual == expected, WrongVtableContents);
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(reinterpret_cast<void*>(state.slot), &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT &&
        memory.Type == MEM_IMAGE && memory.AllocationBase == module && memory.Protect == PAGE_READONLY &&
        state.slot % alignof(void*) == 0, WrongVtableMemory);
    state.pageProtection = memory.Protect;
    state.verified.store(true, std::memory_order_release);
    state.configured.store(true, std::memory_order_release);
}
void RestoreProtection(State& state) noexcept {
    if (!state.protectionPending.load(std::memory_order_acquire)) return;
    DWORD ignored = 0;
    if (VirtualProtect(reinterpret_cast<void*>(state.slot), sizeof(void*), state.pageProtection, &ignored)) state.protectionPending.store(false, std::memory_order_release);
}
void Rollback(State& state) noexcept {
    if (!state.configured.load(std::memory_order_acquire)) return;
    if (state.installed.load(std::memory_order_acquire)) {
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(state.slot), sizeof(void*), PAGE_READWRITE, &old)) {
            state.protectionPending.store(true, std::memory_order_release);
            void* previous = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(state.slot),
                reinterpret_cast<void*>(state.original), reinterpret_cast<void*>(state.replacement));
            // A foreign pointer is never overwritten. Our callback remains
            // pinned even when another owner has already replaced its slot.
            (void)previous;
            state.installed.store(false, std::memory_order_release);
        }
    }
    RestoreProtection(state);
    state.ready.store(false, std::memory_order_release);
}
void WriteReport(State& state) noexcept {
    try {
        std::lock_guard reportLock(state.reportMutex);
        const auto x = GetEngineDispatchObservation();
        const auto temporary = state.output / L"engine-dispatch-observation.tmp";
        const auto destination = state.output / L"engine-dispatch-observation.json";
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << "{\"schema\":1,\"diagnostic_only\":" << (x.diagnosticOnly ? "true" : "false")
            << ",\"game_tls_writes\":0,\"code_patches\":0"
            << ",\"process_id\":" << GetCurrentProcessId() << ",\"report_tick\":" << GetTickCount64()
            << ",\"status\":" << static_cast<uint32_t>(x.status) << ",\"refusal_code\":" << x.refusalCode << ",\"win32_error\":" << x.win32Error
            << ",\"executable_verified\":" << (x.executableVerified ? "true" : "false") << ",\"module_pinned\":" << (x.modulePinned ? "true" : "false")
            << ",\"hook_installed\":" << (x.hookInstalled ? "true" : "false") << ",\"protection_restored\":" << (x.protectionRestored ? "true" : "false")
            << ",\"observation_consistent\":" << (x.observationConsistent ? "true" : "false")
            << ",\"context_ready\":" << (x.contextReady ? "true" : "false") << ",\"required_consecutive_frames\":" << kRequiredFrames
            << ",\"callback_scope_storage\":\"verified_thread_stack\"";
#define FIELD(name, value) out << ",\"" name "\":" << x.value
        FIELD("image_base", imageBase); FIELD("object", object); FIELD("vtable", vtable); FIELD("slot", slot); FIELD("original_method", originalMethod); FIELD("replacement_method", replacementMethod);
        FIELD("expected_caller", expectedCaller); FIELD("observed_object", observedObject); FIELD("observed_caller", observedCaller);
        FIELD("thread_id", threadId); FIELD("engine_thread_id", engineThreadId); FIELD("teb", teb); FIELD("tls_array", tlsArray); FIELD("tls_block", tlsBlock);
        FIELD("tls_index", tlsIndex); FIELD("main_role", mainRole); FIELD("tls_role", tlsRole); FIELD("arena_index", arenaIndex); FIELD("arena_slot", arenaSlot); FIELD("arena", arena);
        FIELD("calls", calls); FIELD("verified_frames", verifiedFrames); FIELD("rejected_frames", rejectedFrames); FIELD("consecutive_verified_frames", consecutiveVerifiedFrames);
        FIELD("wrong_object", wrongObject); FIELD("wrong_caller", wrongCaller); FIELD("wrong_role", wrongRole); FIELD("wrong_thread", wrongThread); FIELD("unreadable_context", unreadableContext); FIELD("dropped_records", droppedRecords);
        FIELD("callback_token", callbackToken); FIELD("callbacks_started", callbacksStarted); FIELD("callbacks_completed", callbacksCompleted);
        FIELD("callbacks_in_flight", callbacksInFlight); FIELD("callback_failure", callbackFailure);
#undef FIELD
        out << ",\"callback_enabled\":" << (x.callbackEnabled ? "true" : "false")
            << ",\"within_callback_on_report_worker\":" << (IsWithinVerifiedEngineCallback() ? "true" : "false") << "}\n";
        out.flush(); const bool written = bool(out); out.close();
        if (written) MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } catch (...) {}
}
DWORD WINAPI ReportWorker(void*) {
    auto& state = Data();
    while (WaitForSingleObject(state.stopEvent, 250) == WAIT_TIMEOUT) {
        if (state.shutdownRequested.exchange(false)) { ShutdownEngineDispatch(); break; }
        if (state.installed.load(std::memory_order_acquire)) {
            uintptr_t pointer = 0;
            const bool readable = Read(state.slot, pointer);
            if (!readable || pointer != state.replacement) {
                state.stopping.store(true); state.ready.store(false); state.reason.store(SlotChangedAfterInstall); state.status.store(EngineDispatchStatus::Refused);
                FailRegistration(state.callbacks, state.callbacks.token.load(), 0x10002);
                if (readable && pointer != state.replacement) state.installed.store(false);
            }
        }
        WriteReport(state);
    }
    WriteReport(state); return 0;
}
}
bool InitializeEngineDispatch(HMODULE owner, EngineDispatchCallback callback) noexcept {
    LastErrorGuard lastError;
    State* pointer = nullptr;
    try { pointer = &Data(); } catch (...) { return false; }
    auto& state = *pointer;
    std::unique_lock<std::mutex> lock;
    try { lock = std::unique_lock<std::mutex>(state.lifecycle); } catch (...) { return false; }
    const auto previousStatus = state.status.load();
    if (previousStatus != EngineDispatchStatus::NotStarted)
        return (previousStatus == EngineDispatchStatus::Observing || previousStatus == EngineDispatchStatus::ContextObserved) &&
            state.installed.load() && !state.stopping.load() && !callback;
    state.status.store(EngineDispatchStatus::Validating);
    try {
        state.owner = owner;
        state.output = ModulePath(owner).parent_path();
        if (state.output.filename() == L"scripts") state.output = state.output.parent_path() / L"SM2DebugMenu" / L"verification" / L"engine-dispatch-probe";
        fs::create_directories(state.output);
        Require(callback == nullptr, CallbackNotAuthorized);
        Validate(state);
        // ScopeGuard uses the verified engine thread's stack. Games may already
        // own every fixed Windows TLS slot; allocating an expansion slot here
        // would otherwise introduce allocation on that thread's first frame.
        HMODULE pinned = nullptr;
        Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(state.replacement), &pinned) && pinned == owner, PinFailed);
        state.pinned.store(true);
        state.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Require(state.stopEvent != nullptr, WorkerFailed);
        state.worker = CreateThread(nullptr, 0, ReportWorker, nullptr, 0, nullptr);
        Require(state.worker != nullptr, WorkerFailed);
        DWORD old = 0;
        Require(VirtualProtect(reinterpret_cast<void*>(state.slot), sizeof(void*), PAGE_READWRITE, &old), ProtectionFailed);
        state.protectionPending.store(true);
        if (old != state.pageProtection) {
            // Restore the protection actually present immediately before our
            // operation, preserving another owner's concurrent protection.
            state.pageProtection = old;
            throw Fault{ProtectionFailed};
        }
        void* previous = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(state.slot), reinterpret_cast<void*>(state.replacement), reinterpret_cast<void*>(state.original));
        if (previous == reinterpret_cast<void*>(state.original)) state.installed.store(true, std::memory_order_release);
        RestoreProtection(state);
        Require(previous == reinterpret_cast<void*>(state.original), ConcurrentSlotChange);
        Require(!state.protectionPending.load(), ProtectionRestoreFailed);
        state.status.store(EngineDispatchStatus::Observing, std::memory_order_release);
        WriteReport(state); return true;
    } catch (const Fault& error) { state.reason.store(error.reason); }
    catch (...) { state.reason.store(WrongImage); }
    state.error.store(GetLastError()); state.stopping.store(true); state.ready.store(false);
    Rollback(state); state.status.store(EngineDispatchStatus::Refused);
    if (state.stopEvent) SetEvent(state.stopEvent);
    WriteReport(state); return false;
}
void ShutdownEngineDispatch() noexcept {
    LastErrorGuard lastError;
    try {
        auto& state = Data();
        if (OwnedScopeActive(state)) {
            state.stopping.store(true); state.ready.store(false);
            RevokeRegistration(state.callbacks, state.callbacks.token.load(), 0, false);
            state.shutdownRequested.store(true); return;
        }
        std::lock_guard lock(state.lifecycle);
        state.stopping.store(true, std::memory_order_release); state.ready.store(false);
        state.status.store(EngineDispatchStatus::Stopping);
        RevokeRegistration(state.callbacks, state.callbacks.token.load(), 2000, true);
        Rollback(state);
        if (state.stopEvent) SetEvent(state.stopEvent);
        if (state.worker && GetThreadId(state.worker) != GetCurrentThreadId() && WaitForSingleObject(state.worker, 2000) == WAIT_OBJECT_0) {
            CloseHandle(state.worker); state.worker = nullptr;
        }
        state.status.store(state.installed.load() || state.protectionPending.load() ? EngineDispatchStatus::Refused : EngineDispatchStatus::Stopped);
        WriteReport(state);
    } catch (...) {}
}
EngineDispatchObservation GetEngineDispatchObservation() noexcept {
    EngineDispatchObservation out;
    try {
        auto& state = Data();
        out.status = state.status.load(); out.refusalCode = state.reason.load(); out.win32Error = state.error.load();
        out.executableVerified = state.verified.load(); out.modulePinned = state.pinned.load();
        out.hookInstalled = state.installed.load(); out.protectionRestored = !state.protectionPending.load(); out.contextReady = state.ready.load();
        if (state.configured.load(std::memory_order_acquire)) {
            out.imageBase = state.base; out.object = state.object; out.vtable = state.table; out.slot = state.slot;
            out.expectedCaller = state.base + kCaller; out.originalMethod = state.original; out.replacementMethod = state.replacement;
        }
        out.engineThreadId = state.engineThread.load(); out.calls = state.calls.load(); out.verifiedFrames = state.valid.load();
        out.rejectedFrames = state.rejected.load(); out.consecutiveVerifiedFrames = state.consecutive.load();
        out.wrongObject = state.wrongObject.load(); out.wrongCaller = state.wrongCaller.load(); out.wrongRole = state.wrongRole.load(); out.wrongThread = state.wrongThread.load();
        out.unreadableContext = state.unreadable.load(); out.droppedRecords = state.dropped.load();
        out.callbackToken = state.callbacks.token.load(); out.callbacksInFlight = state.callbacks.inFlight.load();
        out.callbacksStarted = state.callbacks.started.load(); out.callbacksCompleted = state.callbacks.completed.load();
        out.callbackFailure = state.callbacks.failure.load(); out.callbackEnabled = state.callbacks.published.load() != nullptr;
        out.diagnosticOnly = out.callbackToken == 0 && out.callbacksStarted == 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto before = state.sequence.load(std::memory_order_acquire); if (before & 1) continue;
#define LOAD(field, value) out.field = state.observed.value.load(std::memory_order_relaxed)
            LOAD(observedObject, object); LOAD(observedCaller, caller); LOAD(teb, teb); LOAD(tlsArray, tlsArray); LOAD(tlsBlock, tlsBlock); LOAD(arena, arena);
            LOAD(threadId, thread); LOAD(tlsIndex, index); LOAD(mainRole, mainRole); LOAD(tlsRole, role); LOAD(arenaIndex, arenaIndex); LOAD(arenaSlot, arenaSlot);
#undef LOAD
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == state.sequence.load(std::memory_order_acquire)) { out.observationConsistent = true; break; }
        }
        if (!out.observationConsistent) out.contextReady = false;
    } catch (...) {}
    return out;
}
bool IsWithinVerifiedEngineCallback() noexcept {
    LastErrorGuard lastError;
    try {
        auto& state = Data();
        const auto phase = state.status.load(std::memory_order_acquire);
        if (state.stopping.load() || !state.installed.load() || !state.ready.load() || state.protectionPending.load() ||
            (phase != EngineDispatchStatus::Observing && phase != EngineDispatchStatus::ContextObserved) || !RegistryExecutionAllowed(state.callbacks)) return false;
        CallbackScope active;
        if (!OwnedScopeActive(state, &active) || !BindingStillOwned(state)) return false;
        Context current;
        return ReadContext(state, current) && current.mainRole && current.mainRole == current.role && current.thread == active.thread;
    } catch (...) { return false; }
}
bool RegisterEngineDispatchCallback(HMODULE owner, EngineDispatchCallback callback, uint64_t* token) noexcept {
    LastErrorGuard lastError;
    if (token) *token = 0;
    if (!token || !callback) return false;
    try {
        auto& state = Data();
        if (OwnedScopeActive(state)) return false;
        std::lock_guard lock(state.lifecycle);
        if (state.status.load() != EngineDispatchStatus::ContextObserved || !state.ready.load() || state.stopping.load() ||
            !state.verified.load() || !state.pinned.load() || !state.installed.load() || state.protectionPending.load() ||
            state.consecutive.load() < kRequiredFrames || !BindingStillOwned(state) ||
            !CallbackOwnedBy(state.owner, owner, Address(callback), true)) return false;
        if (!state.ready.load() || state.stopping.load()) return false;
        *token = PublishRegistration(state.callbacks, owner, callback);
        return *token != 0;
    } catch (...) { return false; }
}
bool EngineDispatchCallbacksAvailable() noexcept {
    LastErrorGuard lastError;
    try {
        auto& state = Data();
        return state.status.load() == EngineDispatchStatus::ContextObserved && state.ready.load() && !state.stopping.load() &&
            state.installed.load() && !state.protectionPending.load() && !state.callbacks.failure.load() &&
            state.callbacks.published.load() && !state.callbacks.retiring.load() && BindingStillOwned(state);
    } catch (...) { return false; }
}
bool UnregisterEngineDispatchCallback(uint64_t token, DWORD timeoutMs) noexcept {
    LastErrorGuard lastError;
    try {
        auto& state = Data();
        return RevokeRegistration(state.callbacks, token, timeoutMs, !OwnedScopeActive(state));
    } catch (...) { return false; }
}
void ReportEngineDispatchCallbackFailure(uint64_t token, uint32_t reason) noexcept {
    LastErrorGuard lastError;
    try { auto& state = Data(); FailRegistration(state.callbacks, token, reason); } catch (...) {}
}
}
