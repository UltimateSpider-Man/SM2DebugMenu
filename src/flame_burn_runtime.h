// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace sm2debug::flame_burn {
struct Source {
    std::uint64_t world=0;
    std::uintptr_t actor=0,weapon=0,config=0;
    // nameHash is the nonzero native WeaponGame+0x610 value (virtual+0x88),
    // forwarded into actual DamageEvent.WeaponNameHash; never an input hash.
    std::uint32_t actorHandle=0,weaponHandle=0,nameHash=0;
    std::uint16_t item=0;
};
struct Host {
    bool (*read)(std::uintptr_t,void*,std::size_t)=nullptr;
    // These two resolvers must only read memory; the damage callback uses them.
    std::uintptr_t (*actor)(std::uint32_t)=nullptr;
    std::uintptr_t (*component)(std::uint32_t)=nullptr;
    // Remaining callbacks are used only on the verified engine thread.
    // find: 0 means verified absent; UINTPTR_MAX means invalid/unreadable.
    std::uintptr_t (*find)(std::uintptr_t,std::uintptr_t)=nullptr;
    bool (*sourceValid)(const Source&)=nullptr;
    bool (*engineReady)()=nullptr;
};
struct Counters {
    std::uint64_t accepted=0,queued=0,dropped=0,applied=0,created=0,retired=0;
    std::uint32_t owned=0,inFlight=0;
    bool initialized=false,accepting=false;
};
// Call outside DllMain after the original executable/build has been verified.
// Refuses an occupied native callback slot; never chains unknown callbacks.
bool Initialize(std::uintptr_t base,std::size_t imageSize,const Host& host);
// Call only in the existing engine callback. A null source prevents new burns;
// existing burns may finish in the same world. A different/zero world retires
// all components owned by this module, in material/effect/tracker order.
void EngineTick(const Source* source,std::uint64_t world);
// Stop admission and detach our callback. No native world mutation here.
void RequestStop();
// Call on the engine thread after RequestStop, before destroying its adapter.
// False means owned native components still require another engine tick.
bool DrainOnEngineThread();
// Outside the engine callback: wait only for already-entered callbacks to leave.
// Caller must also require DrainOnEngineThread()==true before unloading DLL.
bool Quiescent(unsigned timeoutMs=1000);
Counters Status();
const char* LastFailure();
} // namespace sm2debug::flame_burn
