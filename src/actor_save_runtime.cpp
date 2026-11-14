// SPDX-License-Identifier: GPL-3.0-or-later
#include "actor_save_runtime.h"
#include "engine_dispatch.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace sm2debug {
namespace {
constexpr std::size_t kActorImageSize = 0xce54000, kActorMapLimit = 8192;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
using ActorResetFunction = void (*)(void*, bool);
struct ActorImage {
    std::uintptr_t base = 0;
    bool Contains(std::uintptr_t p, std::size_t n) const {
        return base && p >= base && n <= kActorImageSize && p-base <= kActorImageSize-n;
    }
    std::uintptr_t At(std::uintptr_t rva) const { return rva < kActorImageSize ? base+rva : 0; }
};
struct ActorRuntime {
    std::mutex mutex;
    ActorImage image;
    bool initialized = false, supported = false, applied = false;
    ReloadToken appliedWorld;
    ActorResetFunction reset = nullptr;
    std::wstring reason = L"Actor-save runtime has not initialized";
    // Scratch storage is allocated with the module, not on an engine stack.
    std::array<std::uint64_t, kActorMapLimit> keys{}, tracked{};
    std::array<unsigned char, kActorMapLimit> values{};
} actorRuntime;
bool ActorRead(std::uintptr_t p, void* value, std::size_t n) {
    SIZE_T copied = 0;
    return p >= 0x10000 && n && n <= UINTPTR_MAX-p &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p), value, n, &copied) && copied == n;
}
template<class T> bool ActorRead(std::uintptr_t p, T& value) { return ActorRead(p, &value, sizeof(value)); }
bool ActorWritable(std::uintptr_t p, std::size_t n) {
    if (p < 0x10000 || !n || n > UINTPTR_MAX-p) return false;
    const auto end = p+n;
    while (p < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            ((m.Protect & 0xff) != PAGE_READWRITE && (m.Protect & 0xff) != PAGE_WRITECOPY)) return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
        if (p < begin || p-begin >= m.RegionSize || m.RegionSize > UINTPTR_MAX-begin) return false;
        p = std::min(end, begin+m.RegionSize);
    }
    return true;
}
void ActorHashBytes(std::uint64_t& hash, const void* pointer, std::size_t length) {
    const auto* bytes = static_cast<const unsigned char*>(pointer);
    for (std::size_t i = 0; i < length; ++i) hash = (hash^bytes[i])*1099511628211ull;
}
bool ActorHashRange(std::uintptr_t p, std::size_t n, std::uint64_t& hash) {
    std::array<unsigned char, 512> bytes{};
    while (n) {
        const auto count = std::min(n, bytes.size());
        if (!ActorRead(p, bytes.data(), count)) return false;
        ActorHashBytes(hash, bytes.data(), count); p += count; n -= count;
    }
    return true;
}
bool ActorString(const ActorImage& image, std::uintptr_t p, const char* expected) {
    const auto n = std::strlen(expected)+1;
    std::array<char, 96> value{};
    return n <= value.size() && image.Contains(p,n) && ActorRead(p,value.data(),n) && std::memcmp(value.data(),expected,n) == 0;
}
bool ActorRtti(const ActorImage& image, std::uintptr_t vt, std::uintptr_t col, std::uintptr_t td, const char* name) {
    std::uintptr_t pointer = 0; std::array<std::uint32_t,6> record{};
    return ActorRead(image.At(vt-8),pointer) && pointer == image.At(col) && ActorRead(pointer,record) &&
        record[0] == 1 && record[1] == 0 && record[3] == td && record[5] == col && ActorString(image,image.At(td+16),name);
}
bool ActorCode(const ActorImage& image) {
    struct Code { std::uintptr_t rva; std::size_t size; std::uint64_t hash; };
    // Full bounded bodies/fragments contain only relative code operands; ASLR
    // does not change these bytes. The backend also pins the complete EXE hash.
    static constexpr Code code[] = {
        {0x35d8a20,0x3e,0x459fb692c2834c46ull},{0x2d74720,0x59,0x403a7f1428f9411bull},
        {0x2cde3b0,0xce,0x97e45ec19337ffebull},{0x30942f0,0x79,0x4466ef20070fe189ull},
        {0x3097090,0x56,0xd23befb488a0b84dull},{0x3b0ce20,0x2dc,0x2ce2109f629fce6cull},
        {0x3b0c860,0x2dc,0x5475e0abc5ce8318ull},{0x3b0d310,0x1e,0xf3c0d47af79ccd9aull},
        {0x3b0d2d0,0x1e,0x8e3213f75385795aull}
    };
    for (const auto& pin : code) {
        std::uint64_t hash = kFnvOffset;
        if (!ActorHashRange(image.At(pin.rva),pin.size,hash) || hash != pin.hash) return false;
    }
    return true;
}
bool ActorTypes(const ActorImage& image) {
    if (!ActorRtti(image,0x83d74c0,0x8fc7e90,0xabfe2f0,".?AVActorSaveSystem@@") ||
        !ActorRtti(image,0x8597f88,0x900ff20,0xac18ff0,".?AVActorSaveSystemSave@@") ||
        !ActorRtti(image,0x834fcd8,0x8f95fe8,0xabea878,".?AVSystemSaveDataContainer@@") ||
        !ActorRtti(image,0x54a1ca0,0x8e18500,0xab512a8,".?AVSaveSystemGame@@")) return false;
    std::uintptr_t name = 0, names = 0, types = 0, counts = 0, offsets = 0;
    std::uint32_t type = 0, size = 0; std::uint16_t count = 0;
    const auto d = image.At(0xa88b210);
    if (!ActorRead(d,name) || name != image.At(0x8597e88) || !ActorString(image,name,"ActorSaveSystemSave") ||
        !ActorRead(d+8,type) || type != 0x4930efc6 || !ActorRead(d+0x14,size) || size != 0x1a8 ||
        !ActorRead(d+0x18,count) || count != 3 || !ActorRead(d+0x60,names) || names != image.At(0x8597eb0) ||
        !ActorRead(d+0x68,types) || types != image.At(0x8597ef8) || !ActorRead(d+0x80,counts) || counts != image.At(0x8597f08) ||
        !ActorRead(d+0x88,offsets) || offsets != image.At(0x8597f18)) return false;
    static constexpr const char* fields[] = {"DeletedByUid","CollectedByUid","ActorFlagsByUid"};
    static constexpr std::uint32_t fieldOffsets[] = {0xb0,0x100,0x150};
    for (std::size_t i=0;i<3;++i) {
        std::uint8_t code = 0; std::uint32_t items = 1, offset = 0;
        if (!ActorRead(names+i*8,name) || !ActorString(image,name,fields[i]) || !ActorRead(types+i,code) || code != 15 ||
            !ActorRead(counts+i*4,items) || items != 0 || !ActorRead(offsets+i*4,offset) || offset != fieldOffsets[i]) return false;
    }
    return true;
}
struct ActorRange { std::uintptr_t address = 0; std::size_t size = 0; };
bool ActorOverlap(ActorRange a, ActorRange b) {
    return a.size && b.size && a.address < b.address+b.size && b.address < a.address+a.size;
}
struct ActorRanges {
    std::array<ActorRange,20> values{}; std::size_t count = 0;
    bool Add(std::uintptr_t p, std::size_t n, bool disjoint = true) {
        if (!n) return true;
        if (p < 0x10000 || n > UINTPTR_MAX-p || count == values.size()) return false;
        const ActorRange next{p,n};
        if (disjoint) for (std::size_t i=0;i<count;++i) if (ActorOverlap(next,values[i])) return false;
        values[count++] = next; return true;
    }
};
template<class T> T ActorHeaderField(const std::array<unsigned char,0x50>& header, std::size_t offset) {
    T value{}; std::memcpy(&value,header.data()+offset,sizeof(value)); return value;
}
bool ActorMap(std::uintptr_t address, const ActorImage& image, ActorRanges& ranges, bool cleared,
              std::uint32_t& entries, std::uint64_t& fingerprint, std::wstring& status) {
    std::array<unsigned char,0x50> header{}, after{};
    if (!ActorRead(address,header)) { status=L"Actor-save map header is unreadable"; return false; }
    const auto keys=ActorHeaderField<std::uintptr_t>(header,0), values=ActorHeaderField<std::uintptr_t>(header,8);
    const auto count=ActorHeaderField<std::int32_t>(header,0x10), capacity=ActorHeaderField<std::int32_t>(header,0x14);
    const auto stride=ActorHeaderField<std::uint32_t>(header,0x20);
    const auto tracking=ActorHeaderField<std::uintptr_t>(header,0x38), allocator=ActorHeaderField<std::uintptr_t>(header,0x48);
    const auto trackedCount=ActorHeaderField<std::int32_t>(header,0x40), trackedCapacity=ActorHeaderField<std::int32_t>(header,0x44);
    if (count < 0 || capacity < 0 || count > capacity || capacity > static_cast<int>(kActorMapLimit) ||
        trackedCount < 0 || trackedCapacity < 0 || trackedCount > trackedCapacity || trackedCapacity > static_cast<int>(kActorMapLimit) ||
        (capacity && stride != 1) || (!capacity && (keys || values || count)) ||
        (!trackedCapacity && (tracking || trackedCount)) || (trackedCapacity && !tracking)) {
        status=L"Actor-save UID map count, capacity or Boolean stride is invalid"; return false;
    }
    fingerprint=kFnvOffset; ActorHashBytes(fingerprint,header.data(),header.size());
    const auto keyBytes=static_cast<std::size_t>(capacity)*8, valueBytes=static_cast<std::size_t>(capacity);
    const auto trackedBytes=static_cast<std::size_t>(trackedCapacity)*8;
    for (const auto range : {ActorRange{keys,keyBytes},ActorRange{values,valueBytes},ActorRange{tracking,trackedBytes}}) {
        if (range.size && (image.Contains(range.address,range.size) || !ActorWritable(range.address,range.size) || !ranges.Add(range.address,range.size))) {
            status=L"Actor-save UID arrays are inaccessible or alias another owned range"; return false;
        }
    }
    auto& source=actorRuntime.keys; auto& deleted=actorRuntime.tracked;
    if ((keyBytes && !ActorRead(keys,source.data(),keyBytes)) || (trackedBytes && !ActorRead(tracking,deleted.data(),trackedBytes)) ||
        (valueBytes && !ActorRead(values,actorRuntime.values.data(),valueBytes))) { status=L"Actor-save UID array read failed"; return false; }
    for (std::size_t i=0;i<valueBytes;++i) if (source[i] && actorRuntime.values[i]>1) {
        status=L"Actor-save occupied Boolean map slot contains an invalid value"; return false;
    }
    ActorHashBytes(fingerprint,actorRuntime.values.data(),valueBytes);
    ActorHashBytes(fingerprint,source.data(),keyBytes); ActorHashBytes(fingerprint,deleted.data(),trackedBytes);
    std::sort(source.begin(),source.begin()+capacity); std::sort(deleted.begin(),deleted.begin()+trackedCapacity);
    const auto sourceBegin=std::upper_bound(source.begin(),source.begin()+capacity,std::uint64_t(0));
    const auto deletedBegin=std::upper_bound(deleted.begin(),deleted.begin()+trackedCapacity,std::uint64_t(0));
    if (source.begin()+capacity-sourceBegin != count || deleted.begin()+trackedCapacity-deletedBegin != trackedCount ||
        std::adjacent_find(sourceBegin,source.begin()+capacity) != source.begin()+capacity ||
        std::adjacent_find(deletedBegin,deleted.begin()+trackedCapacity) != deleted.begin()+trackedCapacity) {
        status=L"Actor-save map occupancy or UID uniqueness is invalid"; return false;
    }
    std::size_t unionCount=static_cast<std::size_t>(trackedCount);
    for (auto it=sourceBegin;it!=source.begin()+capacity;++it)
        if (!std::binary_search(deletedBegin,deleted.begin()+trackedCapacity,*it)) ++unionCount;
    // No native allocator/growth path is admitted. Existing tracking storage
    // must fit every UID; null tracking storage is explicitly skipped by game.
    if (cleared && count && tracking && (unionCount >= static_cast<std::size_t>(trackedCapacity) ||
        (allocator && unionCount*4 > static_cast<std::size_t>(trackedCapacity)*3))) {
        status=L"Actor-save tracking capacity requires an unverified native growth path"; return false;
    }
    if (!ActorRead(address,after) || header != after) { status=L"Actor-save map changed while being read"; return false; }
    entries=static_cast<std::uint32_t>(count); return true;
}
bool SameActorToken(const ActorSaveToken& a, const ActorSaveToken& b) {
    return SameReloadContext(a.world,b.world) && a.block==b.block && a.container==b.container && a.entry==b.entry &&
        a.saveData==b.saveData && a.fingerprint==b.fingerprint && a.entries==b.entries && a.mapFingerprints==b.mapFingerprints;
}
ActorSaveState ActorReadLocked(const ReloadState& requested, bool postCall = false) {
    ActorSaveState state; state.supported=actorRuntime.supported;
    if (!actorRuntime.initialized || !state.supported) { state.status=actorRuntime.reason; return state; }
    const auto current=ReadReloadState();
    if (!requested.supported || !requested.ready || !current.ready || !SameReloadContext(requested.token,current.token) ||
        !current.token.contextSerial || current.token.imageBase != actorRuntime.image.base) {
        state.status=L"Actor-save reset requires the same ready Reload world"; return state;
    }
    if (!postCall && actorRuntime.applied && SameReloadContext(actorRuntime.appliedWorld,current.token)) {
        state.status=L"Actor-save reset already applied for this Reload context"; return state;
    }
    const auto& image=actorRuntime.image; auto& token=state.token; token.world=current.token; token.block=image.At(0xa196290);
    std::uintptr_t save=0, vptr=0, descriptor=0, name=0; std::int32_t initialized=0, kind=-1, scope=-1; std::uint32_t key=0;
    if (!ActorRead(image.At(0xc0bd2d8),save) || save != current.token.saveSystem || !ActorRead(save,vptr) || vptr != image.At(0x54a1ca0) ||
        !ActorRead(save+0x98,initialized) || initialized <= 0 || !ActorRead(token.block,vptr) || vptr != image.At(0x83d74c0) ||
        !ActorRead(image.At(0xc23c6b8),descriptor) || descriptor != image.At(0xa88b210) ||
        !ActorRead(image.At(0xc23c6c0),kind) || kind != 0 || !ActorRead(image.At(0xc23c6c8),name) || name != image.At(0x83d7500) ||
        !ActorRead(image.At(0xc23c6d0),key) || key != 0x8c2d0f2d || !ActorRead(image.At(0xc23c6d4),scope) || scope != 1 ||
        !ActorRead(save+0x30,token.container) || image.Contains(token.container,1) || !ActorRead(token.container,vptr) || vptr != image.At(0x834fcd8)) {
        state.status=L"Actor-save native SaveSystem/block/container identity is unavailable"; return state;
    }
    struct Map32 { std::uintptr_t keys=0,values=0; std::int32_t count=0,capacity=0; std::uint32_t unused=0,stride=0; } map;
    static_assert(sizeof(Map32)==32);
    if (!ActorRead(token.container+0x28,map) || map.count<=0 || map.capacity<=map.count || map.capacity>8192 || map.stride!=80 ||
        !ActorWritable(map.keys,static_cast<std::size_t>(map.capacity)*4) || !ActorWritable(map.values,static_cast<std::size_t>(map.capacity)*80)) {
        state.status=L"Actor-save shared save-container map is invalid"; return state;
    }
    std::array<std::uint32_t,8192> rootKeys{};
    if (!ActorRead(map.keys,rootKeys.data(),static_cast<std::size_t>(map.capacity)*4) ||
        std::count_if(rootKeys.begin(),rootKeys.begin()+map.capacity,[](auto k){return k!=0;}) != map.count) {
        state.status=L"Actor-save shared save-container occupancy differs"; return state;
    }
    const std::uint64_t product=std::uint64_t(key)*0x9e3779b1u;
    const auto initial=static_cast<std::uint32_t>((product>>32)^product)%static_cast<std::uint32_t>(map.capacity);
    std::size_t index=initial, steps=0;
    while (rootKeys[index] && rootKeys[index]!=key && ++steps<static_cast<std::size_t>(map.capacity)) index=(index+1)%map.capacity;
    if (rootKeys[index]!=key || std::count(rootKeys.begin(),rootKeys.begin()+map.capacity,key)!=1) {
        state.status=L"ActorSaveSystem has no unique native shared-container entry"; return state;
    }
    token.entry=map.values+index*80;
    if (!ActorRead(token.entry+0x38,token.saveData) || image.Contains(token.saveData,1) || !ActorWritable(token.saveData,0x1a8) ||
        !ActorRead(token.saveData,vptr) || vptr!=image.At(0x8597f88)) {
        state.status=L"Actor-save live savedata type is unavailable; defaults are refused"; return state;
    }
    ActorRanges ranges;
    if (!ranges.Add(save,0x3300,false) || !ranges.Add(token.container,0x50,false) || !ranges.Add(token.entry,80,false) ||
        !ranges.Add(map.keys,static_cast<std::size_t>(map.capacity)*4,false) || !ranges.Add(map.values,static_cast<std::size_t>(map.capacity)*80,false) ||
        !ranges.Add(token.saveData,0x1a8)) { state.status=L"Actor-save object aliases save-system storage"; return state; }
    static constexpr std::size_t fields[]={0xb0,0x100,0x150};
    for (std::size_t i=0;i<3;++i)
        if (!ActorMap(token.saveData+fields[i],image,ranges,i!=1,token.entries[i],token.mapFingerprints[i],state.status)) return state;
    token.fingerprint=kFnvOffset; ActorHashBytes(token.fingerprint,&map,sizeof(map));
    ActorHashBytes(token.fingerprint,rootKeys.data(),static_cast<std::size_t>(map.capacity)*4);
    ActorHashBytes(token.fingerprint,token.mapFingerprints.data(),sizeof(token.mapFingerprints));
    std::uintptr_t finalData=0; Map32 finalMap{};
    if (!ActorRead(token.container+0x28,finalMap) || std::memcmp(&map,&finalMap,sizeof(map)) ||
        !ActorRead(token.entry+0x38,finalData) || finalData != token.saveData) {
        state.status=L"Actor-save container changed while being read"; return state;
    }
    state.ready=true; state.status=L"Ready: manual Reload can clear saved actor flags; collected items stay"; return state;
}
}

