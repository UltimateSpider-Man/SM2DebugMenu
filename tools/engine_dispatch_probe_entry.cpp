// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/engine_dispatch.h"
namespace { HMODULE self = nullptr; void UnusedCallback() noexcept {} }
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { self = module; DisableThreadLibraryCalls(module); }
    return TRUE;
}
extern "C" __declspec(dllexport) void script_enable() { sm2debug::InitializeEngineDispatch(self, nullptr); }
extern "C" __declspec(dllexport) void script_disable() { sm2debug::ShutdownEngineDispatch(); }
extern "C" __declspec(dllexport) BOOL probe_read(sm2debug::EngineDispatchObservation* output, size_t bytes) {
    if (!output || bytes != sizeof(*output)) return FALSE;
    *output = sm2debug::GetEngineDispatchObservation(); return TRUE;
}
extern "C" __declspec(dllexport) BOOL probe_within_callback() { return sm2debug::IsWithinVerifiedEngineCallback() ? TRUE : FALSE; }
extern "C" __declspec(dllexport) BOOL probe_try_callback() { return sm2debug::InitializeEngineDispatch(self, &UnusedCallback) ? TRUE : FALSE; }
