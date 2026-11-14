// Synthetic host fixture: no game process, engine calls or game TLS.
#include "../src/reload_runtime.cpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool fixtureScope = false;
unsigned assertions = 0, cases = 0, reloadCalls = 0, frontendCalls = 0, unloadCalls = 0, preparationCalls = 0;
unsigned predicateCalls = 0;
void* receivedManager = nullptr;
std::uint32_t receivedKey = 0;
bool receivedFlag = true, acceptNative = true;
bool predicateResult = true, revokeInPredicate = false, changeSequenceInPredicate = false;
std::uintptr_t fixtureKernel = 0, fixtureSave = 0;
bool preparationAccept = true, revokeInPreparation = false;
void Require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
bool FakeReload(void* manager, std::uint32_t key, bool flag) {
    ++reloadCalls; receivedManager = manager; receivedKey = key; receivedFlag = flag;
    if (acceptNative) *reinterpret_cast<std::uint8_t*>(fixtureKernel+0x6308) = 1;
    return acceptNative;
}
bool FakeFrontend(void* receiver) {
    ++frontendCalls; Require(receiver == nullptr, "frontend unused receiver must be null");
    if (acceptNative) *reinterpret_cast<std::uint8_t*>(fixtureKernel+0x6308) = 0;
    return acceptNative;
}
bool FakeUnload(void* manager) {
    ++unloadCalls; receivedManager = manager;
    Require(*reinterpret_cast<std::uint8_t*>(fixtureSave+0x70)==1,"native unload observes original exit-save flag before invocation");
    if (acceptNative) *reinterpret_cast<std::uint8_t*>(fixtureKernel+0x6308) = 0;
    return acceptNative;
}
bool FakePreparation(const sm2debug::ReloadState& admitted, void* context, std::wstring& status) {
    ++preparationCalls;
    Require(context == reinterpret_cast<void*>(fixtureSave),"preparation receives caller context verbatim");
    Require(admitted.ready && admitted.token.kernel==fixtureKernel && predicateCalls==1,"preparation receives fresh admitted world after native predicate");
    Require(!reloadCalls && !sm2debug::reloadRuntime.awaitingTransition,"preparation precedes native request and transition latch");
    status=L"fixture preparation refused";
    if (revokeInPreparation) fixtureScope=false;
    return preparationAccept;
}
bool FakeKernelPredicate(void* kernel) {
    ++predicateCalls; Require(kernel == reinterpret_cast<void*>(fixtureKernel),"predicate receives exact kernel component");
    if (revokeInPredicate) fixtureScope = false;
    if (changeSequenceInPredicate) ++*reinterpret_cast<std::uint16_t*>(fixtureKernel+0x62f0);
    return predicateResult;
}
struct HostImage {
    std::uintptr_t base = 0, kernel = 0, owner = 0, level = 0, mode = 0, table = 0, records = 0, save = 0;
    std::ifstream executable;
    std::vector<void*> allocations;
    explicit HostImage(const wchar_t* path) : executable(std::filesystem::path(path), std::ios::binary) {
        Require(executable.good(), "open actual executable read-only");
        base = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr, sm2debug::kImageSize, MEM_RESERVE, PAGE_NOACCESS));
        Require(base != 0, "reserve synthetic image");
        kernel = Allocate(0x7000); owner = Allocate(0x1000); level = Allocate(0x1000); mode = Allocate(0x1000);
        table = Allocate(0x1000); records = Allocate(0x1000); save = Allocate(0x1000);
        fixtureKernel = kernel; fixtureSave = save;
        for (const auto& pin : std::vector<std::pair<std::uintptr_t, std::size_t>>{
            {0x34cbc90,32},{0x14cd7f0,32},{0x14cd816,29},{0x34c90c0,8},
            {0x345d400,12},{0x2cde290,8},{0x29bd7b0,32},{0x9dd798,10},
            {0x29b9e40,9},{0x28a5b60,76},{0x2d67a00,8},{0x2d696c0,66},{0x2d66430,28},
            {0x34cbdac,7},{0x34ce307,7},{0x34ce317,9},{0x34ce36a,8},{0x34cccda,13},
            {0x34cce4c,17},{0x34ccf79,9},{0x34ccfc7,51},{0x34ce4d1,25},
            {0x34ce628,4},{0x34ce680,4},{0x34cd33b,11},{0x34cdff4,18},{0x34ce00b,22},
            {0x83c7810,13},{0x83c7820,13},{0x34cbf60,225},{0x345e8e0,278},
            {0x34ca120,33},{0x34ca1d0,8},{0x3459510,21},{0x2d694f0,33}}) CopyFile(pin.first,pin.second);
        for (const auto& rtti : std::vector<std::array<std::uintptr_t,3>>{
            {0x547d668,0x8dfc0a0,0xab46140},{0x83c0ef0,0x8fb7248,0xabf7b58},
            {0x54a1ca0,0x8e18500,0xab512a8},{0x547d378,0x8dfbeb0,0xab46070}}) {
            CopyFile(rtti[1],24); CopyFile(rtti[2],96);
            Put(rtti[0]-8,base+rtti[1]);
        }
        Put(0x547d668+0x98,base+0x14cfa20);
        Put(0x83c0ef0+0x30,base+0x29b9e40);
        RUNTIME_FUNCTION entries[] = {{0x14cd7f0,0x14cd839,{0x92ce19c}}, {0x28a5b60,0x28a5bac,{0x94f4d6c}},
            {0x2d696c0,0x2d69702,{0x956cde4}},
            {0x345e8e0,0x345e90e,{0x96363c8}}, {0x345e90e,0x345e9ee,{0x96363d0}},
            {0x345e9ee,0x345e9f6,{0x96363e8}}, {0x34cbc90,0x34cbcc2,{0x964789c}},
            {0x34cbcc2,0x34cbdd5,{0x96478ac}}, {0x34cbf60,0x34cc041,{0x9647894}}, {0x34cca50,0x34cccf0,{0x9647b28}},
            {0x34cce1e,0x34ccea2,{0x9647b5c}}, {0x34ccea2,0x34cd16f,{0x9647b6c}},
            {0x34cd270,0x34ce690,{0x9647e18}}};
        Commit(0x100000,sizeof(entries)); std::memcpy(reinterpret_cast<void*>(base+0x100000), entries, sizeof(entries));
        Commit(0xc1de866,1);
        Reset();
    }
    ~HostImage() {
        for (void* pointer : allocations) VirtualFree(pointer,0,MEM_RELEASE);
        if (base) VirtualFree(reinterpret_cast<void*>(base),0,MEM_RELEASE);
    }
    std::uintptr_t Allocate(std::size_t size) {
        void* result = VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        Require(result != nullptr,"allocate synthetic heap object"); allocations.push_back(result);
        return reinterpret_cast<std::uintptr_t>(result);
    }
    void Commit(std::uintptr_t rva, std::size_t size) {
        const auto first = rva & ~std::uintptr_t(0xfff), end = (rva+size+0xfff)&~std::uintptr_t(0xfff);
        Require(VirtualAlloc(reinterpret_cast<void*>(base+first),end-first,MEM_COMMIT,PAGE_READWRITE) != nullptr,"commit host fixture page");
    }
    template<class T> void Put(std::uintptr_t rva, T value) {
        Commit(rva,sizeof(value)); std::memcpy(reinterpret_cast<void*>(base+rva),&value,sizeof(value));
    }
    void CopyFile(std::uintptr_t rva, std::size_t count) {
        Commit(rva,count);
        const auto raw = rva-(rva < 0x529a000 ? 0xa00 : rva < 0x991c000 ? 0xc00 : 0x1800);
        executable.clear(); executable.seekg(static_cast<std::streamoff>(raw));
        executable.read(reinterpret_cast<char*>(base+rva),count);
        Require(executable.gcount()==static_cast<std::streamsize>(count),"read exact original PE bytes");
    }
    template<class T> void Object(std::uintptr_t address, std::size_t offset, T value) {
        std::memcpy(reinterpret_cast<void*>(address+offset),&value,sizeof(value));
    }
    void Reset() {
        sm2debug::InitializeReloadRuntime(nullptr,false);
        reloadCalls = frontendCalls = predicateCalls = unloadCalls = preparationCalls = 0;
        acceptNative = predicateResult = preparationAccept = true; revokeInPreparation = false;
        fixtureScope = revokeInPredicate = changeSequenceInPredicate = false;
        const auto manager = base+0xb26b150;
        Put(0xc2266a8,manager); Put(0xb26b150,base+0x547d668); Put(0xb26b150+0x10,level);
        Put(0xb26b150+0xc,std::uint32_t(0)); Put(0xc1de866,std::uint8_t(1));
        Put(0xb26b150+0x430628,std::int32_t(22)); Put(0xb26b150+0x199fc0,std::int32_t(0));
        Put(0xb26b150+0x319f68,std::int32_t(0)); Put(0xb26b150+0x430665,std::uint8_t(1));
        Put(0xc21d330,mode); Object(mode,0x10,std::int32_t(9));
        Put(0xc21d630,std::uint32_t((3<<20)|7)); Put(0xc0bebe0,table); Put(0xc0bec00,std::int32_t(8));
        Object(table,7*16,kernel); Object(table,7*16+8,std::uint32_t(3));
        Object(kernel,0,base+0x83c0ef0); Object(kernel,0x10,owner);
        for (const auto& property : std::array<std::pair<std::uintptr_t,std::uintptr_t>,5>{{
            {0x48,0x53bcd58},{0x6268,0x83c0ec0},{0x6298,0x52fa5e0},{0x62e0,0x530ef90},{0x62f8,0x52fa5e0}}}) {
            Object(kernel,property.first,base+property.second); Put(property.second+0x20,base+0x2d694f0);
        }
        Object(kernel,0x58,std::uint64_t(0x9000000012345678)); Object(kernel,0x62c0,std::uint32_t(0xb4946692));
        Object(kernel,0x62f0,std::uint16_t(15)); Object(kernel,0x6308,std::uint8_t(3));
        Put(0xb26b150+0x41ff50,records); Put(0xb26b150+0x41ff58,std::int32_t(3));
        Put(0xc0bd2d8,save); Object(save,0,base+0x54a1ca0); Object(save,0x70,std::uint8_t(0));
        Put(0xc145040,std::uint8_t(0)); Put(0xc0dfb20,std::uintptr_t(0));
        static const char* names[] = {"test_entry","test_mid","test_end"};
        // Values independently checked by the native table/seed proof runner.
        static const std::uint32_t keys[] = {0xb4946692,0x3ba4f72f,0x7af63050};
        for (std::size_t i=0;i<3;++i) {
            std::memset(reinterpret_cast<void*>(records+i*72),0,72);
            Object(records,i*72+0x20,reinterpret_cast<std::uintptr_t>(names[i]));
            Object(records,i*72+0x28,keys[i]);
        }
        std::lock_guard lock(sm2debug::reloadRuntime.mutex);
        auto& runtime = sm2debug::reloadRuntime;
        runtime.initialized = runtime.supported = true;
        runtime.image = {base,sm2debug::kImageSize,base+0x100000,13};
        runtime.reload = FakeReload; runtime.frontend = FakeFrontend; runtime.unload = FakeUnload; runtime.kernelPredicate = FakeKernelPredicate;
    }
    sm2debug::ReloadState Ready() {
        const auto state = sm2debug::ReadReloadState();
        Require(state.ready && state.frontendReady && state.lobbyReady && state.checkpoints && state.checkpoints->size()==3,"valid fixture is ready");
        Require(sm2debug::SameReloadContext(state.token,state.token),"context reflexive after observation");
        return state;
    }
};
}
namespace sm2debug { bool IsWithinVerifiedEngineCallback() noexcept { return fixtureScope; } }

