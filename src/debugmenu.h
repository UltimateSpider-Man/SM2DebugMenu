#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "debug_settings.h"
#include "reload_runtime.h"
#include "primary_objective_runtime.h"
#include "actor_save_runtime.h"
#include "replay_runtime.h"
#include "reload_options_runtime.h"
#include "camera_debug_runtime.h"
#include "reload_selection_runtime.h"
#include "hero_warp_runtime.h"

namespace sm2debug {

enum class Action {
    Heal, AutoHeal, SetMaxHealth, ResetHealth,
    SavePosition, LoadPosition, UndoTeleport,
    MoveXMinus, MoveXPlus, MoveYMinus, MoveYPlus, MoveZMinus, MoveZPlus,
    Borderless, RestoreWindow, ResetAll, ExportDiagnostics, SwitchCharacter,
    HeroNoDamage, HeroNoDeath, EnemyNoDeath, EnemyNoDamage, DebugSetting,
    ReloadCheckpoint, ExitFrontend, CopyReloadData, CompletePrimary, FullReload, ReplayMission, SetNewGamePlus, ExitLobby, CameraShake,
    EquipMJFlamethrower, SwitchMJWeapon, SetHeroScale, ResetHeroScale, HeroWarp
};

struct Snapshot {
    bool supported = false;
    bool heroReady = false;
    bool healthReady = false;
    bool teleportReady = false;
    bool heroScaleReady = false;
    bool heroScaleOwned = false;
    float heroScale = 1.0f;
    bool engineActionsReady = false;
    bool mjFlamethrowerReady = false;
    bool mjWeaponSwitchReady = false;
    uint16_t mjWeaponSelectedItem = 0;
    bool autoHeal = false;
    bool healthFlagsReady = false;
    bool heroNoDamage = false, heroNoDeath = false;
    bool enemyNoDeath = false, enemyNoDamage = false;
    bool savedPosition = false;
    bool undoPosition = false;
    bool borderless = false;
    uint32_t actorHandle = 0;
    float health = 0;
    float maxHealth = 0;
    float x = 0, y = 0, z = 0;
    float moveStep = 10;
    double memoryMiB = 0;
    double cpuPercent = 0;
    int clientWidth = 0, clientHeight = 0;
    unsigned long uptimeSeconds = 0;
    HWND gameWindow = nullptr;
    std::wstring status;
    std::wstring compatibility;
    std::vector<std::wstring> characterNames;
    std::vector<DebugSettingValue> settings;
    ReloadState reload;
    PrimaryObjectiveState primary;
    ActorSaveState actorSave;
    ReplayState replay;
    ReloadOptionsState reloadOptions;
    CameraDebugState cameraDebug;
    ReloadSelectionState reloadSelection;
    HeroWarpState heroWarp;
};

struct ReloadPolicy {
    bool keepCurrentLoadout = true;
    bool resetActorSave = false;
    ReloadOptionsState campaign;
    ActorSaveState actors;
    bool applyMissionSelection = false;
    ReloadSelectionState missions;
};

// Implemented by the backend. Actions are queued and executed on the game's
// verified engine thread (gameplay) or window thread (window commands).
// The UI only consumes immutable snapshots.
void InitializeBackend(HMODULE module, bool preview);
void TickBackend();
Snapshot GetSnapshot();
// Revoke displayed readiness before changing visibility; performs no game reads.
void InvalidateMenuSnapshot();
void SubmitAction(Action action, float value = 0);
void SubmitDebugSetting(const std::string& id, int direction);
// Reload identifiers remain uint32_t throughout; float action values cannot
// represent all native checkpoint keys. The captured context binds the request
// to the world whose choices the user saw.
void SubmitReloadAction(Action action, const ReloadState& captured, uint32_t checkpointKey = 0);
void SubmitCompletePrimary(const PrimaryObjectiveState& captured);
void SubmitReloadWithActorReset(Action action, const ReloadState& captured,
    uint32_t checkpointKey, const ActorSaveState& reset);
void SubmitReplaySelection(const ReplaySelection& selection);
void SubmitReplayAction(const ReplayState& captured);
void SubmitReplayDataCopy(const ReloadState& world, const ReplaySelection& selection);
void SubmitSetNewGamePlus(const ReloadOptionsState& captured, bool enabled);
void SubmitCameraShake(const CameraDebugState& captured, bool enabled);
void SubmitHeroWarp(const HeroWarpState& captured, const HeroWarpPosition& position);
void SubmitReloadMissionSelection(const ReloadMissionSelection& selection);
void SubmitReloadWithPolicy(Action action, const ReloadState& captured, uint32_t checkpointKey,
    const ReloadPolicy& policy);
void SubmitReplayWithPolicy(const ReplayState& captured, const ReloadPolicy& policy);
void ShutdownBackend();
std::wstring OutputDirectory();

// The production UI draws on the game's swapchain. Preview has no game bindings.
int RunMenu(HMODULE module, bool preview);

} // namespace sm2debug
