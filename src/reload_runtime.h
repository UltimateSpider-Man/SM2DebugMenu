// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sm2debug {
// Captured only by guarded reads. None of these pointers own engine objects.
struct ReloadToken {
    std::uintptr_t imageBase = 0, manager = 0, managerVtable = 0, loadedLevel = 0;
    std::uintptr_t mode = 0, componentTable = 0, kernel = 0, kernelOwner = 0;
    std::uintptr_t checkpointRecords = 0, saveSystem = 0, saveSystemVtable = 0;
    std::uint64_t levelAsset = 0, contextSerial = 0;
    std::uint32_t kernelHandle = 0, kernelGeneration = 0, currentCheckpoint = 0;
    std::int32_t managerState = -1, pendingInstances = -1, pendingDeletions = -1;
    std::int32_t modeValue = -1, checkpointCount = 0;
    std::uint16_t kernelSequence = 0;
    std::uint8_t managerReady = 0, kernelState = 0;
};
struct ReloadCheckpoint {
    std::uint32_t key = 0, recordIndex = 0;
    std::wstring name;
};
struct ReloadState {
    bool supported = false, ready = false, frontendReady = false;
    ReloadToken token;
    std::shared_ptr<const std::vector<ReloadCheckpoint>> checkpoints;
    std::wstring status;
    bool lobbyReady = false;
};
// Invoked once after final native admission, with the Reload mutex held. Must
// not call Reload readers or block on another runtime mutex. False cancels the
// request and preserves status. Preparation writes are not rolled back if the
// native request subsequently declines, matching original Reload semantics.
using ReloadPreparation = bool (*)(const ReloadState& admitted, void* context, std::wstring& status);
// supported is the backend's exact executable-hash verdict. This module also
// checks PE shape, code bytes, function boundaries and native RTTI/vtables.
void InitializeReloadRuntime(HMODULE gameModule, bool supported);
ReloadState ReadReloadState();
// Whether passive observation must continue after a native transition hides the menu.
bool ReloadTransitionPending();
bool SameReloadContext(const ReloadToken& left, const ReloadToken& right) noexcept;
// Fresh native admission only: no Reload request, transition latch or retained
// lock. A nonzero key additionally validates its exact captured live record.
bool CheckReloadExecutionContext(const ReloadState& captured, std::wstring& status, std::uint32_t checkpointKey = 0);
// Native calls are permitted only in the currently verified engine callback.
// key==0 selects the current checkpoint through the game's native sentinel.
bool ExecuteReload(const ReloadState& captured, std::uint32_t checkpointKey, std::wstring& status,
                   ReloadPreparation preparation = nullptr, void* preparationContext = nullptr);
// Uses the same request and world guards with the native force-full-unload flag.
bool ExecuteFullReload(const ReloadState& captured, std::uint32_t checkpointKey, std::wstring& status,
                       ReloadPreparation preparation = nullptr, void* preparationContext = nullptr);
bool ExecuteExitFrontend(const ReloadState& captured, std::wstring& status);
// Original lobby action: set native exit-save flag and request unload, without
// the frontend action's hero-setup reset. Destination follows retail routing.
bool ExecuteExitLobby(const ReloadState& captured, std::wstring& status);
}
