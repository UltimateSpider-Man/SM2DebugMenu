// SPDX-License-Identifier: GPL-3.0-or-later
// Negative host: never loads the game, creates windows, or invokes the hook.
#include "../src/engine_dispatch.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
template<class T> T Export(HMODULE module, const char* name) {
    FARPROC value = GetProcAddress(module, name); T out = nullptr;
    static_assert(sizeof(out) == sizeof(value)); std::memcpy(&out, &value, sizeof(out)); return out;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const bool callback = std::wcscmp(argv[2], L"--callback-refused") == 0;
    HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return 3;
    auto enable = Export<void (*)()>(dll, "script_enable");
    auto disable = Export<void (*)()>(dll, "script_disable");
    auto read = Export<BOOL (*)(sm2debug::EngineDispatchObservation*, size_t)>(dll, "probe_read");
    auto within = Export<BOOL (*)()>(dll, "probe_within_callback");
    auto tryCallback = Export<BOOL (*)()>(dll, "probe_try_callback");
    if (!enable || !disable || !read || !within || !tryCallback) return 4;
    constexpr DWORD errorSentinel = 0x73e51;
    SetLastError(errorSentinel);
    bool returnedFalse = true;
    if (callback) returnedFalse = tryCallback() == FALSE; else enable();
    const bool errorPreserved = GetLastError() == errorSentinel;
    sm2debug::EngineDispatchObservation observation;
    const bool observed = read(&observation, sizeof(observation)) != FALSE;
    const bool outsideGuard = within() == FALSE;
    const auto reason = callback ? 1u : 2u;
    const bool passed = observed && returnedFalse && errorPreserved && outsideGuard &&
        observation.status == sm2debug::EngineDispatchStatus::Refused && observation.refusalCode == reason &&
        !observation.executableVerified && !observation.modulePinned && !observation.hookInstalled &&
        observation.protectionRestored && !observation.contextReady && !observation.callbackEnabled && observation.diagnosticOnly &&
        observation.calls == 0 && observation.verifiedFrames == 0 && observation.object == 0 && observation.slot == 0;
    std::printf("{\"case\":\"%s\",\"passed\":%s,\"refusal_code\":%u,\"last_error_preserved\":%s,\"outside_callback_guard_false\":%s,"
        "\"hook_installed\":%s,\"calls\":%llu,\"verified_frames\":%llu,\"game_loaded\":false,\"windows_created\":false}\n",
        callback ? "callback-refused" : "wrong-executable", passed ? "true" : "false", observation.refusalCode,
        errorPreserved ? "true" : "false", outsideGuard ? "true" : "false", observation.hookInstalled ? "true" : "false",
        static_cast<unsigned long long>(observation.calls), static_cast<unsigned long long>(observation.verifiedFrames));
    disable(); FreeLibrary(dll);
    return passed ? 0 : 1;
}
