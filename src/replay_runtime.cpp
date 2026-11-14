// SPDX-License-Identifier: GPL-3.0-or-later
#include "replay_runtime.h"
#include "reload_mission_catalog.h"
#include "engine_dispatch.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <tuple>
#include <vector>

namespace sm2debug {
namespace {
using ReplayPredicate = bool (*)(void*,bool);
using ReplayNative = bool (*)(void*,bool,bool,bool,std::uint32_t);
struct ReplayImage {
    std::uintptr_t base=0,exceptions=0;std::uint32_t exceptionCount=0;
    std::uintptr_t At(std::uintptr_t rva,std::size_t size=1)const {
        return base && rva<0xce54000 && size && size<=0xce54000-rva && base<=UINTPTR_MAX-rva?base+rva:0;
    }
};
struct ReplayRuntime {
    std::mutex mutex;bool supported=false;ReplayImage image;
    ReplayPredicate predicate=nullptr;ReplayNative replay=nullptr;
    bool awaitingTransition=false,transitionSeen=false;
    std::uint64_t epoch=1;
    ReloadToken invokedWorld;
} replayRuntime;
bool ReplayReadBytes(std::uintptr_t address,void* destination,std::size_t size) {
    SIZE_T copied=0;return address>=0x10000 && size && size<=UINTPTR_MAX-address &&
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),destination,size,&copied)&&copied==size;
}
template<class T>bool ReplayRead(std::uintptr_t address,T& value){return ReplayReadBytes(address,&value,sizeof(value));}
std::uintptr_t ReplayOffset(std::uintptr_t address,std::size_t offset){return address>=0x10000&&offset<=UINTPTR_MAX-address?address+offset:0;}
bool ReplaySpan(std::uintptr_t address,std::size_t size,bool writable=false){
    if(address<0x10000||!size||size>1024*1024||size>UINTPTR_MAX-address)return false;
    const auto end=address+size;
    while(address<end){MEMORY_BASIC_INFORMATION memory{};
        if(VirtualQuery(reinterpret_cast<void*>(address),&memory,sizeof(memory))!=sizeof(memory)||memory.State!=MEM_COMMIT||
           (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto protection=memory.Protect&0xff;
        if(writable&&protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY)return false;
        if(!writable&&protection!=PAGE_READONLY&&protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&
           protection!=PAGE_EXECUTE_READ&&protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto first=reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if(first>address||memory.RegionSize>UINTPTR_MAX-first||first+memory.RegionSize<=address)return false;
        address=std::min(end,first+memory.RegionSize);
    }return true;
}
bool ReplayBytes(const ReplayImage& image,std::uintptr_t rva,const char* hex){
    std::array<unsigned char,512> actual{};const auto size=std::strlen(hex)/2;
    if(!size||size>actual.size()||!image.At(rva,size)||!ReplayReadBytes(image.At(rva),actual.data(),size))return false;
    auto nibble=[](char c)->unsigned{return c>='0'&&c<='9'?c-'0':c-'a'+10;};
    for(std::size_t i=0;i<size;++i)if(actual[i]!=((nibble(hex[i*2])<<4)|nibble(hex[i*2+1])))return false;
    return true;
}
bool ReplayFunction(const ReplayImage& image,std::uint32_t start,std::uint32_t end,std::uint32_t unwind){
    std::size_t first=0,last=image.exceptionCount;
    while(first<last){const auto middle=first+(last-first)/2;RUNTIME_FUNCTION value{};
        if(!ReplayRead(image.exceptions+middle*sizeof(value),value))return false;
        if(value.BeginAddress<start)first=middle+1;else last=middle;}
    RUNTIME_FUNCTION value{};return first<image.exceptionCount&&ReplayRead(image.exceptions+first*sizeof(value),value)&&
        value.BeginAddress==start&&value.EndAddress==end&&value.UnwindData==unwind;
}
bool ReplayRtti(const ReplayImage& image,std::uintptr_t vt,std::uintptr_t col,std::uintptr_t td,const char* expected){
    std::uintptr_t actual=0;std::array<std::uint32_t,6> fields{};std::array<char,96> text{};const auto size=std::strlen(expected)+1;
    return size<=text.size()&&ReplayRead(image.At(vt-8),actual)&&actual==image.At(col)&&ReplayRead(actual,fields)&&
        fields[0]==1&&fields[1]==0&&fields[3]==td&&fields[5]==col&&ReplayReadBytes(image.At(td+16),text.data(),size)&&
        std::memcmp(text.data(),expected,size)==0;
}
bool ReplayPins(const ReplayImage& image){
    std::uintptr_t slot=0;
    return ReplayFunction(image,0x15517a0,0x1551a12,0x92e1a1c)&&ReplayFunction(image,0x15516f0,0x1551796,0x92e1a10)&&
        ReplayFunction(image,0x358eef0,0x358f04d,0x966049c)&&ReplayFunction(image,0x153ee10,0x153eea6,0x92dce18)&&
        ReplayBytes(image,0x15517a0,"48895c241048896c2418488974242057415641574883ec40488b01410fb6e90f")&&
        ReplayBytes(image,0x15516f0,"48895c2408574883ec200fb6fa488bd984d2752433d2488d0dc37a150ae84e3e")&&
        ReplayBytes(image,0x358eec0,"8b819000000083f804741384d2740c83f802740a83f8030f94c0c332c0c3b001c3")&&
        ReplayBytes(image,0x358eef0,"48895c24104889742418574883ec40488b010fb6f2410fb6d0410fb6f9488bd9")&&
        ReplayBytes(image,0x153ee10,"48895c240848896c24104889742418574883ec20410fb6f1410fb6f88bda488b")&&
        ReplayBytes(image,0x358f7f0,"488b4118488b889c000000488bc248890ac3")&&
        ReplayBytes(image,0x3582410,"4883ec28488b124883c168e8209e1f004885c07409488b40284883c428c34883c428c3")&&
        ReplayBytes(image,0x3582450,"4883ec28488bc24885d27419418bd0488bc8e8f99d1f004885c07409488b40284883c428c333c04883c428c3")&&
        ReplayBytes(image,0x377c240,"4883c128e9a78191ff")&&ReplayBytes(image,0x377c260,"4881c148010000e9848091ff")&&
        ReplayBytes(image,0x15518a4,"4585f6743f488d542460488bcbe83adf0302")&&
        ReplayBytes(image,0x15518d1,"458bc6488bd0e8740b03024885c074078b784c85ff7574")&&
        ReplayBytes(image,0x15519c4,"488905397f150ac6052e7f150a01e8e976f701")&&
        ReplayBytes(image,0x15519e6,"4533c08bd7488bcbe89da2f701b001")&&
        ReplayRtti(image,0x548a108,0x8e05428,0xab49d50,".?AVMissionNodeGame@@")&&
        ReplayRtti(image,0x86a3d48,0x901d610,0xac1dd20,".?AVObjectiveGraphBuiltGame@@")&&
        ReplayRtti(image,0x86a3ca8,0x901d588,0xac1dcf0,".?AVObjectiveNodeBuiltGame@@")&&
        ReplayRead(image.At(0x548a108+0x118),slot)&&slot==image.At(0x15516f0)&&
        ReplayRead(image.At(0x548a108+0x120),slot)&&slot==image.At(0x15517a0);
}
bool LoadReplayImage(HMODULE module,ReplayImage& image){
    if(!module||module!=GetModuleHandleW(nullptr))return false;
    image.base=reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!ReplayRead(image.base,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<=0||dos.e_lfanew>0x1000||!ReplayRead(image.base+dos.e_lfanew,nt)||
       nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.FileHeader.TimeDateStamp!=0x679b5991||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt.OptionalHeader.SizeOfImage!=0xce54000||
       (nt.OptionalHeader.ImageBase!=0x140000000&&nt.OptionalHeader.ImageBase!=image.base)||nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_EXCEPTION)return false;
    const auto& directory=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if(!directory.Size||directory.Size%sizeof(RUNTIME_FUNCTION)||directory.Size/sizeof(RUNTIME_FUNCTION)>1000000||!image.At(directory.VirtualAddress,directory.Size))return false;
    image.exceptions=image.At(directory.VirtualAddress);image.exceptionCount=directory.Size/sizeof(RUNTIME_FUNCTION);
    return ReplayPins(image);
}
bool SameReplayOrigin(const PrimaryObjectiveState& a,const PrimaryObjectiveState& b){
    const auto& x=a.token;const auto& y=b.token;
    return a.supported&&a.ready&&b.supported&&b.ready&&SameReloadContext(a.world,b.world)&&
        std::tie(x.objectiveSystem,x.objectiveVtable,x.saveSystem,x.saveContainer,x.saveBlock,x.systemData,x.primaryAddress,
            x.missionHash,x.objectiveHash,x.missionNode,x.objectiveNode,x.missionGraph,x.objectiveGraph,x.missionGraphHandle,
            x.objectiveGraphHandle,x.missionNodeId,x.objectiveNodeId,x.missionGraphAssetId)==
        std::tie(y.objectiveSystem,y.objectiveVtable,y.saveSystem,y.saveContainer,y.saveBlock,y.systemData,y.primaryAddress,
            y.missionHash,y.objectiveHash,y.missionNode,y.objectiveNode,y.missionGraph,y.objectiveGraph,y.missionGraphHandle,
            y.objectiveGraphHandle,y.missionNodeId,y.objectiveNodeId,y.missionGraphAssetId);
}
bool SameReplayTarget(const ReplayTargetToken& a,const ReplayTargetToken& b){
    return std::tie(a.node,a.definition,a.graph,a.resource,a.savedData,a.objectiveDefinition,a.graphId,a.nodeId,a.objectiveGraphId,
        a.graphHandle,a.checkpoint,a.missionState,a.replayType)==std::tie(b.node,b.definition,b.graph,b.resource,b.savedData,
        b.objectiveDefinition,b.graphId,b.nodeId,b.objectiveGraphId,b.graphHandle,b.checkpoint,b.missionState,b.replayType);
}
struct ReplayMap{std::uintptr_t entry=0;};
template<class Key>bool ReplayMapFind(std::uintptr_t header,Key key,std::size_t minimum,ReplayMap& result){
    std::array<unsigned char,40> before{},after{};std::uintptr_t keys=0,values=0;std::uint32_t count=0,stride=0;
    if(!key||!ReplayRead(header,before))return false;
    std::memcpy(&keys,before.data(),8);std::memcpy(&values,before.data()+8,8);std::memcpy(&count,before.data()+0x14,4);
    std::memcpy(&stride,before.data()+(sizeof(Key)==8?0x20:0x1c),4);
    if(!count||count>16384||stride<minimum||stride>4096||!ReplaySpan(values,static_cast<std::size_t>(count)*stride))return false;
    std::vector<Key> all(count);if(!ReplayReadBytes(keys,all.data(),all.size()*sizeof(Key))||std::find(all.begin(),all.end(),Key{})==all.end())return false;
    const auto product=static_cast<std::uint64_t>(key)*0x9e3779b1ull;auto slot=static_cast<std::uint32_t>((product>>32)^static_cast<std::uint32_t>(product))%count;
    for(std::uint32_t i=0;i<count;++i){if(all[slot]==key){result.entry=ReplayOffset(values,static_cast<std::size_t>(slot)*stride);Key again=0;
            return result.entry&&ReplayRead(keys+slot*sizeof(Key),again)&&again==key&&ReplayRead(header,after)&&before==after;}
        if(!all[slot])return false;
        slot=(slot+1)%count;}return false;
}
bool ReplayMissionRecord(std::uintptr_t manager,std::uint32_t key,std::uintptr_t& record){
    std::uintptr_t table=0;std::uint16_t count=0,prefix=0;
    if(!ReplayRead(manager+0x20,table)||!ReplayRead(manager+0x28,count)||!ReplayRead(manager+0x2a,prefix)||!count||count>4096||prefix<count||prefix>16384||
       !ReplaySpan(table,static_cast<std::size_t>(prefix)*4+static_cast<std::size_t>(count)*32))return false;
    std::vector<std::uint32_t> keys(count);if(!ReplayReadBytes(table,keys.data(),keys.size()*4))return false;
    const auto found=std::find(keys.begin(),keys.end(),key);if(found==keys.end()||std::find(found+1,keys.end(),key)!=keys.end())return false;
    record=ReplayOffset(table,static_cast<std::size_t>(prefix)*4+static_cast<std::size_t>(found-keys.begin())*32);
    std::uintptr_t again=0;std::uint16_t c=0,p=0;
    return record&&ReplayRead(manager+0x20,again)&&again==table&&ReplayRead(manager+0x28,c)&&c==count&&ReplayRead(manager+0x2a,p)&&p==prefix;
}
bool ReplayName(std::uintptr_t wrapper,const char* expected){
    if(!expected)return false;
    const auto size=std::strlen(expected);if(!size||size>127)return false;
    std::uintptr_t pointer=0;if(!ReplayRead(wrapper,pointer))return false;
    if(static_cast<std::intptr_t>(pointer)<0){const auto relative=static_cast<std::int32_t>(pointer);
        if(relative<0){const auto magnitude=static_cast<std::uint64_t>(-static_cast<std::int64_t>(relative));if(wrapper<magnitude)return false;pointer=wrapper-magnitude;}
        else pointer=ReplayOffset(wrapper,static_cast<std::size_t>(relative));}
    std::array<char,128> actual{};return ReplayReadBytes(pointer,actual.data(),size+1)&&std::memcmp(actual.data(),expected,size+1)==0;
}
bool ReplayPoolNode(const ReplayImage& image,ReplayTargetToken& target){
    std::uint32_t count=0,actual=0;std::uintptr_t pool=0,wrapper=0,node=0;ReplayMap map;
    const auto index=target.graphHandle&0xfffff;
    return target.graphHandle>>20&&ReplayRead(image.At(0x9f47170+0x18),count)&&count<=0x100000&&index<count&&
        ReplayRead(image.At(0x9f47170+0x28),pool)&&ReplayOffset(pool,static_cast<std::size_t>(index)*0x138)==target.graph&&
        ReplayRead(target.graph+8,actual)&&actual==target.graphHandle&&ReplayMapFind(target.graph+0x48,target.nodeId,8,map)&&
        ReplayRead(map.entry,wrapper)&&ReplayRead(ReplayOffset(wrapper,8),node)&&node==target.node;
}
bool ReadReplayTarget(const ReplayImage& image,const PrimaryObjectiveState& origin,const ReplaySelection& selection,
                      const ReloadState& world,ReplayTargetToken& target,std::wstring* reason=nullptr){
    const auto* mission=FindReloadMission(selection.graphId,selection.missionHash);
    const auto* objective=selection.objectiveHash?FindReloadMissionObjective(selection.graphId,selection.missionHash,selection.objectiveHash):nullptr;
    if(!mission||(selection.objectiveHash&&!objective)||!selection.graphId||selection.graphId!=origin.token.missionGraphAssetId||
       origin.world.imageBase!=image.base||origin.world.modeValue!=9||!origin.world.mode||!origin.world.kernel||origin.world.kernelState!=3)return false;
    std::uintptr_t record=0,vt=0;std::int32_t marker=0;std::uint32_t crc=0;std::uint8_t guard=0;
    if(!ReplayRead(image.At(0xb6a9900),guard)||guard||!ReplayRead(origin.token.objectiveSystem+0x1b1,guard)||guard||
       !ReplayMissionRecord(origin.token.objectiveSystem,selection.missionHash,record)||!ReplayRead(record+8,marker)||marker>=0||
       !ReplayRead(record+0x18,target.node)||!ReplaySpan(target.node,0xc0,true)||!ReplayRead(target.node,vt)||vt!=image.At(0x548a108)||
       !ReplayRead(target.node+0x88,crc)||crc!=selection.missionHash||!ReplayRead(target.node+0x90,target.missionState)||
       target.missionState<2||target.missionState>4||!ReplayRead(target.node+0x18,target.definition)||!ReplaySpan(target.definition,0x1f0)||
       !ReplayName(target.definition+0x78,mission->name)||!ReplayRead(target.definition+0x1d0,target.replayType))return false;
    // Type2's predicate passes a cached value to153A6B0 as an unchecked signed
    // array index. Its cached object/index contract is unproved; never call
    // that predicate by treating the cached value as an objective-name CRC.
    if(target.replayType!=1){if(reason)*reason=L"This mission replay policy is not supported";return false;}
    if(!ReplayRead(target.node+8,target.graph)||target.graph!=origin.token.missionGraph||!ReplayRead(target.graph,target.resource)||
       !ReplayRead(ReplayOffset(target.resource,8),target.graphId)||target.graphId!=selection.graphId||
       !ReplayRead(target.graph+8,target.graphHandle)||!ReplayRead(target.definition+8,target.nodeId)||!ReplayPoolNode(image,target)||
       !ReplayRead(target.definition+0x9c,target.objectiveGraphId)||target.objectiveGraphId!=mission->objectiveGraphAssetId)return false;
    ReplayMap savedGraph,savedMission;
    if(!ReplayMapFind(origin.token.saveBlock+0xc0,target.graphId,0x58,savedGraph)||
       !ReplayMapFind(savedGraph.entry+0x30,selection.missionHash,0x30,savedMission)||!ReplayRead(savedMission.entry+0x28,target.savedData)||
       !ReplaySpan(target.savedData,0xf0,true)||!ReplayRead(target.savedData+0x30,marker)||marker!=target.missionState)return false;
    if(objective){ReplayMap graphEntry,definitionEntry;std::uintptr_t built=0;
        if(!ReplayMapFind(origin.token.objectiveSystem+0x90,target.objectiveGraphId,0x30,graphEntry)||!ReplayRead(graphEntry.entry+0x28,built)||
           !ReplayRead(built,vt)||vt!=image.At(0x86a3d48)||!ReplayMapFind(built+0x148,selection.objectiveHash,0x30,definitionEntry)||
           !ReplayRead(definitionEntry.entry+0x28,target.objectiveDefinition)||!ReplayRead(target.objectiveDefinition,vt)||vt!=image.At(0x86a3ca8)||
           !ReplayName(target.objectiveDefinition+0x28,objective->name)||!ReplayRead(target.objectiveDefinition+0x4c,target.checkpoint)||!target.checkpoint||
           !world.checkpoints||std::none_of(world.checkpoints->begin(),world.checkpoints->end(),[&](const ReloadCheckpoint& c){return c.key==target.checkpoint;}))return false;
    }
    return true;
}
bool CaptureReplay(const ReplayImage& image,const PrimaryObjectiveState& origin,const ReplaySelection& selection,
                   const ReloadState& world,ReplayState& result){
    if(!world.ready||!world.supported||!SameReloadContext(origin.world,world.token))return false;
    const auto current=ReadPrimaryObjectiveState(world);
    if(!SameReplayOrigin(origin,current))return false;
    result.origin=current;result.selection=selection;
    return ReadReplayTarget(image,current,selection,world,result.target,&result.status);
}
}
void InitializeReplayRuntime(HMODULE module,bool supported){
    std::lock_guard lock(replayRuntime.mutex);replayRuntime.supported=false;replayRuntime.image={};replayRuntime.predicate=nullptr;replayRuntime.replay=nullptr;
    replayRuntime.awaitingTransition=replayRuntime.transitionSeen=false;replayRuntime.invokedWorld={};
    if(!++replayRuntime.epoch)++replayRuntime.epoch;
    ReplayImage image;if(!supported||!LoadReplayImage(module,image))return;
    (void)ReloadMissionCatalog(); // Materialize immutable vectors on the worker.
    replayRuntime.image=image;replayRuntime.supported=true;replayRuntime.predicate=reinterpret_cast<ReplayPredicate>(image.At(0x15516f0));
    replayRuntime.replay=reinterpret_cast<ReplayNative>(image.At(0x15517a0));
}
bool ReplayTransitionPending() {
    std::unique_lock lock(replayRuntime.mutex, std::try_to_lock);
    return !lock.owns_lock() || replayRuntime.awaitingTransition;
}
ReplayState ReadReplayState(const PrimaryObjectiveState& origin,const ReplaySelection& selection){
    ReplayState result;result.origin=origin;result.selection=selection;result.status=L"Replay requires a current loaded mission in this graph";
    std::unique_lock lock(replayRuntime.mutex,std::try_to_lock);if(!lock.owns_lock()){result.status=L"Replay runtime is busy";return result;}
    result.supported=replayRuntime.supported;result.requestEpoch=replayRuntime.epoch;if(!result.supported)return result;
    try{auto world=ReadReloadState();
        if(replayRuntime.awaitingTransition){
            if(!world.ready){replayRuntime.transitionSeen=true;result.status=L"Waiting for the native replay transition";return result;}
            if(replayRuntime.transitionSeen||!SameReloadContext(replayRuntime.invokedWorld,world.token)){
                replayRuntime.awaitingTransition=replayRuntime.transitionSeen=false;
            }else{result.status=L"Replay already invoked; waiting for a world transition";return result;}
        }
        if(CaptureReplay(replayRuntime.image,origin,selection,world,result)){result.ready=true;result.status=L"Native replay target ready";}
    }catch(...){result.status=L"Replay target read validation failed";}return result;
}
ReplayResult ExecuteReplay(const ReplayState& captured,std::wstring& status,
                           ReloadPreparation prepare,void* preparationContext){
    ReplayResult result;
    if(!IsWithinVerifiedEngineCallback()){status=L"Replay requires the verified engine callback";return result;}
    std::unique_lock lock(replayRuntime.mutex,std::try_to_lock);
    if(!lock.owns_lock()||!replayRuntime.supported||!captured.supported||!captured.ready||replayRuntime.awaitingTransition||
       !captured.requestEpoch||captured.requestEpoch!=replayRuntime.epoch){status=L"Replay is unavailable, stale or waiting for its transition";return result;}
    try{const auto& image=replayRuntime.image;auto world=ReadReloadState();ReplayState current;
        if(!CaptureReplay(image,captured.origin,captured.selection,world,current)||!SameReplayTarget(captured.target,current.target)||
           !ReplayPins(image)||!CheckReloadExecutionContext(world,status)||!IsWithinVerifiedEngineCallback()){
            status=L"Replay target or world changed; request rejected";return result;}
        if(!replayRuntime.predicate(reinterpret_cast<void*>(current.target.node),true)||!IsWithinVerifiedEngineCallback()){
            status=L"The game currently refuses replay for this mission";return result;}
        auto freshWorld=ReadReloadState();ReplayState final;std::uintptr_t slot=0;
        if(!CaptureReplay(image,captured.origin,captured.selection,freshWorld,final)||!SameReplayTarget(current.target,final.target)||
           !ReplayRead(image.At(0x548a108+0x120),slot)||slot!=image.At(0x15517a0)||!ReplayPins(image)||!IsWithinVerifiedEngineCallback()){
            status=L"Replay target changed after its native predicate";return result;}
        // A preparation is allowed only after both target validation and the
        // native predicate. Recheck load admission after that native call.
        if(prepare && (!CheckReloadExecutionContext(freshWorld,status) ||
                       !prepare(freshWorld,preparationContext,status))) return result;
        if(prepare){ReplayState preparedTarget;const auto preparedWorld=ReadReloadState();
            if(!IsWithinVerifiedEngineCallback() || !CaptureReplay(image,captured.origin,captured.selection,preparedWorld,preparedTarget) ||
               !SameReplayTarget(final.target,preparedTarget.target)){
                status=L"Replay world changed after reload preparation";return result;}}
        result.invoked=true;
        replayRuntime.awaitingTransition=true;replayRuntime.transitionSeen=false;replayRuntime.invokedWorld=final.origin.world;
        if(!++replayRuntime.epoch)++replayRuntime.epoch;
        result.accepted=replayRuntime.replay(reinterpret_cast<void*>(final.target.node),true,true,false,captured.selection.objectiveHash);
        // Native replay performs its own loadout/replay state and Reload. Even
        // false may follow some native bookkeeping. Do not read old objects or
        // call ordinary Reload; caller invalidates instance caches if invoked.
        status=result.accepted?L"Native mission replay requested":L"The game refused the native replay request";
        return result;
    }catch(...){status=L"Replay validation failed";return result;}
}
}
