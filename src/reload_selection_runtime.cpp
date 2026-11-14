// SPDX-License-Identifier: GPL-3.0-or-later
#include "reload_selection_runtime.h"
#include "reload_mission_catalog.h"
#include "engine_dispatch.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace sm2debug {
namespace {
constexpr std::size_t kSelectionImageSize = 0xce54000;
constexpr std::uint64_t kSelectionFnv = 14695981039346656037ull;
struct SelectionImage {
    std::uintptr_t base = 0;
    std::uintptr_t At(std::uintptr_t rva) const { return base && rva < kSelectionImageSize ? base + rva : 0; }
};
struct SelectionRuntime {
    std::mutex mutex; SelectionImage image; bool supported = false, ownsPending = false;
    ReloadToken ownedWorld;
    std::uintptr_t ownedSystem = 0;
    std::array<std::uint8_t,0x310> ownedOverrides{};
} selectionRuntime;
bool SelectionRead(std::uintptr_t p, void* out, std::size_t size) {
    SIZE_T copied = 0;
    return p >= 0x10000 && size && size <= UINTPTR_MAX - p &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p), out, size, &copied) && copied == size;
}
template<class T> bool SelectionRead(std::uintptr_t p, T& out) { return SelectionRead(p, &out, sizeof(out)); }
template<class T> bool SelectionMatches(std::uintptr_t p, const T& wanted) { T v{}; return SelectionRead(p, v) && v == wanted; }
bool SelectionSpan(std::uintptr_t p, std::size_t size, bool writable = false) {
    if (p < 0x10000 || !size || size > 1024 * 1024 || size > UINTPTR_MAX - p) return false;
    const auto end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto protection = m.Protect & 0xff;
        if (writable && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY) return false;
        if (!writable && protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const auto first = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
        if (first > p || m.RegionSize > UINTPTR_MAX - first || first + m.RegionSize <= p) return false;
        p = std::min(end, first + m.RegionSize);
    }
    return true;
}
void SelectionHash(std::uint64_t& hash, const void* bytes, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
}
template<class T> void SelectionHash(std::uint64_t& hash, const T& value) { SelectionHash(hash, &value, sizeof(value)); }
bool SelectionText(std::uintptr_t p, const char* wanted) {
    std::array<char, 256> text{};
    const auto size = std::strlen(wanted) + 1;
    return size <= text.size() && SelectionRead(p, text.data(), size) && !std::memcmp(text.data(), wanted, size);
}
bool SelectionRelative(std::uintptr_t wrapper, std::uintptr_t& pointer) {
    if (!SelectionRead(wrapper, pointer)) return false;
    if (static_cast<std::intptr_t>(pointer) < 0) {
        const auto delta = static_cast<std::int32_t>(pointer);
        if (delta < 0 && wrapper < static_cast<std::uint64_t>(-static_cast<std::int64_t>(delta))) return false;
        if (delta >= 0 && wrapper > UINTPTR_MAX - static_cast<std::uint32_t>(delta)) return false;
        pointer = wrapper + delta;
    }
    return pointer >= 0x10000;
}
bool SelectionName(std::uintptr_t wrapper, const char* wanted) {
    std::uintptr_t pointer = 0; return SelectionRelative(wrapper, pointer) && SelectionText(pointer, wanted);
}
struct SelectionType { std::uintptr_t vt, col, td; const char* name; };
constexpr SelectionType selectionTypes[] = {
    {0x5485118,0x8e014e0,0xab48120,".?AVObjectiveSystemGame@@"},
    {0x86a3e88,0x901d720,0xac1dd80,".?AVMissionGraphBuiltGame@@"},
    {0x86a3de8,0x901d698,0xac1dd50,".?AVMissionNodeBuiltGame@@"},
    {0x86a3d48,0x901d610,0xac1dd20,".?AVObjectiveGraphBuiltGame@@"},
    {0x86a3ca8,0x901d588,0xac1dcf0,".?AVObjectiveNodeBuiltGame@@"},
    {0x83fd248,0x8fdb5c8,0xac05878,".?AVSortedNodeNames@@"}
};
bool SelectionTypes(const SelectionImage& image) {
    for (const auto& type : selectionTypes) {
        std::array<std::uint32_t,6> col{};
        if (!SelectionMatches(image.At(type.vt-8), image.At(type.col)) || !SelectionRead(image.At(type.col), col) ||
            col[0] != 1 || col[1] || col[3] != type.td || col[5] != type.col ||
            !SelectionText(image.At(type.td+16), type.name)) return false;
    }
    // Exact retained DDL field identities: AllowAvailable is a Boolean at+38;
    // SortedByDFT is a StringData array at+48, separate from raw Nodes order.
    return SelectionMatches(image.At(0xaa41f00+0x14), std::uint32_t(0x70)) &&
        SelectionMatches(image.At(0xaa41f00+0x60), image.At(0x86a3b10)) &&
        SelectionMatches(image.At(0xaa41f00+0x68), image.At(0x86a3b44)) &&
        SelectionMatches(image.At(0xaa41f00+0x88), image.At(0x86a3b60)) &&
        SelectionMatches(image.At(0x86a3b44+1), std::uint8_t(15)) &&
        SelectionMatches(image.At(0x86a3b60+4), std::uint32_t(0x38)) &&
        SelectionMatches(image.At(0xa78ac20+0x14), std::uint32_t(0x60)) &&
        SelectionMatches(image.At(0xa78ac20+0x88), image.At(0x83fbeb0)) &&
        SelectionMatches(image.At(0x83fbeb0+8), std::uint32_t(0x48));
}
struct SelectionPin { std::uintptr_t rva; std::size_t size; std::uint64_t hash; };
constexpr SelectionPin selectionPins[] = {
    {0x3589e70,0xc,0xa1055a0391265240ull}, {0x357f470,0x83,0x9accdc7ac468818cull},
    {0x3586d30,0xc6,0xfc7cc147dfaa0507ull}, {0x3587fb0,0x70,0x63c2914653248098ull},
    {0x3585952,0x43,0x5b433092bdddbe29ull}, {0x35860a0,0x11f,0x971ceb2478459efeull},
    {0x358edd0,0xf0,0x21ffc35ccdc29db7ull}, {0x35822b0,0x23,0x6eee90ec66217300ull},
    {0x3582350,0x2c,0xdc1f2946c10acf4aull}, {0x3582410,0x23,0x1edc043a9ef48a45ull},
    {0x3582450,0x2c,0x10a9ceb6fd3ce1cdull}, {0x377c1e0,0x9,0x248461d0494ef2f5ull},
    {0x377c200,0xc,0xa5fcc1b540b5f108ull}, {0x377c240,0x9,0xb1232b1567390154ull},
    {0x377c260,0xc,0xb4e2f0f69fbdac0dull}, {0x30942f0,0x79,0x4466ef20070fe189ull},
    {0x30943f0,0x50,0x5404f955e637ef53ull}, {0x30989d0,0xd,0x43506e058104723aull}
};
bool SelectionCode(const SelectionImage& image) {
    std::array<unsigned char,512> bytes{};
    for (const auto& pin : selectionPins) {
        std::uint64_t hash = kSelectionFnv;
        if (pin.size > bytes.size() || !SelectionRead(image.At(pin.rva), bytes.data(), pin.size)) return false;
        SelectionHash(hash, bytes.data(), pin.size);
        if (hash != pin.hash) return false;
    }
    return true;
}
// Implements the same retained native 32/64-bit hash/probe. Rechecks all keys
// and the header, and fingerprints the selected owned entry pointer.
template<class Key> bool SelectionMap(std::uintptr_t header, Key key, std::uintptr_t& object, std::uint64_t& hash,
                                     bool* absent = nullptr) {
    if(absent)*absent=false;
    std::array<unsigned char,40> before{}, after{};
    std::uintptr_t keys = 0, values = 0; std::uint32_t capacity = 0, stride = 0;
    if (!key || !SelectionRead(header, before)) return false;
    std::memcpy(&keys, before.data(),8); std::memcpy(&values,before.data()+8,8);
    std::memcpy(&capacity,before.data()+0x14,4); std::memcpy(&stride,before.data()+(sizeof(Key)==8?0x20:0x1c),4);
    if (!capacity || capacity > 16384 || stride != 64 ||
        !SelectionSpan(keys,static_cast<std::size_t>(capacity)*sizeof(Key)) ||
        !SelectionSpan(values,static_cast<std::size_t>(capacity)*stride)) return false;
    std::vector<Key> all(capacity), check(capacity);
    if (!SelectionRead(keys,all.data(),all.size()*sizeof(Key)) || std::count(all.begin(),all.end(),key)>1 ||
        std::find(all.begin(),all.end(),Key{})==all.end()) return false;
    const std::uint64_t product = static_cast<std::uint64_t>(key)*0x9e3779b1ull;
    auto slot = static_cast<std::uint32_t>((product>>32)^product)%capacity;
    std::uint32_t steps = 0;
    while (all[slot] && all[slot]!=key && ++steps<capacity) slot=(slot+1)%capacity;
    if (all[slot]!=key) {
        // A normal missing definition is distinct from unreadable/corrupt
        // storage. Preserve the complete absent lookup in the stale token.
        if(all[slot] || std::find(all.begin(),all.end(),key)!=all.end() || !SelectionRead(header,after) || before!=after ||
            !SelectionRead(keys,check.data(),check.size()*sizeof(Key)) || all!=check) return false;
        SelectionHash(hash,header);SelectionHash(hash,before);SelectionHash(hash,all.data(),all.size()*sizeof(Key));
        if(absent)*absent=true;
        return false;
    }
    const auto pointer = values+static_cast<std::size_t>(slot)*stride+0x28;
    if (!SelectionRead(pointer,object) || !SelectionSpan(object,0x70) ||
        !SelectionRead(header,after) || before!=after || !SelectionRead(keys,check.data(),check.size()*sizeof(Key)) || all!=check ||
        !SelectionMatches(pointer,object)) return false;
    SelectionHash(hash,header); SelectionHash(hash,before); SelectionHash(hash,all.data(),all.size()*sizeof(Key));
    SelectionHash(hash,pointer); SelectionHash(hash,object);
    return true;
}
bool SelectionSorted(std::uintptr_t sorted, std::uint64_t graph, std::uint32_t mission,
                     std::vector<std::uint32_t>& output, std::uint64_t& hash) {
    const auto& image=selectionRuntime.image;
    std::uint32_t count=0; std::uintptr_t array=0;
    if (!SelectionMatches(sorted,image.At(0x83fd248)) || !SelectionRead(sorted+0x50,count) || count>4096) return false;
    SelectionHash(hash,sorted); SelectionHash(hash,count);
    if (!count) return true;
    if (!SelectionRelative(sorted+0x48,array) || !SelectionSpan(array,static_cast<std::size_t>(count)*16)) return false;
    std::vector<unsigned char> before(static_cast<std::size_t>(count)*16),after(before.size());
    if (!SelectionRead(array,before.data(),before.size())) return false;
    for (std::uint32_t i=0;i<count;++i) {
        std::uint32_t key=0; std::memcpy(&key,before.data()+i*16+12,4);
        const char* name=nullptr;
        if (mission) { const auto* item=FindReloadMissionObjective(graph,mission,key); if (item) name=item->name; }
        else { const auto* item=FindReloadMission(graph,key); if (item) name=item->name; }
        if (!name) continue; // Do not manufacture asset identity for developer-only nodes.
        if (std::find(output.begin(),output.end(),key)!=output.end() || !SelectionName(array+i*16,name)) return false;
        output.push_back(key);
    }
    std::uintptr_t again=0;
    if (!SelectionMatches(sorted+0x50,count) || !SelectionRelative(sorted+0x48,again) || again!=array ||
        !SelectionRead(array,after.data(),after.size()) || before!=after) return false;
    SelectionHash(hash,array); SelectionHash(hash,before.data(),before.size());
    return true;
}

