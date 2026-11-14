// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "primary_objective_runtime.h"

namespace sm2debug {
struct ReplaySelection {
    std::uint64_t graphId = 0;
    std::uint32_t missionHash = 0, objectiveHash = 0;
};
inline bool SameReplaySelection(const ReplaySelection& a,const ReplaySelection& b) noexcept {
    return a.graphId==b.graphId && a.missionHash==b.missionHash && a.objectiveHash==b.objectiveHash;
}
struct ReplayTargetToken {
    std::uintptr_t node = 0, definition = 0, graph = 0, resource = 0;
    std::uintptr_t savedData = 0, objectiveDefinition = 0;
    std::uint64_t graphId = 0, nodeId = 0, objectiveGraphId = 0;
    std::uint32_t graphHandle = 0, checkpoint = 0;
    std::int32_t missionState = -1, replayType = 0;
};
struct ReplayState {
    bool supported = false, ready = false;
    std::uint64_t requestEpoch = 0;
    PrimaryObjectiveState origin;
    ReplaySelection selection;
    ReplayTargetToken target;
    std::wstring status;
};
struct ReplayResult { bool invoked = false, accepted = false; };
void InitializeReplayRuntime(HMODULE gameModule,bool supported);
// Guarded memory reads and immutable catalog lookup only. No native predicate.
ReplayState ReadReplayState(const PrimaryObjectiveState& origin,const ReplaySelection& selection);
bool ReplayTransitionPending();
// Calls native MissionNodeGame replay exactly once; native code owns Reload.
// Any invocation may change world state even when accepted is false.
ReplayResult ExecuteReplay(const ReplayState& captured,std::wstring& status,
    ReloadPreparation prepare = nullptr, void* preparationContext = nullptr);
}
