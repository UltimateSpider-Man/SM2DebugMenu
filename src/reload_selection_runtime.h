// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reload_runtime.h"
#include <array>

namespace sm2debug {
enum class MissionStartState : std::uint8_t { Active = 0, Available = 1, Complete = 2 };
struct ReloadMissionBranch {
    std::uint32_t missionHash = 0, objectiveHash = 0;
    MissionStartState state = MissionStartState::Active;
};
struct ReloadMissionSelection {
    std::uint64_t graphId = 0;
    std::vector<ReloadMissionBranch> branches;
    bool replayMode = false;
};
struct ReloadSelectionToken {
    ReloadToken world;
    std::uintptr_t objectiveSystem = 0;
    std::uint64_t graphFingerprint = 0;
    // Pending native request, NOT the captured state of the running missions.
    // The entire prior request is retained to refuse externally changed state.
    std::array<std::uint8_t, 0x310> priorOverrides{};
    std::int32_t priorEndReason = -1;
};
struct ReloadBranchInfo {
    bool allowAvailable = false;
    std::uint32_t checkpoint = 0;
    // Native SortedByDFT, filtered to exact immutable catalog identities.
    std::vector<std::uint32_t> objectiveOrder;
};
struct ReloadSelectionState {
    bool supported = false, ready = false;
    ReloadMissionSelection selection;
    ReloadSelectionToken token;
    std::vector<std::uint32_t> missionOrder;
    std::vector<ReloadBranchInfo> branches;
    std::wstring status;
};
void InitializeReloadSelectionRuntime(HMODULE gameModule, bool supported);
// Bounded reads only; the requested branches remain a UI selection until an
// admitted Reload/Full Reload explicitly invokes preparation. Empty extra
// editor rows stay aligned in this snapshot and are removed during preparation
// exactly as the source's RemoveEmpty action. An empty main cannot execute.
ReloadSelectionState ReadReloadSelectionState(const ReloadState& world, const ReloadMissionSelection& selection);
// Call only after final Reload admission. Uses a try-lock and supplied fresh
// world; never calls Reload readers under the Reload lock. Writes the original
// DebugReloaded reason and MissionStartArgArray semantics. Like the source,
// preparation persists if the subsequent native reload request declines.
// A fresh capture can retry an exact pending request still owned by this
// adapter in the same world; foreign or changed pending requests refuse.
bool PrepareReloadSelectionAfterAdmission(const ReloadSelectionState& captured,
    const ReloadState& admitted, std::wstring& status);
}
