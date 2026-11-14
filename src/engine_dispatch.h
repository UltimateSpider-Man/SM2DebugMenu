// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <cstdint>

namespace sm2debug {
enum class EngineDispatchStatus : std::uint32_t {
    NotStarted, Validating, Refused, Observing, ContextObserved, Stopping, Stopped
};
// Plain observation data. Reading it never invokes an engine function.
struct EngineDispatchObservation {
    EngineDispatchStatus status = EngineDispatchStatus::NotStarted;
    std::uint32_t refusalCode = 0, win32Error = 0;
    bool executableVerified = false, modulePinned = false;
    bool hookInstalled = false, protectionRestored = true;
    bool contextReady = false, diagnosticOnly = true, callbackEnabled = false;
    bool observationConsistent = false;
    std::uintptr_t imageBase = 0, object = 0, vtable = 0, slot = 0;
    std::uintptr_t expectedCaller = 0, originalMethod = 0, replacementMethod = 0;
    std::uintptr_t observedObject = 0, observedCaller = 0, teb = 0, tlsArray = 0, tlsBlock = 0, arena = 0;
    std::uint32_t threadId = 0, engineThreadId = 0, tlsIndex = 0, mainRole = 0, tlsRole = 0;
    std::int32_t arenaIndex = -1, arenaSlot = -1;
    std::uint64_t calls = 0, verifiedFrames = 0, rejectedFrames = 0, consecutiveVerifiedFrames = 0;
    std::uint64_t wrongObject = 0, wrongCaller = 0, wrongRole = 0, wrongThread = 0, unreadableContext = 0, droppedRecords = 0;
    std::uint64_t callbackToken = 0, callbacksStarted = 0, callbacksCompleted = 0;
    std::uint32_t callbacksInFlight = 0, callbackFailure = 0;
};
using EngineDispatchCallback = void (*)() noexcept;
// Initial probe accepts only nullptr. Non-null callbacks are refused until a
// separately reviewed integration enables them after real context evidence.
bool InitializeEngineDispatch(HMODULE owner, EngineDispatchCallback callback = nullptr) noexcept;
void ShutdownEngineDispatch() noexcept;
EngineDispatchObservation GetEngineDispatchObservation() noexcept;
// This also checks the current callback scope, current thread and current game
// TLS role; a previously observed global readiness flag alone is insufficient.
bool IsWithinVerifiedEngineCallback() noexcept;
// Registration is separate from observation. It is accepted only after the
// current process has observed at least30 consecutive valid engine frames.
// The callback must belong to the same pinned owner as the dispatcher. It must
// catch its own C++ exceptions and report failure; throwing out of noexcept is
// not recoverable by an outer dispatcher catch.
bool RegisterEngineDispatchCallback(HMODULE owner, EngineDispatchCallback callback, std::uint64_t* token) noexcept;
// Capability for UI/scheduling. This does not authorize a native call from the
// calling thread; native calls must separately require IsWithin... above.
bool EngineDispatchCallbacksAvailable() noexcept;
// Clears publication before waiting. A stale token never affects a newer
// registration. Inside a callback this only requests revocation and returns
// false while that invocation is still active. Never free callback state on a
// false return; the pinned module and registration record remain resident.
bool UnregisterEngineDispatchCallback(std::uint64_t token, DWORD timeoutMs = 2000) noexcept;
void ReportEngineDispatchCallbackFailure(std::uint64_t token, std::uint32_t reason) noexcept;
}