void InitializeActorSaveRuntime(HMODULE gameModule, bool supported) {
    std::lock_guard lock(actorRuntime.mutex); auto& runtime=actorRuntime;
    runtime.initialized=true; runtime.supported=false; runtime.image={}; runtime.reset=nullptr; runtime.applied=false;
    runtime.reason=L"Actor-save reset requires the verified retail executable";
    if (!supported || !gameModule || gameModule!=GetModuleHandleW(nullptr)) return;
    ActorImage image{reinterpret_cast<std::uintptr_t>(gameModule)}; IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!ActorRead(image.base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x1000 ||
        !ActorRead(image.base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.TimeDateStamp!=0x679b5991 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage!=kActorImageSize ||
        (nt.OptionalHeader.ImageBase!=0x140000000 && nt.OptionalHeader.ImageBase!=image.base) || !ActorCode(image) || !ActorTypes(image)) {
        runtime.reason=L"Actor-save native executable/code/type validation failed"; return;
    }
    runtime.image=image; runtime.reset=reinterpret_cast<ActorResetFunction>(image.At(0x35d8a20));
    runtime.supported=true; runtime.reason=L"Actor-save runtime initialized";
}
ActorSaveState ReadActorSaveState(const ReloadState& world) {
    std::lock_guard lock(actorRuntime.mutex); return ActorReadLocked(world);
}
ActorSaveResetOutcome ExecuteActorSaveReset(const ActorSaveState& captured, std::wstring& status) {
    if (!IsWithinVerifiedEngineCallback()) { status=L"Actor-save reset requires the current verified engine callback"; return ActorSaveResetOutcome::Refused; }
    std::unique_lock lock(actorRuntime.mutex,std::try_to_lock);
    if (!lock.owns_lock()) { status=L"Actor-save reset is busy"; return ActorSaveResetOutcome::Refused; }
    if (!captured.ready || !captured.supported || !actorRuntime.reset || !ActorCode(actorRuntime.image) || !ActorTypes(actorRuntime.image)) {
        status=L"Actor-save reset has no valid captured state or native contract"; return ActorSaveResetOutcome::Refused;
    }
    const auto fresh=ActorReadLocked(ReloadState{true,true,false,captured.token.world,nullptr,L""});
    if (!fresh.ready || !SameActorToken(captured.token,fresh.token)) {
        status=L"Actor-save world or maps changed; capture a fresh Reload request"; return ActorSaveResetOutcome::Refused;
    }
    if (!IsWithinVerifiedEngineCallback()) { status=L"Actor-save callback authorization expired"; return ActorSaveResetOutcome::Refused; }
    std::uint64_t beforeDirty=0;
    if (!ActorRead(fresh.token.saveData+0x18,beforeDirty)) { status=L"Actor-save dirty state is unreadable"; return ActorSaveResetOutcome::Refused; }
    actorRuntime.applied=true; actorRuntime.appliedWorld=fresh.token.world;
    try { actorRuntime.reset(reinterpret_cast<void*>(fresh.token.block),true); }
    catch (...) {
        status=L"Actor-save reset was invoked and failed; saved-state changes cannot be rolled back";
        return ActorSaveResetOutcome::AppliedButVerificationFailed;
    }
    const auto after=ActorReadLocked(ReloadState{true,true,false,fresh.token.world,nullptr,L""},true);
    std::uint64_t afterDirty=0;
    if (!after.ready || after.token.saveData!=fresh.token.saveData || after.token.entries[0] || after.token.entries[2] ||
        after.token.entries[1]!=fresh.token.entries[1] || after.token.mapFingerprints[1]!=fresh.token.mapFingerprints[1] ||
        !ActorRead(fresh.token.saveData+0x18,afterDirty) || afterDirty!=(beforeDirty|5)) {
        status=L"Actor-save reset was invoked, but verification failed; saved-state changes cannot be rolled back";
        return ActorSaveResetOutcome::AppliedButVerificationFailed;
    }
    status=L"Saved deleted-actor and actor-flag state reset; collected items kept. Reset persists if Reload later fails";
    return ActorSaveResetOutcome::Applied;
}
}
