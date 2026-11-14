// SPDX-License-Identifier: GPL-3.0-or-later
// Isolated host only: execute the byte-pinned, position-independent native
// Open decision function against a synthetic object and callback. No game,
// native engine method, controller, input injection or live vtable is used.
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fixture {
struct Object { void** vtable; std::array<unsigned char,0x40-sizeof(void*)> fields{}; } object{};
std::array<void*,16> table{};
bool menu = false, nativeAllows = true;
unsigned calls = 0, cases = 0, assertions = 0;
DWORD callerThread = 0;
void* slot = reinterpret_cast<void*>(0x1111);
DWORD protection = PAGE_READONLY;
unsigned failReadOnly = 0, failWritable = 0;
bool refuseRead = false;
bool observationModel = false;
std::uintptr_t observedSingleton = 0, observedVtable = 0;
std::uint8_t observedInitialized = 1;
std::uint32_t observedState = 14;
void Check(bool value, const char* reason) { ++assertions; if (!value) throw std::runtime_error(reason); }
bool Visible() { return menu; }
bool Original(void* pointer) {
    Check(pointer == &object, "Native continuation received another object");
    Check(GetCurrentThreadId() == callerThread, "Original did not preserve native caller thread");
    ++calls; return nativeAllows;
}
BOOL WINAPI Protect(LPVOID pointer, SIZE_T bytes, DWORD desired, PDWORD old) {
    if (pointer != &slot) return ::VirtualProtect(pointer,bytes,desired,old);
    *old = protection;
    if (desired == PAGE_READONLY && failReadOnly) { --failReadOnly; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (desired == PAGE_READWRITE && failWritable) { --failWritable; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    protection = desired; return TRUE;
}
BOOL WINAPI Read(HANDLE process, LPCVOID pointer, LPVOID target, SIZE_T size, SIZE_T* count) {
    if (pointer == &slot && refuseRead) { if (count) *count = 0; return FALSE; }
    if (observationModel) {
        constexpr std::uintptr_t base = 0x180000000;
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        auto copy = [&](const auto& value) { if (size != sizeof(value)) return FALSE; std::memcpy(target,&value,size); if (count) *count = size; return TRUE; };
        if (address == base+0xc237f90) return copy(observedSingleton);
        if (address == base+0xb6ba560) return copy(observedVtable);
        if (address == base+0xb6ba59c) return copy(observedInitialized);
        if (address == base+0xb6ba598) return copy(observedState);
    }
    return ::ReadProcessMemory(process,pointer,target,size,count);
}
}
#define VirtualProtect fixture::Protect
#define ReadProcessMemory fixture::Read
#include "../src/photo_mode_gate.cpp"
#undef ReadProcessMemory
#undef VirtualProtect

namespace fixture {
namespace gate = sm2debug::photo_gate;
using Open = bool (*)(void*);
std::uint32_t& State() { return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<unsigned char*>(&object)+0x38); }
std::uint32_t& Previous() { return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<unsigned char*>(&object)+0x34); }
void Reset() {
    gate::Shutdown();
    gate::patch = {};
    menu = false; nativeAllows = true; calls = 0; callerThread = GetCurrentThreadId();
    object = {}; table.fill(nullptr); object.vtable = table.data();
    table[8] = reinterpret_cast<void*>(&gate::Gate);
    gate::imageBase = reinterpret_cast<std::uintptr_t>(&object)-gate::objectRva;
    gate::nativeCanEnter = &Original; gate::visible = &Visible;
    gate::blocked = gate::forwarded = gate::invalidObjects = 0;
    slot = reinterpret_cast<void*>(0x1111); protection = PAGE_READONLY;
    failReadOnly = failWritable = 0; refuseRead = false;
    observationModel = false;
    State() = 14; Previous() = 9;
}
std::vector<unsigned char> NativeCode(const char* executable) {
    std::ifstream input(executable,std::ios::binary);
    Check(bool(input), "Pinned executable not readable");
    IMAGE_DOS_HEADER dos{}; input.read(reinterpret_cast<char*>(&dos),sizeof(dos));
    Check(dos.e_magic == IMAGE_DOS_SIGNATURE, "Invalid PE DOS header");
    input.seekg(dos.e_lfanew); IMAGE_NT_HEADERS64 nt{}; input.read(reinterpret_cast<char*>(&nt),sizeof(nt));
    Check(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.TimeDateStamp == 0x679b5991 &&
        nt.OptionalHeader.SizeOfImage == 0xce54000, "Unsupported native PE identity");
    const auto sectionOffset = dos.e_lfanew+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    input.seekg(sectionOffset);
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    input.read(reinterpret_cast<char*>(sections.data()),sections.size()*sizeof(sections[0]));
    constexpr DWORD rva = 0x359a640, length = 0x34;
    for (const auto& section : sections) if (rva >= section.VirtualAddress && rva-section.VirtualAddress+length <= section.SizeOfRawData) {
        input.seekg(section.PointerToRawData+rva-section.VirtualAddress);
        std::vector<unsigned char> code(length); input.read(reinterpret_cast<char*>(code.data()),code.size());
        Check(bool(input), "Native Open bytes truncated");
        constexpr unsigned char expected[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x01,0x48,0x8b,0xd9,0xff,0x50,0x40,0x84,
            0xc0,0x74,0x19,0x8b,0x43,0x38,0x85,0xc0,0x74,0x0a,0x89,0x43,0x34,0xc7,0x43,0x38,0x00,0x00,0x00,0x00,
            0xb0,0x01,0x48,0x83,0xc4,0x20,0x5b,0xc3,0x32,0xc0,0x48,0x83,0xc4,0x20,0x5b,0xc3};
        Check(code.size() == sizeof(expected) && !std::memcmp(code.data(),expected,sizeof(expected)), "Native Open changed");
        return code;
    }
    throw std::runtime_error("Native Open missing");
}
void Decisions(Open open) {
    Reset(); ++cases;
    table[8] = reinterpret_cast<void*>(&Original); menu = true;
    Check(open(&object) && State() == 0 && calls == 1, "Original behavior does not demonstrate photo-open leak");
    for (const char* mode : {"Debug", "Dev"}) {
        (void)mode;
        for (const char* route : {"controller", "pause", "startup", "request", "resume"}) {
            (void)route; Reset(); menu = true; ++cases;
            const Object before = object;
            Check(!open(&object), "Visible overlay allowed native Open");
            Check(!std::memcmp(&object,&before,sizeof(object)), "Blocked Open changed native state");
            Check(calls == 0 && gate::blocked == 1 && gate::forwarded == 0, "Blocked route called native CanEnter");
            menu = false;
            Check(open(&object) && State() == 0 && Previous() == 14 && calls == 1, "Closing overlay did not restore native Open");
        }
    }
    Reset(); nativeAllows = false; ++cases;
    const auto before = object;
    Check(!open(&object) && !std::memcmp(&object,&before,sizeof(object)) && calls == 1, "Original native restriction was bypassed");
    Reset(); State() = 0; Previous() = 7; ++cases;
    Check(open(&object) && Previous() == 7 && calls == 1, "Native already-opening behavior changed");
    Reset(); ++cases; Object other = object;
    Check(!open(&other) && calls == 0 && gate::invalidObjects == 1, "Foreign photo object reached original");
    Reset(); ++cases; gate::nativeCanEnter = nullptr;
    Check(!open(&object) && State() == 14, "Missing original was not fail-closed");
    Reset(); ++cases; gate::visible = nullptr;
    Check(open(&object) && calls == 1, "In-flight wrapper could not continue during shutdown");
    Reset(); ++cases; callerThread = 0;
    std::thread worker([&] { callerThread = GetCurrentThreadId(); Check(open(&object), "Native caller thread continuation failed"); });
    worker.join(); Check(calls == 1 && State() == 0, "Thread continuation did not complete");
}
void SetPatch() { gate::patch = {&slot,reinterpret_cast<void*>(0x1111),0,false,false}; }
void Lifecycle() {
    Reset(); SetPatch(); ++cases;
    Check(gate::Apply() && slot == reinterpret_cast<void*>(&gate::Gate) && protection == PAGE_READONLY, "Patch apply failed");
    Check(gate::Restore() && slot == reinterpret_cast<void*>(0x1111) && protection == PAGE_READONLY, "Owned patch not restored");
    Check(gate::Restore(), "Repeated restore failed");
    Reset(); SetPatch(); failWritable = 1; ++cases;
    Check(!gate::Apply() && !gate::patch.applied && !gate::patch.protectionPending && slot == reinterpret_cast<void*>(0x1111), "Writable refusal changed slot");
    Reset(); SetPatch(); failReadOnly = 1; ++cases;
    Check(!gate::Apply() && gate::patch.applied && gate::patch.protectionPending && protection == PAGE_READWRITE, "Partial apply lost protection ownership");
    Check(gate::Restore() && slot == reinterpret_cast<void*>(0x1111) && protection == PAGE_READONLY, "Partial apply rollback failed");
    Reset(); SetPatch(); failReadOnly = 3; ++cases;
    Check(!gate::Apply() && !gate::Restore() && gate::patch.protectionPending, "Repeated protection failure lost obligation");
    Check(slot == reinterpret_cast<void*>(0x1111) && protection == PAGE_READWRITE, "Rollback did not restore pointer first");
    Check(!gate::Restore() && gate::patch.protectionPending, "Deferred restoration was discarded");
    Check(gate::Restore() && protection == PAGE_READONLY, "Deferred page restoration did not recover");
    Reset(); SetPatch(); slot = reinterpret_cast<void*>(0x2222); ++cases;
    Check(!gate::Apply() && !gate::patch.applied && slot == reinterpret_cast<void*>(0x2222) && protection == PAGE_READONLY, "Failed CAS overwrote foreign pointer");
    Check(gate::Restore(), "Failed CAS could not retire cleanly");
    Reset(); SetPatch(); Check(gate::Apply(), "Setup failed"); ++cases;
    slot = reinterpret_cast<void*>(0x2222);
    Check(gate::Restore() && slot == reinterpret_cast<void*>(0x2222), "Foreign later hook was overwritten");
    Reset(); SetPatch(); failReadOnly = 1; Check(!gate::Apply(), "Partial setup not exercised"); ++cases;
    slot = reinterpret_cast<void*>(0x2222);
    Check(gate::Restore() && slot == reinterpret_cast<void*>(0x2222) && protection == PAGE_READONLY, "Foreign pointer/protection partial case failed");
    Reset(); SetPatch(); Check(gate::Apply(), "Setup failed"); ++cases; refuseRead = true;
    Check(!gate::Restore() && gate::patch.applied, "Unreadable slot lost rollback ownership");
    refuseRead = false; Check(gate::Restore(), "Readable retry failed");
    Reset(); SetPatch(); Check(gate::Apply(), "Setup failed"); ++cases; failWritable = 1;
    Check(!gate::Restore() && gate::patch.applied && slot == reinterpret_cast<void*>(&gate::Gate), "Unpatch failure lost owned pointer");
    Check(gate::Restore(), "Unpatch retry failed");
    Reset(); ++cases;
    Check(!gate::Initialize(GetModuleHandleW(nullptr),true,&Visible), "Wrong process was admitted");
    Check(!gate::Initialize(GetModuleHandleW(nullptr),false,&Visible), "Unsupported flag was admitted");
    Check(!gate::Initialize(nullptr,true,&Visible), "Null owner admitted");
    Check(!gate::Initialize(GetModuleHandleW(nullptr),true,nullptr), "Null visibility admitted");
}
void Observations() {
    Reset(); observationModel = true;
    constexpr std::uintptr_t base = 0x180000000;
    gate::imageBase = base; observedSingleton = base+gate::objectRva; observedVtable = base+gate::vtableRva;
    observedInitialized = 1;
    for (unsigned state = 0; state <= 14; ++state) {
        ++cases; observedState = state;
        const auto result = gate::Observe();
        const bool active = state < 10 || state == 13;
        Check(result.stateKnown && result.nativeState == state && result.photoActive == active, "Native photo activity observation incorrect");
    }
    ++cases; observedState = UINT32_MAX;
    Check(!gate::Observe().stateKnown, "Unknown photo state reported closed/known");
    ++cases; observedState = 14; observedInitialized = 0;
    Check(!gate::Observe().stateKnown, "Uninitialized photo object reported known");
    ++cases; observedInitialized = 1; ++observedSingleton;
    Check(!gate::Observe().stateKnown, "Foreign singleton admitted for photo state");
    ++cases; --observedSingleton; ++observedVtable;
    Check(!gate::Observe().stateKnown, "Foreign photo type admitted for activity");
    observationModel = false;
}
}
int main(int argc,char** argv) {
    void* code = nullptr;
    try {
        fixture::Check(argc == 2,"Pass exact pinned executable path");
        const auto native = fixture::NativeCode(argv[1]);
        code = VirtualAlloc(nullptr,native.size(),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        fixture::Check(code != nullptr,"Isolated executable allocation failed");
        std::memcpy(code,native.data(),native.size()); DWORD old = 0;
        fixture::Check(VirtualProtect(code,native.size(),PAGE_EXECUTE_READ,&old) != FALSE,"Isolated executable protection failed");
        FlushInstructionCache(GetCurrentProcess(),code,native.size());
        fixture::Decisions(reinterpret_cast<fixture::Open>(code));
        fixture::Lifecycle();
        fixture::Observations();
        sm2debug::photo_gate::Shutdown();
        VirtualFree(code,0,MEM_RELEASE);
        std::cout << "{\"passed\":true,\"cases\":" << fixture::cases << ",\"assertions\":" << fixture::assertions
                  << ",\"old_native_open_leak_reproduced\":true,\"game_calls\":0,\"physical_input_verified\":false}";
        return 0;
    } catch (const std::exception& error) {
        if (code) VirtualFree(code,0,MEM_RELEASE);
        std::cerr << error.what() << '\n'; return 1;
    }
}
