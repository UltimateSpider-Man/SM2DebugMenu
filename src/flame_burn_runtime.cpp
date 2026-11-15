// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "flame_burn_runtime.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace sm2debug::flame_burn {
namespace {
constexpr std::uint64_t WeaponAsset=0xaff67d91d723dd1dull;
constexpr std::uint32_t DamageEventType=0xa37b9229;
constexpr std::uintptr_t CallbackSlot=0xc2193b8,TrackerDescriptor=0xb87ca20;
constexpr std::uintptr_t FireDescriptor=0xb877d70,MaterialDescriptor=0xb6a1e80;
constexpr std::uintptr_t BotDescriptor=0xad6ec60;
constexpr std::size_t QueueCapacity=64,TargetCapacity=32;
struct Pin { std::uintptr_t rva; std::size_t size; std::uint64_t fnv; };
// Generated from the original 54215324... executable by the verification
// native-burn-review/verify_native_pins.py. Exact spans, not wildcard patterns.
constexpr Pin Pins[] = {
    {0x3425160,0xa50,0x6962f0adca7c3650ull}, // accepted-damage
    {0x3425c50,0x369,0x44101efbf3fa9b3dull}, // event-publisher
    {0x3425fc0,0x8,0x4cd05fc9a9abb0fbull}, // callback-setter
    {0x1c5c610,0x24,0x52a469511692198cull}, // tracker-prius-ctor
    {0x1c5b000,0x8a,0x5de0483cc4803383ull}, // tracker-prius-reset
    {0x16ae070,0x1fc,0x719a5f08abf3c8caull}, // tracker-damage
    {0x16ae270,0x1aa,0xfb8a44e6dfea042full}, // tracker-accumulate
    {0x16e0030,0x70,0x0c3abb0d673c85e2ull}, // tracker-fire-init
    {0x16e00a0,0x19d,0x6fb0609c4f90f480ull}, // tracker-fire-start
    {0x16e0240,0x94,0xa8dc610f355f2712ull}, // tracker-fire-refresh
    {0x16e02e0,0x1f,0x6d36204e54f6b646ull}, // tracker-fire-end
    {0x16ae560,0x5d,0xaa8ce5d7297b83d7ull}, // tracker-attribution
    {0x16ae5c0,0x74,0x40828b6a7bff235dull}, // tracker-duration
    {0x29ba570,0x1b4,0x1e09d8e1cb180104ull}, // component-remove
    {0x28a42c0,0x31,0xb983e61329834c49ull}, // component-create
    {0x39e1d00,0xba,0x8ae2c0fc282aa6caull}, // event-ctor
    {0x39e95b0,0x44,0xdb383fe97e0178cdull}, // event-reset
    {0x39e77d0,0x30,0xf90fc3de247db811ull}, // event-release
    {0x34263b0,0xec,0xecaea58ddd03664aull}, // event-add-status
    {0x150b7c0,0xf,0x5cb17b02a2a4e75dull}, // fire-material-init
    {0x150b7d0,0x10,0xaaee91d4d0c84c43ull}, // fire-material-remove
    {0x150b890,0x84,0xfdba88f133d45b8aull}, // fire-material-create
    {0x150b960,0x20a,0xc06ee28827935fe1ull}, // fire-material-update
    {0x3680a30,0x86,0x1e76619ccc0acf18ull}, // fire-timer
    {0x16afa10,0x228,0xef95175825a3f8faull}, // fire-tick
    {0x16ad420,0x20,0xd3626604fe42a3c6ull}, // fire-config-getter
    {0x4e5260,0x7,0x1719e43f4d65732cull}, // weapon-name-getter
    {0x3607910,0xa,0xfc78f4205c19f7d7ull}, // shot-name-setter
    {0x3747ba5,0x25,0x220968426e729863ull}, // shot-name-forwarding
    {0x364b927,0x13,0xd71334507e249833ull}, // damage-name-forwarding
    {0x36ea400,0xf,0x5f90d56b37547f00ull}, // status-manager-create
    {0x16ade80,0x1be,0x533a5bfcc0eff0dcull}, // tracker-base-init
    {0x364b1b0,0x68,0xd2d408dd053f3189ull}, // shot-damage-defaults
    {0x364b7cb,0x30,0x48a09b63b4ba32dbull}, // shot-status-forwarding
    {0x364b905,0x22,0x22e438fdc2e54dcbull}, // damage-hash-forwarding
};
Host host{};
std::uintptr_t imageBase=0;
HMODULE callbackModule=nullptr;
std::atomic<bool> initialized{false},accepting{false};
std::atomic<unsigned> inFlight{0},ownedCount{0};
std::atomic<const char*> failure{"not initialized"};
std::atomic<std::uint64_t> acceptedCount{0},queuedCount{0},droppedCount{0};
std::atomic<std::uint64_t> appliedCount{0},createdCount{0},retiredCount{0};
// Every member is atomic, so the sequence retry does not hide a C++ data race.
struct Published {
    std::atomic<std::uint64_t> sequence{0},world{0};
    std::atomic<std::uintptr_t> actor{0},weapon{0},config{0};
    std::atomic<std::uint32_t> actorHandle{0},weaponHandle{0},nameHash{0},item{0};
} published;
struct Hit {
    Source source{};
    std::uint32_t target=0,frame=0,flags=0,request=0,damageHash=0;
    std::uint8_t macroType=0;
    float amount=0,status=0,duration=0,auxiliary=-1;
};
struct QueueEntry { std::atomic<unsigned> state{0}; Hit hit{}; };
std::array<QueueEntry,QueueCapacity> queue{};
struct Owned {
    std::uint64_t world=0;
    std::uintptr_t actor=0,tracker=0,fire=0,material=0;
    std::uint32_t actorHandle=0,trackerHandle=0,fireHandle=0,materialHandle=0;
};
std::array<Owned,TargetCapacity> targets{};

template<class T> bool Read(std::uintptr_t p,T& v) {
    return p && host.read && host.read(p,&v,sizeof v);
}
template<class T> T Field(const unsigned char* data,std::size_t offset) {
    T value{}; std::memcpy(&value,data+offset,sizeof value); return value;
}
template<class T> void Put(unsigned char* data,std::size_t offset,const T& value) {
    std::memcpy(data+offset,&value,sizeof value);
}
#ifdef SM2_FLAME_BURN_TEST
std::uintptr_t (*testNative)(std::uintptr_t)=nullptr;
#endif
template<class F> F Native(std::uintptr_t rva) {
#ifdef SM2_FLAME_BURN_TEST
    if (testNative) return reinterpret_cast<F>(testNative(rva));
#endif
    return reinterpret_cast<F>(imageBase+rva);
}
void Fail(const char* text) { failure.store(text,std::memory_order_relaxed); }
bool ComponentIdentity(std::uintptr_t p,std::uint32_t handle,std::uintptr_t actor,std::uintptr_t vt) {
    std::uintptr_t table=0,owner=0; std::uint32_t self=0; unsigned char flags=0;
    return p && handle && host.component(handle)==p && Read(p,table) && table==imageBase+vt &&
        Read(p+0x10,owner) && owner==actor && Read(p+0x1c,self) && self==handle &&
        Read(p+0x25,flags) && !(flags&2);
}
bool SourceIdentity(const Source& s) {
    std::uintptr_t table=0,config=0; std::uint64_t asset=0;
    std::uint32_t owner=0,nameHash=0;std::int64_t tagged=0;
    constexpr char expected[]="MJFlamethrower";
    std::array<char,sizeof expected> name{};
    if (!Read(s.config+0x30,tagged)) return false;
    const auto namePointer=tagged<0 ? s.config+0x30+static_cast<std::int32_t>(tagged) :
        static_cast<std::uintptr_t>(tagged);
    if (!namePointer || !host.read(namePointer,name.data(),name.size()) ||
        std::memcmp(name.data(),expected,sizeof expected)) return false;
    return s.world && s.item && s.nameHash && s.actorHandle && s.weaponHandle &&
        host.actor(s.actorHandle)==s.actor && host.component(s.weaponHandle)==s.weapon &&
        Read(s.weapon,table) && table==imageBase+0x5323378 &&
        Read(s.weapon+0x48,config) && config==s.config &&
        Read(config+8,asset) && asset==WeaponAsset &&
        Read(s.weapon+0x610,nameHash) && nameHash==s.nameHash &&
        Read(s.weapon+0x5b0,owner) && owner==s.actorHandle;
}
void Publish(const Source* s) {
    published.sequence.fetch_add(1,std::memory_order_acq_rel);
    const Source empty{}; const Source& v=s ? *s : empty;
    published.world.store(v.world,std::memory_order_relaxed);
    published.actor.store(v.actor,std::memory_order_relaxed);
    published.weapon.store(v.weapon,std::memory_order_relaxed);
    published.config.store(v.config,std::memory_order_relaxed);
    published.actorHandle.store(v.actorHandle,std::memory_order_relaxed);
    published.weaponHandle.store(v.weaponHandle,std::memory_order_relaxed);
    published.nameHash.store(v.nameHash,std::memory_order_relaxed);
    published.item.store(v.item,std::memory_order_relaxed);
    published.sequence.fetch_add(1,std::memory_order_release);
}
bool Snapshot(Source& s) {
    for (unsigned attempt=0;attempt<3;++attempt) {
        const auto before=published.sequence.load(std::memory_order_acquire);
        if (before&1) continue;
        s.world=published.world.load(std::memory_order_relaxed);
        s.actor=published.actor.load(std::memory_order_relaxed);
        s.weapon=published.weapon.load(std::memory_order_relaxed);
        s.config=published.config.load(std::memory_order_relaxed);
        s.actorHandle=published.actorHandle.load(std::memory_order_relaxed);
        s.weaponHandle=published.weaponHandle.load(std::memory_order_relaxed);
        s.nameHash=published.nameHash.load(std::memory_order_relaxed);
        s.item=static_cast<std::uint16_t>(published.item.load(std::memory_order_relaxed));
        if (before==published.sequence.load(std::memory_order_acquire)) return s.world!=0;
    }
    return false;
}
bool Capture(std::uint32_t type,std::uintptr_t event,Hit& hit) {
    if (type!=DamageEventType || !Snapshot(hit.source)) return false;
    std::array<unsigned char,0x2b0> bytes{};
    if (!host.read(event,bytes.data(),bytes.size()) ||
        Field<std::uintptr_t>(bytes.data(),0)!=imageBase+0x84e9c00 ||
        Field<std::uint32_t>(bytes.data(),0x138)!=hit.source.actorHandle ||
        Field<std::uint16_t>(bytes.data(),0x1c8)!=hit.source.item ||
        !Field<std::uint32_t>(bytes.data(),0x1cc) ||
        Field<std::uint32_t>(bytes.data(),0x1d0)!=hit.source.nameHash ||
        Field<std::int32_t>(bytes.data(),0x13c)!=3 ||
        (Field<std::uint32_t>(bytes.data(),0x288)&0xf0)) return false;
    hit.amount=Field<float>(bytes.data(),0x28c);
    hit.target=Field<std::uint32_t>(bytes.data(),0x1e0);
    if (!std::isfinite(hit.amount) || hit.amount<=0 || hit.amount>100000 || !hit.target ||
        hit.target==hit.source.actorHandle || !SourceIdentity(hit.source)) return false;
    hit.frame=Field<std::uint32_t>(bytes.data(),0x28);
    hit.flags=Field<std::uint32_t>(bytes.data(),0x160);
    hit.request=Field<std::uint32_t>(bytes.data(),0x168);
    hit.damageHash=Field<std::uint32_t>(bytes.data(),0x1cc);
    hit.macroType=Field<std::uint8_t>(bytes.data(),0x1d4);
    const auto count=Field<std::uint32_t>(bytes.data(),0x178);
    const auto tagged=Field<std::int64_t>(bytes.data(),0x170);
    const auto entries=tagged<0 ? event+0x170+static_cast<std::int32_t>(tagged) :
        static_cast<std::uintptr_t>(tagged);
    if (!entries || !count || count>16) return false;
    for (unsigned i=0;i<count;++i) {
        std::array<unsigned char,0x38> entry{};
        if (!host.read(entries+i*entry.size(),entry.data(),entry.size())) return false;
        if (Field<std::int32_t>(entry.data(),0x28)!=3) continue;
        const float amount=Field<float>(entry.data(),0x2c);
        const float duration=Field<float>(entry.data(),0x30),aux=Field<float>(entry.data(),0x34);
        if (!std::isfinite(amount) || amount<=0 || amount>1000 ||
            !std::isfinite(duration) || !std::isfinite(aux)) return false;
        hit.status+=amount;
        if (duration>hit.duration) hit.duration=duration;
        if (aux>hit.auxiliary) hit.auxiliary=aux;
    }
    // A normal native DoT has no projectile weapon-name/status payload. It
    // cannot feed this queue and renew its own finite lifetime.
    return std::isfinite(hit.status) && hit.status>0 && hit.status<=1000 &&
        hit.duration>=0 && hit.duration<=5 && hit.auxiliary<=5;
}
void DamageCallback(std::uint32_t type,void* event) {
    inFlight.fetch_add(1,std::memory_order_acq_rel);
    if (accepting.load(std::memory_order_acquire)) {
        Hit hit;
        if (Capture(type,reinterpret_cast<std::uintptr_t>(event),hit)) {
            acceptedCount.fetch_add(1,std::memory_order_relaxed);
            bool pushed=false;
            for (auto& slot:queue) {
                unsigned empty=0;
                if (!slot.state.compare_exchange_strong(empty,1,std::memory_order_acquire)) continue;
                slot.hit=hit;
                if (accepting.load(std::memory_order_acquire)) {
                    slot.state.store(2,std::memory_order_release);
                    queuedCount.fetch_add(1,std::memory_order_relaxed);pushed=true;
                } else slot.state.store(0,std::memory_order_release);
                break;
            }
            if (!pushed) droppedCount.fetch_add(1,std::memory_order_relaxed);
        }
    }
    inFlight.fetch_sub(1,std::memory_order_release);
}
bool PinsMatch(std::uintptr_t base,std::size_t imageSize) {
    if (sizeof(Pins)/sizeof(Pins[0])<10 || imageSize<=CallbackSlot+8) return false;
    std::array<unsigned char,4096> data{};
    for (const auto& pin:Pins) {
        if (pin.size>data.size() || pin.rva>=imageSize || pin.size>imageSize-pin.rva ||
            !host.read(base+pin.rva,data.data(),pin.size)) return false;
        std::uint64_t hash=14695981039346656037ull;
        for (std::size_t i=0;i<pin.size;++i) { hash^=data[i];hash*=1099511628211ull; }
        if (hash!=pin.fnv) return false;
    }
    return true;
}
std::uintptr_t Find(std::uintptr_t actor,std::uintptr_t descriptor) {
    return host.find(actor,imageBase+descriptor);
}
bool TargetIdentity(const Owned& value) {
    return value.actorHandle && host.actor(value.actorHandle)==value.actor;
}
bool DeleteOwned(std::uintptr_t actor,std::uintptr_t component,std::uint32_t handle,
                 std::uintptr_t vt,std::uintptr_t descriptor) {
    if (!component) return true;
    const auto current=Find(actor,descriptor);
    if (current==UINTPTR_MAX) return false;
    if (current!=component) return true; // Never remove a replacement.
    // Native pools can reuse the address before the next engine tick. The
    // generation handle distinguishes that replacement from our old object.
    if (host.component(handle)!=component) return true;
    if (!ComponentIdentity(component,handle,actor,vt)) return false;
    Native<void(*)(void*,bool)>(0x29ba570)(reinterpret_cast<void*>(component),false);
    return Find(actor,descriptor)!=component;
}
bool Retire(Owned& value) {
    if (!value.actor) return true;
    if (!TargetIdentity(value)) { value={};return true; }
    // FireMaterial reads its tracker even after Fire has expired. Remove it
    // first and confirm removal before touching that dependency.
    if (!DeleteOwned(value.actor,value.material,value.materialHandle,0x5482100,MaterialDescriptor)) return false;
    value.material=0;value.materialHandle=0;
    if (!DeleteOwned(value.actor,value.fire,value.fireHandle,0x54b7590,FireDescriptor)) return false;
    value.fire=0;value.fireHandle=0;
    // A foreign replacement material/effect may depend on this tracker.
    if (Find(value.actor,MaterialDescriptor) || Find(value.actor,FireDescriptor)) return false;
    if (!DeleteOwned(value.actor,value.tracker,value.trackerHandle,0x54bc0d8,TrackerDescriptor)) return false;
    value={};retiredCount.fetch_add(1,std::memory_order_relaxed);return true;
}
bool GlobalFireReady() {
    std::uintptr_t entry=0,data=0,shader=0;unsigned char loaded=0;float duration=0,dps=0;
    // Exact read-only equivalent of16ad420, avoiding its sentinel0x30 result.
    if (!Read(imageBase+0x99f1ee8+0x10,entry) || !Read(entry,loaded) || loaded!=4 ||
        !Read(entry+0x30,data) || !data || !Read(data+0x38,duration) ||
        !Read(data+0x3c,dps) || !std::isfinite(duration) || duration!=5.0f ||
        !std::isfinite(dps) || dps!=10.0f) return false;
    std::int64_t tagged=0;
    if (!Read(data+0x40,tagged)) return false;
    shader=tagged<0 ? data+0x40+static_cast<std::int32_t>(tagged) : static_cast<std::uintptr_t>(tagged);
    // This is an already loaded shader asset pointer, not an asset identifier.
    std::uintptr_t readable=0;
    return shader && Read(shader,readable);
}
bool Apply(const Hit& hit,std::uint64_t world) {
    if (hit.source.world!=world || !SourceIdentity(hit.source) || !host.sourceValid(hit.source)) return false;
    const auto actor=host.actor(hit.target);
    if (!actor || actor==hit.source.actor) return false;
    const auto bot=Find(actor,BotDescriptor);
    std::uintptr_t vt=0,owner=0;std::uint32_t botHandle=0;
    if (!Read(bot,vt) || vt!=imageBase+0x5301708 || !Read(bot+0x10,owner) || owner!=actor ||
        !Read(bot+0x1c,botHandle) || host.component(botHandle)!=bot) return false;
    Owned* record=nullptr;
    for (auto& value:targets) if (value.actorHandle==hit.target && value.actor==actor && value.world==world) {
        record=&value;break;
    }
    if (!record) {
        if (Find(actor,TrackerDescriptor) || Find(actor,FireDescriptor) || Find(actor,MaterialDescriptor) ||
            !GlobalFireReady()) return false;
        for (auto& value:targets) if (!value.actor) { record=&value;break; }
        if (!record) { Fail("burn target capacity reached");return false; }
        alignas(16) std::array<unsigned char,0x30> prius{};
        Native<void*(*)(void*)>(0x1c5c610)(prius.data());
        Native<void(*)(void*)>(0x1c5b000)(prius.data());
        Put(prius.data(),8,1.0f); // one accepted M2 hit crosses the local threshold
        prius[0x25]=0; // no subscription to other weapons' damage events
        prius[0x28]=0; // never delete the target actor when a burn completes
        const auto tracker=Native<std::uintptr_t(*)(void*,void*,void*)>(0x28a42c0)(
            reinterpret_cast<void*>(actor),reinterpret_cast<void*>(imageBase+TrackerDescriptor),prius.data());
        std::uint32_t trackerHandle=0;
        // Record a returned object before validation so a failed confirmation
        // cannot silently abandon a native component we just created. An
        // unreadable identity keeps shutdown pending until actor destruction.
        if (tracker) {
            Read(tracker+0x1c,trackerHandle);
            *record={world,actor,tracker,0,0,hit.target,trackerHandle,0,0};
            createdCount.fetch_add(1,std::memory_order_relaxed);
        }
        if (!Read(tracker+0x1c,trackerHandle) ||
            !ComponentIdentity(tracker,trackerHandle,actor,0x54bc0d8) || host.actor(hit.target)!=actor) {
            Fail("native fire tracker creation not confirmed");return false;
        }
    }
    if (!ComponentIdentity(record->tracker,record->trackerHandle,actor,0x54bc0d8)) return false;
    // Never refresh a replacement belonging to another native feature.
    const auto oldFire=Find(actor,FireDescriptor),oldMaterial=Find(actor,MaterialDescriptor);
    if ((oldFire && oldFire!=record->fire) || (oldMaterial && oldMaterial!=record->material)) return false;
    // Only owned storage is populated. This object is consumed by the status
    // tracker; it is never sent to DamageSystem or to event recipients.
    alignas(16) std::array<unsigned char,0x4d0> event{};
    Native<void*(*)(void*)>(0x39e1d00)(event.data());
    Native<bool(*)(void*)>(0x39e95b0)(event.data());
    Put(event.data(),0x28,hit.frame);Put(event.data(),0x138,hit.source.actorHandle);
    Put(event.data(),0x13c,std::int32_t{3});Put(event.data(),0x144,hit.amount);
    Put(event.data(),0x160,hit.flags);Put(event.data(),0x168,hit.request);
    Put(event.data(),0x1c8,hit.source.item);Put(event.data(),0x1cc,hit.damageHash);
    Put(event.data(),0x1d0,hit.source.nameHash);Put(event.data(),0x1d4,hit.macroType);
    Put(event.data(),0x1e0,hit.target);Put(event.data(),0x28c,hit.amount);
    constexpr std::uint64_t present=(1ull<<0)|(1ull<<9)|(1ull<<10)|(1ull<<12)|
        (1ull<<19)|(1ull<<21)|(1ull<<26)|(1ull<<27)|(1ull<<28)|(1ull<<29)|
        (1ull<<33)|(1ull<<42);
    Put(event.data(),8,Field<std::uint64_t>(event.data(),8)|present);
    Put(event.data(),0x18,Field<std::uint64_t>(event.data(),0x18)|present);
    Native<void(*)(void*,float,int,float,float)>(0x34263b0)(
        event.data(),hit.status,3,hit.duration,hit.auxiliary);
    Native<void(*)(void*,void*)>(0x16ae070)(reinterpret_cast<void*>(record->tracker),event.data());
    Native<void(*)(void*)>(0x39e77d0)(event.data());
    const auto fire=Find(actor,FireDescriptor),material=Find(actor,MaterialDescriptor);
    std::uint32_t fireHandle=0,materialHandle=0,damageHash=0;std::uint16_t weaponId=0;
    if (fire && Read(fire+0x1c,fireHandle) && ComponentIdentity(fire,fireHandle,actor,0x54b7590)) {
        record->fire=fire;record->fireHandle=fireHandle;
    }
    if (material && Read(material+0x1c,materialHandle) &&
        ComponentIdentity(material,materialHandle,actor,0x5482100)) {
        record->material=material;record->materialHandle=materialHandle;
    }
    if (!record->fire || !record->material || !Read(fire+0x78,damageHash) || damageHash!=hit.damageHash ||
        !Read(fire+0x7c,weaponId) || weaponId!=hit.source.item) {
        Fail("native burn effect/material not confirmed");return false;
    }
    appliedCount.fetch_add(1,std::memory_order_relaxed);return true;
}
void ClearQueue() {
    for (auto& slot:queue) { unsigned ready=2;slot.state.compare_exchange_strong(ready,0,std::memory_order_acq_rel); }
}
void CountOwned() {
    unsigned n=0;for (const auto& v:targets) if (v.actor) ++n;
    ownedCount.store(n,std::memory_order_release);
}
} // namespace

