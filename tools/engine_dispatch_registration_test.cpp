// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests of the production publication/retirement implementation. Synthetic
// CPU callbacks below are not engine context proof and never invoke FrameHook.
#include "../src/engine_dispatch.h"
#include <atomic>
// Instrument production calls only. Fixture-owned TLS below uses the real APIs.
// This ensures a low-slot regression cannot hide behind an available test slot.
namespace fixture_tls {
std::atomic<unsigned> allocateCalls{0}, getCalls{0}, setCalls{0}, freeCalls{0};
inline DWORD WINAPI Allocate() { allocateCalls.fetch_add(1); return ::TlsAlloc(); }
inline LPVOID WINAPI Get(DWORD index) { getCalls.fetch_add(1); return ::TlsGetValue(index); }
inline BOOL WINAPI Set(DWORD index, LPVOID value) { setCalls.fetch_add(1); return ::TlsSetValue(index, value); }
inline BOOL WINAPI Free(DWORD index) { freeCalls.fetch_add(1); return ::TlsFree(index); }
unsigned Calls() { return allocateCalls.load() + getCalls.load() + setCalls.load() + freeCalls.load(); }
}
#define TlsAlloc fixture_tls::Allocate
#define TlsGetValue fixture_tls::Get
#define TlsSetValue fixture_tls::Set
#define TlsFree fixture_tls::Free
#include "../src/engine_dispatch.cpp"
#undef TlsAlloc
#undef TlsGetValue
#undef TlsSetValue
#undef TlsFree
#include <functional>
#include <iostream>
#include <thread>

