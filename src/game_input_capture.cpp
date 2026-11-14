// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_input_capture.h"
#include <hidsdi.h>
#include <xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace sm2debug::input_capture {
namespace {
struct Layout { std::size_t state = 0, buttons = 0; bool ps5 = false, extended = false, bluetooth = false; };
bool Product(std::uint16_t value) { return value == 0x05c4 || value == 0x09cc || value == 0x0ce6 || value == 0x0df2; }
std::uint32_t Crc(const std::uint8_t* data, std::size_t bytes) {
    std::uint32_t crc = 0xffffffffu;
    const auto add = [&crc](std::uint8_t value) {
        crc ^= value;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    };
    add(0xa1);
    for (std::size_t i = 0; i < bytes; ++i) add(data[i]);
    return ~crc;
}
bool Describe(std::uint16_t product, const std::uint8_t* data, std::size_t bytes, Layout& out) {
    if (!Product(product) || !data || !bytes) return false;
    const bool ps4 = product == 0x05c4 || product == 0x09cc;
    if (ps4 && data[0] == 1 && (bytes == 10 || bytes == 64 || bytes == 78))
        out = {1, 5, false, bytes >= 64, false};
    else if (ps4 && data[0] == 0x11 && bytes == 78 && (data[1] & 0x80))
        out = {3, 7, false, true, true};
    else if (!ps4 && data[0] == 1 && bytes == 64)
        out = {1, 8, true, true, false};
    else if (!ps4 && data[0] == 1 && (bytes == 10 || bytes == 78))
        out = {1, 5, false, false, false};
    else if (!ps4 && data[0] == 0x31 && bytes == 78)
        out = {2, 9, true, true, true};
    else return false;
    if ((data[out.buttons] & 15) > 8) return false;
    if (out.bluetooth) {
        std::uint32_t supplied = 0;
        std::memcpy(&supplied, data + bytes - 4, 4);
        if (supplied != Crc(data, bytes - 4)) return false;
    }
    return true;
}
using ReadFn = decltype(&ReadFile);
using ResultFn = decltype(&GetOverlappedResult);
using ResultExFn = decltype(&GetOverlappedResultEx);
using ProcFn = decltype(&GetProcAddress);
using RawFn = decltype(&GetRawInputData);
using KeyFn = decltype(&GetKeyState);
using XStateFn = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
ReadFn readFile = &ReadFile;
ResultFn overlappedResult = &GetOverlappedResult;
ResultExFn overlappedResultEx = &GetOverlappedResultEx;
ProcFn getProc = &GetProcAddress;
RawFn rawInput = &GetRawInputData;
KeyFn keyState = &GetKeyState;
std::atomic<bool (*)()> visible{nullptr};
std::atomic<bool> installed{false}, priorCapture{false}, releaseKeys{false};
std::array<std::atomic<bool>, 256> blockedKeys{};
std::atomic<std::uint64_t> sonyCount{0}, rawCount{0}, xCount{0}, keyCount{0}, messageCount{0}, unknownCount{0};
struct Patch {
    void** slot = nullptr; void* original = nullptr; void* replacement = nullptr;
    DWORD originalProtection = 0;
    bool applied = false, protectionPending = false;
};
std::array<Patch, 8> patches{};
std::array<Patch, 3> sdkPatches{};
std::atomic<bool> sdkInstalled{false};
std::atomic<unsigned> patchCount{0};
std::mutex setupMutex, reportsMutex;
struct Pending { HANDLE handle = nullptr; OVERLAPPED* operation = nullptr; void* buffer = nullptr; DWORD capacity = 0; std::uint16_t product = 0; };
std::array<Pending, 128> pending{};
struct DeviceGate { HANDLE handle = nullptr; bool draining = false; };
std::array<DeviceGate, 64> gates{};
std::array<std::atomic<XStateFn>, 4> xFunctions{};
std::array<std::atomic<bool>, 4> xDraining{};
struct XPacketState {
    bool initialized = false, connected = false;
    DWORD packet = 0;
    XINPUT_GAMEPAD gamepad{};
};
std::array<XPacketState, 4> xPackets{};
std::mutex xPacketsMutex;

bool Capturing() {
    const auto callback = visible.load(std::memory_order_acquire);
    const bool capture = callback && callback();
    const bool old = priorCapture.exchange(capture);
    if (capture && !old) {
        for (unsigned i = 0; i < blockedKeys.size(); ++i)
            if (GetAsyncKeyState(static_cast<int>(i)) & 0x8000) blockedKeys[i] = true;
        releaseKeys = true;
    }
    return capture;
}
bool KeyBlocked(int key, bool capture) {
    if (key < 0 || key >= 256) return capture;
    if (capture) { blockedKeys[static_cast<unsigned>(key)] = true; return true; }
    if (!blockedKeys[static_cast<unsigned>(key)]) return false;
    if ((GetAsyncKeyState(key) & 0x8000) == 0) blockedKeys[static_cast<unsigned>(key)] = false;
    return blockedKeys[static_cast<unsigned>(key)].load();
}
bool MouseBlocked(bool capture) {
    bool blocked = capture;
    for (const auto key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
        blocked = KeyBlocked(key, capture) || blocked;
    return blocked;
}
std::size_t SonyReportStride(std::uint16_t product, const std::uint8_t* data, std::size_t bytes) {
    Layout layout;
    if (Describe(product, data, bytes, layout)) return bytes;
    // libScePad reads up to 32 USB (64-byte) or Bluetooth (78-byte)
    // reports in one completion. Validate the entire burst before changing it.
    if (!data || bytes > 32 * 78) return 0;
    for (const std::size_t stride : {64u, 78u}) {
        if (bytes % stride || bytes / stride < 2 || bytes / stride > 32) continue;
        bool valid = true;
        for (std::size_t offset = 0; offset < bytes; offset += stride)
            if (!Describe(product, data + offset, stride, layout)) { valid = false; break; }
        if (valid) return stride;
    }
    return 0;
}
void FilterSony(HANDLE device, std::uint16_t product, void* target, DWORD bytes) {
    if (!target || bytes < 10 || bytes > 32 * 78) return;
    const bool capture = Capturing();
    std::lock_guard lock(reportsMutex);
    DeviceGate* gate = nullptr;
    for (auto& entry : gates) if (entry.handle == device) { gate = &entry; break; }
    if (!gate) for (auto& entry : gates) if (!entry.handle) { entry.handle = device; gate = &entry; break; }
    if (!gate) { ++unknownCount; return; }
    auto* data = static_cast<std::uint8_t*>(target);
    if (capture) gate->draining = true;
    if (!capture && !gate->draining) return;
    const auto stride = SonyReportStride(product, data, bytes);
    if (!stride) { ++unknownCount; return; }
    for (std::size_t offset = 0; offset < bytes; offset += stride) {
        if (!capture && SonyControlsNeutral(product, data + offset, stride)) gate->draining = false;
        if (capture || gate->draining) {
            if (NeutralizeSonyReport(product, data + offset, stride)) ++sonyCount;
            else ++unknownCount;
        }
    }
}
std::uint16_t SonyHandle(HANDLE handle, DWORD capacity) {
    if (capacity < 10 || capacity > 32 * 78) return 0;
    HIDD_ATTRIBUTES attributes{}; attributes.Size = sizeof(attributes);
    return HidD_GetAttributes(handle, &attributes) && attributes.VendorID == 0x054c && Product(attributes.ProductID)
        ? attributes.ProductID : 0;
}
void Remember(const Pending& request) {
    std::lock_guard lock(reportsMutex);
    for (auto& entry : pending) if (entry.handle == request.handle && entry.operation == request.operation) { entry = request; return; }
    for (auto& entry : pending) if (!entry.operation) { entry = request; return; }
    ++unknownCount;
}
void Completed(HANDLE handle, OVERLAPPED* operation, DWORD count, bool success) {
    Pending request;
    {
        std::lock_guard lock(reportsMutex);
        for (auto& entry : pending) if (entry.handle == handle && entry.operation == operation) { request = entry; entry = {}; break; }
    }
    if (success && request.buffer && count <= request.capacity) FilterSony(handle, request.product, request.buffer, count);
}
BOOL WINAPI Read(HANDLE handle, LPVOID buffer, DWORD capacity, LPDWORD count, LPOVERLAPPED operation) {
    const DWORD before = GetLastError();
    const auto product = SonyHandle(handle, capacity);
    if (product && operation) Remember({handle, operation, buffer, capacity, product});
    SetLastError(before);
    const BOOL ok = readFile(handle, buffer, capacity, count, operation);
    const DWORD error = GetLastError();
    if (product && ok) {
        DWORD actual = count ? *count : 0;
        if (!count && operation) overlappedResult(handle, operation, &actual, FALSE);
        if (operation) Completed(handle, operation, actual, true);
        else if (count && actual <= capacity) FilterSony(handle, product, buffer, actual);
    } else if (product && operation && error != ERROR_IO_PENDING) Completed(handle, operation, 0, false);
    SetLastError(error); return ok;
}
BOOL WINAPI Result(HANDLE handle, LPOVERLAPPED operation, LPDWORD count, BOOL wait) {
    const BOOL ok = overlappedResult(handle, operation, count, wait); const DWORD error = GetLastError();
    if (ok) Completed(handle, operation, count ? *count : 0, true);
    else if (error != ERROR_IO_INCOMPLETE) Completed(handle, operation, 0, false);
    SetLastError(error); return ok;
}
BOOL WINAPI ResultEx(HANDLE handle, LPOVERLAPPED operation, LPDWORD count, DWORD timeout, BOOL alertable) {
    const BOOL ok = overlappedResultEx(handle, operation, count, timeout, alertable); const DWORD error = GetLastError();
    if (ok) Completed(handle, operation, count ? *count : 0, true);
    else if (error != ERROR_IO_INCOMPLETE && error != WAIT_TIMEOUT && error != WAIT_IO_COMPLETION) Completed(handle, operation, 0, false);
    SetLastError(error); return ok;
}
UINT WINAPI Raw(HRAWINPUT input, UINT command, LPVOID target, PUINT size, UINT headerSize) {
    const UINT count = rawInput(input, command, target, size, headerSize); const DWORD error = GetLastError();
    if (count == static_cast<UINT>(-1) || !target || command != RID_INPUT || count < sizeof(RAWINPUTHEADER) ||
        headerSize != sizeof(RAWINPUTHEADER)) { SetLastError(error); return count; }
    auto* raw = static_cast<RAWINPUT*>(target);
    const bool capture = Capturing();
    if (raw->header.dwSize > count || raw->header.dwSize < sizeof(RAWINPUTHEADER)) { SetLastError(error); return count; }
    const auto extent = raw->header.dwSize;
    if (raw->header.dwType == RIM_TYPEMOUSE && extent >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) && MouseBlocked(capture)) {
        auto& mouse = raw->data.mouse;
        const USHORT buttons = mouse.usButtonFlags;
        mouse = {}; mouse.usFlags = MOUSE_MOVE_RELATIVE;
        mouse.usButtonFlags = static_cast<USHORT>((buttons & RI_MOUSE_LEFT_BUTTON_DOWN ? RI_MOUSE_LEFT_BUTTON_UP : 0) |
            (buttons & RI_MOUSE_RIGHT_BUTTON_DOWN ? RI_MOUSE_RIGHT_BUTTON_UP : 0) |
            (buttons & RI_MOUSE_MIDDLE_BUTTON_DOWN ? RI_MOUSE_MIDDLE_BUTTON_UP : 0) |
            (buttons & RI_MOUSE_BUTTON_4_DOWN ? RI_MOUSE_BUTTON_4_UP : 0) |
            (buttons & RI_MOUSE_BUTTON_5_DOWN ? RI_MOUSE_BUTTON_5_UP : 0));
        ++rawCount;
    } else if (raw->header.dwType == RIM_TYPEKEYBOARD && extent >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD) &&
               KeyBlocked(raw->data.keyboard.VKey, capture)) {
        raw->data.keyboard.Flags |= RI_KEY_BREAK; raw->data.keyboard.Message = WM_KEYUP; ++rawCount;
    } else if (raw->header.dwType == RIM_TYPEHID && extent >= offsetof(RAWINPUT, data.hid.bRawData)) {
        RID_DEVICE_INFO info{}; info.cbSize = sizeof(info); UINT infoSize = sizeof(info);
        const auto& hid = raw->data.hid;
        const std::size_t available = extent - offsetof(RAWINPUT, data.hid.bRawData);
        if (hid.dwSizeHid >= 10 && hid.dwSizeHid <= 128 && hid.dwCount <= available / hid.dwSizeHid &&
            GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICEINFO, &info, &infoSize) != static_cast<UINT>(-1) &&
            info.dwType == RIM_TYPEHID && info.hid.dwVendorId == 0x054c && Product(static_cast<std::uint16_t>(info.hid.dwProductId))) {
            for (DWORD i = 0; i < hid.dwCount; ++i) FilterSony(raw->header.hDevice,
                static_cast<std::uint16_t>(info.hid.dwProductId), raw->data.hid.bRawData + i * hid.dwSizeHid, hid.dwSizeHid);
        }
    }
    SetLastError(error); return count;
}
SHORT WINAPI Key(int key) {
    const SHORT value = keyState(key); const DWORD error = GetLastError();
    const bool blocked = KeyBlocked(key, Capturing());
    if (blocked) ++keyCount;
    SetLastError(error); return blocked ? 0 : value;
}
template<unsigned Slot> DWORD WINAPI XState(DWORD user, XINPUT_STATE* state) {
    const auto original = xFunctions[Slot].load();
    if (!original) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD result = original(user, state); const DWORD error = GetLastError();
    if (result == ERROR_SUCCESS && state && user < 4) {
        const bool capture = Capturing();
        std::lock_guard lock(xPacketsMutex);
        auto& drain = xDraining[user];
        if (capture) drain = true;
        const auto& pad = state->Gamepad;
        const bool neutral = !pad.wButtons && pad.bLeftTrigger == 0 && pad.bRightTrigger == 0 &&
            std::abs(static_cast<int>(pad.sThumbLX)) <= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
            std::abs(static_cast<int>(pad.sThumbLY)) <= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
            std::abs(static_cast<int>(pad.sThumbRX)) <= XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE &&
            std::abs(static_cast<int>(pad.sThumbRY)) <= XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
        if (!capture && neutral) drain = false;
        if (capture || drain) { state->Gamepad = {}; ++xCount; }
        auto& emitted = xPackets[user];
        // The game ignores a payload whose packet number matches its cached state.
        // Track emitted controls, including capture/release, rather than the device packet.
        if (!emitted.initialized) {
            emitted.packet = state->dwPacketNumber;
            if (capture || drain) ++emitted.packet; // Also invalidate a pre-hook game cache.
        } else if (!emitted.connected || std::memcmp(&emitted.gamepad, &state->Gamepad, sizeof(state->Gamepad)) != 0) {
            ++emitted.packet;
        }
        emitted.initialized = emitted.connected = true;
        emitted.gamepad = state->Gamepad;
        state->dwPacketNumber = emitted.packet;
    } else if (result != ERROR_SUCCESS && user < 4) {
        std::lock_guard lock(xPacketsMutex);
        xPackets[user].connected = false;
    }
    SetLastError(error); return result;
}
bool XModule(HMODULE module) {
    wchar_t path[MAX_PATH]{}; if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    const auto* slash = std::wcsrchr(path, L'\\'); const auto* name = slash ? slash + 1 : path;
    return _wcsicmp(name, L"xinput1_4.dll") == 0 || _wcsicmp(name, L"xinput1_3.dll") == 0 ||
           _wcsicmp(name, L"xinput9_1_0.dll") == 0 || _wcsicmp(name, L"xinput1_2.dll") == 0 || _wcsicmp(name, L"xinput1_1.dll") == 0;
}
void* WrapXState(void* function) {
    HMODULE owner = nullptr;
    if (!function || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(function), &owner) || !XModule(owner) ||
            reinterpret_cast<void*>(getProc(owner, "XInputGetState")) != function) return function;
    const std::array<void*, 4> hooks{reinterpret_cast<void*>(&XState<0>), reinterpret_cast<void*>(&XState<1>),
        reinterpret_cast<void*>(&XState<2>), reinterpret_cast<void*>(&XState<3>)};
    const auto original = reinterpret_cast<XStateFn>(function);
    for (unsigned i = 0; i < hooks.size(); ++i) if (xFunctions[i].load() == original) return hooks[i];
    for (unsigned i = 0; i < hooks.size(); ++i) {
        XStateFn empty = nullptr;
        if (xFunctions[i].compare_exchange_strong(empty, original)) return hooks[i];
        if (empty == original) return hooks[i];
    }
    ++unknownCount; return function;
}
FARPROC WINAPI Proc(HMODULE module, LPCSTR name) {
    const auto result = getProc(module, name); const DWORD error = GetLastError();
    FARPROC replacement = result;
    if (reinterpret_cast<std::uintptr_t>(name) > 65535 && std::strcmp(name, "XInputGetState") == 0 && XModule(module))
        replacement = reinterpret_cast<FARPROC>(WrapXState(reinterpret_cast<void*>(result)));
    SetLastError(error); return replacement;
}
bool Patchable(void** slot) {
    MEMORY_BASIC_INFORMATION memory{};
    if (reinterpret_cast<std::uintptr_t>(slot) % sizeof(void*) ||
        VirtualQuery(slot, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT ||
        memory.Type != MEM_IMAGE || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        ((memory.Protect & 0xff) != PAGE_READONLY && (memory.Protect & 0xff) != PAGE_READWRITE &&
         (memory.Protect & 0xff) != PAGE_WRITECOPY)) return false;
    return true;
}
bool ApplyPatch(Patch& patch) {
    if (!Patchable(patch.slot) || patch.applied || patch.protectionPending) return false;
    DWORD old = 0;
    if (!VirtualProtect(patch.slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    // Keep the true original protection even if restoration fails. A retry
    // observing temporary RW must never redefine RW as the original state.
    patch.originalProtection = old; patch.protectionPending = true;
    patch.applied = InterlockedCompareExchangePointer(patch.slot, patch.replacement, patch.original) == patch.original;
    DWORD ignored = 0;
    if (VirtualProtect(patch.slot, sizeof(void*), old, &ignored)) patch.protectionPending = false;
    return patch.applied && !patch.protectionPending;
}
bool RestorePatch(Patch& patch) {
    if (!patch.applied && !patch.protectionPending) return true;
    if (!Patchable(patch.slot)) return false;
    DWORD temporary = 0;
    if (!VirtualProtect(patch.slot, sizeof(void*), PAGE_READWRITE, &temporary)) return false;
    const DWORD desiredProtection = patch.protectionPending ? patch.originalProtection : temporary;
    if (patch.applied) {
        // A foreign replacement owns the pointer now; do not overwrite it.
        InterlockedCompareExchangePointer(patch.slot, patch.original, patch.replacement);
        patch.applied = false;
    }
    patch.originalProtection = desiredProtection;
    patch.protectionPending = true;
    DWORD ignored = 0;
    if (VirtualProtect(patch.slot, sizeof(void*), desiredProtection, &ignored)) patch.protectionPending = false;
    return !patch.protectionPending;
}
bool RestoreAll() {
    bool complete = true;
    for (auto& patch : sdkPatches) if (!RestorePatch(patch)) complete = false;
    sdkInstalled = false;
    for (unsigned i = patchCount; i > 0; --i) if (!RestorePatch(patches[i - 1])) complete = false;
    if (complete) patchCount = 0;
    return complete;
}
bool SonySdkIdentity(HMODULE module) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{}; SIZE_T count = 0;
    const auto read = [&](std::uintptr_t address, void* out, std::size_t size) {
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, size, &count) && count == size;
    };
    if (!base || !read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0x40 || dos.e_lfanew > 0x1000 || !read(base + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.TimeDateStamp != 0x6985843d || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage != 0x8c000 ||
        reinterpret_cast<std::uintptr_t>(getProc(module, "scePadReadState")) != base + 0x14670) return false;
    // PSPC SDK S22 7.02.00.01: both native read loops use these three imports.
    struct Pin { std::uintptr_t rva; std::array<std::uint8_t, 6> bytes; };
    const std::array<Pin, 6> pins{{
        {0x4225, {0xff,0x15,0x3d,0x3e,0x02,0x00}}, {0x4268, {0xff,0x15,0x82,0x3e,0x02,0x00}},
        {0x42e8, {0xff,0x15,0xfa,0x3d,0x02,0x00}}, {0x6c41, {0xff,0x15,0x21,0x14,0x02,0x00}},
        {0x6c74, {0xff,0x15,0x76,0x14,0x02,0x00}}, {0x6cc1, {0xff,0x15,0x21,0x14,0x02,0x00}}}};
    for (const auto& pin : pins) {
        std::array<std::uint8_t, 6> actual{};
        if (!read(base + pin.rva, actual.data(), actual.size()) || actual != pin.bytes) return false;
    }
    return true;
}
bool InstallSonySdk() {
    if (sdkInstalled) return true;
    for (auto& patch : sdkPatches) if (!RestorePatch(patch)) return false;
    const auto module = GetModuleHandleW(L"libScePad.dll");
    if (!SonySdkIdentity(module)) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    struct Spec { std::uintptr_t rva; const char* name; void* replacement; };
    const std::array<Spec, 3> specs{{
        {0x28068, "ReadFile", reinterpret_cast<void*>(&Read)},
        {0x280e8, "GetOverlappedResult", reinterpret_cast<void*>(&Result)},
        {0x280f0, "GetOverlappedResultEx", reinterpret_cast<void*>(&ResultEx)}}};
    for (unsigned i = 0; i < specs.size(); ++i) {
        auto** slot = reinterpret_cast<void**>(base + specs[i].rva);
        const auto original = reinterpret_cast<void*>(getProc(GetModuleHandleW(L"kernel32.dll"), specs[i].name));
        if (!Patchable(slot) || !original || *slot != original) return false;
        sdkPatches[i] = {slot, original, specs[i].replacement};
    }
    // Keep owned import slots valid for shutdown/retry even if the game releases
    // its SDK reference. This does not patch the SDK on disk or system DLLs.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return false;
    for (auto& patch : sdkPatches) {
        if (!ApplyPatch(patch)) {
            for (auto& rollback : sdkPatches) RestorePatch(rollback);
            return false;
        }
    }
    sdkInstalled = true;
    return true;
}
}

bool SonyControlsNeutral(std::uint16_t product, const std::uint8_t* report, std::size_t bytes) {
    Layout layout; if (!Describe(product, report, bytes, layout)) return false;
    const auto* data = report + layout.state;
    for (unsigned i = 0; i < 4; ++i) if (data[i] < 120 || data[i] > 136) return false;
    if (layout.ps5) {
        if (data[4] || data[5] || data[7] != 8 || data[8] || data[9] || data[10]) return false;
        return (data[32] & 0x80) && (data[36] & 0x80);
    }
    if (data[4] != 8 || data[5] || (data[6] & 3) || data[7] || data[8]) return false;
    return !layout.extended || ((data[34] & 0x80) && (data[38] & 0x80));
}
bool NeutralizeSonyReport(std::uint16_t product, std::uint8_t* report, std::size_t bytes) {
    Layout layout; if (!Describe(product, report, bytes, layout)) return false;
    auto* data = report + layout.state;
    // Field offsets follow SDL's primary HIDAPI PS4/PS5 state packet structs:
    // github.com/libsdl-org/SDL/src/joystick/hidapi/SDL_hidapi_ps{4,5}.c
    std::fill(data, data + 4, 128);
    if (layout.ps5) {
        data[4] = data[5] = 0; data[7] = 8; data[8] = data[9] = data[10] = 0;
        std::fill(data + 15, data + 27, 0); // gyro and accelerometer inputs
        data[32] |= 0x80; data[36] |= 0x80; // both touch contacts released
    } else {
        data[4] = 8; data[5] = 0; data[6] &= 0xfc; data[7] = data[8] = 0;
        if (layout.extended) {
            std::fill(data + 12, data + 24, 0);
            data[34] |= 0x80; data[38] |= 0x80;
        }
    }
    if (layout.bluetooth) { const auto crc = Crc(report, bytes - 4); std::memcpy(report + bytes - 4, &crc, 4); }
    return true;
}

bool Initialize(HMODULE self, bool supported, bool (*menuVisible)()) {
    std::lock_guard lock(setupMutex);
    if (installed) { InstallSonySdk(); return true; }
    if (patchCount && !RestoreAll()) return false;
    if (!self || !supported || !menuVisible) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0x40 || dos->e_lfanew > 0x1000) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.TimeDateStamp != 0x679b5991 || nt->OptionalHeader.SizeOfImage != 0xce54000) return false;
    struct Spec { std::uintptr_t rva; const wchar_t* library; const char* name; void* replacement; };
    const std::array<Spec, 6> specs{{
        {0x529a5d0, L"kernel32.dll", "ReadFile", reinterpret_cast<void*>(&Read)},
        {0x529a728, L"kernel32.dll", "GetOverlappedResult", reinterpret_cast<void*>(&Result)},
        {0x529a730, L"kernel32.dll", "GetOverlappedResultEx", reinterpret_cast<void*>(&ResultEx)},
        {0x529a898, L"kernel32.dll", "GetProcAddress", reinterpret_cast<void*>(&Proc)},
        {0x529b080, L"user32.dll", "GetRawInputData", reinterpret_cast<void*>(&Raw)},
        {0x529b210, L"user32.dll", "GetKeyState", reinterpret_cast<void*>(&Key)}}};
    for (unsigned i = 0; i < specs.size(); ++i) {
        const auto& spec = specs[i];
        auto** slot = reinterpret_cast<void**>(base + spec.rva);
        const auto original = reinterpret_cast<void*>(getProc(GetModuleHandleW(spec.library), spec.name));
        if (!original || *slot != original) return false; // Do not replace another mod's hook.
        patches[i] = {slot, original, spec.replacement};
    }
    // Keep callbacks valid for the process lifetime even if a loader frees its
    // script reference. Only verified game/SDK imports are patched, never system DLL code.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(self), &pinned) || pinned != self) return false;
    visible = menuVisible;
    patchCount = static_cast<unsigned>(specs.size());
    for (unsigned i = 0; i < specs.size(); ++i) {
        if (!ApplyPatch(patches[i])) {
            RestoreAll(); // Retain any residual ownership/protection for retry.
            visible = nullptr; return false;
        }
    }
    // The actual native initializer3222050 caches XInputGetState at C1DD680.
    // Handle the already-resolved case as well as future GetProcAddress calls.
    auto** cached = reinterpret_cast<void**>(base + 0xc1dd680);
    void* original = *cached; void* replacement = WrapXState(original);
    if (original && replacement != original) {
        const auto index = patchCount.load();
        patches[index] = {cached, original, replacement};
        ++patchCount;
        if (!ApplyPatch(patches[index])) { visible = nullptr; RestoreAll(); return false; }
    }
    installed = true;
    InstallSonySdk();
    return true;
}

