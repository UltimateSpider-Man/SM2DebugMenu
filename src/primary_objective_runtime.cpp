// SPDX-License-Identifier: GPL-3.0-or-later
#include "primary_objective_runtime.h"
#include "engine_dispatch.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <tuple>
#include <vector>

namespace sm2debug {
namespace {
constexpr std::size_t kPrimaryImageSize = 0xce54000;
constexpr std::uint32_t kObjectiveSystemCRC = 0x21a6159f;
// Exact native output of3583A20. It is plain lookup data, not a fabricated
// ScriptAction, Node or engine-owned container. Padding starts zero on both
// the independently read handle and the native output before comparison.
struct alignas(8) PrimaryHandle {
    std::uint32_t graph = 0, pad0 = 0;
    std::uint64_t node = 0;
    std::uint32_t missionGraph = 0, pad1 = 0;
    std::uint64_t missionNode = 0;
    std::uint32_t mission = 0, pad2 = 0;
};
static_assert(sizeof(PrimaryHandle) == 40 && alignof(PrimaryHandle) == 8);
using PrimaryLookup = PrimaryHandle* (*)(void*, PrimaryHandle*, std::uint32_t, std::uint32_t);
using PrimaryStateFunction = int (*)(void*, const PrimaryHandle*);
using PrimaryGraphFunction = void* (*)(const PrimaryHandle*);
using PrimaryNodeFunction = void* (*)(void*, std::uint64_t);
using PrimaryCompleteFunction = void (*)(void*);
struct PrimaryImage {
    std::uintptr_t base = 0, exceptions = 0;
    std::uint32_t exceptionCount = 0;
    bool Contains(std::uintptr_t address, std::size_t size = 1) const {
        return base && address >= base && size && size <= kPrimaryImageSize && address-base <= kPrimaryImageSize-size;
    }
    std::uintptr_t At(std::uintptr_t rva, std::size_t size = 1) const {
        return base && rva < kPrimaryImageSize && size && size <= kPrimaryImageSize-rva && base <= UINTPTR_MAX-rva ? base+rva : 0;
    }
};
struct PrimaryRuntime {
    std::mutex mutex;
    bool supported = false;
    PrimaryImage image;
    PrimaryLookup lookup = nullptr;
    PrimaryStateFunction state = nullptr;
    PrimaryGraphFunction graph = nullptr;
    PrimaryNodeFunction node = nullptr;
    PrimaryCompleteFunction complete = nullptr;
    std::wstring reason = L"Primary objective runtime has not initialized";
} primaryRuntime;
bool PrimaryReadBytes(std::uintptr_t address, void* destination, std::size_t size) {
    SIZE_T copied = 0;
    return address >= 0x10000 && size && size <= UINTPTR_MAX-address &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, size, &copied) && copied == size;
}
template<class T> bool PrimaryRead(std::uintptr_t address, T& value) { return PrimaryReadBytes(address, &value, sizeof(value)); }
std::uintptr_t PrimaryOffset(std::uintptr_t address, std::size_t offset) {
    return address >= 0x10000 && offset <= UINTPTR_MAX-address ? address+offset : 0;
}
bool PrimarySpan(std::uintptr_t address, std::size_t size, bool writable = false) {
    if (address < 0x10000 || !size || size > 1024*1024 || size > UINTPTR_MAX-address) return false;
    const auto end = address+size;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto protection = region.Protect & 0xff;
        if (writable && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY) return false;
        if (!writable && protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const auto first = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (first > address || region.RegionSize > UINTPTR_MAX-first || first+region.RegionSize <= address) return false;
        address = std::min(end, first+region.RegionSize);
    }
    return true;
}
bool PrimaryBytes(const PrimaryImage& image, std::uintptr_t rva, const char* hexadecimal) {
    const auto size = std::strlen(hexadecimal)/2;
    std::array<unsigned char, 512> bytes{};
    if (!size || size > bytes.size() || !image.At(rva, size) || !PrimaryReadBytes(image.At(rva), bytes.data(), size)) return false;
    auto nibble = [](char c) -> unsigned { return c >= '0' && c <= '9' ? c-'0' : c-'a'+10; };
    for (std::size_t i = 0; i < size; ++i) if (bytes[i] != ((nibble(hexadecimal[i*2]) << 4) | nibble(hexadecimal[i*2+1]))) return false;
    return true;
}
bool PrimaryText(const PrimaryImage& image, std::uintptr_t address, const char* text) {
    const auto size = std::strlen(text)+1;
    std::array<char, 96> bytes{};
    return size <= bytes.size() && image.Contains(address, size) && PrimaryReadBytes(address, bytes.data(), size) && std::memcmp(bytes.data(), text, size) == 0;
}
bool PrimaryRtti(const PrimaryImage& image, std::uintptr_t vt, std::uintptr_t col, std::uintptr_t type, const char* name) {
    std::uintptr_t actual = 0;
    std::array<std::uint32_t,6> locator{};
    return PrimaryRead(image.At(vt-8), actual) && actual == image.At(col) && PrimaryRead(actual, locator) &&
        locator[0] == 1 && locator[1] == 0 && locator[3] == type && locator[5] == col && PrimaryText(image, image.At(type+16), name);
}
bool PrimaryFunction(const PrimaryImage& image, std::uint32_t begin, std::uint32_t end, std::uint32_t unwind) {
    std::size_t first = 0, last = image.exceptionCount;
    while (first < last) {
        const auto middle = first+(last-first)/2;
        RUNTIME_FUNCTION function{};
        if (!PrimaryRead(image.exceptions+middle*sizeof(function), function)) return false;
        if (function.BeginAddress < begin) first = middle+1; else last = middle;
    }
    RUNTIME_FUNCTION function{};
    return first < image.exceptionCount && PrimaryRead(image.exceptions+first*sizeof(function), function) &&
        function.BeginAddress == begin && function.EndAddress == end && function.UnwindData == unwind;
}
bool PrimaryCodePins(const PrimaryImage& image) {
    return PrimaryFunction(image,0x3583a20,0x3583b62,0x965f0e0) &&
        PrimaryFunction(image,0x3583d90,0x3583e4b,0x965f13c) &&
        PrimaryFunction(image,0x2bc3cc0,0x2bc3cea,0x953dbb8) &&
        PrimaryFunction(image,0x3591750,0x35917e7,0x9660920) &&
        PrimaryBytes(image,0x3583a20,"48895c240848896c2410488974241848897c242041564883ec2033ff458bf189") &&
        PrimaryBytes(image,0x3583d90,"48895c2408574883ec20488bda488bf94883c210e8c7fdffff448bc083c0fd83") &&
        PrimaryBytes(image,0x2bc3cc0,"4883ec284883c148e843024d004885c07411488b084885c974094883c428e9fd1d000033c04883c428c3") &&
        PrimaryBytes(image,0x2bc4fe0,"8b11448bc241c1e814488d0d8021380781e2ffff0f00e9d5030000") &&
        PrimaryBytes(image,0x2bc53d0,"3b511873208bc24869d038010000488b41284803c233d28b4808c1e914443bc1480f45c2c333c0c3") &&
        PrimaryBytes(image,0x2bc5ae0,"488b4108c3") &&
        PrimaryBytes(image,0x1555540,"803db943150a000f8403c20302c3") &&
        PrimaryBytes(image,0x3591750,"40534883ec20488b01488bd9c7811001000002000000ff9008010000488b0341") &&
        PrimaryBytes(image,0x1555330,"8b810c010000c3") && PrimaryBytes(image,0x15514c0,"8b8188000000c3") &&
        PrimaryBytes(image,0x3583e50,"4883ec2833d2e8c5087fff488b80b00000004883c0384883c428c3");
}
bool PrimaryTypes(const PrimaryImage& image) {
    const std::array<std::array<std::uintptr_t,2>,8> slots{{
        {0x5485118+0x18,0x1539470},{0x548a108+0x38,0x15514c0},
        {0x548ab78+0x38,0x1555330},{0x548ab78+0xe8,0x1555540},
        {0x548ab78+0x108,0x1555970},{0x548ab78+0xa0,0x3590520},
        {0x548ab78+0x50,0x3591890},{0x548ab78+0x100,0x15555f0}}};
    if (!PrimaryRtti(image,0x5485118,0x8e014e0,0xab48120,".?AVObjectiveSystemGame@@") ||
        !PrimaryRtti(image,0x54a1ca0,0x8e18500,0xab512a8,".?AVSaveSystemGame@@") ||
        !PrimaryRtti(image,0x85979a8,0x900fe70,0xac18fc0,".?AVObjectiveSystemSaveBlock@@") ||
        !PrimaryRtti(image,0x548a108,0x8e05428,0xab49d50,".?AVMissionNodeGame@@") ||
        !PrimaryRtti(image,0x548ab78,0x8e059a0,0xab49f00,".?AVObjectiveNodeGame@@")) return false;
    for (const auto& slot : slots) { std::uintptr_t value = 0; if (!PrimaryRead(image.At(slot[0]),value) || value != image.At(slot[1])) return false; }
    return true;
}
bool LoadPrimaryImage(HMODULE module, PrimaryImage& image) {
    if (!module || module != GetModuleHandleW(nullptr)) return false;
    image.base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!PrimaryRead(image.base,dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 ||
        !PrimaryRead(image.base+dos.e_lfanew,nt) || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.TimeDateStamp != 0x679b5991 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != kPrimaryImageSize ||
        (nt.OptionalHeader.ImageBase != 0x140000000 && nt.OptionalHeader.ImageBase != image.base) ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return false;
    const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.Size || directory.Size % sizeof(RUNTIME_FUNCTION) || directory.Size/sizeof(RUNTIME_FUNCTION) > 1000000 ||
        !image.At(directory.VirtualAddress,directory.Size)) return false;
    image.exceptions = image.At(directory.VirtualAddress); image.exceptionCount = directory.Size/sizeof(RUNTIME_FUNCTION);
    return PrimaryCodePins(image) && PrimaryTypes(image);
}
bool SamePrimaryToken(const PrimaryObjectiveToken& a, const PrimaryObjectiveToken& b) {
    return std::tie(a.objectiveSystem,a.objectiveVtable,a.saveSystem,a.saveContainer,a.saveEntry,a.mapKeys,a.mapValues,
        a.saveBlock,a.systemData,a.primaryAddress,a.mapCapacity,a.mapStride,a.missionHash,a.objectiveHash,
        a.missionNode,a.objectiveNode,a.missionGraph,a.objectiveGraph,a.missionGraphHandle,a.objectiveGraphHandle,a.missionNodeId,a.objectiveNodeId,a.missionGraphAssetId) ==
        std::tie(b.objectiveSystem,b.objectiveVtable,b.saveSystem,b.saveContainer,b.saveEntry,b.mapKeys,b.mapValues,
        b.saveBlock,b.systemData,b.primaryAddress,b.mapCapacity,b.mapStride,b.missionHash,b.objectiveHash,
        b.missionNode,b.objectiveNode,b.missionGraph,b.objectiveGraph,b.missionGraphHandle,b.objectiveGraphHandle,b.missionNodeId,b.objectiveNodeId,b.missionGraphAssetId);
}
struct PrimaryMap {
    std::uintptr_t keys = 0, values = 0, entry = 0;
    std::uint32_t capacity = 0, stride = 0;
};
template<class Key> bool PrimaryMapFind(std::uintptr_t address, Key key, std::size_t minimumStride, PrimaryMap& result) {
    // These are the exact native32/64-bit keyed maps used by30942F0/3093F10.
    // Native probing has no full-table bound; our reader requires a zero slot
    // and bounds every allocation, product, and probe before native execution.
    std::array<unsigned char,40> before{}, after{};
    if (!key || !PrimaryRead(address,before)) return false;
    std::memcpy(&result.keys,before.data(),8); std::memcpy(&result.values,before.data()+8,8);
    std::memcpy(&result.capacity,before.data()+0x14,4);
    std::memcpy(&result.stride,before.data()+(sizeof(Key)==8?0x20:0x1c),4);
    if (!result.capacity || result.capacity > 16384 || result.stride < minimumStride || result.stride > 4096 ||
        !PrimarySpan(result.values, static_cast<std::size_t>(result.capacity)*result.stride)) return false;
    std::vector<Key> keys(result.capacity);
    if (!PrimaryReadBytes(result.keys,keys.data(),keys.size()*sizeof(Key)) || std::find(keys.begin(),keys.end(),Key{}) == keys.end()) return false;
    const auto product = static_cast<std::uint64_t>(key)*0x9e3779b1ull;
    auto slot = static_cast<std::uint32_t>((product>>32)^static_cast<std::uint32_t>(product))%result.capacity;
    for (std::size_t i = 0; i < result.capacity; ++i) {
        if (keys[slot] == key) {
            result.entry = PrimaryOffset(result.values,static_cast<std::size_t>(slot)*result.stride);
            Key live = 0;
            return result.entry && PrimaryRead(result.keys+slot*sizeof(Key),live) && live == key &&
                PrimaryRead(address,after) && before == after;
        }
        if (!keys[slot]) return false;
        slot = (slot+1)%result.capacity;
    }
    return false;
}
template<class Key> bool CompactFind(std::uintptr_t address, std::size_t pointerOffset, Key key,
                                    std::size_t stride, std::uintptr_t& record) {
    std::uintptr_t storage = 0; std::uint16_t count = 0, prefix = 0;
    if (!PrimaryRead(PrimaryOffset(address,pointerOffset),storage) || !PrimaryRead(PrimaryOffset(address,pointerOffset+8),count) ||
        !PrimaryRead(PrimaryOffset(address,pointerOffset+10),prefix) || !count || count > 4096 || prefix < count || prefix > 16384) return false;
    const auto total = static_cast<std::size_t>(prefix)*sizeof(Key)+static_cast<std::size_t>(count)*stride;
    if (!PrimarySpan(storage,total)) return false;
    std::vector<Key> keys(count);
    if (!PrimaryReadBytes(storage,keys.data(),keys.size()*sizeof(Key))) return false;
    const auto found = std::find(keys.begin(),keys.end(),key);
    if (found == keys.end() || std::find(found+1,keys.end(),key) != keys.end()) return false;
    record = PrimaryOffset(storage,static_cast<std::size_t>(prefix)*sizeof(Key)+static_cast<std::size_t>(found-keys.begin())*stride);
    std::uintptr_t again = 0; std::uint16_t c = 0, p = 0;
    return record && PrimaryRead(address+pointerOffset,again) && again == storage &&
        PrimaryRead(address+pointerOffset+8,c) && c == count && PrimaryRead(address+pointerOffset+10,p) && p == prefix;
}
bool PrimaryWorldMatches(const PrimaryImage& image, const ReloadToken& world) {
    if (!world.contextSerial || world.imageBase != image.base || !world.manager || !world.loadedLevel || !world.saveSystem ||
        world.saveSystemVtable != image.At(0x54a1ca0)) return false;
    std::uintptr_t value = 0; std::int32_t number = 0; std::uint8_t byte = 0; std::uint16_t sequence = 0;
    if (!(PrimaryRead(image.At(0xc2266a8),value) && value == world.manager &&
        PrimaryRead(world.manager,value) && value == world.managerVtable &&
        PrimaryRead(world.manager+0x10,value) && value == world.loadedLevel &&
        PrimaryRead(world.manager+0x430628,number) && number == world.managerState &&
        PrimaryRead(world.manager+0x199fc0,number) && number == 0 && number == world.pendingInstances &&
        PrimaryRead(world.manager+0x319f68,number) && number == 0 && number == world.pendingDeletions &&
        PrimaryRead(world.manager+0x430665,byte) && byte == 1 && byte == world.managerReady &&
        PrimaryRead(image.At(0xc21d330),value) && value == world.mode &&
        PrimaryRead(image.At(0xc0bd2d8),value) && value == world.saveSystem &&
        PrimaryRead(world.saveSystem,value) && value == world.saveSystemVtable)) return false;
    // ReloadState owns online/offline readiness. A legitimate offline world
    // can have no mode/kernel at all; never reinterpret missing optional data
    // as an invented online mode9/kernel3 requirement here.
    if (world.mode && (!PrimaryRead(world.mode+0x10,number) || number != world.modeValue)) return false;
    if (world.kernel && (!PrimaryRead(world.kernel+0x58,value) || value != world.levelAsset ||
        !PrimaryRead(world.kernel+0x62f0,sequence) || sequence != world.kernelSequence ||
        !PrimaryRead(world.kernel+0x6308,byte) || byte != world.kernelState)) return false;
    return true;
}
bool PrimarySaveToken(const PrimaryImage& image, const ReloadToken& world, PrimaryObjectiveToken& token) {
    token.saveSystem = world.saveSystem;
    std::uint8_t guard = 0; std::uintptr_t value = 0; std::uint32_t number = 0;
    const auto registration = image.At(0xb6a9170,32);
    if (!PrimaryRead(image.At(0xc236a20),token.objectiveSystem) || token.objectiveSystem != image.At(0xb6a91d0) ||
        !PrimarySpan(token.objectiveSystem,0x760,true) || !PrimaryRead(token.objectiveSystem,token.objectiveVtable) ||
        token.objectiveVtable != image.At(0x5485118) || !PrimaryRead(token.objectiveSystem+0x1b1,guard) || guard != 0 ||
        !PrimaryRead(image.At(0xb6a9900),guard) || guard != 0 ||
        !PrimaryRead(registration,value) || value != image.At(0xa88ad60) ||
        !PrimaryRead(registration+8,number) || number != 0 || !PrimaryRead(registration+16,value) || value != image.At(0x54850b0) ||
        !PrimaryRead(registration+24,number) || number != kObjectiveSystemCRC || !PrimaryRead(registration+28,number) || number != 1 ||
        !PrimaryRead(token.saveSystem+0x98,number) || !number || !PrimaryRead(token.saveSystem+0x30,token.saveContainer)) return false;
    PrimaryMap map;
    if (!PrimaryMapFind(token.saveContainer+0x28,kObjectiveSystemCRC,0x40,map)) return false;
    token.mapKeys=map.keys; token.mapValues=map.values; token.mapCapacity=map.capacity; token.mapStride=map.stride; token.saveEntry=map.entry;
    if (!PrimaryRead(token.saveEntry+0x38,token.saveBlock) || !PrimaryRead(token.saveBlock,value) || value != image.At(0x85979a8) ||
        !PrimaryRead(token.saveBlock+0xb0,token.systemData) || image.Contains(token.systemData) ||
        !PrimarySpan(token.systemData,0x68,true) || !PrimaryRead(token.systemData+0x60,token.missionHash) || !token.missionHash ||
        !PrimaryRead(token.systemData+0x64,token.objectiveHash) || !token.objectiveHash) return false;
    token.primaryAddress = token.systemData+0x38;
    return true;
}
bool RawPrimaryGraph(const PrimaryImage& image, std::uint32_t handle, std::uintptr_t& graph) {
    std::uint32_t count = 0, actual = 0; std::uintptr_t pool = 0;
    const auto index = handle & 0xfffff;
    return (handle>>20) && PrimaryRead(image.At(0x9f47170+0x18),count) && count <= 0x100000 && index < count &&
        PrimaryRead(image.At(0x9f47170+0x28),pool) &&
        (graph=PrimaryOffset(pool,static_cast<std::size_t>(index)*0x138)) && PrimarySpan(graph,0x138) &&
        PrimaryRead(graph+8,actual) && actual == handle;
}
bool RawPrimaryNode(const PrimaryImage& image, std::uint32_t graphHandle, std::uint64_t nodeId,
                    std::uintptr_t expected, std::uintptr_t& graph) {
    PrimaryMap map; std::uintptr_t wrapper = 0, actual = 0;
    return RawPrimaryGraph(image,graphHandle,graph) && PrimaryMapFind(graph+0x48,nodeId,8,map) &&
        PrimaryRead(map.entry,wrapper) && PrimaryRead(PrimaryOffset(wrapper,8),actual) && actual == expected;
}
bool SavedPrimaryState(std::uintptr_t block, std::uintptr_t node, bool mission, std::int32_t expected) {
    std::uintptr_t runtime = 0, resource = 0, data = 0; std::uint64_t asset = 0; std::uint32_t crc = 0; std::int32_t state = 0;
    PrimaryMap graph, entry;
    return PrimaryRead(node+8,runtime) && PrimaryRead(runtime,resource) && PrimaryRead(PrimaryOffset(resource,8),asset) && asset &&
        PrimaryRead(node+(mission?0x88:0x10c),crc) && crc && PrimaryMapFind(block+0xc0,asset,0x58,graph) &&
        PrimaryMapFind(graph.entry+0x30,crc,0x30,entry) && PrimaryRead(entry.entry+0x28,data) &&
        PrimarySpan(data,0x50,true) && PrimaryRead(data+0x30,state) && state == expected;
}
bool RawPrimaryHandle(const PrimaryImage& image, PrimaryObjectiveToken& token, PrimaryHandle& handle) {
    // Mirror only the bounded read-only lookups in3583A20. The native function
    // remains the execution authority: its output must match this independent
    // handle exactly before any native mutation can be reached.
    std::uintptr_t missionRecord = 0, graphRecord = 0, objectiveRecord = 0, vt = 0;
    std::int32_t marker = 0, state = 0; std::uint32_t crc = 0; std::uint64_t asset = 0;
    std::uintptr_t graphRuntime = 0, definition = 0, objectiveRuntime = 0;
    if (!CompactFind(token.objectiveSystem,0x20,token.missionHash,32,missionRecord) ||
        !PrimaryRead(missionRecord+8,marker) || marker >= 0 || !PrimaryRead(missionRecord+0x18,token.missionNode) ||
        !PrimaryRead(token.missionNode,vt) || vt != image.At(0x548a108) || !PrimarySpan(token.missionNode,0x1b0,true) ||
        !PrimaryRead(token.missionNode+0x88,crc) || crc != token.missionHash ||
        !PrimaryRead(missionRecord+0x0c,crc)) return false;
    asset=crc; if (!PrimaryRead(missionRecord+0x10,crc)) return false; asset |= static_cast<std::uint64_t>(crc)<<32;
    if (!CompactFind(token.objectiveSystem,0x160,asset,0x440,graphRecord) ||
        !CompactFind(graphRecord+0xa0,0x10,token.objectiveHash,8,objectiveRecord) ||
        !PrimaryRead(objectiveRecord,token.objectiveNode) || !PrimaryRead(token.objectiveNode,vt) || vt != image.At(0x548ab78) ||
        !PrimarySpan(token.objectiveNode,0x128,true) || !PrimaryRead(token.objectiveNode+0x10c,crc) || crc != token.objectiveHash ||
        //3592800 assigns the owning mission name/CRC to every loaded
        // objective at+88/+108. A reused graph must still belong to the
        // selected primary mission, not merely contain the same objective.
        !PrimaryRead(token.objectiveNode+0x108,crc) || crc != token.missionHash ||
        !PrimaryRead(token.objectiveNode+0x110,state) || state != 1 ||
        !PrimaryRead(token.missionNode+8,graphRuntime) || !PrimaryRead(graphRuntime+8,handle.missionGraph) ||
        !PrimaryRead(token.missionNode+0x18,definition) || !PrimaryRead(PrimaryOffset(definition,8),handle.missionNode) ||
        !PrimaryRead(token.objectiveNode+8,objectiveRuntime) || !PrimaryRead(objectiveRuntime+8,handle.graph) ||
        !PrimaryRead(token.objectiveNode+0x18,definition) || !PrimaryRead(PrimaryOffset(definition,8),handle.node)) return false;
    handle.mission = token.missionHash;
    token.missionGraphHandle=handle.missionGraph; token.objectiveGraphHandle=handle.graph;
    token.missionNodeId=handle.missionNode; token.objectiveNodeId=handle.node;
    std::uintptr_t missionResource=0;
    return RawPrimaryNode(image,handle.missionGraph,handle.missionNode,token.missionNode,token.missionGraph) &&
        PrimaryRead(token.missionGraph,missionResource) && PrimaryRead(PrimaryOffset(missionResource,8),token.missionGraphAssetId) && token.missionGraphAssetId &&
        token.missionGraph == graphRuntime && RawPrimaryNode(image,handle.graph,handle.node,token.objectiveNode,token.objectiveGraph) &&
        token.objectiveGraph == objectiveRuntime && SavedPrimaryState(token.saveBlock,token.missionNode,true,2) &&
        SavedPrimaryState(token.saveBlock,token.objectiveNode,false,1);
}
bool CapturePrimary(const PrimaryImage& image, const ReloadState& world, PrimaryObjectiveState& result, PrimaryHandle& handle) {
    result.supported = true; result.world = world.token;
    if (!world.supported || !world.ready || !PrimaryWorldMatches(image,world.token) || !PrimarySaveToken(image,world.token,result.token) ||
        !RawPrimaryHandle(image,result.token,handle)) return false;
    PrimaryObjectiveToken check;
    return PrimarySaveToken(image,world.token,check) && check.objectiveSystem == result.token.objectiveSystem &&
        check.saveBlock == result.token.saveBlock && check.systemData == result.token.systemData &&
        check.missionHash == result.token.missionHash && check.objectiveHash == result.token.objectiveHash &&
        PrimaryWorldMatches(image,world.token);
}
}
void InitializePrimaryObjectiveRuntime(HMODULE module, bool supported) {
    std::lock_guard lock(primaryRuntime.mutex);
    primaryRuntime.supported = false; primaryRuntime.image = {};
    primaryRuntime.lookup=nullptr; primaryRuntime.state=nullptr; primaryRuntime.graph=nullptr; primaryRuntime.node=nullptr; primaryRuntime.complete=nullptr;
    primaryRuntime.reason=L"Primary objective completion unavailable for this executable or native binding";
    PrimaryImage image;
    if (!supported || !LoadPrimaryImage(module,image)) return;
    primaryRuntime.image=image; primaryRuntime.supported=true;
    primaryRuntime.lookup=reinterpret_cast<PrimaryLookup>(image.At(0x3583a20));
    primaryRuntime.state=reinterpret_cast<PrimaryStateFunction>(image.At(0x3583d90));
    primaryRuntime.graph=reinterpret_cast<PrimaryGraphFunction>(image.At(0x2bc4fe0));
    primaryRuntime.node=reinterpret_cast<PrimaryNodeFunction>(image.At(0x2bc3cc0));
    primaryRuntime.complete=reinterpret_cast<PrimaryCompleteFunction>(image.At(0x1555540));
    primaryRuntime.reason=L"Waiting for an active supported primary objective";
}
PrimaryObjectiveState ReadPrimaryObjectiveState(const ReloadState& world) {
    PrimaryObjectiveState result;
    std::unique_lock lock(primaryRuntime.mutex,std::try_to_lock);
    if (!lock.owns_lock()) { result.status=L"Primary objective runtime is busy"; return result; }
    result.supported=primaryRuntime.supported; result.status=primaryRuntime.reason;
    if (!primaryRuntime.supported) return result;
    PrimaryHandle handle;
    try {
        if (!CapturePrimary(primaryRuntime.image,world,result,handle)) return result;
        result.ready=true; result.status=L"Active primary objective ready for native completion";
    } catch (...) { result.ready=false; result.status=L"Primary objective read validation failed"; }
    return result;
}
bool ExecuteCompletePrimary(const PrimaryObjectiveState& captured, std::wstring& status) {
    if (!IsWithinVerifiedEngineCallback()) { status=L"Primary objective completion requires the verified engine callback"; return false; }
    std::unique_lock lock(primaryRuntime.mutex,std::try_to_lock);
    if (!lock.owns_lock() || !primaryRuntime.supported || !captured.supported || !captured.ready) {
        status=L"Primary objective completion is unavailable or busy"; return false;
    }
    try {
        const auto& image=primaryRuntime.image;
        auto world=ReadReloadState(); PrimaryObjectiveState fresh; PrimaryHandle expected;
        if (!SameReloadContext(captured.world,world.token) || !CapturePrimary(image,world,fresh,expected) ||
            !SamePrimaryToken(captured.token,fresh.token) || !PrimaryCodePins(image) || !PrimaryTypes(image) || !IsWithinVerifiedEngineCallback()) {
            status=L"Primary objective changed or its native binding is unavailable; request rejected"; return false;
        }
        PrimaryHandle actual{};
        if (primaryRuntime.lookup(reinterpret_cast<void*>(fresh.token.objectiveSystem),&actual,fresh.token.missionHash,fresh.token.objectiveHash) != &actual ||
            std::memcmp(&actual,&expected,sizeof(actual)) != 0 || !IsWithinVerifiedEngineCallback() ||
            primaryRuntime.state(reinterpret_cast<void*>(fresh.token.objectiveSystem),&actual) != 1 || !IsWithinVerifiedEngineCallback()) {
            status=L"The native primary objective is no longer active"; return false;
        }
        void* graph=primaryRuntime.graph(&actual);
        if (!IsWithinVerifiedEngineCallback() || graph != reinterpret_cast<void*>(fresh.token.objectiveGraph)) {
            status=L"Primary objective graph identity changed"; return false;
        }
        void* node=primaryRuntime.node(graph,actual.node);
        if (!IsWithinVerifiedEngineCallback() || node != reinterpret_cast<void*>(fresh.token.objectiveNode)) {
            status=L"Primary objective node identity changed"; return false;
        }
        // Resolver/predicate calls may process native code. Repeat the guarded
        // copy and world check after them; never borrow a previous callback's
        // authority or trust the queued pointer by itself.
        auto finalWorld=ReadReloadState(); PrimaryObjectiveState final; PrimaryHandle finalHandle;
        std::uintptr_t vt=0, slot=0;
        if (!SameReloadContext(captured.world,finalWorld.token) || !CapturePrimary(image,finalWorld,final,finalHandle) ||
            !SamePrimaryToken(fresh.token,final.token) || std::memcmp(&actual,&finalHandle,sizeof(actual)) != 0 ||
            !PrimaryRead(final.token.objectiveNode,vt) || vt != image.At(0x548ab78) ||
            !PrimaryRead(vt+0xe8,slot) || slot != image.At(0x1555540) || !PrimaryCodePins(image) || !IsWithinVerifiedEngineCallback()) {
            status=L"Primary objective changed while validating the native completion request"; return false;
        }
        primaryRuntime.complete(node);
        // Native outputs can destroy nodes/actors or advance the level. Do not
        // read any old-world object after this call, or send an invented script
        // action OnComplete event. The next callback publishes fresh state.
        status=L"Native primary objective completion requested; normal progression and rewards apply";
        return true;
    } catch (...) { status=L"Primary objective completion validation failed"; return false; }
}
}