namespace {
using namespace sm2debug;
std::atomic<uint64_t> calls{0};
std::atomic<bool> entered{false}, releaseCallback{false}, stopDriver{false};
CallbackRegistry* fixtureRegistry = nullptr;
uint64_t fixtureToken = 0;
bool selfReturned = true;
bool executionAllowedAfterRevocation = true;
uint64_t selfElapsed = 0;
int writableData = 0;
void Count() noexcept { calls.fetch_add(1); }
void Block() noexcept {
    entered.store(true);
    while (!releaseCallback.load()) Sleep(1);
    calls.fetch_add(1);
}
void SelfRevoke() noexcept {
    const auto begin = GetTickCount64();
    selfReturned = RevokeRegistration(*fixtureRegistry, fixtureToken, 2000, false);
    executionAllowedAfterRevocation = RegistryExecutionAllowed(*fixtureRegistry);
    selfElapsed = GetTickCount64() - begin;
}
void CatchAndReport() noexcept {
    try { throw std::runtime_error("synthetic callback failure"); }
    catch (...) {
        FailRegistration(*fixtureRegistry, fixtureToken, 7);
        executionAllowedAfterRevocation = RegistryExecutionAllowed(*fixtureRegistry);
    }
}
struct Worker {
    std::thread thread;
    ~Worker() { releaseCallback.store(true); stopDriver.store(true); if (thread.joinable()) thread.join(); }
};
struct OccupiedTls {
    std::vector<std::pair<DWORD, void*>> slots;
    ~OccupiedTls() {
        for (const auto& slot : slots) { TlsSetValue(slot.first, slot.second); TlsFree(slot.first); }
    }
    bool FillFixedSlots() {
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto index = TlsAlloc();
            if (index == TLS_OUT_OF_INDEXES) return false;
            slots.emplace_back(index, TlsGetValue(index));
            // An opaque owned-slot sentinel is never interpreted as an object.
            if (!TlsSetValue(index, reinterpret_cast<void*>(uintptr_t(0xC0D00000) + index))) return false;
            if (index >= 64) return true;
        }
        return false;
    }
    bool SentinelsIntact() const {
        for (const auto& slot : slots)
            if (TlsGetValue(slot.first) != reinterpret_cast<void*>(uintptr_t(0xC0D00000) + slot.first)) return false;
        return true;
    }
};
Context SyntheticContext(State& state) {
    state.base = 0x140000000; state.object = 0x123456;
    state.configured.store(true); state.engineThread.store(GetCurrentThreadId());
    Context context; context.object = state.object; context.caller = state.base + kCaller; context.thread = GetCurrentThreadId();
    return context;
}
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void WaitFor(const std::function<bool()>& condition) {
    const auto start = GetTickCount64();
    while (!condition()) { if (GetTickCount64() - start > 2000) throw std::runtime_error("Fixture synchronization timeout"); Sleep(1); }
}
struct Result { const char* name; std::string error; };
std::vector<Result> results;
void Test(const char* name, const std::function<void()>& body) {
    calls.store(0); entered.store(false); releaseCallback.store(false); stopDriver.store(false);
    fixtureRegistry = nullptr; fixtureToken = 0; selfReturned = true; selfElapsed = 0; executionAllowedAfterRevocation = true;
    try { body(); results.push_back({name, {}}); }
    catch (const std::exception& error) { results.push_back({name, error.what()}); }
}
std::string Escape(const std::string& value) {
    std::string out;
    for (char c : value) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    return out;
}
}
int main() {
    using namespace sm2debug;
    Test("public_registration_refused_before_real_context_observation", [] {
        uint64_t token = 99;
        SetLastError(0x7315);
        Check(!RegisterEngineDispatchCallback(GetModuleHandleW(nullptr), &Count, &token) && token == 0, "Unobserved host registered callback");
        Check(GetLastError() == 0x7315, "Public registration changed LastError");
        Check(!EngineDispatchCallbacksAvailable() && !IsWithinVerifiedEngineCallback(), "Unobserved host gained engine capability");
        Check(!RegisterEngineDispatchCallback(GetModuleHandleW(nullptr), nullptr, &token), "Null callback accepted");
        Check(!RegisterEngineDispatchCallback(GetModuleHandleW(nullptr), &Count, nullptr), "Null token output accepted");
    });
    Test("same_owner_executable_image_constraints", [] {
        const HMODULE self = GetModuleHandleW(nullptr);
        Check(CallbackOwnedBy(self, self, Address(&Count), false), "Known same-owner text function rejected");
        Check(CallbackOwnedBy(self, self, Address(&Count), true), "Known callback owner could not be pinned");
        Check(!CallbackOwnedBy(self, GetModuleHandleW(L"kernel32.dll"), Address(&Count), false), "Foreign owner accepted");
        Check(!CallbackOwnedBy(self, self, reinterpret_cast<uintptr_t>(&writableData), false), "Writable data accepted as callback");
        Check(!CallbackOwnedBy(self, nullptr, Address(&Count), false), "Null owner accepted");
        Check(!CallbackOwnedBy(self, self, 0, false), "Null address accepted");
    });
    Test("current_stack_scope_lifetime_and_exception_cleanup", [] {
        State state; const auto context = SyntheticContext(state);
        state.ready.store(true); state.status.store(EngineDispatchStatus::ContextObserved);
        Check(!OwnedScopeActive(state), "Readiness alone authorized an absent scope");
        {
            ScopeGuard guard(state, context);
            CallbackScope copy;
            Check(guard.active && OwnedScopeActive(state, &copy), "Known current stack scope rejected");
            Check(copy.active && copy.object == context.object && copy.caller == context.caller && copy.thread == context.thread,
                  "Owned scope copied mismatched provenance");
            Check(OnCurrentStack(reinterpret_cast<uintptr_t>(&guard.current), sizeof(guard.current)), "Real TIB bounds rejected stack object");
        }
        Check(state.activeScope.load() == 0 && !OwnedScopeActive(state), "Normal scope exit retained authorization");
        bool deliberateUnwind = false;
        try {
            ScopeGuard guard(state, context);
            Check(guard.active && OwnedScopeActive(state), "Exception fixture scope did not activate");
            deliberateUnwind = true;
            throw std::runtime_error("Deliberate synthetic unwind");
        } catch (const std::runtime_error&) {}
        Check(deliberateUnwind, "Exception fixture did not reach the deliberate unwind");
        Check(state.activeScope.load() == 0 && !OwnedScopeActive(state), "Unwinding retained scope authorization");
        Check(state.ready.load() && !state.stopping.load(), "Normal cleanup unexpectedly disabled synthetic state");
    });
    Test("fixed_tls_exhaustion_does_not_block_scope_or_write_tls", [] {
        OccupiedTls tls;
        Check(tls.FillFixedSlots(), "Could not exercise a Windows TLS index at least64");
        Check(tls.slots.back().first >= 64 && tls.SentinelsIntact(), "Low-slot exhaustion setup failed");
        const auto privateCalls = fixture_tls::Calls();
        const auto staticTlsArray = static_cast<uintptr_t>(__readgsqword(0x58));
        State state; const auto context = SyntheticContext(state);
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            ScopeGuard guard(state, context);
            Check(guard.active && OwnedScopeActive(state), "Exhausted fixed TLS prevented stack-scope authorization");
        }
        Check(state.activeScope.load() == 0 && !OwnedScopeActive(state), "Repeated scope cleanup leaked publication");
        Check(fixture_tls::Calls() == privateCalls, "Production scope called a private Windows TLS API");
        Check(staticTlsArray == static_cast<uintptr_t>(__readgsqword(0x58)), "Scope changed the thread static TLS array pointer");
        Check(tls.SentinelsIntact(), "Scope changed an owned opaque TLS sentinel");
    });
    Test("scope_provenance_mismatches_are_denied", [] {
        State state; const auto context = SyntheticContext(state);
        ScopeGuard guard(state, context);
        Check(guard.active && OwnedScopeActive(state), "Positive scope fixture failed");
        guard.current.active = false; Check(!OwnedScopeActive(state), "Inactive scope accepted"); guard.current.active = true;
        guard.current.object ^= 1; Check(!OwnedScopeActive(state), "Wrong scope object accepted"); guard.current.object = context.object;
        guard.current.caller ^= 1; Check(!OwnedScopeActive(state), "Wrong scope caller accepted"); guard.current.caller = context.caller;
        guard.current.thread ^= 1; Check(!OwnedScopeActive(state), "Wrong scope thread accepted"); guard.current.thread = context.thread;
        state.configured.store(false); Check(!OwnedScopeActive(state), "Unconfigured state accepted"); state.configured.store(true);
        state.engineThread.store(0); Check(!OwnedScopeActive(state), "Unset engine thread accepted"); state.engineThread.store(context.thread);
        Check(OwnedScopeActive(state), "Restored scope provenance remained rejected");
    });
    Test("foreign_thread_and_noncurrent_stack_scopes_are_denied", [] {
        State state; const auto context = SyntheticContext(state);
        ScopeGuard guard(state, context);
        Check(guard.active, "Positive stack scope did not activate");
        bool foreignAccepted = true, foreignAcquired = true;
        std::thread other([&] {
            foreignAccepted = OwnedScopeActive(state);
            ScopeGuard foreign(state, context); foreignAcquired = foreign.active;
        });
        other.join();
        Check(!foreignAccepted && !foreignAcquired, "Foreign thread accepted or replaced engine stack scope");
        const auto known = state.activeScope.load();
        auto heap = std::make_unique<CallbackScope>(guard.current);
        Check(!OnCurrentStack(reinterpret_cast<uintptr_t>(heap.get()), sizeof(*heap)), "Heap object unexpectedly belongs to current stack");
        state.activeScope.store(reinterpret_cast<uintptr_t>(heap.get()));
        Check(!OwnedScopeActive(state), "Matching heap scope authorized");
        const auto stackBase = static_cast<uintptr_t>(__readgsqword(0x08));
        const auto stackLimit = static_cast<uintptr_t>(__readgsqword(0x10));
        Check(stackLimit && stackLimit < stackBase, "Current NT_TIB stack bounds unavailable");
        for (auto invalid : {uintptr_t(0), uintptr_t(1), stackLimit - 1, stackBase,
                             stackBase - sizeof(CallbackScope) + 1, UINTPTR_MAX}) {
            state.activeScope.store(invalid);
            Check(!OnCurrentStack(invalid, sizeof(CallbackScope)) && !OwnedScopeActive(state), "Out-of-bounds scope pointer accepted");
        }
        state.activeScope.store(known);
        Check(OwnedScopeActive(state), "Restored actual stack scope was lost");
    });
    Test("nested_scope_cas_preserves_outer_owner", [] {
        State state; const auto context = SyntheticContext(state);
        {
            ScopeGuard outer(state, context);
            Check(outer.active, "Outer scope did not activate");
            const auto owned = state.activeScope.load();
            {
                ScopeGuard inner(state, context);
                Check(!inner.active && state.activeScope.load() == owned, "Nested scope replaced outer publication");
                Check(OwnedScopeActive(state), "Failed nested acquisition damaged outer scope");
            }
            Check(state.activeScope.load() == owned && OwnedScopeActive(state) && !state.stopping.load(),
                  "Inactive nested destructor cleared or refused outer owner");
        }
        Check(!state.activeScope.load() && !OwnedScopeActive(state), "Outer destructor failed to clear owned scope");
    });
    Test("scope_cas_ownership_loss_fails_closed_without_clobbering", [] {
        State state; const auto context = SyntheticContext(state);
        state.ready.store(true); state.status.store(EngineDispatchStatus::ContextObserved);
        const auto foreign = uintptr_t(1);
        {
            ScopeGuard guard(state, context); Check(guard.active, "Scope did not acquire publication");
            state.activeScope.store(foreign);
        }
        Check(state.activeScope.load() == foreign, "Destructor overwrote an unexpected owner");
        Check(state.stopping.load() && !state.ready.load() && state.status.load() == EngineDispatchStatus::Refused &&
              state.reason.load() == PrivateScopeUnavailable && !OwnedScopeActive(state),
              "Scope ownership loss left execution authorized");
    });
    Test("scope_constructor_rejects_unconfigured_or_wrong_thread_context", [] {
        State state; auto context = SyntheticContext(state);
        state.configured.store(false);
        { ScopeGuard guard(state, context); Check(!guard.active && !state.activeScope.load(), "Unconfigured scope was published"); }
        state.configured.store(true); context.thread ^= 1;
        { ScopeGuard guard(state, context); Check(!guard.active && !state.activeScope.load(), "Foreign context thread was published"); }
        context.thread = GetCurrentThreadId(); state.engineThread.store(0);
        { ScopeGuard guard(state, context); Check(!guard.active && !state.activeScope.load(), "Unverified engine thread was published"); }
    });
    Test("one_consumer_and_eligibility_checked_before_invocation", [] {
        CallbackRegistry registry;
        const auto token = PublishRegistration(registry, GetModuleHandleW(nullptr), &Count);
        Check(token != 0, "Publication failed");
        Check(PublishRegistration(registry, GetModuleHandleW(nullptr), &Count) == 0, "Second consumer replaced first");
        Check(!InvokeRegistration(registry, []() noexcept { return false; }) && calls.load() == 0, "Ineligible callback executed");
        Check(InvokeRegistration(registry, []() noexcept { return true; }) && calls.load() == 1, "Eligible CPU callback did not execute");
        Check(registry.started.load() == 1 && registry.completed.load() == 1 && registry.inFlight.load() == 0, "Invocation accounting incorrect");
        Check(RevokeRegistration(registry, token, 100, true), "Completed registration did not retire");
        Check(!InvokeRegistration(registry, []() noexcept { return true; }) && calls.load() == 1, "Revoked callback executed");
    });
    Test("stale_tokens_cannot_revoke_or_fault_new_registration", [] {
        CallbackRegistry registry;
        const auto first = PublishRegistration(registry, GetModuleHandleW(nullptr), &Count);
        Check(RevokeRegistration(registry, first, 100, true), "First registration did not retire");
        const auto second = PublishRegistration(registry, GetModuleHandleW(nullptr), &Count);
        Check(second > first, "Registration tokens reused");
        Check(!RevokeRegistration(registry, first, 0, true), "Stale token revoked current callback");
        FailRegistration(registry, first, 9);
        Check(registry.failure.load() == 0 && registry.token.load() == second, "Stale failure affected new callback");
        Check(InvokeRegistration(registry, []() noexcept { return true; }), "Current callback lost after stale operations");
        Check(RevokeRegistration(registry, second, 100, true), "Second registration did not retire");
    });
    Test("in_flight_timeout_retains_record_and_prevents_replacement", [] {
        CallbackRegistry registry;
        const auto token = PublishRegistration(registry, GetModuleHandleW(nullptr), &Block);
        Worker worker; worker.thread = std::thread([&] { InvokeRegistration(registry, []() noexcept { return true; }); });
        WaitFor([] { return entered.load(); });
        const auto started = GetTickCount64();
        Check(!RevokeRegistration(registry, token, 5, true), "Retirement ignored active invocation");
        Check(GetTickCount64() - started < 500, "Bounded retirement blocked too long");
        Check(registry.published.load() == nullptr && registry.inFlight.load() == 1 && registry.resident.size() == 1, "Timed-out record was freed or left published");
        Check(PublishRegistration(registry, GetModuleHandleW(nullptr), &Count) == 0, "Replacement accepted before quiescence");
        releaseCallback.store(true); worker.thread.join();
        Check(RevokeRegistration(registry, token, 100, true), "Retirement retry failed after completion");
        Check(calls.load() == 1 && registry.resident.size() == 1, "Record did not remain resident");
    });
    Test("callback_self_revocation_is_nonblocking", [] {
        CallbackRegistry registry; fixtureRegistry = &registry;
        fixtureToken = PublishRegistration(registry, GetModuleHandleW(nullptr), &SelfRevoke);
        Check(InvokeRegistration(registry, []() noexcept { return true; }), "Self-revoking callback not invoked");
        Check(!selfReturned && selfElapsed < 100, "Self revocation waited for its own frame");
        Check(!executionAllowedAfterRevocation, "Active scope retained native execution authorization after self revocation");
        Check(!registry.published.load() && registry.inFlight.load() == 0, "Self revocation left callback published");
        Check(PublishRegistration(registry, GetModuleHandleW(nullptr), &Count) == 0, "Self-revoked token reused before explicit retirement");
        Check(RevokeRegistration(registry, fixtureToken, 100, true), "Worker could not finalize self revocation");
    });
    Test("caught_callback_exception_reports_sticky_failure", [] {
        CallbackRegistry registry; fixtureRegistry = &registry;
        fixtureToken = PublishRegistration(registry, GetModuleHandleW(nullptr), &CatchAndReport);
        Check(InvokeRegistration(registry, []() noexcept { return true; }), "Failure callback not invoked");
        Check(registry.failure.load() == 7 && !registry.published.load(), "Callback failure did not revoke publication");
        Check(!executionAllowedAfterRevocation, "Active scope retained native execution authorization after failure");
        Check(registry.started.load() == 1 && registry.completed.load() == 1, "Caught failure damaged invocation bookkeeping");
        Check(RevokeRegistration(registry, fixtureToken, 100, true), "Failed callback did not retire");
        Check(PublishRegistration(registry, GetModuleHandleW(nullptr), &Count) == 0, "Failed callback automatically restarted");
    });
    Test("concurrent_frame_reader_and_300_retirements", [] {
        CallbackRegistry registry;
        Worker worker;
        worker.thread = std::thread([&] {
            while (!stopDriver.load()) { InvokeRegistration(registry, []() noexcept { return true; }); Sleep(0); }
        });
        for (unsigned cycle = 0; cycle < 300; ++cycle) {
            uint64_t token = 0;
            WaitFor([&] { token = PublishRegistration(registry, GetModuleHandleW(nullptr), &Count); return token != 0; });
            const auto before = calls.load();
            WaitFor([&] { return calls.load() > before; });
            Check(RevokeRegistration(registry, token, 2000, true), "Concurrent retirement did not complete");
            const auto retired = calls.load(); Sleep(1);
            Check(calls.load() == retired, "Callback started after successful retirement");
        }
        stopDriver.store(true); worker.thread.join();
        Check(registry.started.load() == registry.completed.load() && registry.inFlight.load() == 0, "Concurrency left active invocation");
    });
    Test("public_failure_with_stale_token_cannot_create_capability", [] {
        ReportEngineDispatchCallbackFailure(0x8888, 1);
        Check(!UnregisterEngineDispatchCallback(0x8888, 0), "Unknown public token accepted");
        const auto observation = GetEngineDispatchObservation();
        Check(!observation.executableVerified && !observation.hookInstalled && !observation.contextReady &&
            !observation.callbackEnabled && observation.callbacksStarted == 0 && observation.calls == 0,
            "Synthetic registry fixtures altered actual game dispatcher state");
    });
    bool passed = true;
    std::cout << "{\"schema\":1,\"synthetic_registry_only\":true,\"game_context_proven\":false,\"game_started\":false,\"native_commands\":0,\"tests\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i) std::cout << ',';
        const auto& result = results[i];
        std::cout << "{\"name\":\"" << result.name << "\",\"passed\":" << (result.error.empty() ? "true" : "false");
        if (!result.error.empty()) { passed = false; std::cout << ",\"failure\":\"" << Escape(result.error) << '"'; }
        std::cout << '}';
    }
    std::cout << "],\"passed\":" << (passed ? "true" : "false") << "}\n";
    return passed ? 0 : 1;
}
