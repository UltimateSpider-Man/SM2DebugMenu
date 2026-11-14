// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller_input.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sm2debug::controller {
namespace {
constexpr std::uint16_t SonyVendor = 0x054c;
constexpr std::uint16_t DualShock4 = 0x05c4;
constexpr std::uint16_t DualShock4V2 = 0x09cc;
constexpr std::uint16_t DualSense = 0x0ce6;
constexpr std::uint16_t DualSenseEdge = 0x0df2;
constexpr std::uint64_t RepeatDelayMs = 350;
constexpr std::uint64_t RepeatIntervalMs = 110;
constexpr std::uint64_t ToggleDebounceMs = 250;

bool BluetoothCrcValid(const std::uint8_t* data, std::size_t length) {
    if (length != 78) return false;
    std::uint32_t crc = 0xffffffffu;
    const auto addByte = [&crc](std::uint8_t value) {
        crc ^= value;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    };
    addByte(0xa1); // Bluetooth HID input transaction header, excluded by HidD.
    for (std::size_t i = 0; i < length - 4; ++i) addByte(data[i]);
    const std::uint32_t supplied = static_cast<std::uint32_t>(data[74]) |
        (static_cast<std::uint32_t>(data[75]) << 8) |
        (static_cast<std::uint32_t>(data[76]) << 16) |
        (static_cast<std::uint32_t>(data[77]) << 24);
    return ~crc == supplied;
}
}

bool IsSupportedProduct(std::uint16_t product) {
    return product == DualShock4 || product == DualShock4V2 ||
           product == DualSense || product == DualSenseEdge;
}

bool IsControllerWindowClass(const wchar_t* className) {
    if (!className) return false;
    if (std::wcscmp(className, L"GameNxApp") == 0) return true;
#ifdef SM2DEBUG_PREVIEW
    if (std::wcscmp(className, L"SM2OverlayPreview") == 0) return true;
#endif
    return false;
}

bool ParseSonyReport(std::uint16_t product, const std::uint8_t* report,
                     std::size_t length, std::uint32_t& buttons) {
    buttons = 0;
    if (!IsSupportedProduct(product) || !report || length == 0) return false;
    // Layouts documented by the primary Sony Linux driver and SDL HID backends:
    // https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c
    // https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps4.c
    // https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c
    // Windows may pad a simple Bluetooth ID-1 packet from 10 to 78 bytes.
    std::size_t firstButton = 0;
    const bool ps4 = product == DualShock4 || product == DualShock4V2;
    if (ps4 && report[0] == 0x01 &&
        (length == 10 || length == 64 || length == 78)) {
        firstButton = 5;
    } else if (ps4 && report[0] == 0x11 && length == 78) {
        if ((report[1] & 0x80u) == 0 || !BluetoothCrcValid(report, length)) return false;
        firstButton = 7;
    } else if (!ps4 && report[0] == 0x01 && length == 64) {
        firstButton = 8;
    } else if (!ps4 && report[0] == 0x01 && (length == 10 || length == 78)) {
        firstButton = 5;
    } else if (!ps4 && report[0] == 0x31 && length == 78) {
        if (!BluetoothCrcValid(report, length)) return false;
        firstButton = 9;
    } else {
        return false;
    }
    const auto b0 = report[firstButton];
    const auto b1 = report[firstButton + 1];
    const auto b2 = report[firstButton + 2];
    const auto hat = b0 & 0x0fu;
    if (hat > 8) return false;
    if (hat == 0 || hat == 1 || hat == 7) buttons |= Up;
    if (hat == 3 || hat == 4 || hat == 5) buttons |= Down;
    if (hat == 5 || hat == 6 || hat == 7) buttons |= Left;
    if (hat == 1 || hat == 2 || hat == 3) buttons |= Right;
    if (b0 & 0x20u) buttons |= Cross;
    if (b0 & 0x40u) buttons |= Circle;
    if (b1 & 0x01u) buttons |= L1;
    if (b1 & 0x02u) buttons |= R1;
    if (b2 & 0x01u) buttons |= PS;
    return true;
}

