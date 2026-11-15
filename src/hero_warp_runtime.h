// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reload_runtime.h"

namespace sm2debug {
struct HeroWarpPosition { float x = 0, y = 0, z = 0; };
struct HeroWarpState {
    bool supported = false, ready = false;
    ReloadToken world;
    std::uintptr_t currentEntry = 0, actor = 0, transform = 0, eventClass = 0;
    std::uint32_t actorHandle = 0;
    HeroWarpPosition position;
    std::wstring status;
};
void InitializeHeroWarpRuntime(HMODULE gameModule, bool executableVerified);
// Read-only observation in the verified engine callback. The supplied handle
// must still be the current hero; it is never used as an arbitrary actor target.
HeroWarpState ReadHeroWarpState(const ReloadState& world, std::uint32_t currentActorHandle);
// Original UI format: three comma-separated coordinates. Reject partial input,
// special values and coordinates outside the existing position-control bounds.
bool ParseHeroWarpPosition(const std::wstring& text, HeroWarpPosition& position, std::wstring& status);
// Pure snapshot comparison; does not read native memory or authorize dispatch.
bool SameHeroWarpContext(const HeroWarpState& left, const HeroWarpState& right) noexcept;
// Revalidates world, hero generation, native types and complete function bodies.
// Dispatches the original PerformWarpEvent synchronously. A true return means
// an event handler ran, not that streaming or every warp consequence finished.
bool ExecuteHeroWarp(const HeroWarpState& captured, const HeroWarpPosition& position, std::wstring& status);
}
