// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reload_runtime.h"
#include <array>

namespace sm2debug {
struct ActorSaveToken {
    ReloadToken world;
    std::uintptr_t block = 0, container = 0, entry = 0, saveData = 0;
    std::uint64_t fingerprint = 0;
    std::array<std::uint64_t, 3> mapFingerprints{};
    // DeletedByUid, CollectedByUid, ActorFlagsByUid, respectively.
    std::array<std::uint32_t, 3> entries{};
};
struct ActorSaveState {
    bool supported = false, ready = false;
    ActorSaveToken token;
    std::wstring status;
};
enum class ActorSaveResetOutcome {
    Refused, Applied, AppliedButVerificationFailed
};
void InitializeActorSaveRuntime(HMODULE gameModule, bool supported);
// Read-only, including polling: no native getter or cleanup method is called.
ActorSaveState ReadActorSaveState(const ReloadState& world);
// Call only in the verified engine callback, immediately BEFORE manual Reload.
// Clears DeletedByUid and ActorFlagsByUid; preserves CollectedByUid.
// Revalidates the immutable captured world/save/maps before the first mutation.
// Applied* means native reset was invoked. This persistent mutation cannot be
// rolled back if a subsequent Reload refuses or post-call verification fails.
// Do not invoke while holding reload_runtime's internal mutex.
ActorSaveResetOutcome ExecuteActorSaveReset(const ActorSaveState& captured, std::wstring& status);
}