bool Initialize(std::uintptr_t base,std::size_t imageSize,const Host& adapter) {
    if (initialized.load() || !base || !adapter.read || !adapter.actor || !adapter.component ||
        !adapter.find || !adapter.sourceValid || !adapter.engineReady) return false;
    host=adapter;imageBase=base;
    if (!PinsMatch(base,imageSize)) { Fail("native burn code pins refused");return false; }
    // The native producer may have loaded its callback immediately before
    // detachment. Keep our code mapped for process lifetime; do not race a
    // delayed entry against FreeLibrary. No native object stores DLL callbacks.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&DamageCallback),&callbackModule)) {
        Fail("could not pin damage callback module");return false;
    }
    auto* slot=reinterpret_cast<void* volatile*>(base+CallbackSlot);
    if (InterlockedCompareExchangePointer(slot,reinterpret_cast<void*>(&DamageCallback),nullptr)!=nullptr) {
        Fail("native damage callback already occupied");return false;
    }
    initialized.store(true,std::memory_order_release);
    accepting.store(true,std::memory_order_release);Fail("none");return true;
}
void EngineTick(const Source* source,std::uint64_t world) {
    if (!initialized.load(std::memory_order_acquire) || !host.engineReady()) return;
    const bool admit=accepting.load(std::memory_order_acquire);
    const bool valid=admit && source && source->world==world && SourceIdentity(*source) && host.sourceValid(*source);
    Publish(valid ? source : nullptr);
    for (auto& value:targets) {
        if (!value.actor) continue;
        if (!admit || !world || value.world!=world || !TargetIdentity(value)) { Retire(value);continue; }
        // Native expiry removes Fire first. Keep its tracker through the
        // remaining material cleanup, including dead/corpse targets.
        if (!Find(value.actor,FireDescriptor)) Retire(value);
    }
    std::array<std::uint32_t,QueueCapacity> seen{};unsigned seenCount=0;
    for (auto& slot:queue) {
        unsigned ready=2;
        if (!slot.state.compare_exchange_strong(ready,3,std::memory_order_acquire)) continue;
        const Hit hit=slot.hit;slot.state.store(0,std::memory_order_release);
        bool duplicate=false;
        for (unsigned i=0;i<seenCount;++i) if (seen[i]==hit.target) {duplicate=true;break;}
        if (admit && world && !duplicate) {
            seen[seenCount++]=hit.target;
            Apply(hit,world);
        }
    }
    CountOwned();
}
void RequestStop() {
    accepting.store(false,std::memory_order_release);
    if (initialized.load(std::memory_order_acquire))
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(imageBase+CallbackSlot),
            nullptr,reinterpret_cast<void*>(&DamageCallback));
}
bool DrainOnEngineThread() {
    if (!initialized.load()) return true;
    if (!host.engineReady()) return false;
    Publish(nullptr);ClearQueue();
    bool clean=true;for (auto& value:targets) if (!Retire(value)) clean=false;
    CountOwned();
    return clean && !inFlight.load(std::memory_order_acquire);
}
bool Quiescent(unsigned timeoutMs) {
    const auto until=GetTickCount64()+timeoutMs;
    while (inFlight.load(std::memory_order_acquire)) {
        if (GetTickCount64()>=until) return false;
        Sleep(1);
    }
    return !accepting.load(std::memory_order_acquire) && !ownedCount.load(std::memory_order_acquire);
}
Counters Status() {
    return {acceptedCount.load(),queuedCount.load(),droppedCount.load(),appliedCount.load(),
        createdCount.load(),retiredCount.load(),ownedCount.load(),inFlight.load(),
        initialized.load(),accepting.load()};
}
const char* LastFailure() { return failure.load(std::memory_order_relaxed); }
} // namespace sm2debug::flame_burn
