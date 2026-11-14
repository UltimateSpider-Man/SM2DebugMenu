// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace sm2debug {
// These are immutable, exact-build asset facts. TOC membership does not mean
// that an asset or node is currently registered, loaded, or safe to execute.
struct ReloadMissionCheckpoint {
    const char* name = "";
    // Serialized native name CRC, not an independently admitted live-world key.
    // An empty name has CRC0; resolve against the current native checkpoint list.
    std::uint32_t nameCrc = 0;
};
struct ReloadMissionLoadout {
    const char* assetPath = "";
    std::uint64_t assetId = 0;
};
struct ReloadMissionObjective {
    const char* name = "";
    std::uint32_t nameCrc = 0;
    std::uint64_t assetNodeId = 0;
    // Raw Nodes array index from the objective graph. This is NOT proven
    // native "Mission Order", depth-first order, or progression order.
    std::uint32_t assetNodeOrder = 0;
    ReloadMissionCheckpoint checkpoint;
    std::vector<ReloadMissionLoadout> debugLoadouts;
};
struct ReloadMission {
    const char* name = "";
    std::uint32_t nameCrc = 0;
    std::uint64_t assetNodeId = 0;
    // Raw Nodes array index in the containing mission graph, with the same
    // ordering limitation as ReloadMissionObjective::assetNodeOrder.
    std::uint32_t assetNodeOrder = 0;
    const char* objectiveGraphPath = "";
    std::uint64_t objectiveGraphAssetId = 0;
    ReloadMissionCheckpoint availableCheckpoint;
    std::vector<ReloadMissionLoadout> debugLoadouts;
    // Sorted by exact ASCII name; each objective comes from this mission's
    // typed ObjectiveGraph reference, never a name-prefix association.
    std::vector<ReloadMissionObjective> objectives;
};
struct ReloadMissionGraph {
    std::uint64_t graphAssetId = 0;
    const char* graphPath = "";
    // Sorted by exact ASCII mission name. Missions whose referenced objective
    // graph is absent from the known TOC are excluded.
    std::vector<ReloadMission> missions;
};

// Five graph records, sorted by ASCII graphPath. No game calls or writes.
// Initialize once on the mod's normal worker/UI thread before engine callbacks;
// first access constructs the immutable vectors, later lookups do not allocate.
const std::vector<ReloadMissionGraph>& ReloadMissionCatalog();
const ReloadMissionGraph* FindReloadMissionGraph(std::uint64_t graphAssetId);
const ReloadMission* FindReloadMission(std::uint64_t graphAssetId, std::uint32_t missionNameCrc);
const ReloadMissionObjective* FindReloadMissionObjective(std::uint64_t graphAssetId,
    std::uint32_t missionNameCrc, std::uint32_t objectiveNameCrc);
}
