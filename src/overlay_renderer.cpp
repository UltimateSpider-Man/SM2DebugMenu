// SPDX-License-Identifier: GPL-3.0-or-later
// The production path creates no window and never patches executable code.
#include "overlay.h"
#include "debugmenu.h"
#include "screen_capture.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef SM2DEBUG_PREVIEW
#include <dwmapi.h>
#endif

namespace sm2debug {
namespace {
namespace fs = std::filesystem;
template<class T> struct Com {
    T* value = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Com& operator=(Com&& other) noexcept { reset(); value = std::exchange(other.value, nullptr); return *this; }
    ~Com() { reset(); }
    void reset() { if (value) { value->Release(); value = nullptr; } }
    T** put() { reset(); return &value; }
    T* operator->() const { return value; }
};
struct Slot {
    void** address = nullptr;
    void* original = nullptr;
    void* replacement = nullptr;
    DWORD protection = 0;
    bool changed = false, protectionPending = false;
};
struct Frame {
    Com<ID3D12Resource> buffer;
    Com<ID3D12CommandAllocator> allocator;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fence = 0;
};
struct Renderer {
    std::mutex mutex;
    std::atomic<bool> stopping{false};
    HMODULE module = nullptr, dxgi = nullptr;
    fs::path root, systemDirectory;
    bool privateRuntime = false;
    bool preview = false, initialized = false, resources = false;
    bool dxBackend = false, platformBackend = false, untrackedSubmission = false;
    std::atomic<bool> failed{false};
    HWND window = nullptr;
    void** factoryTable = nullptr;
    void** swapTable = nullptr;
    void* factoryOld = nullptr;
    void* factoryHwnd = nullptr;
    void* present = nullptr;
    void* present1 = nullptr;
    void* resize = nullptr;
    void* resize1 = nullptr;
    std::vector<Slot> slots;
    Com<IDXGISwapChain3> swap;
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12Device> device;
    Com<ID3D12DescriptorHeap> rtvHeap, srvHeap;
    Com<ID3D12GraphicsCommandList> list;
    Com<ID3D12Fence> fence;
    HANDLE event = nullptr;
    std::vector<Frame> frames;
    std::vector<UINT64> uploadFences;
    ImGuiContext* context = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT64 nextFence = 1, lastFence = 0, sequence = 0;
    UINT64 rendered = 0, hidden = 0, busy = 0, resizes = 0;
    bool srvUsed = false;
#ifdef SM2DEBUG_PREVIEW
    bool previewFaultInjected = false;
    bool previewCaptureRequested = false;
    unsigned previewCaptureStyle = 0;
    std::array<bool, 2> previewCaptureCompleted{};
    std::array<UINT, 2> previewCapturedGreen{}, previewCapturedWhite{}, previewCapturedVertices{};
#endif
};
// A callback may already be executing during shutdown. The pinned callback
// module and POD/original pointers therefore outlive every COM vtable hook.
Renderer& State() { static Renderer* state = new Renderer; return *state; }
thread_local bool insidePresent = false;
thread_local bool insideResize = false;

void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void Check(HRESULT result, const char* reason) { Require(SUCCEEDED(result), reason); }
std::wstring ModulePath(HMODULE module) {
    wchar_t path[32768]{};
    DWORD count = GetModuleFileNameW(module, path, 32768);
    Require(count && count < 32768, "Cannot identify graphics module path");
    return std::wstring(path, count);
}
bool SamePath(const fs::path& first, const fs::path& second) {
    auto a = first.lexically_normal().wstring(), b = second.lexically_normal().wstring();
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
void Log(const std::string& message) noexcept {
    try {
        fs::path directory(OutputDirectory());
        if (directory.empty()) directory = State().root / L"SM2DebugMenu";
        std::ofstream out(directory / L"overlay.log", std::ios::app | std::ios::binary);
        out << GetTickCount64() << ' ' << message << '\n';
    } catch (...) {}
}
template<class Function> void* Address(Function function) {
    void* address = nullptr;
    static_assert(sizeof(address) == sizeof(function));
    std::memcpy(&address, &function, sizeof(address));
    return address;
}
template<class Function> Function FunctionAt(void* address) {
    Function function = nullptr;
    static_assert(sizeof(address) == sizeof(function));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}
template<class Function> Function Export(HMODULE module, const char* name) {
    auto symbol = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(symbol) == sizeof(function));
    std::memcpy(&function, &symbol, sizeof(symbol));
    Require(function != nullptr, "Required graphics export is absent");
    return function;
}
HMODULE Owner(void* address) {
    HMODULE module = nullptr;
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &module), "Cannot identify COM method implementation");
    return module;
}
void VerifiedDxgi(void* address) { Require(Owner(address) == State().dxgi, "Swapchain/factory method is outside the selected DXGI module"); }
void VerifiedDevice(void* address) {
    const auto& state = State();
    auto path = ModulePath(Owner(address));
    const bool approved = state.privateRuntime ?
        (SamePath(path, state.root / L"d3d12core.dll") || SamePath(path, state.root / L"d3d12.dll")) :
        (SamePath(path, state.systemDirectory / L"d3d12.dll") ||
         SamePath(path, state.systemDirectory / L"D3D12Core.dll") ||
         SamePath(path, state.root / L"D3D12" / L"D3D12Core.dll"));
    Require(approved, "D3D12 queue/device implementation is outside the selected graphics runtime");
}
void PinCallbacks() {
    HMODULE module = nullptr;
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(Address(&PinCallbacks)), &module) && module == State().module,
        "Cannot pin overlay callback module");
}
void ChangeSlot(Slot& slot, bool install) {
    DWORD previousProtection = 0;
    Require(VirtualProtect(slot.address, sizeof(void*), PAGE_READWRITE, &previousProtection), "Cannot write verified COM vtable data");
    slot.protectionPending = true;
    void* expected = install ? slot.original : slot.replacement;
    void* replacement = install ? slot.replacement : slot.original;
    void* previous = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot.address), replacement, expected);
    if (previous == expected) slot.changed = install;
    DWORD ignored = 0;
    bool protectedAgain = VirtualProtect(slot.address, sizeof(void*), slot.protection, &ignored) != FALSE;
    if (protectedAgain) slot.protectionPending = false;
    Require(protectedAgain, "Cannot restore COM vtable protection");
    Require(previous == expected, "COM vtable entry changed concurrently; refusing to overwrite it");
}
void Hook(void** table, unsigned index, void* replacement) {
    auto& state = State();
    void** address = table + index;
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT &&
        memory.Type == MEM_IMAGE && memory.AllocationBase == state.dxgi &&
        !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        ((memory.Protect & 0xff) == PAGE_READONLY || (memory.Protect & 0xff) == PAGE_READWRITE || (memory.Protect & 0xff) == PAGE_WRITECOPY) &&
        (reinterpret_cast<uintptr_t>(address) % alignof(void*)) == 0,
        "Hook target is outside aligned, non-executable selected DXGI vtable data");
    VerifiedDxgi(*address);
    state.slots.push_back({address, *address, replacement, memory.Protect, false, false});
    ChangeSlot(state.slots.back(), true);
}
void RestoreHooks() noexcept {
    auto& state = State();
    for (auto it = state.slots.rbegin(); it != state.slots.rend(); ++it) {
        if (it->changed) { try { ChangeSlot(*it, false); } catch (...) { Log("COM hook pointer rollback failed; callback remains pinned."); } }
        if (it->protectionPending) {
            DWORD ignored = 0;
            if (VirtualProtect(it->address, sizeof(void*), it->protection, &ignored)) it->protectionPending = false;
            else Log("COM vtable protection rollback remains pending.");
        }
    }
}
bool WaitGpu(DWORD timeout) {
    auto& state = State();
    if (state.untrackedSubmission) return false;
    if (!state.fence.value || !state.lastFence || state.fence->GetCompletedValue() >= state.lastFence) return true;
    if (!state.event || FAILED(state.fence->SetEventOnCompletion(state.lastFence, state.event))) return false;
    return WaitForSingleObject(state.event, timeout) == WAIT_OBJECT_0 && state.fence->GetCompletedValue() >= state.lastFence;
}
void ReleaseResources() {
    auto& state = State();
    if (state.context) ImGui::SetCurrentContext(state.context);
    if (state.dxBackend) { ImGui_ImplDX12_Shutdown(); state.dxBackend = false; }
    state.list.reset(); state.frames.clear(); state.uploadFences.clear();
    state.rtvHeap.reset(); state.srvHeap.reset(); state.fence.reset();
    if (state.event) { CloseHandle(state.event); state.event = nullptr; }
    state.nextFence = 1; state.lastFence = state.sequence = 0; state.srvUsed = false; state.resources = false;
}
void AllocateSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    auto& state = *static_cast<Renderer*>(info->UserData);
    // ImGui 1.91.9b allocates exactly one descriptor for its font atlas.
    IM_ASSERT(!state.srvUsed);
    state.srvUsed = true;
    *cpu = state.srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = state.srvHeap->GetGPUDescriptorHandleForHeapStart();
}
void FreeSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {
    static_cast<Renderer*>(info->UserData)->srvUsed = false;
}
void CreateContext() {
    auto& state = State();
    if (state.context) { ImGui::SetCurrentContext(state.context); return; }
    IMGUI_CHECKVERSION();
    state.context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    wchar_t windowsDirectory[32768]{};
    UINT count = GetWindowsDirectoryW(windowsDirectory, 32768);
    if (count && count < 32768) {
        auto fonts = fs::path(windowsDirectory) / L"Fonts";
        auto font = fonts / L"courbd.ttf";
        if (!fs::is_regular_file(font)) font = fonts / L"cour.ttf";
        std::string path = font.u8string();
        if (!fs::is_regular_file(font) || !io.Fonts->AddFontFromFileTTF(path.c_str(), 19.0f)) io.Fonts->AddFontDefault();
        const auto frontendFont = fonts / L"segoeuib.ttf";
        if (fs::is_regular_file(frontendFont)) io.Fonts->AddFontFromFileTTF(frontendFont.u8string().c_str(), 34.0f);
    } else io.Fonts->AddFontDefault();
    Require(ImGui_ImplWin32_Init(state.window), "Cannot initialize overlay platform backend on existing window");
    state.platformBackend = true;
}
void CreateResources() {
    auto& state = State();
    DXGI_SWAP_CHAIN_DESC description{};
    Check(state.swap->GetDesc(&description), "Cannot inspect actual swapchain");
    Require(description.BufferCount >= 2 && description.BufferCount <= 16, "Unsupported overlay backbuffer count");
    CreateContext();
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = description.BufferCount;
    Check(state.device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(state.rtvHeap.put())), "Cannot create overlay RTV heap");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 1; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(state.device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(state.srvHeap.put())), "Cannot create overlay font descriptor heap");
    state.frames.resize(description.BufferCount);
    state.uploadFences.assign(description.BufferCount, 0);
    auto handle = state.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    UINT increment = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    state.format = description.BufferDesc.Format;
    for (UINT index = 0; index < description.BufferCount; ++index) {
        auto& frame = state.frames[index];
        Check(state.swap->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(frame.buffer.put())), "Cannot acquire overlay backbuffer");
        Check(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(frame.allocator.put())), "Cannot create overlay frame allocator");
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = state.format; view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        frame.rtv = handle;
        state.device->CreateRenderTargetView(frame.buffer.value, &view, handle);
        handle.ptr += increment;
    }
