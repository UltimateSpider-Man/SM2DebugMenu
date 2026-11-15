// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>

namespace sm2debug::flame_audio {
// Call outside DllMain and outside engine callbacks. Idempotent while running.
// True means the worker was created; Status/LastError report device readiness.
bool Initialize(HMODULE resourceModule, unsigned resourceId = 101) noexcept;

// Engine-frame heartbeat only: no allocation, locks, device calls or file I/O.
// A missing heartbeat silences playback within250ms (worker scheduling permitting).
void Update(bool active) noexcept;

// Call outside DllMain after the producer stops sending active heartbeats.
// False means the bounded join timed out. The worker keeps its module reference
// and all buffers/handles until it exits; it is never forcibly terminated.
bool Shutdown(unsigned timeoutMs = 1000) noexcept;

// Stable string literals and a Win32/MMRESULT error code; safe to poll for logs.
const char* Status() noexcept;
unsigned LastError() noexcept;
} // namespace sm2debug::flame_audio