void ButtonTracker::Reset() { *this = ButtonTracker{}; }

void ButtonTracker::LoseFocus() {
    focused_ = false;
    nextRepeat_.fill(0);
}

InputBatch ButtonTracker::Feed(std::uint32_t buttons, std::uint64_t nowMs,
                              bool focused, bool menuVisible) {
    InputBatch batch;
    const auto add = [&batch](MenuInput value) {
        if (batch.count < batch.values.size()) batch.values[batch.count++] = value;
    };
    if (!initialized_ || !focused || !focused_) {
        initialized_ = true;
        focused_ = focused;
        previous_ = buttons;
        nextRepeat_.fill(0);
        return batch;
    }
    const std::uint32_t rising = buttons & ~previous_;
    previous_ = buttons;
    if ((rising & PS) && (!toggled_ || nowMs - lastToggle_ >= ToggleDebounceMs)) {
        lastToggle_ = nowMs;
        toggled_ = true;
        nextRepeat_.fill(0);
        add(MenuInput::Toggle);
        return batch;
    }
    if (!menuVisible) {
        nextRepeat_.fill(0);
        return batch;
    }
    constexpr std::array<std::uint32_t, 4> directions{Up, Down, Left, Right};
    constexpr std::array<MenuInput, 4> inputs{
        MenuInput::Up, MenuInput::Down, MenuInput::Left, MenuInput::Right};
    for (std::size_t i = 0; i < directions.size(); ++i) {
        if ((buttons & directions[i]) == 0) {
            nextRepeat_[i] = 0;
        } else if (rising & directions[i]) {
            add(inputs[i]);
            nextRepeat_[i] = nowMs + RepeatDelayMs;
        } else if (nextRepeat_[i] != 0 && nowMs >= nextRepeat_[i]) {
            add(inputs[i]);
            // Do not catch up with a burst after a scheduling delay.
            nextRepeat_[i] = nowMs + RepeatIntervalMs;
        }
    }
    if (rising & Cross) add(MenuInput::Accept);
    if (rising & Circle) add(MenuInput::Back);
    if (rising & L1) add(MenuInput::PreviousPage);
    if (rising & R1) add(MenuInput::NextPage);
    return batch;
}

namespace {
constexpr std::size_t MaxDevices = 8;
constexpr std::size_t MaxRetiringDevices = 16;

struct HidDevice {
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    std::vector<std::uint8_t> buffer;
    std::uint16_t product = 0;
    bool pending = false;
    bool focusArmed = false;
    ButtonTracker tracker;
    ~HidDevice() {
        // Only destroyed after the overlapped operation has completed.
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (event) CloseHandle(event);
    }
};

struct ControllerManager {
    std::mutex mutex;
    std::atomic<bool> stopped{false};
    bool enumerated = false;
    std::uint64_t lastEnumeration = 0;
    std::vector<std::unique_ptr<HidDevice>> devices;
    std::vector<std::unique_ptr<HidDevice>> retiring;
    Diagnostics diagnostics;
};

ControllerManager& Manager() {
    // Intentionally process-lifetime storage: an uncooperative HID driver may
    // delay cancellation. Its kernel I/O must never outlive its buffer/event.
    // No completion callback runs code in this module, and no thread is created.
    static ControllerManager* manager = new ControllerManager;
    return *manager;
}

bool GameFocused(HWND window) {
    if (!window || GetForegroundWindow() != window) return false;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId())
        return false;
    wchar_t className[64]{};
    return GetClassNameW(window, className, 64) > 0 &&
           IsControllerWindowClass(className);
}

