// SPDX-License-Identifier: GPL-3.0-or-later
#include "hero_warp_runtime.h"
#include "engine_dispatch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <locale>
#include <mutex>
#include <sstream>

namespace sm2debug {
namespace {
constexpr std::uintptr_t kImageSize = 0xce54000;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
struct WarpCodePin { std::uintptr_t rva; std::size_t size; std::uint64_t hash; };
constexpr WarpCodePin kWarpCode[] = {
    {0x2f8a5e0,0xbd,0x1082e5203439c02full}, // PerformWarpEvent constructor
    {0x2f91af0,0x73,0x255e506170d9f63aull}, // InitDefaults
    {0x2ffdb70,0x4b9,0x96867c2bae9a36f1ull}, // DDLTransformFromMatrix4
    {0x2a3f450,0x1f3,0xbe407eb2be90f006ull}, // immediate targeted dispatch
    {0x2f8aa70,0xf,0x2ed42edb7e8e3d28ull}, // nondeleting destructor
    {0x2f86100,0xa5,0x379a4ea326406d2cull}, // EventBase constructor
    {0x2f861b0,0xb,0x6e434a88a6d88287ull}, // EventBase destructor
    {0x28a8970,0x47,0x10434cf595c86102ull}, // actor handle layout
};
using WarpConstruct = void* (*)(void*);
using WarpDefaults = bool (*)(void*);
using WarpMatrix = void (*)(void*, const void*, bool);
using WarpDispatch = bool (*)(void*, void*, std::uint32_t);
using WarpDestroy = void (*)(void*);
struct WarpRuntime {
    std::mutex mutex;
    std::uintptr_t base = 0;
    bool supported = false;
    WarpConstruct construct = nullptr;
    WarpDefaults defaults = nullptr;
    WarpMatrix matrix = nullptr;
    WarpDispatch dispatch = nullptr;
    WarpDestroy destroy = nullptr;
} warpRuntime;

bool WarpReadBytes(std::uintptr_t address, void* value, std::size_t size) {
    SIZE_T copied = 0;
    return address >= 0x10000 && size && size <= UINTPTR_MAX-address &&
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),value,size,&copied) && copied == size;
}
template<class T> bool WarpRead(std::uintptr_t address, T& value) { return WarpReadBytes(address,&value,sizeof(value)); }
template<class T> bool WarpAt(std::uintptr_t address, std::uintptr_t offset, T& value) {
    return address && offset <= UINTPTR_MAX-address && WarpRead(address+offset,value);
}
bool WarpContains(std::uintptr_t base, std::uintptr_t pointer, std::size_t size) {
    return base && pointer >= base && size <= kImageSize && pointer-base <= kImageSize-size;
}
bool WarpCode(std::uintptr_t base) {
    std::array<unsigned char,256> bytes{};
    for (const auto& pin : kWarpCode) {
        std::uint64_t hash = kFnvOffset;
        for (std::size_t offset=0; offset<pin.size;) {
            const auto size=std::min(bytes.size(),pin.size-offset);
            if (!WarpReadBytes(base+pin.rva+offset,bytes.data(),size)) return false;
            for (std::size_t i=0;i<size;++i) hash=(hash^bytes[i])*1099511628211ull;
            offset+=size;
        }
        if (hash != pin.hash) return false;
    }
    return true;
}
bool WarpString(std::uintptr_t base, std::uintptr_t pointer, const char* expected) {
    std::array<char,64> value{}; const auto n=std::strlen(expected)+1;
    return n<=value.size() && WarpContains(base,pointer,n) && WarpReadBytes(pointer,value.data(),n) &&
        std::memcmp(value.data(),expected,n)==0;
}
bool WarpTypes(std::uintptr_t base) {
    std::uintptr_t name=0, types=0, offsets=0, counts=0, locator=0, method=0;
    std::uint32_t type=0, size=0; std::uint16_t count=0;
    const auto d=base+0x9fd36d0;
    if (!WarpRead(d,name) || !WarpString(base,name,"PerformWarpEvent") ||
        !WarpRead(d+8,type) || type!=0x38008fe3 || !WarpRead(d+0x14,size) || size!=0x240 ||
        !WarpRead(d+0x18,count) || count!=25 || !WarpRead(d+0x68,types) || !WarpContains(base,types,25) ||
        !WarpRead(d+0x80,counts) || !WarpContains(base,counts,100) ||
        !WarpRead(d+0x88,offsets) || !WarpContains(base,offsets,100)) return false;
    constexpr unsigned char expectedTypes[]={2,2,2,2,2,2,8,13,0,13,2,2,2,2,11,15,15,15,15,15,15,15,15,15,2};
    constexpr std::uint32_t expectedOffsets[]={0x28,0x2c,0x30,0x34,0x38,0x3c,0x40,0x48,0x128,0x138,
        0x218,0x21c,0x220,0x224,0x228,0x22c,0x22d,0x22e,0x22f,0x230,0x231,0x232,0x233,0x234,0x238};
    for (std::size_t i=0;i<25;++i) {
        unsigned char actualType=0; std::uint32_t actualOffset=0, actualCount=0;
        if (!WarpRead(types+i,actualType) || actualType!=expectedTypes[i] ||
            !WarpRead(offsets+i*4,actualOffset) || actualOffset!=expectedOffsets[i] ||
            !WarpRead(counts+i*4,actualCount) || actualCount!=1) return false;
    }
    std::array<std::uint32_t,6> col{};
    return WarpRead(base+0x8324630,locator) && locator==base+0x8f91ea8 && WarpRead(locator,col) &&
        col[0]==1 && col[1]==0 && col[3]==0xaad6e48 && col[5]==0x8f91ea8 &&
        WarpString(base,base+0xaad6e58,".?AVPerformWarpEvent@@") &&
        WarpRead(base+0x8324638+0x50,method) && method==base+0x2f91af0;
}
bool WarpImage(std::uintptr_t base) {
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    return WarpRead(base,dos) && dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>0 && dos.e_lfanew<4096 &&
        WarpAt(base,static_cast<std::uintptr_t>(dos.e_lfanew),nt) && nt.Signature==IMAGE_NT_SIGNATURE &&
        nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64 && nt.OptionalHeader.SizeOfImage==kImageSize &&
        WarpCode(base) && WarpTypes(base);
}
bool WarpPositionValid(const HeroWarpPosition& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
        std::fabs(p.x)<100000 && std::fabs(p.y)<100000 && std::fabs(p.z)<100000;
}
bool WarpMatrixValid(const std::array<float,16>& m) {
    for (float value : m) if (!std::isfinite(value)) return false;
    if (!WarpPositionValid({m[12],m[13],m[14]}) || std::fabs(m[15]-1.0f)>0.001f) return false;
    for (unsigned i=0;i<3;++i) {
        const double n=double(m[i*4])*m[i*4]+double(m[i*4+1])*m[i*4+1]+double(m[i*4+2])*m[i*4+2];
        if (n<1e-8 || n>10000 || std::fabs(m[i*4+3])>0.001f) return false;
    }
    const double determinant=double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9])-
        double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8])+double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);
    return std::fabs(determinant)>1e-6;
}
bool WarpHero(std::uintptr_t base, std::uint32_t requested, HeroWarpState& state, std::array<float,16>& matrix) {
    const auto index=requested&0xfffffu;
    const auto generation=static_cast<std::uint16_t>((requested>>20)&0x7ffu);
    std::uintptr_t pool=0, table=0, afterEntry=0, afterPool=0, afterTransform=0;
    std::uint32_t count=0, handle=0, afterCount=0, actorIndex=0;
    std::uint16_t actualGeneration=0;
    if (!generation || (requested&0x80000000u) || !WarpRead(base+0x99c9160,table) || !WarpContains(base,table,8) ||
        !WarpRead(base+0x99c9160+0x150,state.currentEntry) || !WarpAt(state.currentEntry,4,handle) || handle!=requested ||
        !WarpRead(base+0xc0bec50,pool) || !pool || !WarpRead(base+0xc0bec6c,count) || count>(1u<<20) || index>=count ||
        index>(UINTPTR_MAX-pool)/192) return false;
    state.actor=pool+index*192ull; state.actorHandle=requested;
    if (!WarpAt(state.actor,8,actualGeneration) || actualGeneration!=generation ||
        !WarpAt(state.actor,0xc,actorIndex) || (actorIndex&0xfffffu)!=index ||
        !WarpRead(state.actor,state.transform) || !WarpRead(state.transform,matrix) || !WarpMatrixValid(matrix) ||
        !WarpRead(base+0x99c9160+0x150,afterEntry) || afterEntry!=state.currentEntry ||
        !WarpAt(afterEntry,4,handle) || handle!=requested || !WarpRead(base+0xc0bec50,afterPool) || afterPool!=pool ||
        !WarpRead(base+0xc0bec6c,afterCount) || afterCount!=count || !WarpRead(state.actor,afterTransform) || afterTransform!=state.transform ||
        !WarpAt(state.actor,8,actualGeneration) || actualGeneration!=generation) return false;
    state.position={matrix[12],matrix[13],matrix[14]}; return true;
}
bool WarpClass(std::uintptr_t base, std::uintptr_t& eventClass) {
    std::uint16_t index=0; std::int32_t count=0; std::uintptr_t parents=0, after=0;
    std::int16_t parent=0;
    return WarpRead(base+0xc140670,eventClass) && eventClass && WarpRead(eventClass,index) &&
        WarpRead(base+0x9f0b840,count) && count>0 && count<=65536 && index<count &&
        WarpRead(base+0xbb51c20+0x33210,parents) && parents && WarpRead(parents+index*2ull,parent) &&
        (parent<0 || parent<count) && WarpRead(base+0xc140670,after) && after==eventClass;
}
bool WarpSameActor(const HeroWarpState& a, const HeroWarpState& b) {
    return a.actorHandle && a.actorHandle==b.actorHandle && a.actor==b.actor && a.currentEntry==b.currentEntry &&
        a.transform==b.transform && a.eventClass==b.eventClass;
}
HeroWarpState WarpCapture(const ReloadState& world, std::uint32_t handle, std::array<float,16>& matrix) {
    HeroWarpState state; state.supported=warpRuntime.supported;
    if (!state.supported || !IsWithinVerifiedEngineCallback()) { state.status=L"Warp requires the supported build and verified engine callback"; return state; }
    const auto fresh=ReadReloadState();
    if (!world.ready || !fresh.ready || fresh.token.imageBase!=warpRuntime.base || !SameReloadContext(world.token,fresh.token)) {
        state.status=L"Warp requires the same stable loaded world"; return state;
    }
    state.world=fresh.token;
    if (!WarpCode(warpRuntime.base) || !WarpTypes(warpRuntime.base) || !WarpClass(warpRuntime.base,state.eventClass) ||
        !WarpHero(warpRuntime.base,handle,state,matrix)) { state.status=L"Current hero or native Warp binding is unavailable"; return state; }
    state.ready=true; state.status=L"Current hero ready for native Warp"; return state;
}
template<class T> void WarpPut(std::array<unsigned char,0x240>& event, std::size_t offset, T value) {
    std::memcpy(event.data()+offset,&value,sizeof(value));
}
void WarpSourceFlags(std::array<unsigned char,0x240>& event) {
    // Exact source Hero/Warp callback overrides after InitDefaults. Other
    // defaults, notably UseDestinationActorMatrix and ForceMoveZero, are kept.
    WarpPut<std::uint64_t>(event,0x224,0); WarpPut<std::uint16_t>(event,0x22c,0);
    WarpPut<std::uint8_t>(event,0x22f,0); WarpPut<std::uint32_t>(event,0x238,0);
    WarpPut<std::uint8_t>(event,0x231,0); WarpPut<std::uint32_t>(event,0x220,0);
    for (const auto offset : {8u,24u}) {
        std::uint64_t flags=0; std::memcpy(&flags,event.data()+offset,8);
        WarpPut<std::uint64_t>(event,offset,flags|0x115f000ull);
    }
}
bool WarpSpace(wchar_t c) { return c==L' ' || c==L'\t' || c==L'\r' || c==L'\n'; }
}

