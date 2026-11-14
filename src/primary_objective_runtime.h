// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reload_runtime.h"
#include <cstdint>
#include <string>

namespace sm2debug {
// Borrowed identity only. No engine object is constructed, owned or retained.
struct PrimaryObjectiveToken {
    std::uintptr_t objectiveSystem = 0, objectiveVtable = 0;
    std::uintptr_t saveSystem = 0, saveContainer = 0, saveEntry = 0;
    std::uintptr_t mapKeys = 0, mapValues = 0;
    std::uintptr_t saveBlock = 0, systemData = 0, primaryAddress = 0;
    std::uintptr_t missionNode = 0, objectiveNode = 0, missionGraph = 0, objectiveGraph = 0;
    std::uint32_t mapCapacity = 0, mapStride = 0;
    std::uint32_t missionHash = 0, objectiveHash = 0;
    std::uint32_t missionGraphHandle = 0, objectiveGraphHandle = 0;
    std::uint64_t missionNodeId = 0, objectiveNodeId = 0;
    std::uint64_t missionGraphAssetId = 0;
};
struct PrimaryObjectiveState {
    bool supported = false, ready = false;
    PrimaryObjectiveToken token;
    ReloadToken world;
    std::wstring status;
};
void InitializePrimaryObjectiveRuntime(HMODULE gameModule, bool supported);
// Guarded process-memory reads only; never calls a native getter or resolver.
PrimaryObjectiveState ReadPrimaryObjectiveState(const ReloadState& world);
// Ordinary native objective completion, with normal script/reward advancement.
// Does not create a new game, reset progression, or fabricate script actions.
bool ExecuteCompletePrimary(const PrimaryObjectiveState& captured, std::wstring& status);
}