bool ReadFinished(HidDevice& device) {
    if (!device.pending) return true;
    DWORD bytes = 0;
    if (GetOverlappedResult(device.handle, &device.overlapped, &bytes, FALSE) ||
        GetLastError() != ERROR_IO_INCOMPLETE) {
        device.pending = false;
        return true;
    }
    return false;
}

void Retire(ControllerManager& manager, std::unique_ptr<HidDevice> device) {
    device->tracker.Reset();
    if (device->pending) {
        CancelIoEx(device->handle, &device->overlapped);
        if (!ReadFinished(*device)) {
            manager.retiring.push_back(std::move(device));
            return;
        }
    }
    // A completed or cancelled operation can now release its event and buffer.
}

void ReapRetired(ControllerManager& manager) {
    auto& devices = manager.retiring;
    devices.erase(std::remove_if(devices.begin(), devices.end(),
        [](const auto& device) { return ReadFinished(*device); }), devices.end());
}

std::unique_ptr<HidDevice> OpenController(const std::wstring& path,
                                        Diagnostics& diagnostics) {
    auto device = std::make_unique<HidDevice>();
    device->path = path;
    device->handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    if (device->handle == INVALID_HANDLE_VALUE) return {};
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    if (!HidD_GetAttributes(device->handle, &attributes) ||
        attributes.VendorID != SonyVendor || !IsSupportedProduct(attributes.ProductID))
        return {};
    ++diagnostics.sonyCandidates;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(device->handle, &preparsed)) return {};
    HIDP_CAPS caps{};
    const auto status = HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
    if (status != HIDP_STATUS_SUCCESS || caps.UsagePage != 0x01 ||
        (caps.Usage != 0x04 && caps.Usage != 0x05) ||
        caps.InputReportByteLength < 10 || caps.InputReportByteLength > 128)
        return {};
    device->product = attributes.ProductID;
    device->buffer.resize(caps.InputReportByteLength);
    device->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!device->event) return {};
    device->overlapped.hEvent = device->event;
    ++diagnostics.openedDevices;
    return device;
}

void Enumerate(ControllerManager& manager, std::uint64_t nowMs) {
    if (manager.enumerated && nowMs - manager.lastEnumeration < 2000) return;
    manager.enumerated = true;
    manager.lastEnumeration = nowMs;
    ++manager.diagnostics.enumerationRuns;
    GUID guid{};
    HidD_GetHidGuid(&guid);
    const HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> present;
    bool complete = false;
    for (DWORD index = 0; index < 512; ++index) {
        SP_DEVICE_INTERFACE_DATA entry{};
        entry.cbSize = sizeof(entry);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, index, &entry)) {
            complete = GetLastError() == ERROR_NO_MORE_ITEMS;
            break;
        }
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &entry, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || required > 65536)
            continue;
        std::vector<std::uint8_t> storage(required);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &entry, detail, required, nullptr, nullptr))
            continue;
        const std::wstring path(detail->DevicePath);
        present.push_back(path);
        const bool known = std::any_of(manager.devices.begin(), manager.devices.end(),
            [&path](const auto& device) { return device->path == path; });
        const bool retiring = std::any_of(manager.retiring.begin(), manager.retiring.end(),
            [&path](const auto& device) { return device->path == path; });
        if (!known && !retiring && manager.devices.size() < MaxDevices &&
            manager.retiring.size() < MaxRetiringDevices) {
            auto device = OpenController(path, manager.diagnostics);
            if (device) manager.devices.push_back(std::move(device));
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    if (complete) {
        for (auto it = manager.devices.begin(); it != manager.devices.end();) {
            if (std::find(present.begin(), present.end(), (*it)->path) == present.end()) {
                auto removed = std::move(*it);
                it = manager.devices.erase(it);
                Retire(manager, std::move(removed));
            } else ++it;
        }
    }
}