void InitializeHeroWarpRuntime(HMODULE gameModule, bool executableVerified) {
    std::lock_guard lock(warpRuntime.mutex);
    warpRuntime.supported=false; warpRuntime.base=reinterpret_cast<std::uintptr_t>(gameModule);
    if (!executableVerified || !WarpImage(warpRuntime.base)) return;
    const auto base=warpRuntime.base;
    warpRuntime.construct=reinterpret_cast<WarpConstruct>(base+0x2f8a5e0);
    warpRuntime.defaults=reinterpret_cast<WarpDefaults>(base+0x2f91af0);
    warpRuntime.matrix=reinterpret_cast<WarpMatrix>(base+0x2ffdb70);
    warpRuntime.dispatch=reinterpret_cast<WarpDispatch>(base+0x2a3f450);
    warpRuntime.destroy=reinterpret_cast<WarpDestroy>(base+0x2f8aa70);
    warpRuntime.supported=true;
}
HeroWarpState ReadHeroWarpState(const ReloadState& world, std::uint32_t currentActorHandle) {
    std::unique_lock lock(warpRuntime.mutex,std::try_to_lock);
    if (!lock.owns_lock()) { HeroWarpState state; state.status=L"Warp runtime is busy"; return state; }
    std::array<float,16> matrix{};
    return WarpCapture(world,currentActorHandle,matrix);
}
bool ParseHeroWarpPosition(const std::wstring& text, HeroWarpPosition& position, std::wstring& status) {
    const auto fail=[&status]() { status=L"Enter three finite coordinates as X, Y, Z (each between -100000 and 100000)"; return false; };
    if (text.empty() || text.size()>255) return fail();
    std::array<float,3> values{}; std::size_t at=0;
    const auto digit=[](wchar_t c) { return c>=L'0' && c<=L'9'; };
    for (unsigned axis=0;axis<3;++axis) {
        while (at<text.size() && WarpSpace(text[at])) ++at;
        const auto start=at;
        if (at<text.size() && (text[at]==L'+' || text[at]==L'-')) ++at;
        unsigned digits=0;
        while (at<text.size() && digit(text[at])) { ++at; ++digits; }
        if (at<text.size() && text[at]==L'.') { ++at; while (at<text.size() && digit(text[at])) { ++at; ++digits; } }
        if (!digits) return fail();
        if (at<text.size() && (text[at]==L'e' || text[at]==L'E')) {
            ++at; if (at<text.size() && (text[at]==L'+' || text[at]==L'-')) ++at;
            const auto exponent=at; while (at<text.size() && digit(text[at])) ++at;
            if (at==exponent) return fail();
        }
        std::string number; for (auto i=start;i<at;++i) number.push_back(static_cast<char>(text[i]));
        std::istringstream stream(number); stream.imbue(std::locale::classic()); double value=0;
        if (!(stream>>value) || !stream.eof() || !std::isfinite(value) || std::fabs(value)>=100000) return fail();
        values[axis]=static_cast<float>(value);
        while (at<text.size() && WarpSpace(text[at])) ++at;
        if (axis<2) { if (at==text.size() || text[at++]!=L',') return fail(); }
        else if (at!=text.size()) return fail();
    }
    const HeroWarpPosition parsed{values[0],values[1],values[2]};
    if (!WarpPositionValid(parsed)) return fail();
    position=parsed; status=L"Warp coordinates valid"; return true;
}
bool SameHeroWarpContext(const HeroWarpState& left, const HeroWarpState& right) noexcept {
    return left.supported && left.ready && right.supported && right.ready &&
        SameReloadContext(left.world,right.world) && WarpSameActor(left,right);
}
bool ExecuteHeroWarp(const HeroWarpState& captured, const HeroWarpPosition& position, std::wstring& status) {
    if (!IsWithinVerifiedEngineCallback() || !WarpPositionValid(position)) { status=L"Warp requires valid coordinates and the verified engine callback"; return false; }
    std::unique_lock lock(warpRuntime.mutex,std::try_to_lock);
    if (!lock.owns_lock() || !warpRuntime.supported || !captured.supported || !captured.ready) { status=L"Warp is unavailable or busy"; return false; }
    const auto world=ReadReloadState(); std::array<float,16> matrix{};
    const auto fresh=WarpCapture(world,captured.actorHandle,matrix);
    if (!fresh.ready || !SameReloadContext(captured.world,fresh.world) || !WarpSameActor(captured,fresh)) {
        status=L"Hero or world changed; stale Warp request rejected"; return false;
    }
    matrix[12]=position.x; matrix[13]=position.y; matrix[14]=position.z; matrix[15]=1;
    alignas(16) std::array<unsigned char,0x240> event{};
    if (warpRuntime.construct(event.data())!=event.data()) { status=L"Native Warp constructor refused"; return false; }
    struct Cleanup { void* event; WarpDestroy destroy; ~Cleanup() { destroy(event); } } cleanup{event.data(),warpRuntime.destroy};
    if (!IsWithinVerifiedEngineCallback() || !warpRuntime.defaults(event.data())) { status=L"Native Warp defaults refused"; return false; }
    WarpSourceFlags(event);
    if (!IsWithinVerifiedEngineCallback()) { status=L"Warp callback authority changed"; return false; }
    warpRuntime.matrix(event.data()+0x138,matrix.data(),true);
    // Local preparation must not authorize dispatch into a changed engine world.
    std::array<float,16> finalMatrix{};
    const auto final=WarpCapture(ReadReloadState(),captured.actorHandle,finalMatrix);
    std::uintptr_t table=0, eventClass=0;
    std::memcpy(&table,event.data(),sizeof(table)); std::memcpy(&eventClass,event.data()+0x130,sizeof(eventClass));
    if (!final.ready || !SameReloadContext(captured.world,final.world) || !WarpSameActor(fresh,final) ||
        table!=warpRuntime.base+0x8324638 || eventClass!=final.eventClass || !IsWithinVerifiedEngineCallback()) {
        status=L"Native Warp context changed during preparation"; return false;
    }
    const bool handled=warpRuntime.dispatch(reinterpret_cast<void*>(warpRuntime.base+0xbb51c20),event.data(),captured.actorHandle);
    // The synchronous dispatcher may unload objects. Only our stack event is
    // touched afterward; no old actor, transform or world pointer is reused.
    status=handled ? L"Native Warp event handled" : L"Native Warp event had no accepting handler";
    return handled;
}
}
