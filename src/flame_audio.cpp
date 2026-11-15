// SPDX-License-Identifier: GPL-3.0-or-later
#include "flame_audio.h"
#include <mmsystem.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace sm2debug::flame_audio {
namespace {
constexpr ULONGLONG kHeartbeatMs = 200;
constexpr DWORD kPollMs = 20;
constexpr std::size_t kMaxWaveBytes = 4 * 1024 * 1024;
enum class State { Stopped, Loading, Ready, Playing, Stopping, ResourceError,
                   FormatError, DeviceError, WorkerError, CleanupPending };
std::atomic<State> g_state{State::Stopped};
std::atomic<unsigned> g_error{0};
std::atomic<bool> g_accepting{false}, g_stop{false};
std::atomic<ULONGLONG> g_heartbeat{0};
static_assert(std::atomic<ULONGLONG>::is_always_lock_free, "x64 lock-free heartbeat required");
SRWLOCK g_control = SRWLOCK_INIT;
HANDLE g_thread = nullptr, g_wake = nullptr;
HMODULE g_sourceModule = nullptr;
unsigned g_resourceId = 0;

void Fail(State state, unsigned error) noexcept {
    g_error.store(error, std::memory_order_relaxed);
    g_state.store(state, std::memory_order_release);
}
std::uint16_t U16(const unsigned char* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}
std::uint32_t U32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(U16(p)) | (static_cast<std::uint32_t>(U16(p + 2)) << 16);
}
struct WaveView { WAVEFORMATEX format{}; std::size_t offset = 0; DWORD bytes = 0; };
// Strict bounded RIFF parsing. The original PCM bytes are never transformed.
bool ParseWave(const unsigned char* data, std::size_t bytes, WaveView& out) noexcept {
    out = {};
    if (!data || bytes < 44 || bytes > kMaxWaveBytes ||
        std::memcmp(data, "RIFF", 4) || std::memcmp(data + 8, "WAVE", 4) ||
        static_cast<std::uint64_t>(U32(data + 4)) + 8 != bytes) return false;
    bool haveFormat = false, haveData = false;
    for (std::size_t pos = 12; pos < bytes;) {
        if (bytes - pos < 8) return false;
        const auto length = static_cast<std::size_t>(U32(data + pos + 4));
        const auto begin = pos + 8;
        if (length > bytes - begin) return false;
        if (!std::memcmp(data + pos, "fmt ", 4)) {
            if (haveFormat || (length != 16 && length != 18)) return false;
            const auto* f = data + begin;
            if (U16(f) != WAVE_FORMAT_PCM || U16(f + 2) != 1 || U32(f + 4) != 24000 ||
                U32(f + 8) != 48000 || U16(f + 12) != 2 || U16(f + 14) != 16 ||
                (length == 18 && U16(f + 16) != 0)) return false;
            out.format.wFormatTag = WAVE_FORMAT_PCM; out.format.nChannels = 1;
            out.format.nSamplesPerSec = 24000; out.format.nAvgBytesPerSec = 48000;
            out.format.nBlockAlign = 2; out.format.wBitsPerSample = 16;
            haveFormat = true;
        } else if (!std::memcmp(data + pos, "data", 4)) {
            if (haveData || length == 0 || (length & 1) || length > MAXDWORD) return false;
            out.offset = begin; out.bytes = static_cast<DWORD>(length); haveData = true;
        }
        pos = begin + length;
        if (length & 1) { if (pos == bytes) return false; ++pos; }
    }
    return haveFormat && haveData;
}
bool WantsPlayback(ULONGLONG heartbeat, ULONGLONG now, bool stopping) noexcept {
    const auto stamp = heartbeat >> 1;
    return !stopping && (heartbeat & 1) && now >= stamp && now - stamp < kHeartbeatMs;
}