bool HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                         WNDPROC previous, LRESULT& result) {
    if (!installed) return false;
    const bool capture = Capturing();
    if (releaseKeys.exchange(false) && previous) {
        // Run only inside the original game window thread. Release keys already
        // held before opening so a cached W/mouse hold cannot keep moving. New
        // key presses are consumed until close and physical release.
        for (unsigned key = 8; key < blockedKeys.size(); ++key) if (blockedKeys[key])
            CallWindowProcW(previous, window, WM_KEYUP, key,
                static_cast<LPARAM>((MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16) | 0xc0000001u));
        const std::array<std::pair<int, UINT>, 5> mouse{{{VK_LBUTTON, WM_LBUTTONUP}, {VK_RBUTTON, WM_RBUTTONUP},
            {VK_MBUTTON, WM_MBUTTONUP}, {VK_XBUTTON1, WM_XBUTTONUP}, {VK_XBUTTON2, WM_XBUTTONUP}}};
        for (const auto& button : mouse) if (blockedKeys[static_cast<unsigned>(button.first)])
            CallWindowProcW(previous, window, button.second,
                button.first == VK_XBUTTON1 ? MAKEWPARAM(0, XBUTTON1) : button.first == VK_XBUTTON2 ? MAKEWPARAM(0, XBUTTON2) : 0, lParam);
    }
    bool blocked = false;
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) blocked = KeyBlocked(static_cast<int>(wParam), capture);
    else if (message == WM_CHAR || message == WM_SYSCHAR || message == WM_DEADCHAR || message == WM_SYSDEADCHAR)
        blocked = capture;
    else if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) blocked = MouseBlocked(capture);
    // WM_INPUT is deliberately forwarded: its main-executable GetRawInputData
    // hook supplies neutral/release states and Windows retains normal cleanup.
    if (blocked) { ++messageCount; result = 0; return true; }
    return false;
}
Diagnostics Observe() {
    return {installed.load(), patchCount, sonyCount.load(), rawCount.load(), xCount.load(),
        keyCount.load(), messageCount.load(), unknownCount.load(), sdkInstalled.load()};
}
void Shutdown() {
    std::lock_guard lock(setupMutex);
    visible = nullptr;
    RestoreAll();
    installed = false;
}
}