#ifdef SM2DEBUG_PREVIEW
    if (state.preview && !state.previewFaultInjected && std::wcsstr(GetCommandLineW(), L"--overlay-fault-resize-test")) {
        state.previewFaultInjected = true;
        throw std::runtime_error("Injected preview failure after acquiring backbuffers, before font/backend initialization");
    }
#endif
    Check(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, state.frames[0].allocator.value, nullptr,
        __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(state.list.put())), "Cannot create overlay command list");
    Check(state.list->Close(), "Cannot close initial overlay command list");
    Check(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(state.fence.put())), "Cannot create overlay frame fence");
    state.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(state.event != nullptr, "Cannot create overlay fence event");
    ImGui_ImplDX12_InitInfo info{};
    info.Device = state.device.value; info.CommandQueue = state.queue.value;
    info.NumFramesInFlight = static_cast<int>(description.BufferCount); info.RTVFormat = state.format; info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = &state; info.SrvDescriptorHeap = state.srvHeap.value;
    info.SrvDescriptorAllocFn = AllocateSrv; info.SrvDescriptorFreeFn = FreeSrv;
    Require(ImGui_ImplDX12_Init(&info), "Cannot initialize ImGui on actual D3D12 queue");
    state.dxBackend = true;
    Require(ImGui_ImplDX12_CreateDeviceObjects(), "Cannot create actual overlay font/pipeline resources");
    state.resources = true;
    Log("Overlay GPU resources ready on captured game queue; format=" + std::to_string(state.format) + " buffers=" + std::to_string(description.BufferCount));
}
#ifdef SM2DEBUG_PREVIEW
void CaptureGpuFrame(Frame& frame);
#endif