struct Api {
    HMODULE module = nullptr;
    decltype(&waveOutOpen) open = nullptr;
    decltype(&waveOutPrepareHeader) prepare = nullptr;
    decltype(&waveOutWrite) write = nullptr;
    decltype(&waveOutReset) reset = nullptr;
    decltype(&waveOutUnprepareHeader) unprepare = nullptr;
    decltype(&waveOutClose) close = nullptr;
    template<class T> bool Resolve(T& fn, const char* name) noexcept {
        const auto address = GetProcAddress(module, name);
        static_assert(sizeof(fn) == sizeof(address));
        std::memcpy(&fn, &address, sizeof(fn)); return fn != nullptr;
    }
    bool Load() noexcept {
        wchar_t path[MAX_PATH + 16]{};
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        constexpr wchar_t suffix[] = L"\\winmm.dll";
        if (!n || n >= MAX_PATH) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return false; }
        std::memcpy(path + n, suffix, sizeof(suffix));
        module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return false;
        wchar_t actual[MAX_PATH + 16]{};
        const DWORD count = GetModuleFileNameW(module, actual, MAX_PATH + 16);
        if (!count || count >= MAX_PATH + 16 || _wcsicmp(path, actual)) {
            SetLastError(ERROR_INVALID_DLL); return false;
        }
        if (!Resolve(open, "waveOutOpen") || !Resolve(prepare, "waveOutPrepareHeader") ||
            !Resolve(write, "waveOutWrite") || !Resolve(reset, "waveOutReset") ||
            !Resolve(unprepare, "waveOutUnprepareHeader") || !Resolve(close, "waveOutClose")) {
            SetLastError(ERROR_PROC_NOT_FOUND); return false;
        }
        return true;
    }
    ~Api() { if (module) FreeLibrary(module); }
};
struct Session {
    Api& api;
    HWAVEOUT device = nullptr;
    WAVEHDR header{};
    bool prepared = false, playing = false;
    explicit Session(Api& functions) noexcept : api(functions) {}
    MMRESULT Open(const WaveView& wave, unsigned char* data) noexcept {
        auto result = api.open(&device, WAVE_MAPPER, &wave.format, 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) { device = nullptr; return result; }
        header.lpData = reinterpret_cast<LPSTR>(data + wave.offset);
        header.dwBufferLength = wave.bytes;
        header.dwFlags = WHDR_BEGINLOOP | WHDR_ENDLOOP;
        header.dwLoops = MAXDWORD;
        result = api.prepare(device, &header, sizeof(header));
        prepared = result == MMSYSERR_NOERROR; return result;
    }
    MMRESULT SetPlaying(bool desired) noexcept {
        if (desired == playing) return MMSYSERR_NOERROR;
        if (desired) {
            if (!device || !prepared) return MMSYSERR_INVALHANDLE;
            // Reset completed before a restart; never rewrite an owned header.
            header.dwFlags = WHDR_PREPARED | WHDR_BEGINLOOP | WHDR_ENDLOOP;
            header.dwLoops = MAXDWORD;
            const auto result = api.write(device, &header, sizeof(header));
            if (result == MMSYSERR_NOERROR) playing = true;
            return result;
        }
        const auto result = api.reset(device);
        if (result == MMSYSERR_NOERROR) playing = false;
        return result;
    }
    // Returns false without destroying buffers or unloading API code while a
    // driver may still own the header. Only the worker retries this operation.
    bool Close() noexcept {
        if (!device) return true;
        if (playing) {
            const auto code = SetPlaying(false);
            if (code != MMSYSERR_NOERROR) { Fail(State::CleanupPending, code); return false; }
        }
        if (prepared) {
            const auto code = api.unprepare(device, &header, sizeof(header));
            if (code != MMSYSERR_NOERROR) { Fail(State::CleanupPending, code); api.reset(device); return false; }
            prepared = false;
        }
        const auto code = api.close(device);
        if (code != MMSYSERR_NOERROR) { Fail(State::CleanupPending, code); return false; }
        device = nullptr; return true;
    }
};
struct Context { std::vector<unsigned char> bytes; WaveView wave; HANDLE wake = nullptr; HMODULE codeModule = nullptr; };

void Run(Context& context) noexcept {
    Api api;
    if (!api.Load()) { Fail(State::DeviceError, GetLastError()); return; }
    Session session(api);
    auto result = session.Open(context.wave, context.bytes.data());
    if (result != MMSYSERR_NOERROR) Fail(State::DeviceError, result);
    else {
        g_state.store(State::Ready, std::memory_order_release);
        while (!g_stop.load(std::memory_order_acquire)) {
            const bool desired = WantsPlayback(g_heartbeat.load(std::memory_order_acquire), GetTickCount64(), false);
            result = session.SetPlaying(desired);
            if (result != MMSYSERR_NOERROR) { Fail(State::DeviceError, result); break; }
            g_state.store(session.playing ? State::Playing : State::Ready, std::memory_order_release);
            const auto wait = WaitForSingleObject(context.wake, kPollMs);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) { Fail(State::WorkerError, GetLastError()); break; }
        }
    }
    g_accepting.store(false, std::memory_order_release);
    // Driver cleanup can stall or transiently refuse ownership release. Keep
    // all state alive; Shutdown's caller-side join remains bounded.
    while (!session.Close()) Sleep(kPollMs);
    if (g_error.load(std::memory_order_relaxed) == 0) g_state.store(State::Stopped, std::memory_order_release);
    else if (g_state.load(std::memory_order_relaxed) == State::CleanupPending)
        g_state.store(State::DeviceError, std::memory_order_release);
}
void Reap() noexcept;
DWORD WINAPI Worker(void* argument) noexcept {
    auto* context = static_cast<Context*>(argument);
    const auto module = context->codeModule;
    Run(*context);
    g_accepting.store(false, std::memory_order_release);
    // Shutdown waits on its own duplicated thread handle without this lock.
    // Closing our originals here also handles a caller that timed out and
    // never retries shutdown. The module reference survives until thread exit.
    AcquireSRWLockExclusive(&g_control);
    Reap();
    ReleaseSRWLockExclusive(&g_control);
    delete context;
    FreeLibraryAndExitThread(module, 0);
}
void Reap() noexcept {
    if (g_thread) CloseHandle(g_thread);
    if (g_wake) CloseHandle(g_wake);
    g_thread = g_wake = nullptr; g_sourceModule = nullptr; g_resourceId = 0;
}
} // namespace

