// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reload_runtime.h"

namespace sm2debug::venom_feed {
// Supplied from the backend's exact-build, read-only metadata bindings.
// These are borrowed native addresses; this module never owns an engine object.
struct Bindings {
    HMODULE gameModule = nullptr;
    bool executableVerified = false;
    std::uintptr_t heroSystem = 0, componentRegistry = 0, componentInfos = 0;
};
struct Environment {
    HWND gameWindow = nullptr;
    bool menuVisible = true;
    // The backend must positively establish pause admission in this callback.
    // Unknown is a refusal, even when the last displayed UI snapshot was ready.
    bool pauseKnown = false, paused = true;
};
struct State {
    bool supported = false, ready = false, nativeSelectionReady = false;
    // Read-only observation cannot equate the native selection with a kill.
    bool canDoPredicateEvaluated = false, canKillKnown = false, canKill = false;
    ReloadToken world;
    std::uintptr_t heroActor = 0, heroWamUser = 0, characterManager = 0;
    std::uintptr_t targetActor = 0, targetWam = 0, targetFinisherTracker = 0;
    std::uint32_t heroHandle = 0, ownerComponentHandle = 0, targetHandle = 0;
    std::int32_t characterIndex = -1;
    std::uint64_t configuredGraph = 0, activeGraph = 0, observedAtMilliseconds = 0;
    float distance = 0, targetHealth = 0, targetMaximumHealth = 0;
    std::wstring status;
};
void Initialize(const Bindings& bindings);
// Guarded current-process reads only, accepted solely inside the verified
// engine callback. No predicates, action/input calls, queues or writes occur.
// ready admits a native finisher INPUT REQUEST; native combat still owns final
// target/kill/synchronization checks. It does not mean a victim was killed.
State ReadReadiness(const ReloadState& world, const Environment& environment);
// A later request must reject a changed actor, target, graph or world. This is
// still read-only and never submits a finisher/input request.
bool Revalidate(const State& captured, const Environment& environment, std::wstring& status);
}