bool SelectionAdmittedWorldMatches(const ReloadToken& t) {
    const auto& image = selectionRuntime.image;
    if (t.imageBase != image.base || !t.contextSerial || t.manager != image.At(0xb26b150) ||
        t.managerVtable != image.At(0x547d668) || t.managerState != 22 || t.managerReady != 1 ||
        t.pendingInstances || t.pendingDeletions || t.modeValue != 9 || t.kernelState != 3 ||
        t.loadedLevel < 0x10000 || t.mode < 0x10000 || t.mode > UINTPTR_MAX - 0x14 ||
        t.kernel < 0x10000 || t.kernel > UINTPTR_MAX - 0x6800 || t.kernelOwner < 0x10000 ||
        !t.levelAsset || static_cast<std::int32_t>(t.levelAsset >> 32) >= 0 ||
        t.checkpointRecords < 0x10000 || t.checkpointCount <= 0 || t.checkpointCount > 4096 ||
        t.saveSystem < 0x10000 || t.saveSystemVtable != image.At(0x54a1ca0) ||
        !SelectionMatches(image.At(0xc2266a8), t.manager) || !SelectionMatches(t.manager, t.managerVtable) ||
        !SelectionMatches(t.manager + 0x10, t.loadedLevel) || !SelectionMatches(t.manager + 0x430628, t.managerState) ||
        !SelectionMatches(t.manager + 0x199fc0, t.pendingInstances) || !SelectionMatches(t.manager + 0x319f68, t.pendingDeletions) ||
        !SelectionMatches(t.manager + 0x430665, t.managerReady) || !SelectionMatches(image.At(0xc21d330), t.mode) ||
        !SelectionMatches(t.mode + 0x10, t.modeValue) || !SelectionMatches(image.At(0xc21d630), t.kernelHandle) ||
        !SelectionMatches(image.At(0xc0bebe0), t.componentTable)) return false;
    const auto index = t.kernelHandle & 0xfffff;
    std::int32_t count = 0;
    if (!t.kernelGeneration || t.kernelGeneration != t.kernelHandle >> 20 ||
        !SelectionRead(image.At(0xc0bec00), count) || count <= 0 || count > 0x100000 ||
        index >= static_cast<std::uint32_t>(count) || t.componentTable < 0x10000 ||
        t.componentTable > UINTPTR_MAX - static_cast<std::size_t>(index) * 16 - 16) return false;
    const auto entry = t.componentTable + static_cast<std::size_t>(index) * 16;
    if (!SelectionMatches(entry, t.kernel) || !SelectionMatches(entry + 8, t.kernelGeneration) ||
        !SelectionMatches(t.kernel, image.At(0x83c0ef0)) || !SelectionMatches(t.kernel + 0x10, t.kernelOwner) ||
        !SelectionMatches(t.kernel + 0x58, t.levelAsset) || !SelectionMatches(t.kernel + 0x62c0, t.currentCheckpoint) ||
        !SelectionMatches(t.kernel + 0x62f0, t.kernelSequence) || !SelectionMatches(t.kernel + 0x6308, t.kernelState) ||
        !SelectionMatches(t.manager + 0x41ff50, t.checkpointRecords) ||
        !SelectionMatches(t.manager + 0x41ff58, t.checkpointCount) ||
        !SelectionMatches(image.At(0xc0bd2d8), t.saveSystem) || !SelectionMatches(t.saveSystem, t.saveSystemVtable)) return false;
    std::array<unsigned char, 0x140> owner{};
    std::array<unsigned char, 8> loaded{};
    return SelectionRead(t.kernelOwner, owner.data(), owner.size()) && SelectionRead(t.loadedLevel, loaded.data(), loaded.size());
}
bool SameSelectionToken(const ReloadSelectionToken& a, const ReloadSelectionToken& b) {
    return SameReloadContext(a.world,b.world) && a.objectiveSystem==b.objectiveSystem &&
        a.graphFingerprint==b.graphFingerprint && a.priorOverrides==b.priorOverrides && a.priorEndReason==b.priorEndReason;
}
ReloadSelectionState SelectionReadSupplied(const ReloadState& requested, const ReloadState& current,
                                          const ReloadMissionSelection& selection) {
    ReloadSelectionState result;
    result.supported=selectionRuntime.supported; result.selection=selection;
    result.status=L"Mission start selection requires the current loaded graph definitions";
    if (!result.supported || !requested.supported || !requested.ready || !current.supported || !current.ready ||
        !SameReloadContext(requested.token,current.token) || current.token.imageBase!=selectionRuntime.image.base ||
        !SelectionAdmittedWorldMatches(current.token) || selection.branches.size()>64) return result;
    const auto* catalog=FindReloadMissionGraph(selection.graphId);
    if (!catalog || static_cast<std::int32_t>(selection.graphId>>32)>=0) return result;
    const auto& image=selectionRuntime.image;
    auto& token=result.token; token.world=current.token; token.objectiveSystem=image.At(0xb6a91d0);
    if (!SelectionMatches(image.At(0xc236a20),token.objectiveSystem) ||
        !SelectionMatches(token.objectiveSystem,image.At(0x5485118)) || !SelectionSpan(token.objectiveSystem,0x760,true) ||
        !SelectionMatches(token.objectiveSystem+0x1b1,std::uint8_t(0)) ||
        !SelectionRead(token.objectiveSystem+0x1d8,token.priorOverrides) ||
        !SelectionRead(token.objectiveSystem+0x514,token.priorEndReason)) return result;
    std::int32_t priorCount=0; std::memcpy(&priorCount,token.priorOverrides.data()+0x308,4);
    if (priorCount<0 || priorCount>64 || token.priorOverrides[0x30c]>1 || token.priorOverrides[0x30d]>1 ||
        token.priorEndReason < -1 || token.priorEndReason>10) return result;
    auto& fingerprint=token.graphFingerprint; fingerprint=kSelectionFnv;
    std::uintptr_t graph=0;
    if (!SelectionMap(token.objectiveSystem+0xe0,selection.graphId,graph,fingerprint) ||
        !SelectionSpan(graph,0x188) || !SelectionMatches(graph,image.At(0x86a3e88)) ||
        !SelectionMatches(graph+0x28,selection.graphId) ||
        !SelectionSorted(graph+0x120,selection.graphId,0,result.missionOrder,fingerprint)) return result;
    // Retail preserves the byte in the POD but omits the source's replay-only
    // load consumer. Replay remains on the separately verified MissionNode API.
    bool valid=!selection.branches.empty() && !selection.replayMode;
    result.branches.resize(selection.branches.size());
    for (std::size_t i=0;i<selection.branches.size();++i) {
        const auto& branch=selection.branches[i]; auto& info=result.branches[i];
        if (i && !branch.missionHash && !branch.objectiveHash) continue;
        const auto* mission=FindReloadMission(selection.graphId,branch.missionHash);
        if (!mission) { valid=false; continue; }
        std::uintptr_t definition=0, objectives=0; std::uint8_t available=0; std::uint32_t availableCheckpoint=0;
        if (!SelectionMap(graph+0xd8,branch.missionHash,definition,fingerprint) ||
            !SelectionMatches(definition,image.At(0x86a3de8)) || !SelectionName(definition+0x28,mission->name) ||
            !SelectionMatches(definition+0x34,branch.missionHash) || !SelectionRead(definition+0x38,available) || available>1 ||
            !SelectionMatches(definition+0x4c,mission->objectiveGraphAssetId) ||
            !SelectionRead(definition+0x6c,availableCheckpoint) || availableCheckpoint!=mission->availableCheckpoint.nameCrc) {
            valid=false; continue;
        }
        SelectionHash(fingerprint,available); SelectionHash(fingerprint,availableCheckpoint);
        info.allowAvailable=available!=0;
        bool absent=false;
        const bool haveObjectives=SelectionMap(token.objectiveSystem+0x90,mission->objectiveGraphAssetId,objectives,fingerprint,&absent);
        if ((!haveObjectives&&!absent) || (haveObjectives&&(!SelectionSpan(objectives,0x198) ||
            !SelectionMatches(objectives,image.At(0x86a3d48)) ||
            !SelectionSorted(objectives+0xe8,selection.graphId,branch.missionHash,info.objectiveOrder,fingerprint)))) {
            valid=false; continue;
        }
        const auto state=static_cast<std::uint8_t>(branch.state);
        if (state>2 || (state==1 && !available) || (state!=0 && branch.objectiveHash)) { valid=false; continue; }
        if (state==1) info.checkpoint=availableCheckpoint;
        if (state==0 && branch.objectiveHash) {
            const auto* objective=FindReloadMissionObjective(selection.graphId,branch.missionHash,branch.objectiveHash);
            std::uintptr_t node=0; std::uint32_t checkpoint=0;
            if (!haveObjectives || !objective || !SelectionMap(objectives+0x148,branch.objectiveHash,node,fingerprint) ||
                !SelectionMatches(node,image.At(0x86a3ca8)) || !SelectionName(node+0x28,objective->name) ||
                !SelectionMatches(node+0x34,branch.objectiveHash) || !SelectionRead(node+0x4c,checkpoint) ||
                checkpoint!=objective->checkpoint.nameCrc) { valid=false; continue; }
            info.checkpoint=checkpoint; SelectionHash(fingerprint,checkpoint);
        }
    }
    std::array<std::uint8_t,0x310> after{};
    if (!SelectionRead(token.objectiveSystem+0x1d8,after) || after!=token.priorOverrides ||
        !SelectionMatches(token.objectiveSystem+0x514,token.priorEndReason) ||
        !SelectionMatches(image.At(0xc236a20),token.objectiveSystem) ||
        !SelectionMatches(token.objectiveSystem+0x1b1,std::uint8_t(0)) || !SelectionAdmittedWorldMatches(current.token)) return result;
    // A declined native Reload leaves preparation in place. Permit a fresh
    // explicit retry only while the complete pending request is still ours;
    // never clear, roll back or adopt another subsystem's pending request.
    const bool owned=selectionRuntime.ownsPending && token.priorEndReason==5 &&
        SameReloadContext(selectionRuntime.ownedWorld,token.world) &&
        selectionRuntime.ownedSystem==token.objectiveSystem && selectionRuntime.ownedOverrides==token.priorOverrides;
    if (selectionRuntime.ownsPending && !owned) selectionRuntime.ownsPending=false;
    if (token.priorEndReason!=-1 && !owned) {
        result.status=L"A native mission reload preparation is already pending; selection was not changed";
        return result;
    }
    result.ready=valid;
    result.status=valid ? (owned ? L"Owned mission overrides are pending; an explicit Reload or Full Reload can retry them" :
        L"Selected mission branches ready for an explicit Reload or Full Reload") :
        (selection.replayMode ? L"Replay uses the separate native mission replay action; retail has no verified override-array replay consumer" :
        L"Choose valid mission branches; Available requires AllowAvailable and non-active branches have no objective");
    return result;
}
}
void InitializeReloadSelectionRuntime(HMODULE module, bool supported) {
    std::lock_guard lock(selectionRuntime.mutex);
    selectionRuntime.supported=false; selectionRuntime.image={};selectionRuntime.ownsPending=false;
    selectionRuntime.ownedWorld={};selectionRuntime.ownedSystem=0;selectionRuntime.ownedOverrides={};
    if (!supported || !module || module!=GetModuleHandleW(nullptr)) return;
    const SelectionImage image{reinterpret_cast<std::uintptr_t>(module)};
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!SelectionRead(image.base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x1000 ||
        !SelectionRead(image.base+dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.TimeDateStamp!=0x679b5991 ||
        nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage!=kSelectionImageSize ||
        (nt.OptionalHeader.ImageBase!=0x140000000 && nt.OptionalHeader.ImageBase!=image.base) ||
        !SelectionCode(image) || !SelectionTypes(image)) return;
    selectionRuntime.image=image; selectionRuntime.supported=true;
}
ReloadSelectionState ReadReloadSelectionState(const ReloadState& world, const ReloadMissionSelection& selection) {
    std::lock_guard lock(selectionRuntime.mutex);
    if (!selectionRuntime.supported || !world.supported || !world.ready) return SelectionReadSupplied(world,{},selection);
    return SelectionReadSupplied(world,ReadReloadState(),selection);
}
bool PrepareReloadSelectionAfterAdmission(const ReloadSelectionState& captured,
                                          const ReloadState& admitted, std::wstring& status) {
    status=L"Mission start preparation refused: callback, world, graph definitions or prior override changed";
    if (!IsWithinVerifiedEngineCallback()) return false;
    std::unique_lock lock(selectionRuntime.mutex,std::try_to_lock);
    if (!lock.owns_lock() || !selectionRuntime.supported || !captured.supported || !captured.ready ||
        !admitted.supported || !admitted.ready || !SameReloadContext(captured.token.world,admitted.token) ||
        !SelectionCode(selectionRuntime.image) || !SelectionTypes(selectionRuntime.image)) return false;
    const ReloadState requested{true,true,false,captured.token.world,nullptr,L""};
    const auto fresh=SelectionReadSupplied(requested,admitted,captured.selection);
    if (!fresh.ready || !SameSelectionToken(captured.token,fresh.token)) return false;
    const auto final=SelectionReadSupplied(requested,admitted,captured.selection);
    if (!final.ready || !SameSelectionToken(fresh.token,final.token) ||
        !SelectionAdmittedWorldMatches(admitted.token) || !IsWithinVerifiedEngineCallback()) return false;
    auto bytes=final.token.priorOverrides;
    std::memcpy(bytes.data(),&captured.selection.graphId,8);
    std::int32_t count=0;
    for (const auto& branch:captured.selection.branches) {
        if (!branch.missionHash && !branch.objectiveHash) continue;
        auto* record=bytes.data()+8+static_cast<std::size_t>(count++)*12;
        std::memcpy(record,&branch.objectiveHash,4); std::memcpy(record+4,&branch.missionHash,4);
        record[8]=static_cast<std::uint8_t>(branch.state);
    }
    std::memcpy(bytes.data()+0x308,&count,4); bytes[0x30c]=1; bytes[0x30d]=captured.selection.replayMode?1:0;
    // Exact fixed ObjectiveSystem instance, admitted engine thread, no engine
    // object is fabricated or called. The native copy touches graph, count,
    // active records and the two flags; preserve unused records and padding.
    const auto system=final.token.objectiveSystem;
    const std::int32_t reason=5; // ObjectiveEndReason::DebugReloaded in both builds.
    std::memcpy(reinterpret_cast<void*>(system+0x514),&reason,4);
    std::memcpy(reinterpret_cast<void*>(system+0x1d8),bytes.data(),8);
    std::memcpy(reinterpret_cast<void*>(system+0x4e0),bytes.data()+0x308,4);
    std::memcpy(reinterpret_cast<void*>(system+0x1e0),bytes.data()+8,static_cast<std::size_t>(count)*12);
    std::memcpy(reinterpret_cast<void*>(system+0x4e4),bytes.data()+0x30c,2);
    selectionRuntime.ownsPending=true;selectionRuntime.ownedWorld=admitted.token;
    selectionRuntime.ownedSystem=system;selectionRuntime.ownedOverrides=bytes;
    status=L"Mission start branches prepared; overrides remain if the native reload request is refused";
    return true;
}
}