bool Initialize(HMODULE resourceModule, unsigned resourceId) noexcept {
    AcquireSRWLockExclusive(&g_control);
    if (g_thread) {
        if (WaitForSingleObject(g_thread, 0) != WAIT_OBJECT_0) {
            const bool same = g_accepting.load(std::memory_order_acquire) &&
                              g_sourceModule == resourceModule && g_resourceId == resourceId;
            ReleaseSRWLockExclusive(&g_control); return same;
        }
        Reap();
    }
    g_error.store(0, std::memory_order_relaxed); g_state.store(State::Loading, std::memory_order_release);
    g_accepting.store(false, std::memory_order_release); g_heartbeat.store(0, std::memory_order_release);
    g_stop.store(false, std::memory_order_release);
    Context* context = nullptr;
    bool ready = false;
    do {
        if (!resourceModule || resourceId == 0 || resourceId > 0xffff) { Fail(State::ResourceError, ERROR_INVALID_PARAMETER); break; }
        const auto resource = FindResourceW(resourceModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!resource) { Fail(State::ResourceError, GetLastError()); break; }
        const auto length = SizeofResource(resourceModule, resource);
        const auto loaded = LoadResource(resourceModule, resource);
        const auto* bytes = static_cast<const unsigned char*>(loaded ? LockResource(loaded) : nullptr);
        WaveView view;
        if (!ParseWave(bytes, length, view)) { Fail(State::FormatError, ERROR_BAD_FORMAT); break; }
        context = new(std::nothrow) Context;
        if (!context) { Fail(State::WorkerError, ERROR_NOT_ENOUGH_MEMORY); break; }
        try { context->bytes.assign(bytes, bytes + length); } catch (...) { Fail(State::WorkerError, ERROR_NOT_ENOUGH_MEMORY); break; }
        context->wave = view;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&Worker), &context->codeModule)) { Fail(State::WorkerError, GetLastError()); break; }
        g_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr); context->wake = g_wake;
        if (!g_wake) { Fail(State::WorkerError, GetLastError()); break; }
        g_sourceModule = resourceModule; g_resourceId = resourceId;
        g_accepting.store(true, std::memory_order_release);
        g_thread = CreateThread(nullptr, 0, Worker, context, 0, nullptr);
        if (!g_thread) { g_accepting.store(false, std::memory_order_release); Fail(State::WorkerError, GetLastError()); break; }
        context = nullptr; ready = true;
    } while (false);
    if (!ready) {
        if (context && context->codeModule) FreeLibrary(context->codeModule);
        delete context; Reap();
    }
    ReleaseSRWLockExclusive(&g_control); return ready;
}
void Update(bool active) noexcept {
    if (g_accepting.load(std::memory_order_acquire))
        g_heartbeat.store((GetTickCount64() << 1) | (active ? 1ull : 0ull), std::memory_order_release);
}
bool Shutdown(unsigned timeoutMs) noexcept {
    AcquireSRWLockExclusive(&g_control);
    g_accepting.store(false, std::memory_order_release); g_heartbeat.store(0, std::memory_order_release);
    g_stop.store(true, std::memory_order_release);
    if (!g_thread) { ReleaseSRWLockExclusive(&g_control); return true; }
    if (g_error.load(std::memory_order_relaxed) == 0) g_state.store(State::Stopping, std::memory_order_release);
    SetEvent(g_wake);
    HANDLE join = nullptr;
    const bool duplicated = DuplicateHandle(GetCurrentProcess(), g_thread, GetCurrentProcess(),
                                            &join, SYNCHRONIZE, FALSE, 0) != FALSE;
    ReleaseSRWLockExclusive(&g_control);
    if (!duplicated) return false;
    // Even an accidental INFINITE argument cannot turn shutdown into a hang.
    const auto wait = WaitForSingleObject(join, timeoutMs > 2000 ? 2000 : timeoutMs);
    const bool finished = wait == WAIT_OBJECT_0;
    CloseHandle(join); return finished;
}
const char* Status() noexcept {
    switch (g_state.load(std::memory_order_acquire)) {
        case State::Stopped: return "stopped"; case State::Loading: return "loading";
        case State::Ready: return "ready"; case State::Playing: return "playing";
        case State::Stopping: return "stopping"; case State::ResourceError: return "resource-error";
        case State::FormatError: return "format-error"; case State::DeviceError: return "device-error";
        case State::CleanupPending: return "cleanup-pending"; default: return "worker-error";
    }
}
unsigned LastError() noexcept { return g_error.load(std::memory_order_relaxed); }
} // namespace sm2debug::flame_audio
