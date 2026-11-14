// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "reload_runtime.h"

namespace sm2debug {
struct ReloadOptionsToken {
    ReloadToken world;
    std::uintptr_t block = 0, container = 0, entry = 0, campaign = 0;
    std::uintptr_t keys = 0, values = 0;
    std::uint64_t mapFingerprint = 0;
};
struct ReloadOptionsState {
    bool supported = false, ready = false, newGamePlus = false;
    bool defaultLoadoutApplied = false;
    ReloadOptionsToken token;
    std::wstring status;
};
void InitializeReloadOptionsRuntime(HMODULE gameModule, bool supported);
// Guarded reads only. Does not call a native resolver or change a saved flag.
ReloadOptionsState ReadReloadOptionsState(const ReloadState& world);
// Ports the original developer NG+ checkbox: changes the current campaign's
// saved flag and its native dirty masks, without starting a new playthrough.
bool ExecuteSetNewGamePlus(const ReloadOptionsState& captured, bool enabled, std::wstring& status);
// Called only for Keep Current Loadout=false, after native reload admission.
// Normal/full reload callers hold the admission lock; Replay supplies a fresh
// admitted snapshot. This helper rechecks live identity without reacquiring
// that lock and only tries the options lock. Original semantics retain this reset even
// if the subsequent native request refuses; Keep=true omits preparation.
bool PrepareReloadLoadoutAfterAdmission(const ReloadOptionsState& captured,
                                       const ReloadState& admitted, std::wstring& status);
}
