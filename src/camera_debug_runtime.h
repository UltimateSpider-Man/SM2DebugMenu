// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reload_runtime.h"
#include <array>

namespace sm2debug {
struct CameraDebugToken {
    ReloadToken world;
    std::uintptr_t cameraSystem = 0, flagsAddress = 0;
    // Borrowed allocation identities: roots, node pools, previous roots,
    // shake handles, shakes and view proxies. No native object is owned.
    std::array<std::uintptr_t, 7> allocations{};
    std::uint32_t viewCount = 0, flags = 0;
};
struct CameraDebugState {
    bool supported = false, ready = false, shakeEnabled = false;
    CameraDebugToken token;
    std::wstring status;
};
void InitializeCameraDebugRuntime(HMODULE gameModule, bool supported);
// Guarded reads only. The original setting is a runtime mask, not a save flag.
CameraDebugState ReadCameraDebugState(const ReloadState& world);
bool ExecuteSetCameraShake(const CameraDebugState& captured, bool enabled, std::wstring& status);
// Verified engine callback only. Restore the initial bit only while the same
// world/allocations and last owned bit remain; preserve every other mask bit.
// Changed worlds, initialization or game writes release ownership without a write.
bool ResetOwnedCameraDebugOverrides(std::wstring& status);
}