void Render(IDXGISwapChain* object, UINT flags) noexcept {
    auto& state = State();
    if (state.stopping.load() || insidePresent || (flags & DXGI_PRESENT_TEST)) return;
    std::unique_lock lock(state.mutex, std::try_to_lock);
    if (!lock || state.failed || state.stopping.load() || !state.swap.value || object != static_cast<IDXGISwapChain*>(state.swap.value)) return;
    try {
        // Toggle events must be consumed even when no overlay GPU work runs.
        const bool menuVisible = PumpMenuInput();
        ProcessScreenCapture(state.device.value,state.queue.value,state.swap.value,OutputDirectory(),menuVisible);
        if (!menuVisible) { ++state.hidden; return; }
        RECT client{};
        if (!GetClientRect(state.window, &client) || client.right <= client.left || client.bottom <= client.top) return;
        if (!state.resources) CreateResources();
        ImGui::SetCurrentContext(state.context);
        UINT index = state.swap->GetCurrentBackBufferIndex();
        Require(index < state.frames.size(), "Actual backbuffer index is outside overlay frame resources");
        auto& frame = state.frames[index];
        UINT64 completed = state.fence->GetCompletedValue();
        auto uploadIndex = static_cast<size_t>(state.sequence % state.uploadFences.size());
        // Track both the actual backbuffer and ImGui's rotating upload buffers.
        if (frame.fence > completed || state.uploadFences[uploadIndex] > completed) { ++state.busy; return; }
        Check(frame.allocator->Reset(), "Cannot reset completed overlay allocator");
        Check(state.list->Reset(frame.allocator.value, nullptr), "Cannot reset overlay command list");
        ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame(); RenderMenuFrame(); ImGui::Render();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource = frame.buffer.value;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        state.list->ResourceBarrier(1, &barrier);
        state.list->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[]{state.srvHeap.value}; state.list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), state.list.value);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        state.list->ResourceBarrier(1, &barrier);
        Check(state.list->Close(), "Cannot close overlay command list");
        ID3D12CommandList* lists[]{state.list.value}; state.queue->ExecuteCommandLists(1, lists);
        UINT64 fence = state.nextFence++;
        HRESULT signal = state.queue->Signal(state.fence.value, fence);
        if (FAILED(signal)) { state.untrackedSubmission = true; throw std::runtime_error("Overlay queue signal failed; resources retained until process exit"); }
        state.lastFence = frame.fence = state.uploadFences[uploadIndex] = fence;
        ++state.sequence; ++state.rendered;
        if (state.rendered == 1) Log("First in-game overlay frame submitted to actual swapchain.");
#ifdef SM2DEBUG_PREVIEW
        if (state.previewCaptureRequested) CaptureGpuFrame(frame);