int wmain(int argc,wchar_t** argv) {
    try {
        Require(argc==2,"actual executable path required");
        HostImage host(argv[1]);
        IMAGE_NT_HEADERS64 actualHeader{};
        host.executable.clear(); host.executable.seekg(0x1f0);
        host.executable.read(reinterpret_cast<char*>(&actualHeader),sizeof(actualHeader));
        Require(host.executable.gcount()==sizeof(actualHeader),"read original native executable header");
        Require(actualHeader.OptionalHeader.ImageBase==0x140000000 && sm2debug::HeaderIdentity(actualHeader,host.base),"preferred file base passes actual header predicate");
        actualHeader.OptionalHeader.ImageBase=host.base;
        Require(sm2debug::HeaderIdentity(actualHeader,host.base),"Windows mapped actual module base passes same production predicate");
        actualHeader.OptionalHeader.ImageBase=0x180000000;
        Require(!sm2debug::HeaderIdentity(actualHeader,host.base),"foreign header base rejected");
        actualHeader.OptionalHeader.ImageBase=host.base;actualHeader.FileHeader.TimeDateStamp^=1;
        Require(!sm2debug::HeaderIdentity(actualHeader,host.base),"mapped base does not relax timestamp identity");
        ++cases;
        Require(sm2debug::CodePins(sm2debug::reloadRuntime.image),"actual PE code pins pass in relocated synthetic image");
        Require(sm2debug::FullReloadCodePins(sm2debug::reloadRuntime.image),"actual force-full-unload producer, consumers and unwind fragments pass");
        Require(sm2debug::NativeTypes(sm2debug::reloadRuntime.image),"actual PE RTTI and vtable pins pass");
        auto state=host.Ready(); auto cached=sm2debug::ReadReloadState();
        Require(state.token.managerState==22,"native stable loaded-level state22 is the only positive fixture state");
        Require(state.checkpoints==cached.checkpoints,"immutable choices reused while raw table and context match"); ++cases;
        std::wstring status;
        Require(!sm2debug::CheckReloadExecutionContext(state,status) && !predicateCalls && !reloadCalls,"admission refuses window context before native calls");
        fixtureScope=true;
        Require(sm2debug::CheckReloadExecutionContext(state,status,state.checkpoints->at(1).key),"admission validates exact live selected checkpoint");
        Require(predicateCalls==1 && !reloadCalls && !frontendCalls && !sm2debug::reloadRuntime.awaitingTransition,
            "admission only calls read-only predicate and does not request or latch transition");
        Require(*reinterpret_cast<const std::uint8_t*>(host.kernel+0x6308)==3,"admission does not modify synthetic native kernel state");
        Require(sm2debug::reloadRuntime.mutex.try_lock(),"admission returns with no retained runtime mutex");sm2debug::reloadRuntime.mutex.unlock();
        Require(sm2debug::SameReloadContext(state.token,host.Ready().token),"admission preserves current token and readiness");++cases;
        host.Reset();state=host.Ready();fixtureScope=true;predicateResult=false;
        Require(!sm2debug::CheckReloadExecutionContext(state,status) && predicateCalls==1 && !reloadCalls && !frontendCalls,
            "admission rejects native inactive predicate without downstream actions");++cases;
        host.Reset();state=host.Ready();fixtureScope=true;revokeInPredicate=true;
        Require(!sm2debug::CheckReloadExecutionContext(state,status) && predicateCalls==1 && !reloadCalls,"admission rejects revoked callback after predicate");++cases;
        host.Reset();state=host.Ready();fixtureScope=true;changeSequenceInPredicate=true;
        Require(!sm2debug::CheckReloadExecutionContext(state,status) && predicateCalls==1 && !reloadCalls,
            "admission repeats fresh world/generation check after native preflight");++cases;
        host.Reset();state=host.Ready();fixtureScope=true;
        Require(!sm2debug::CheckReloadExecutionContext(state,status,0xdeadbeef) && !predicateCalls,"admission rejects foreign key before predicate");
        auto admissionForged=state;auto admissionChoices=std::make_shared<std::vector<sm2debug::ReloadCheckpoint>>(*state.checkpoints);
        admissionChoices->at(1).name=L"different captured checkpoint";admissionForged.checkpoints=admissionChoices;
        Require(!sm2debug::CheckReloadExecutionContext(admissionForged,status,admissionChoices->at(1).key) && !predicateCalls,
            "admission compares exact captured checkpoint record/name");++cases;
        host.Reset();state=host.Ready();fixtureScope=true;changeSequenceInPredicate=true;
        Require(!sm2debug::ExecuteFullReload(state,0,status) && predicateCalls==1 && !reloadCalls,
            "shared request admission prevents Full Reload if native preflight changed generation");++cases;
        host.Reset();state=host.Ready();fixtureScope=true;changeSequenceInPredicate=true;
        Require(!sm2debug::ExecuteExitFrontend(state,status) && predicateCalls==1 && !frontendCalls && !reloadCalls,
            "frontend repeats fresh world/generation check after native preflight before routing writes");++cases;
        host.Reset();state=host.Ready();
        Require(!sm2debug::ExecuteReload(state,0,status) && !reloadCalls,"window/render path cannot call native request");
        Require(!sm2debug::ExecuteExitFrontend(state,status) && !frontendCalls,"window path cannot call frontend");
        fixtureScope=true;
        Require(sm2debug::ExecuteReload(state,0,status),"current checkpoint reload accepted");
        Require(reloadCalls==1 && receivedManager==reinterpret_cast<void*>(host.base+0xb26b150) && !receivedKey && !receivedFlag,"exact Reload ABI arguments");
        Require(!sm2debug::ReadReloadState().ready,"pending transition does not expose stale choices"); ++cases;
        host.Object(host.kernel,0x6308,std::uint8_t(3)); host.Object(host.kernel,0x62f0,std::uint16_t(16));
        auto after=host.Ready(); Require(!sm2debug::SameReloadContext(state.token,after.token),"new transition invalidates old command token");
        fixtureScope=true; Require(!sm2debug::ExecuteReload(state,0,status) && reloadCalls==1,"stale queued request rejected"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true;
        Require(sm2debug::ExecuteReload(state,state.checkpoints->at(1).key,status),"exact selected current-world key accepted");
        Require(receivedKey==state.checkpoints->at(1).key && !receivedFlag,"selected key forwarded verbatim"); ++cases;
        host.Reset(); state=host.Ready();
        Require(!sm2debug::ExecuteFullReload(state,0,status) && !reloadCalls && !predicateCalls,"Full Reload refuses window context before any native call");
        fixtureScope=true;
        Require(sm2debug::ExecuteFullReload(state,0,status),"current checkpoint Full Reload accepted");
        Require(reloadCalls==1 && receivedManager==reinterpret_cast<void*>(host.base+0xb26b150) && !receivedKey && receivedFlag,"exact Full Reload ABI: same manager, current sentinel, force true");
        Require(status.find(L"forced world unload")!=std::wstring::npos,"Full Reload status states real forceful unload semantics");
        Require(!sm2debug::ReadReloadState().ready,"Full Reload latches transition and removes stale choices"); ++cases;
        host.Object(host.kernel,0x6308,std::uint8_t(3)); host.Object(host.kernel,0x62f0,std::uint16_t(16));
        after=host.Ready(); fixtureScope=true;
        Require(!sm2debug::ExecuteFullReload(state,0,status) && reloadCalls==1,"Full Reload rejects stale world token"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true;
        Require(sm2debug::ExecuteFullReload(state,state.checkpoints->at(1).key,status),"selected current-world checkpoint Full Reload accepted");
        Require(receivedKey==state.checkpoints->at(1).key && receivedFlag,"Full Reload selected exact key forwarded with force true"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true; acceptNative=false;
        Require(!sm2debug::ExecuteFullReload(state,0,status) && reloadCalls==1 && receivedFlag,"Full Reload exposes native false result");
        Require(sm2debug::ReadReloadState().ready,"rejected Full Reload keeps world available"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true; predicateResult=false;
        Require(!sm2debug::ExecuteFullReload(state,0,status) && predicateCalls==1 && !reloadCalls,"Full Reload false active-kernel predicate prevents downstream request"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true; revokeInPredicate=true;
        Require(!sm2debug::ExecuteFullReload(state,0,status) && predicateCalls==1 && !reloadCalls,"Full Reload rechecks revoked callback after preflight"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true;
        Require(!sm2debug::ExecuteFullReload(state,0xdeadbeef,status) && !predicateCalls && !reloadCalls,"Full Reload rejects foreign checkpoint before preflight"); ++cases;
        for (const auto rva : {0x34cbdacu,0x34ce317u,0x34cce4cu,0x34ccfc7u,0x34ce4d1u}) {
            host.Reset(); state=host.Ready(); host.Put(rva,std::uint8_t(0x90)); fixtureScope=true;
            Require(!sm2debug::ExecuteFullReload(state,0,status) && !predicateCalls && !reloadCalls,"Full Reload altered producer/teardown pins fail before native calls");
            host.CopyFile(rva,1);
        } ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true; acceptNative=false;
        Require(!sm2debug::ExecuteReload(state,0,status) && reloadCalls==1,"native bool rejection is truthful");
        Require(sm2debug::ReadReloadState().ready,"rejected native request does not latch pending transition"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true;
        Require(!sm2debug::ExecuteReload(state,0xdeadbeef,status) && !reloadCalls,"foreign checkpoint cannot reach native call");
        auto forged=state; auto choices=std::make_shared<std::vector<sm2debug::ReloadCheckpoint>>(*state.checkpoints);
        choices->at(1).name=L"different name"; forged.checkpoints=choices;
        Require(!sm2debug::ExecuteReload(forged,choices->at(1).key,status) && !reloadCalls,"captured exact name is rechecked");
        choices->at(1)=state.checkpoints->at(1); choices->at(1).recordIndex=2;
        Require(!sm2debug::ExecuteReload(forged,choices->at(1).key,status) && !reloadCalls,"captured record index is rechecked"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true;
        Require(sm2debug::ExecuteExitFrontend(state,status) && frontendCalls==1 && !reloadCalls,"full frontend action called once"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true;
        Require(sm2debug::ExecuteReload(state,0,status),"transition request before skipped observation");
        host.Object(host.kernel,0x6308,std::uint8_t(3)); host.Object(host.kernel,0x62f0,std::uint16_t(17));
        Require(host.Ready().ready,"completed transition between snapshots unlocks on sequence change"); ++cases;
        host.Reset(); state=host.Ready(); DWORD old=0;
        Require(VirtualProtect(reinterpret_cast<void*>(host.save),0x1000,PAGE_READONLY,&old)!=0,"protect synthetic save page");
        auto readOnlySave=sm2debug::ReadReloadState();
        Require(readOnlySave.ready && !readOnlySave.frontendReady,"read-only save routing storage disables frontend only");
        fixtureScope=true; Require(!sm2debug::ExecuteExitFrontend(state,status) && !frontendCalls,"read-only frontend target never called");
        Require(VirtualProtect(reinterpret_cast<void*>(host.save),0x1000,old,&old)!=0,"restore synthetic save protection"); ++cases;
        host.Reset(); state=host.Ready();
        Require(VirtualProtect(reinterpret_cast<void*>((host.base+0xc1de866)&~std::uintptr_t(0xfff)),0x1000,PAGE_EXECUTE_READWRITE,&old)!=0,"protect synthetic routing global executable");
        Require(!sm2debug::ReadReloadState().frontendReady,"executable writable storage is rejected");
        Require(VirtualProtect(reinterpret_cast<void*>((host.base+0xc1de866)&~std::uintptr_t(0xfff)),0x1000,PAGE_READWRITE,&old)!=0,"restore routing global"); ++cases;
        host.Reset(); state=host.Ready(); std::thread worker([&]{std::wstring reason;Require(!sm2debug::ExecuteReload(state,0,reason),"foreign thread has no fixture callback scope");});worker.join();
        Require(!reloadCalls,"foreign thread made no native call"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true; predicateResult=false;
        Require(!sm2debug::ExecuteReload(state,0,status) && predicateCalls==1 && !reloadCalls,"false native predicate prevents Reload cleanup/request path");
        Require(!sm2debug::ExecuteExitFrontend(state,status) && predicateCalls==2 && !frontendCalls,"false predicate prevents frontend routing writes"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true; revokeInPredicate=true;
        Require(!sm2debug::ExecuteReload(state,0,status) && predicateCalls==1 && !reloadCalls,"callback revocation after predicate prevents request"); ++cases;
        host.Reset(); state=host.Ready(); fixtureScope=true;
        host.Put(0xc145040,std::uint8_t(1));host.Object(host.owner,0xa8,std::uint32_t(1));
        Require(!sm2debug::ExecuteReload(state,0,status) && !predicateCalls && !reloadCalls,"unreadable replica table rejected before native predicate");
        host.Put(0xc0dfb20,host.table); host.Object(host.table,8,host.owner+0x200);
        host.Object(host.owner+0x200,0x11f,std::uint8_t(0));host.Object(host.owner+0x200,0x121,std::uint8_t(2));
        Require(sm2debug::ExecuteReload(state,0,status) && predicateCalls==1 && reloadCalls==1,"valid weak replica inputs permit predicate and request"); ++cases;
        // Each independent ownership/readiness corruption fails closed.
        for (unsigned index=0;index<15;++index) {
            host.Reset(); state=host.Ready();
            switch(index) {
                case 0:host.Put(0xc2266a8,std::uintptr_t(0));break;
                case 1:host.Put(0xb26b150,host.base+0x547d670);break;
                case 2:host.Put(0xb26b150+0x430628,std::int32_t(0));break;
                case 3:host.Put(0xb26b150+0x199fc0,std::int32_t(1));break;
                case 4:host.Put(0xb26b150+0x319f68,std::int32_t(1));break;
                case 5:host.Put(0xb26b150+0x430665,std::uint8_t(0));break;
                case 6:host.Object(host.mode,0x10,std::int32_t(8));break;
                case 7:host.Object(host.table,7*16+8,std::uint32_t(4));break;
                case 8:host.Object(host.kernel,0,host.base+0x83c0ef8);break;
                case 9:host.Object(host.kernel,0x10,std::uintptr_t(0));break;
                case 10:host.Object(host.kernel,0x58,std::uint64_t(1));break;
                case 11:host.Object(host.kernel,0x6308,std::uint8_t(2));break;
                case 12:host.Put(0xb26b150+0x41ff58,std::int32_t(4097));break;
                case 13:host.Object(host.records,0x28,std::uint32_t(1));break;
                case 14:host.Object(host.records,72+0x28,std::uint32_t(0xb4946692));break;
            }
            auto invalid=sm2debug::ReadReloadState(); Require(!invalid.ready && !invalid.checkpoints,"invalid live state returns no stale entries");
            fixtureScope=true;Require(!sm2debug::ExecuteReload(state,0,status) && !reloadCalls,"invalid world rejected at execution");
            Require(!sm2debug::ExecuteFullReload(state,0,status) && !reloadCalls,"Full Reload uses identical world ownership and readiness guards");
            Require(!sm2debug::CheckReloadExecutionContext(state,status) && !predicateCalls && !reloadCalls,"admission uses identical type/generation/readiness guards before native predicate");
        } ++cases;
        // A ready byte alone cannot admit loading, transition or foreign states.
        for (std::int32_t nativeState=-1;nativeState<=26;++nativeState) {
            if(nativeState==22)continue;
            host.Reset(); state=host.Ready();host.Put(0xb26b150+0x430628,nativeState);
            Require(!sm2debug::ReadReloadState().ready,"every state except22 rejects despite ready byte1");fixtureScope=true;
            Require(!sm2debug::ExecuteReload(state,0,status) && !sm2debug::ExecuteFullReload(state,0,status) &&
                !sm2debug::ExecuteExitFrontend(state,status) && !sm2debug::CheckReloadExecutionContext(state,status) && !predicateCalls && !reloadCalls && !frontendCalls,
                "start/load/transition states perform zero native preflight or requests");
        } ++cases;
        host.Reset();state=host.Ready();host.Put(0x34ce019,std::uint8_t(0x90));fixtureScope=true;
        Require(!sm2debug::ExecuteReload(state,0,status) && !sm2debug::ExecuteFullReload(state,0,status) &&
            !predicateCalls && !reloadCalls,"changed native ready producer disables both reload requests");
        host.CopyFile(0x34ce019,1);++cases;
        host.Reset();state=host.Ready();host.Put(0x34cbc90,std::uint8_t(0x90));fixtureScope=true;
        Require(!sm2debug::ExecuteReload(state,0,status) && !reloadCalls,"modified native entry refused");
        host.CopyFile(0x34cbc90,32); ++cases;
        // Preparation is an optional, once-only seam after complete admission.
        for (bool full : {false,true}) {
            host.Reset();state=host.Ready();fixtureScope=true;
            const auto request = full ? sm2debug::ExecuteFullReload : sm2debug::ExecuteReload;
            Require(request(state,0,status,FakePreparation,reinterpret_cast<void*>(host.save)) && preparationCalls==1 && reloadCalls==1,
                "admitted request invokes preparation exactly once before native call");
            Require(receivedFlag==full,"preparation preserves normal/full native flag");++cases;
            host.Reset();state=host.Ready();fixtureScope=true;preparationAccept=false;
            Require(!request(state,0,status,FakePreparation,reinterpret_cast<void*>(host.save)) && preparationCalls==1 && !reloadCalls &&
                !sm2debug::reloadRuntime.awaitingTransition,"preparation refusal prevents native call and latch");
            Require(status==L"fixture preparation refused","preparation refusal status preserved");++cases;
            host.Reset();state=host.Ready();fixtureScope=true;revokeInPreparation=true;
            Require(!request(state,0,status,FakePreparation,reinterpret_cast<void*>(host.save)) && preparationCalls==1 && !reloadCalls,
                "callback revocation after preparation prevents request");++cases;
            host.Reset();state=host.Ready();fixtureScope=true;predicateResult=false;
            Require(!request(state,0,status,FakePreparation,reinterpret_cast<void*>(host.save)) && !preparationCalls && !reloadCalls,
                "failed native admission never invokes preparation");++cases;
            host.Reset();state=host.Ready();fixtureScope=true;acceptNative=false;
            Require(!request(state,0,status,FakePreparation,reinterpret_cast<void*>(host.save)) && preparationCalls==1 && reloadCalls==1 &&
                !sm2debug::reloadRuntime.awaitingTransition,"native failure after preparation does not latch or repeat it");++cases;
        }
        host.Reset();state=host.Ready();
        Require(sm2debug::LobbyCodePins(sm2debug::reloadRuntime.image),"actual retained unload code and unwind fragments pass");
        Require(!sm2debug::ExecuteExitLobby(state,status) && !unloadCalls && !predicateCalls &&
            *reinterpret_cast<std::uint8_t*>(host.save+0x70)==0,"window path cannot mutate lobby save flag or invoke unload");
        fixtureScope=true;
        Require(sm2debug::ExecuteExitLobby(state,status) && unloadCalls==1 && !reloadCalls && !frontendCalls,"Lobby uses its exact native unload seam once");
        Require(receivedManager==reinterpret_cast<void*>(host.base+0xb26b150),"Lobby forwards exact current manager");
        Require(*reinterpret_cast<std::uint8_t*>(host.base+0xc1de866)==1,"Lobby preserves frontend hero-setup routing byte");
        Require(!sm2debug::ReadReloadState().ready,"accepted lobby unload latches transition");++cases;
        host.Reset();state=host.Ready();fixtureScope=true;acceptNative=false;
        Require(!sm2debug::ExecuteExitLobby(state,status) && unloadCalls==1 && *reinterpret_cast<std::uint8_t*>(host.save+0x70)==1,
            "native Lobby refusal retains original save flag side effect");
        Require(status.find(L"remains set")!=std::wstring::npos && !sm2debug::reloadRuntime.awaitingTransition,
            "Lobby failure reports retained flag and does not latch");++cases;
        for (unsigned failure=0;failure<11;++failure) {
            host.Reset();state=host.Ready();fixtureScope=true;
            switch(failure) {
                case 0:predicateResult=false;break;
                case 1:revokeInPredicate=true;break;
                case 2:changeSequenceInPredicate=true;break;
                case 3:host.Object(host.save,0x70,std::uint8_t(2));break;
                case 4:host.Object(host.kernel,0x62f8,std::uintptr_t(0));break;
                case 5:host.Put(0x52fa5e0+0x20,host.base+0x2d694f1);break;
                case 6:host.Put(0xb26b150+0xc,std::uint32_t((3<<20)|8));break;
                case 7:host.Put(0xb26b150+0xc,std::uint32_t((3<<20)|6));host.Object(host.table,6*16+8,std::uint32_t(3));host.Object(host.table,6*16,std::uintptr_t(1));break;
                case 8:host.Put(0x34cbf60,std::uint8_t(0x90));break;
                case 9:host.Put(0x345e8e0,std::uint8_t(0x90));break;
                case 10:host.Object(host.save,0,host.base+0x54a1ca8);break;
            }
            const auto initialFlag=*reinterpret_cast<std::uint8_t*>(host.save+0x70);
            Require(!sm2debug::ExecuteExitLobby(state,status) && !unloadCalls && !reloadCalls && !frontendCalls,
                "Lobby fails closed for inactive/revoked/stale world, save, property, event owner or code");
            Require(*reinterpret_cast<std::uint8_t*>(host.save+0x70)==initialFlag && !sm2debug::reloadRuntime.awaitingTransition,
                "rejected Lobby preflight performs zero writes or latch");
            host.CopyFile(0x34cbf60,1);host.CopyFile(0x345e8e0,1);++cases;
        }
        host.Reset();state=host.Ready();
        Require(VirtualProtect(reinterpret_cast<void*>(host.save),0x1000,PAGE_READONLY,&old)!=0,"protect save for Lobby");
        fixtureScope=true;
        Require(!sm2debug::ReadReloadState().lobbyReady && !sm2debug::ExecuteExitLobby(state,status) && !unloadCalls,"readonly save disables Lobby without native call");
        Require(VirtualProtect(reinterpret_cast<void*>(host.save),0x1000,PAGE_READWRITE,&old)!=0,"restore Lobby save");++cases;
        host.Reset();state=host.Ready();
        Require(VirtualProtect(reinterpret_cast<void*>((host.base+0xc1de866)&~std::uintptr_t(0xfff)),0x1000,PAGE_READONLY,&old)!=0,"protect frontend-only routing");
        state=sm2debug::ReadReloadState();fixtureScope=true;
        Require(state.ready && state.lobbyReady && !state.frontendReady && sm2debug::ExecuteExitLobby(state,status),
            "Lobby remains usable without writable frontend-only routing");
        Require(VirtualProtect(reinterpret_cast<void*>((host.base+0xc1de866)&~std::uintptr_t(0xfff)),0x1000,PAGE_READWRITE,&old)!=0,"restore frontend-only routing");++cases;
        host.Reset();state=host.Ready();host.Put(0x34cbf60,std::uint8_t(0x90));fixtureScope=true;
        Require(!sm2debug::ReadReloadState().lobbyReady && sm2debug::ExecuteReload(state,0,status),"missing Lobby pin does not regress existing Reload");
        host.CopyFile(0x34cbf60,1);++cases;
        sm2debug::InitializeReloadRuntime(GetModuleHandleW(nullptr),true);
        Require(!sm2debug::ReadReloadState().supported,"actual wrong-executable host cannot initialize production bindings"); ++cases;
        std::cout<<"{\"passed\":true,\"cases\":"<<cases<<",\"assertions\":"<<assertions
            <<",\"game_launched\":false,\"real_engine_context_proven\":false,\"boundary\":\"synthetic host callback only\"}\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<"\n";return 1;}
}
