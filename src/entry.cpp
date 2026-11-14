// SPDX-License-Identifier: GPL-3.0-or-later
#include "debugmenu.h"
#include "overlay.h"
#include "game_input_capture.h"
#include "photo_mode_gate.h"
#include <atomic>

#ifndef SM2DEBUG_PREVIEW
namespace {
HMODULE moduleHandle = nullptr;
std::atomic<bool> started{false};
DWORD WINAPI MenuThread(void*) {
    try {
        sm2debug::InitializeBackend(moduleHandle, false);
        if (!sm2debug::input_capture::Initialize(moduleHandle, sm2debug::GetSnapshot().supported, &sm2debug::MenuVisible))
            OutputDebugStringA("SM2DebugMenu: game input capture refused; inspect supported build/import ownership.\n");
        if (!sm2debug::photo_gate::Initialize(moduleHandle, sm2debug::GetSnapshot().supported, &sm2debug::MenuVisible))
            OutputDebugStringA("SM2DebugMenu: native photo-mode entry gate refused; inspect supported build/vtable ownership.\n");
        sm2debug::RunMenu(moduleHandle, false);
        sm2debug::ShutdownBackend();
    } catch (...) {
        sm2debug::photo_gate::Shutdown();
        sm2debug::input_capture::Shutdown();
        OutputDebugStringA("SM2DebugMenu: menu worker stopped after an exception.\n");
    }
    return 0;
}
}
#endif

#ifdef SM2DEBUG_PREVIEW
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    sm2debug::InitializeBackend(instance, true);
    const int result = sm2debug::RunMenu(instance, true);
    sm2debug::ShutdownBackend();
    return result;
}
#else
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        moduleHandle = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void script_enable() {
    if (started.exchange(true)) return;
    // Attach before returning to the script loader: the game may create its
    // swapchain before the worker finishes hashing the supported executable.
    if (!sm2debug::InitializeOverlay(moduleHandle)) { started = false; return; }
    HANDLE thread = CreateThread(nullptr, 0, MenuThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else started = false;
}
#endif