#endif
    } catch (const std::exception& error) {
        state.failed = true;
        SetScreenCaptureAvailable(false,L"Display capture is unavailable because the renderer stopped");
        Log(std::string("Overlay rendering disabled: ") + error.what());
        // Leave resources resident if an unknown GPU submission is in flight.
    } catch (...) { state.failed = true; SetScreenCaptureAvailable(false,L"Display capture renderer failed"); Log("Overlay rendering disabled after unexpected exception."); }
}
HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* object, UINT sync, UINT flags) {
    auto original = FunctionAt<HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT)>(State().present);
    bool outer = !insidePresent;
    if (outer) Render(object, flags);
    bool previous = insidePresent; insidePresent = true;
    HRESULT result = original(object, sync, flags);
    insidePresent = previous;
    return result;
}
HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* object, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    auto original = FunctionAt<HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*)>(State().present1);
    if (!insidePresent) Render(object, flags);
    bool previous = insidePresent; insidePresent = true;
    HRESULT result = original(object, sync, flags, parameters);
    insidePresent = previous;
    return result;
}
template<class Operation> HRESULT Resize(IDXGISwapChain* object, Operation operation) {
    auto& state = State();
    if (insideResize) return operation();
    struct Guard { Guard() { insideResize = true; } ~Guard() { insideResize = false; } } guard;
    std::lock_guard lock(state.mutex);
    bool selected = state.swap.value && object == static_cast<IDXGISwapChain*>(state.swap.value);
    if (selected && !DrainScreenCapture(2000)) {
        Log("Resize deferred: screen capture still owns an in-flight backbuffer.");
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    // A failed initialization may already own GetBuffer references without
    // having reached resources=true. Those references must also be released.
    bool partialResources = state.resources || state.dxBackend || state.rtvHeap.value || state.srvHeap.value ||
        state.list.value || state.fence.value || state.event || !state.frames.empty();
    if (selected && partialResources) {
        if (!WaitGpu(2000)) { Log("Resize deferred: overlay GPU fence did not finish within 2000ms; resources remain alive."); return DXGI_ERROR_WAS_STILL_DRAWING; }
        ReleaseResources();
    }
    HRESULT result = operation();
    if (selected && SUCCEEDED(result)) {
        ++state.resizes;
        DXGI_SWAP_CHAIN_DESC description{};
        const bool ready=!state.failed&&!state.stopping.load()&&SUCCEEDED(object->GetDesc(&description))&&SupportsDisplayCaptureFormat(description.BufferDesc.Format);
        SetScreenCaptureAvailable(ready,L"Display capture requires a supported 8-bit display format");
        Log("Actual swapchain resize completed; overlay resources will be rebuilt when visible.");
    }
    return result;
}
HRESULT STDMETHODCALLTYPE ResizeBuffers(IDXGISwapChain* object, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    auto original = FunctionAt<HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT)>(State().resize);
    try { return Resize(object, [&] { return original(object, count, width, height, format, flags); }); }
    catch (...) { State().failed = true; SetScreenCaptureAvailable(false,L"Display capture resize failed"); Log("Overlay resize failed; C++ exception was contained."); return DXGI_ERROR_INVALID_CALL; }
}
HRESULT STDMETHODCALLTYPE ResizeBuffers1(IDXGISwapChain3* object, UINT count, UINT width, UINT height, DXGI_FORMAT format,
    UINT flags, const UINT* masks, IUnknown* const* queues) {
    auto original = FunctionAt<HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*)>(State().resize1);
    try {
        return Resize(object, [&] {
            HRESULT result = original(object, count, width, height, format, flags, masks, queues);
            auto& state = State();
            if (SUCCEEDED(result) && queues && state.swap.value == object) {
                DXGI_SWAP_CHAIN_DESC description{};
                bool sameQueue = SUCCEEDED(object->GetDesc(&description)) && description.BufferCount <= 16;
                UINT actualCount = count ? count : description.BufferCount;
                sameQueue = sameQueue && actualCount > 0 && actualCount <= 16;
                for (UINT index = 0; sameQueue && index < actualCount; ++index) {
                    Com<ID3D12CommandQueue> queue;
                    sameQueue = queues[index] && SUCCEEDED(queues[index]->QueryInterface(__uuidof(ID3D12CommandQueue),
                        reinterpret_cast<void**>(queue.put()))) && queue.value == state.queue.value;
                }
                if (!sameQueue) {
                    // The game's successful resize remains successful. Stop
                    // overlay submissions rather than use an unproven queue.
                    state.failed = true;
                    SetScreenCaptureAvailable(false,L"The display presentation queue changed");
                    Log("Overlay disabled: ResizeBuffers1 changed or split the actual presentation command queue.");
                }
            }
            return result;
        });
    }
    catch (...) { State().failed = true; SetScreenCaptureAvailable(false,L"Display capture resize failed"); Log("Overlay ResizeBuffers1 failed; C++ exception was contained."); return DXGI_ERROR_INVALID_CALL; }
}
void Capture(IUnknown* queueObject, IDXGISwapChain* object) noexcept {
    auto& state = State();
    if (state.stopping.load() || !queueObject || !object) return;
    std::lock_guard lock(state.mutex);
    try {
        if (state.swap.value) return; // One primary game swapchain per session.
        DXGI_SWAP_CHAIN_DESC description{};
        Check(object->GetDesc(&description), "Cannot inspect captured swapchain");
        wchar_t windowClass[128]{};
        if (!description.OutputWindow || !GetClassNameW(description.OutputWindow, windowClass, 128)) return;
        if (std::wcscmp(windowClass, state.preview ? L"SM2OverlayPreview" : L"GameNxApp") != 0) return;
        Com<ID3D12CommandQueue> queue;
        if (FAILED(queueObject->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(queue.put()))) || !queue.value) return;
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        auto queueTable = *reinterpret_cast<void***>(queue.value);
        VerifiedDevice(queueTable[0]); VerifiedDevice(queueTable[10]);
        Com<ID3D12Device> device;
        Check(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(device.put())), "Cannot acquire actual command queue device");
        VerifiedDevice((*reinterpret_cast<void***>(device.value))[0]);
        Com<IDXGISwapChain3> swap;
        Check(object->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(swap.put())), "Actual swapchain lacks required backbuffer index interface");
        auto table = *reinterpret_cast<void***>(swap.value);
        Require(table == *reinterpret_cast<void***>(object), "Swapchain interfaces do not share primary vtable");
        for (UINT slot : {0u, 8u, 13u, 22u, 39u}) VerifiedDxgi(table[slot]);
        state.present = table[8]; state.resize = table[13]; state.present1 = table[22]; state.resize1 = table[39];
        state.swapTable = table;
        PinCallbacks();
        Hook(table, 8, Address(&Present)); Hook(table, 13, Address(&ResizeBuffers));
        Hook(table, 22, Address(&Present1)); Hook(table, 39, Address(&ResizeBuffers1));
        state.swap = std::move(swap); state.queue = std::move(queue); state.device = std::move(device);
        state.window = description.OutputWindow;
        SetScreenCaptureAvailable(SupportsDisplayCaptureFormat(description.BufferDesc.Format),
            L"Display capture requires a supported 8-bit display format");
        Log(state.privateRuntime ?
            "Captured actual private DXVK swapchain and genuine D3D12 DIRECT queue; no window created." :
            "Captured actual native Windows DXGI swapchain and genuine D3D12 DIRECT queue; no window created.");
    } catch (const std::exception& error) { state.failed = true; SetScreenCaptureAvailable(false,L"Display attachment failed"); Log(std::string("Swapchain capture refused: ") + error.what()); RestoreHooks(); }
    catch (...) { state.failed = true; SetScreenCaptureAvailable(false,L"Display attachment failed"); Log("Swapchain capture refused after an unexpected exception."); RestoreHooks(); }
}
HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** output) {
    auto original = FunctionAt<HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**)>(State().factoryOld);
    HRESULT result = original(factory, device, description, output);
    if (SUCCEEDED(result) && output && *output) Capture(device, *output);
    return result;
}
HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* description,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrictToOutput, IDXGISwapChain1** output) {
    auto original = FunctionAt<HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**)>(State().factoryHwnd);
    HRESULT result = original(factory, device, window, description, fullscreen, restrictToOutput, output);
    if (SUCCEEDED(result) && output && *output) Capture(device, *output);
    return result;
}
fs::path GameRoot(HMODULE module) {
    fs::path directory = fs::path(ModulePath(module)).parent_path();
    for (unsigned i = 0; i < 5; ++i) {
        if (fs::is_regular_file(directory / L"Spider-Man2.exe")) return directory;
        directory = directory.parent_path();
    }
    throw std::runtime_error("Cannot locate game root");
}
} // namespace

