// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <cstdint>

namespace sm2debug::photo_gate {
struct Diagnostics {
    bool installed = false, protectionRestored = true;
    bool stateKnown = false, photoActive = false;
    std::uint32_t nativeState = UINT32_MAX;
    std::uint64_t blocked = 0, forwarded = 0, invalidObjects = 0;
};
// Exact-build native CanEnter vtable gate. The original method runs only as
// continuation of its existing native caller; no worker invokes the engine.
bool Initialize(HMODULE owner, bool supported, bool (*menuVisible)());
void Shutdown();
// Reads the static native object's state with ReadProcessMemory; no game call.
// Unknown state must not be interpreted as "photo mode is closed".
Diagnostics Observe();
}