// Processes only completed input reports; there is no WaitFor* or blocking read.
bool ServiceDevice(HidDevice& device, Diagnostics& diagnostics,
                   std::uint64_t nowMs, bool focused, bool visible,
                   std::vector<MenuInput>& actions) {
    if (!focused) {
        device.tracker.LoseFocus();
        device.focusArmed = false;
    }
    // On focus gain/connect, discard the buffered reports through the next
    // genuinely pending read. An old background PS edge cannot become a menu
    // toggle just because several queued reports finish in the same poll.
    const bool deliverFocused = focused && device.focusArmed;
    for (int drained = 0; drained < 16; ++drained) {
        DWORD bytes = 0;
        if (device.pending) {
            if (!GetOverlappedResult(device.handle, &device.overlapped, &bytes, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return true;
                device.pending = false;
                ++diagnostics.readErrors;
                return false;
            }
            device.pending = false;
        } else {
            ResetEvent(device.event);
            device.overlapped = {};
            device.overlapped.hEvent = device.event;
            if (!ReadFile(device.handle, device.buffer.data(),
                          static_cast<DWORD>(device.buffer.size()), &bytes,
                          &device.overlapped)) {
                if (GetLastError() == ERROR_IO_PENDING) {
                    device.pending = true;
                    if (focused) device.focusArmed = true;
                    return true;
                }
                ++diagnostics.readErrors;
                return false;
            }
        }
        std::uint32_t buttons = 0;
        if (ParseSonyReport(device.product, device.buffer.data(), bytes, buttons)) {
            ++diagnostics.reportsParsed;
            const auto batch = device.tracker.Feed(buttons, nowMs, deliverFocused, visible);
            for (std::size_t i = 0; i < batch.count; ++i)
                if (actions.size() < 64) actions.push_back(batch.values[i]);
        } else {
            ++diagnostics.malformedReports;
        }
    }
    return true;
}
}

Diagnostics GetDiagnostics() {
    auto& manager = Manager();
    std::lock_guard<std::mutex> lock(manager.mutex);
    auto snapshot = manager.diagnostics;
    snapshot.activeDevices = manager.devices.size();
    snapshot.retiringDevices = manager.retiring.size();
    for (std::size_t i = 0; i < manager.devices.size(); ++i) {
        snapshot.products[i] = manager.devices[i]->product;
        if (manager.devices[i]->pending) ++snapshot.pendingReads;
    }
    for (const auto& device : manager.retiring)
        if (device->pending) ++snapshot.pendingReads;
    return snapshot;
}
}

namespace sm2debug {
void PollControllerInput(HWND gameWindow) {
    auto& manager = controller::Manager();
    if (manager.stopped.load()) return;
    // Read the menu model before taking the HID lock, so model/input locks never
    // nest. QueueMenuInput is likewise called only after releasing the HID lock.
    const bool visible = MenuVisible();
    const bool focused = controller::GameFocused(gameWindow);
    std::vector<MenuInput> actions;
    {
        std::lock_guard<std::mutex> lock(manager.mutex);
        if (manager.stopped.load()) return;
        const auto nowMs = GetTickCount64();
        controller::ReapRetired(manager);
        controller::Enumerate(manager, nowMs);
        for (auto it = manager.devices.begin(); it != manager.devices.end();) {
            if (!controller::ServiceDevice(**it, manager.diagnostics, nowMs,
                                           focused, visible, actions)) {
                auto removed = std::move(*it);
                it = manager.devices.erase(it);
                controller::Retire(manager, std::move(removed));
            } else ++it;
        }
    }
    if (manager.stopped.load() || !controller::GameFocused(gameWindow)) return;
    for (const auto action : actions) QueueMenuInput(action);
}

void ShutdownControllerInput() {
    auto& manager = controller::Manager();
    std::lock_guard<std::mutex> lock(manager.mutex);
    manager.stopped = true;
    for (auto& device : manager.devices)
        controller::Retire(manager, std::move(device));
    manager.devices.clear();
    controller::ReapRetired(manager);
}
}