bool InitializeOverlay(HMODULE module) {
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (state.initialized) return true;
    try {
        state.module = module; state.root = GameRoot(module);
        wchar_t systemDirectory[32768]{};
        const UINT systemLength = GetSystemDirectoryW(systemDirectory, 32768);
        Require(systemLength && systemLength < 32768, "Cannot identify the Windows system directory");
        state.systemDirectory = fs::path(std::wstring(systemDirectory, systemLength));
        const bool localDxgi = fs::exists(state.root / L"dxgi.dll");
        const bool localD3d12 = fs::exists(state.root / L"d3d12.dll");
        const bool localCore = fs::exists(state.root / L"d3d12core.dll");
        state.privateRuntime = localDxgi || localD3d12 || localCore;
        // A partial local stack must not silently mix Windows DXGI/D3D12 with
        // translation DLLs. Native mode leaves the game's Agility subfolder intact.
        Require(!state.privateRuntime ||
            (fs::is_regular_file(state.root / L"dxgi.dll") &&
             fs::is_regular_file(state.root / L"d3d12.dll") &&
             fs::is_regular_file(state.root / L"d3d12core.dll")),
            "Incomplete app-local graphics runtime; refusing mixed native/private modules");
        const auto dxgiPath = (state.privateRuntime ? state.root : state.systemDirectory) / L"dxgi.dll";
        state.dxgi = LoadLibraryExW(dxgiPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(state.dxgi && SamePath(ModulePath(state.dxgi), dxgiPath), "Selected DXGI module resolved to an unexpected path");
        auto create = Export<HRESULT (WINAPI*)(REFIID, void**)>(state.dxgi, "CreateDXGIFactory1");
        VerifiedDxgi(Address(create));
        Com<IDXGIFactory2> factory;
        Check(create(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())), "Cannot inspect selected DXGI factory");
        auto table = *reinterpret_cast<void***>(factory.value);
        for (UINT slot : {0u, 10u, 15u}) VerifiedDxgi(table[slot]);
        state.factoryTable = table; state.factoryOld = table[10]; state.factoryHwnd = table[15];
        state.slots.reserve(6); PinCallbacks();
        Hook(table, 10, Address(&CreateSwapChain)); Hook(table, 15, Address(&CreateSwapChainForHwnd));
        state.initialized = true;
        Log(state.privateRuntime ?
            "Private factory COM data hooks ready; waiting for actual game swapchain creation." :
            "Native Windows factory COM data hooks ready; waiting for actual game swapchain creation.");
        return true;
    } catch (const std::exception& error) { Log(std::string("Overlay initialization refused: ") + error.what()); RestoreHooks(); return false; }
    catch (...) { Log("Overlay initialization refused after an unexpected exception."); RestoreHooks(); return false; }
}
void ShutdownOverlay() {
    auto& state = State();
    state.stopping = true;
    std::lock_guard lock(state.mutex);
    SetScreenCaptureAvailable(false,L"Renderer is stopping");
    RestoreHooks();
    if (!DrainScreenCapture(2000)) { Log("Shutdown retains unfinished screen capture resources until process exit."); return; }
    if (!WaitGpu(2000)) { Log("Shutdown retains in-flight overlay resources and pinned callbacks until process exit."); return; }
    ReleaseResources();
    if (state.context) {
        ImGui::SetCurrentContext(state.context);
        if (state.platformBackend) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(state.context); state.context = nullptr; state.platformBackend = false;
    }
    state.swap.reset(); state.queue.reset(); state.device.reset();
    Log("Overlay hooks restored and completed GPU resources released.");
}

#ifdef SM2DEBUG_PREVIEW
namespace {
UINT previewWidth = 1280, previewHeight = 800;
bool previewResize = false;
bool VerifyDisplayCapture(const std::wstring& path,UINT width,UINT height) {
    std::ifstream input(fs::path(path),std::ios::binary);
    BITMAPFILEHEADER file{};BITMAPINFOHEADER header{};
    input.read(reinterpret_cast<char*>(&file),sizeof(file));
    input.read(reinterpret_cast<char*>(&header),sizeof(header));
    if(!input||file.bfType!=0x4d42||header.biWidth!=static_cast<LONG>(width)||
       std::abs(header.biHeight)!=static_cast<LONG>(height)||header.biCompression!=BI_RGB||
       (header.biBitCount!=24&&header.biBitCount!=32))return false;
    const auto stride=((width*header.biBitCount+31)/32)*4;
    std::vector<unsigned char> pixels(static_cast<size_t>(stride)*height);
    input.seekg(file.bfOffBits);input.read(reinterpret_cast<char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    if(!input)return false;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
        const auto* pixel=pixels.data()+static_cast<size_t>(y)*stride+x*(header.biBitCount/8);
        // Exact preview clear color; overlay text/dimming would change pixels.
        if(pixel[0]!=27||pixel[1]!=14||pixel[2]!=9)return false;
    }
    return true;
}
struct PreviewCanvas {
    Com<ID3D12DescriptorHeap> heap;
    Com<ID3D12GraphicsCommandList> list;
    Com<ID3D12Fence> fence;
    std::vector<Frame> frames;
    UINT64 next = 1, last = 0;
    HANDLE event = nullptr;
    bool untracked = false;
    bool Wait() {
        if (untracked) return false;
        if (!fence.value || fence->GetCompletedValue() >= last) return true;
        return SUCCEEDED(fence->SetEventOnCompletion(last, event)) && WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
    }
    void Release() {
        Require(Wait(), "Preview background GPU fence did not finish within 2000ms");
        frames.clear(); list.reset(); heap.reset(); fence.reset();
        if (event) { CloseHandle(event); event = nullptr; }
        next = 1; last = 0;
    }
    void Create(ID3D12Device* device, IDXGISwapChain3* swap) {
        DXGI_SWAP_CHAIN_DESC description{}; Check(swap->GetDesc(&description), "Cannot query preview backbuffers");
        frames.resize(description.BufferCount);
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{}; heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = description.BufferCount;
        Check(device->CreateDescriptorHeap(&heapDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(heap.put())), "Cannot create preview background RTV heap");
        auto handle = heap->GetCPUDescriptorHandleForHeapStart();
        UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < description.BufferCount; ++i) {
            auto& frame = frames[i]; frame.rtv = handle; handle.ptr += step;
            Check(swap->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(frame.buffer.put())), "Cannot acquire preview background buffer");
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(frame.allocator.put())), "Cannot create preview background allocator");
            device->CreateRenderTargetView(frame.buffer.value, nullptr, frame.rtv);
        }
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames[0].allocator.value, nullptr, __uuidof(ID3D12GraphicsCommandList),
            reinterpret_cast<void**>(list.put())), "Cannot create preview background list");
        Check(list->Close(), "Cannot close preview background list");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(fence.put())), "Cannot create preview background fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event, "Cannot create preview background event");
    }
    void Clear(ID3D12CommandQueue* queue, IDXGISwapChain3* swap) {
        auto& frame = frames.at(swap->GetCurrentBackBufferIndex());
        if (frame.fence > fence->GetCompletedValue()) Require(Wait(), "Preview background allocator remains busy");
        Check(frame.allocator->Reset(), "Cannot reset preview background allocator");
        Check(list->Reset(frame.allocator.value, nullptr), "Cannot reset preview background list");
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.buffer.value; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        const float color[]{0.035f, 0.055f, 0.105f, 1.0f};
        list->ClearRenderTargetView(frame.rtv, color, 0, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier);
        Check(list->Close(), "Cannot close preview background commands");
        ID3D12CommandList* lists[]{list.value}; queue->ExecuteCommandLists(1, lists);
        UINT64 value = next++;
        if (FAILED(queue->Signal(fence.value, value))) { untracked = true; throw std::runtime_error("Preview background submission could not be tracked"); }
        last = frame.fence = value;
    }
};
void CaptureGpuFrame(Frame& frame) {
    auto& state = State();
    struct Capture {
        Com<ID3D12Resource> buffer;
        Com<ID3D12CommandAllocator> allocator;
        Com<ID3D12GraphicsCommandList> list;
        Com<ID3D12Fence> fence;
        HANDLE event = nullptr;
        ~Capture() { if (event) CloseHandle(event); }
    };
    auto* capture = new Capture;
    bool submitted = false, complete = false;
    try {
        auto sourceDescription = frame.buffer->GetDesc();
        Require(sourceDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Unexpected preview readback pixel format");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0; UINT64 rowSize = 0, bytes = 0;
        state.device->GetCopyableFootprints(&sourceDescription, 0, 1, 0, &footprint, &rows, &rowSize, &bytes);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC description{}; description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = bytes; description.Height = 1; description.DepthOrArraySize = 1; description.MipLevels = 1;
        description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(state.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            __uuidof(ID3D12Resource), reinterpret_cast<void**>(capture->buffer.put())), "Cannot allocate actual GPU preview readback");
        Check(state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void**>(capture->allocator.put())), "Cannot create preview copy allocator");
        Check(state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, capture->allocator.value, nullptr,
            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(capture->list.put())), "Cannot create preview copy list");
        Check(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(capture->fence.put())), "Cannot create preview copy fence");
        capture->event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(capture->event, "Cannot create preview copy event");
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.buffer.value; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        capture->list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = frame.buffer.value; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = capture->buffer.value;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
        capture->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); capture->list->ResourceBarrier(1, &barrier);
        Check(capture->list->Close(), "Cannot close preview copy commands");
        ID3D12CommandList* lists[]{capture->list.value}; state.queue->ExecuteCommandLists(1, lists); submitted = true;
        Check(state.queue->Signal(capture->fence.value, 1), "Cannot track actual preview GPU copy");
        Check(capture->fence->SetEventOnCompletion(1, capture->event), "Cannot wait for actual preview GPU copy");
        Require(WaitForSingleObject(capture->event, 2000) == WAIT_OBJECT_0, "Actual preview GPU copy exceeded 2000ms");
        complete = true;
        unsigned char* mapped = nullptr; D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)};
        Check(capture->buffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Cannot map completed preview GPU readback");
        int width = static_cast<int>(sourceDescription.Width), height = static_cast<int>(sourceDescription.Height);
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        UINT green = 0, white = 0;
        for (int y = 0; y < height; ++y) {
            const auto* row = mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
            auto* output = pixels.data() + static_cast<size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                output[x * 4] = row[x * 4 + 2]; output[x * 4 + 1] = row[x * 4 + 1]; output[x * 4 + 2] = row[x * 4]; output[x * 4 + 3] = 255;
                if (row[x * 4 + 1] > 150 && row[x * 4 + 1] > row[x * 4] * 1.2 && row[x * 4 + 1] > row[x * 4 + 2] * 1.1) ++green;
                if (row[x * 4] > 170 && row[x * 4 + 1] > 170 && row[x * 4 + 2] > 170) ++white;
            }
        }
        D3D12_RANGE written{0, 0}; capture->buffer->Unmap(0, &written);
        BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
        BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(info);
        header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
        Require(state.previewCaptureStyle < state.previewCaptureCompleted.size(), "Invalid preview capture style");
        const auto output = fs::path(OutputDirectory()) / (state.previewCaptureStyle ? L"overlay-dev.bmp" : L"overlay-debug.bmp");
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        Require(bool(file), "Cannot create actual GPU preview image");
        file.write(reinterpret_cast<const char*>(&header), sizeof(header)); file.write(reinterpret_cast<const char*>(&info), sizeof(info));
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        Require(bool(file), "Cannot write actual GPU preview image");
        file.close();
        // Preserve the original fixture artifact as the Debug root capture.
        if (!state.previewCaptureStyle) fs::copy_file(output, fs::path(OutputDirectory()) / L"overlay-preview.bmp", fs::copy_options::overwrite_existing);
        const unsigned style = state.previewCaptureStyle;
        state.previewCapturedGreen[style] = green; state.previewCapturedWhite[style] = white;
        state.previewCapturedVertices[style] = static_cast<UINT>(ImGui::GetDrawData()->TotalVtxCount);
        state.previewCaptureCompleted[style] = true; state.previewCaptureRequested = false;
        delete capture;
    } catch (...) {
        if (submitted && !complete) { state.untrackedSubmission = true; Log("Preview copy resources retained after an incomplete GPU fence."); }
        else delete capture;
        throw;
    }
}
LRESULT CALLBACK PreviewProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CHAR && MenuVisible() && wparam <= 0xffff)
        QueueMenuText(static_cast<wchar_t>(wparam));
    if (message == WM_SIZE && wparam != SIZE_MINIMIZED) { previewWidth = LOWORD(lparam); previewHeight = HIWORD(lparam); previewResize = true; return 0; }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int RunOverlayPreview(HINSTANCE instance) {
    auto& state = State(); state.preview = true;
    HWND window = nullptr;
    // Keep this context resident if a fault leaves GPU work in flight.
    PreviewCanvas* canvas = new PreviewCanvas;
    try {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = PreviewProc; windowClass.hInstance = instance;
        windowClass.lpszClassName = L"SM2OverlayPreview"; windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        Require(RegisterClassW(&windowClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Cannot register preview test window");
        window = CreateWindowExW(0, windowClass.lpszClassName, L"SM2 in-game D3D12 overlay preview - F10 / PS button", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(previewWidth), static_cast<int>(previewHeight), nullptr, nullptr, instance, nullptr);
        Require(window != nullptr, "Cannot create preview test window");
        Require(InitializeOverlay(instance), "Cannot initialize preview overlay capture");
        Com<IDXGIFactory2> factory;
        auto createFactory = Export<HRESULT (WINAPI*)(REFIID, void**)>(state.dxgi, "CreateDXGIFactory1");
        Check(createFactory(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put())), "Cannot create preview DXGI factory");
        if (state.privateRuntime) {
            const auto corePath = state.root / L"d3d12core.dll";
            HMODULE core = LoadLibraryExW(corePath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            Require(core && SamePath(ModulePath(core), corePath), "Cannot load exact preview private D3D12 core");
        }
        const auto d3d12Path = (state.privateRuntime ? state.root : state.systemDirectory) / L"d3d12.dll";
        HMODULE d3d12 = LoadLibraryExW(d3d12Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(d3d12 && SamePath(ModulePath(d3d12), d3d12Path), "Cannot load exact preview D3D12 backend");
        auto createDevice = Export<HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**)>(d3d12, "D3D12CreateDevice");
        Com<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapters1(0, adapter.put()), "No actual preview GPU adapter");
        Com<ID3D12Device> device;
        Check(createDevice(adapter.value, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(device.put())), "Cannot create real preview D3D12 device");
        D3D12_COMMAND_QUEUE_DESC queueDescription{}; queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Com<ID3D12CommandQueue> queue;
        Check(device->CreateCommandQueue(&queueDescription, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(queue.put())), "Cannot create preview DIRECT queue");
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = previewWidth; description.Height = previewHeight; description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1; description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2; description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Com<IDXGISwapChain1> initialSwap;
        Check(factory->CreateSwapChainForHwnd(queue.value, window, &description, nullptr, nullptr, initialSwap.put()), "Cannot create real preview swapchain");
        Com<IDXGISwapChain3> swap;
        Check(initialSwap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(swap.put())), "Preview swapchain interface unavailable");
        Require(state.swap.value && !state.failed, "Actual preview swapchain was not captured");
        initialSwap.reset();
        canvas->Create(device.value, swap.value);
        ShowWindow(window, SW_SHOW); UpdateWindow(window);
        QueueMenuInput(MenuInput::Toggle);
        bool faultTest = std::wcsstr(GetCommandLineW(), L"--overlay-fault-resize-test") != nullptr;
        bool smoke = faultTest || std::wcsstr(GetCommandLineW(), L"--overlay-smoke-test") != nullptr;
        bool quit = false, changedSize = false, hiddenStable = true;
        bool faultResizeRecovered = false, devNavigationQueued = false;
        bool displayCaptureQueued = false, displayCaptureVerified = false, displayCaptureReopened = false;
        UINT displayCaptureWidth = 0, displayCaptureHeight = 0;
        UINT64 presents = 0, hiddenBaseline = 0;
        UINT visibleGreenPixels = 0;
        ULONGLONG started = GetTickCount64(), lastBackend = 0;
        previewResize = false;
        while (!quit && IsWindow(window)) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { quit = true; break; }
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (quit) break;
            // GPU fixtures use only their scripted input, so a physical PS
            // press during the user's work cannot change their visibility.
            if (!smoke) PollControllerInput(window);
            if (GetTickCount64() - lastBackend >= 100) { TickBackend(); lastBackend = GetTickCount64(); }
            if (previewResize && previewWidth && previewHeight) {
                canvas->Release();
                if (presents > 80) {
                    UINT masks[]{1, 1}; IUnknown* queues[]{queue.value, queue.value};
                    Check(swap->ResizeBuffers1(2, previewWidth, previewHeight, DXGI_FORMAT_UNKNOWN, 0, masks, queues), "Preview ResizeBuffers1 exercise failed");
                }
                else Check(swap->ResizeBuffers(0, previewWidth, previewHeight, DXGI_FORMAT_UNKNOWN, 0), "Preview resize exercise failed");
                canvas->Create(device.value, swap.value);
                if (faultTest && state.previewFaultInjected && !faultResizeRecovered) {
                    // Only the isolated preview re-enables rendering after an
                    // injected failure; production failures remain disabled.
                    faultResizeRecovered = state.frames.empty() && !state.resources;
                    Require(faultResizeRecovered, "Partial overlay backbuffer references survived resize");
                    state.failed = false;
                    SetScreenCaptureAvailable(SupportsDisplayCaptureFormat(description.Format),L"Unsupported fixture display format");
                }
                previewResize = false;
            }
            // The preview intentionally uses the exact production Present path.
            canvas->Clear(queue.value, swap.value);
            if (smoke && presents >= 189) {
                if (!MenuVisible()) QueueMenuInput(MenuInput::Toggle);
                if (state.previewCaptureCompleted[0] && !devNavigationQueued) {
                    // Exercise the production navigation model through the
                    // same public queue used by the shoulder-button input.
                    QueueMenuInput(MenuInput::NextPage);
                    devNavigationQueued = true;
                    state.previewCaptureStyle = 1;
                }
                state.previewCaptureRequested = !state.previewCaptureCompleted[state.previewCaptureStyle];
                if (presents == 189) Log("Preview GPU readback requested; rendered=" + std::to_string(state.rendered) + " hidden=" + std::to_string(state.hidden));
                Require(!state.failed, "Preview renderer failed before final GPU readback");
            }
            if (presents % 2) { DXGI_PRESENT_PARAMETERS parameters{}; Check(swap->Present1(1, 0, &parameters), "Actual preview Present1 failed"); }
            else Check(swap->Present(1, 0), "Actual preview Present failed");
            ++presents;
            if (faultTest && presents == 5) SetWindowPos(window, nullptr, 0, 0, 1150, 820, SWP_NOMOVE | SWP_NOZORDER);
            if (smoke && presents == 60) { SetWindowPos(window, nullptr, 0, 0, 1040, 760, SWP_NOMOVE | SWP_NOZORDER); changedSize = true; }
            if (smoke && presents == 90) SetWindowPos(window, nullptr, 0, 0, 1200, 850, SWP_NOMOVE | SWP_NOZORDER);
            if (smoke && presents == 110) { QueueMenuInput(MenuInput::Toggle); hiddenBaseline = state.rendered; }
            if (smoke && presents == 135) { hiddenStable = state.rendered == hiddenBaseline; QueueMenuInput(MenuInput::Toggle); }
            if (smoke && presents == 145) {
                DXGI_SWAP_CHAIN_DESC captureDescription{};
                Check(swap->GetDesc(&captureDescription),"Cannot inspect fixture capture dimensions");
                displayCaptureWidth=captureDescription.BufferDesc.Width;displayCaptureHeight=captureDescription.BufferDesc.Height;
                Require(ReadScreenCaptureState().format==CaptureImageFormat::Bmp,"Fixture capture format changed");
                displayCaptureQueued=RequestDisplayCapture();Require(displayCaptureQueued,"Cannot queue actual display capture");
                QueueMenuInput(MenuInput::Close);
            }
            // Exercise resize with a submitted capture retaining the backbuffer.
            if(smoke&&presents==146)SetWindowPos(window,nullptr,0,0,1190,840,SWP_NOMOVE|SWP_NOZORDER);
            if(smoke&&displayCaptureQueued&&!displayCaptureReopened&&ReadScreenCaptureState().completed==1){
                displayCaptureVerified=VerifyDisplayCapture(ReadScreenCaptureState().lastPath,displayCaptureWidth,displayCaptureHeight);
                Require(displayCaptureVerified,"Display capture pixels/dimensions differ from actual pre-overlay frame");
                QueueMenuInput(MenuInput::Toggle);displayCaptureReopened=true;
            }
            if (smoke && presents >= 190 && state.previewCaptureCompleted[0] && state.previewCaptureCompleted[1]) {
                visibleGreenPixels = state.previewCapturedGreen[0]; quit = true;
            }
            if (smoke && GetTickCount64() - started > 30000) throw std::runtime_error("Preview smoke test exceeded 30 seconds");
        }
        bool passed = state.rendered > 30 && state.resizes >= 2 && changedSize && hiddenStable &&
            state.previewCapturedGreen[0] == 0 && state.previewCapturedGreen[1] == 0 && !state.failed &&
            devNavigationQueued && state.previewCapturedWhite[0] > 100 && state.previewCapturedWhite[1] > 100 &&
            state.previewCapturedVertices[0] > 500 && state.previewCapturedVertices[1] > 500 &&
            displayCaptureQueued&&displayCaptureVerified&&displayCaptureReopened&&
            (!faultTest || (state.previewFaultInjected && faultResizeRecovered));
        if (smoke) {
            std::ofstream out(fs::path(OutputDirectory()) / L"overlay-preview-test.json", std::ios::binary | std::ios::trunc);
            out << "{\"passed\":" << (passed ? "true" : "false") << ",\"actual_d3d12_device\":true,\"actual_swapchain\":true"
                << ",\"graphics_backend\":\"" << (state.privateRuntime ? "private" : "native") << '\"'
                << ",\"actual_private_swapchain\":" << (state.privateRuntime ? "true" : "false")
                << ",\"actual_native_swapchain\":" << (state.privateRuntime ? "false" : "true")
                << ",\"rendered_overlay_frames\":" << state.rendered << ",\"present_calls\":" << presents << ",\"resizes\":" << state.resizes
                << ",\"hidden_frames_submit_no_gpu_work\":" << (hiddenStable ? "true" : "false")
                << ",\"captured_visible_green_pixels\":" << visibleGreenPixels
                << ",\"debug_root_gpu_capture\":" << (state.previewCaptureCompleted[0] ? "true" : "false")
                << ",\"dev_root_gpu_capture\":" << (state.previewCaptureCompleted[1] ? "true" : "false")
                << ",\"dev_root_navigation_queued\":" << (devNavigationQueued ? "true" : "false")
                << ",\"debug_white_pixels\":" << state.previewCapturedWhite[0] << ",\"dev_white_pixels\":" << state.previewCapturedWhite[1]
                << ",\"debug_vertices\":" << state.previewCapturedVertices[0] << ",\"dev_vertices\":" << state.previewCapturedVertices[1]
                << ",\"capture_source\":\"actual_D3D12_backbuffer_readback_before_Present\""
                << ",\"partial_resource_fault_injected\":" << (state.previewFaultInjected ? "true" : "false")
                << ",\"partial_resource_resize_recovered\":" << (faultResizeRecovered ? "true" : "false")
                << ",\"production_display_capture_verified\":" << (displayCaptureVerified ? "true" : "false")
                << ",\"capture_resize_released_backbuffer\":" << (displayCaptureVerified&&state.resizes>=3 ? "true" : "false")
                << ",\"Present_and_Present1_exercised\":true,\"ResizeBuffers_and_ResizeBuffers1_exercised\":true"
                << ",\"game_started\":false,\"gameplay_bindings_enabled\":false}\n";
        }
        canvas->Release();
        ShutdownControllerInput(); ShutdownOverlay();
        if (IsWindow(window)) DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, instance);
        return smoke && !passed ? 1 : 0;
    } catch (const std::exception& error) {
        Log(std::string("Preview failed: ") + error.what());
        ShutdownControllerInput(); ShutdownOverlay();
        if (window && IsWindow(window)) DestroyWindow(window);
        return 1;
    } catch (...) {
        Log("Preview failed after an unexpected exception.");
        ShutdownControllerInput(); ShutdownOverlay();
        if (window && IsWindow(window)) DestroyWindow(window);
        return 1;
    }
}
#else
int RunOverlayPreview(HINSTANCE) { return ERROR_NOT_SUPPORTED; }
#endif
} // namespace sm2debug
