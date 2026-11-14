// SPDX-License-Identifier: GPL-3.0-or-later
// Isolated CPU host: real production filters, emulated input API completions.
// No controller/game/GUI/input injection or real import-slot patching occurs.
#include <windows.h>
#include <hidsdi.h>
#include <xinput.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fixture {
bool visible = false, complete = false, asynchronous = false;
DWORD resultError = ERROR_IO_INCOMPLETE, overrideTransferred = 0;
std::array<bool, 256> held{};
std::vector<std::uint8_t> physical;
std::vector<UINT> releases;
std::uint16_t product = 0x0ce6;
RAWINPUT raw{};
std::vector<std::uint8_t> rawBytes;
void* patchSlot = reinterpret_cast<void*>(0x1111);
DWORD modelProtection = PAGE_READONLY;
unsigned failRestores = 0;
unsigned calls = 0, assertions = 0, cases = 0;
void Check(bool ok, const char* why) { ++assertions; if (!ok) throw std::runtime_error(why); }
bool Visible() { return visible; }
SHORT WINAPI Async(int key) { return key >= 0 && key < 256 && held[static_cast<unsigned>(key)] ? static_cast<SHORT>(0x8000) : 0; }
BOOLEAN WINAPI Attributes(HANDLE handle, PHIDD_ATTRIBUTES value) {
    if (handle != reinterpret_cast<HANDLE>(0x1234)) return FALSE;
    value->VendorID = 0x054c; value->ProductID = product; return TRUE;
}
UINT WINAPI RawInfo(HANDLE, UINT, LPVOID target, PUINT size) {
    if (!target || *size < sizeof(RID_DEVICE_INFO)) return static_cast<UINT>(-1);
    auto* info = static_cast<RID_DEVICE_INFO*>(target);
    info->dwType = RIM_TYPEHID; info->hid.dwVendorId = 0x054c; info->hid.dwProductId = product;
    return sizeof(RID_DEVICE_INFO);
}
LRESULT WINAPI Window(WNDPROC, HWND, UINT message, WPARAM, LPARAM) { releases.push_back(message); return 0; }
SIZE_T WINAPI Query(LPCVOID address, PMEMORY_BASIC_INFORMATION value, SIZE_T size) {
    if (address != &patchSlot) return ::VirtualQuery(address, value, size);
    *value = {}; value->BaseAddress = &patchSlot; value->AllocationBase = GetModuleHandleW(nullptr);
    value->RegionSize = sizeof(patchSlot); value->State = MEM_COMMIT; value->Type = MEM_IMAGE; value->Protect = modelProtection;
    return sizeof(*value);
}
BOOL WINAPI Protect(LPVOID address, SIZE_T bytes, DWORD desired, PDWORD old) {
    if (address != &patchSlot) return ::VirtualProtect(address, bytes, desired, old);
    *old = modelProtection;
    if (desired == PAGE_READONLY && failRestores) { --failRestores; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    modelProtection = desired; return TRUE;
}
}
#define GetAsyncKeyState fixture::Async
#define HidD_GetAttributes fixture::Attributes
#define GetRawInputDeviceInfoW fixture::RawInfo
#define CallWindowProcW fixture::Window
#define VirtualQuery fixture::Query
#define VirtualProtect fixture::Protect
#include "../src/game_input_capture.cpp"
#undef VirtualProtect
#undef VirtualQuery
#undef CallWindowProcW
#undef GetRawInputDeviceInfoW
#undef HidD_GetAttributes
#undef GetAsyncKeyState
#include "../src/controller_input.cpp"
namespace sm2debug { void QueueMenuInput(MenuInput) {} bool MenuVisible() { return fixture::visible; } }

namespace fixture {
namespace cap = sm2debug::input_capture;
std::uint32_t BluetoothCrc(const std::uint8_t* data, std::size_t bytes) {
    // Independent table implementation, including the documented HID A1 prefix.
    std::array<std::uint32_t, 256> table{};
    for (unsigned i = 0; i < table.size(); ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit) value = value & 1 ? (value >> 1) ^ 0xedb88320u : value >> 1;
        table[i] = value;
    }
    std::uint32_t value = 0xffffffffu;
    value = (value >> 8) ^ table[(value ^ 0xa1) & 255];
    for (std::size_t i = 0; i < bytes; ++i) value = (value >> 8) ^ table[(value ^ data[i]) & 255];
    return ~value;
}
std::vector<std::uint8_t> Packet(std::uint16_t pid, unsigned report, unsigned length) {
    std::vector<std::uint8_t> value(length, 0x35); value[0] = static_cast<std::uint8_t>(report);
    const bool ps4 = pid == 0x05c4 || pid == 0x09cc;
    const bool full5 = !ps4 && ((report == 1 && length == 64) || report == 0x31);
    const unsigned base = report == 0x11 ? 3 : report == 0x31 ? 2 : 1;
    if (report == 0x11) value[1] = 0x80;
    value[base] = 0; value[base + 1] = 255; value[base + 2] = 17; value[base + 3] = 240;
    const unsigned buttons = full5 ? base + 7 : base + 4;
    value[buttons] = 0xf0; value[buttons + 1] = 0xff; value[buttons + 2] = 0x03;
    if (report == 0x11 || report == 0x31) {
        const auto crc = BluetoothCrc(value.data(), length - 4); std::memcpy(value.data() + length - 4, &crc, 4);
    }
    return value;
}
BOOL WINAPI Read(HANDLE, LPVOID buffer, DWORD capacity, LPDWORD count, LPOVERLAPPED) {
    ++calls; if (capacity < physical.size()) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    std::memcpy(buffer, physical.data(), physical.size());
    if (asynchronous) { SetLastError(ERROR_IO_PENDING); return FALSE; }
    if (count) *count = static_cast<DWORD>(physical.size());
    SetLastError(321); return TRUE;
}
BOOL WINAPI Result(HANDLE, LPOVERLAPPED, LPDWORD count, BOOL) {
    ++calls; if (!complete) { SetLastError(resultError); return FALSE; }
    if (count) *count = overrideTransferred ? overrideTransferred : static_cast<DWORD>(physical.size());
    SetLastError(654); return TRUE;
}
BOOL WINAPI ResultEx(HANDLE h, LPOVERLAPPED op, LPDWORD count, DWORD, BOOL) { return Result(h, op, count, FALSE); }
UINT WINAPI Raw(HRAWINPUT, UINT command, LPVOID buffer, PUINT bytes, UINT) {
    ++calls;
    if (!rawBytes.empty()) {
        if (!buffer) { *bytes = static_cast<UINT>(rawBytes.size()); return 0; }
        std::memcpy(buffer, rawBytes.data(), rawBytes.size()); return static_cast<UINT>(rawBytes.size());
    }
    if (!buffer) { *bytes = sizeof(raw); return 0; }
    if (command == RID_HEADER) { std::memcpy(buffer, &raw.header, sizeof(raw.header)); return sizeof(raw.header); }
    std::memcpy(buffer, &raw, sizeof(raw)); SetLastError(987); return sizeof(raw);
}
SHORT WINAPI Key(int) { ++calls; SetLastError(345); return static_cast<SHORT>(0x8001); }
DWORD WINAPI XState(DWORD, XINPUT_STATE* state) {
    ++calls; state->dwPacketNumber = 456; state->Gamepad.wButtons = visible || held['A'] ? XINPUT_GAMEPAD_A : 0;
    state->Gamepad.sThumbLX = visible || held['A'] ? 30000 : 0; SetLastError(876); return ERROR_SUCCESS;
}
void Reset() {
    visible = complete = asynchronous = false; held.fill(false); releases.clear(); calls = 0;
    resultError = ERROR_IO_INCOMPLETE; overrideTransferred = 0; rawBytes.clear();
    cap::visible = &Visible; cap::priorCapture = false; cap::releaseKeys = false;
    for (auto& key : cap::blockedKeys) key = false;
    for (auto& drain : cap::xDraining) drain = false;
    cap::xPackets.fill({});
    cap::pending.fill({}); cap::gates.fill({}); cap::installed = true;
    cap::patchCount = 0; cap::patches.fill({});
    cap::sdkPatches.fill({}); cap::sdkInstalled = false;
    cap::readFile = &Read; cap::overlappedResult = &Result; cap::overlappedResultEx = &ResultEx;
    cap::rawInput = &Raw; cap::keyState = &Key; cap::xFunctions[0] = &XState;
}
void ProtocolCases() {
    for (auto pid : {0x05c4, 0x09cc, 0x0ce6, 0x0df2}) {
        const bool ps4 = pid == 0x05c4 || pid == 0x09cc;
        for (const auto shape : {std::pair<unsigned,unsigned>{1, 10}, {1, 64}, {1, 78}, {ps4 ? 0x11u : 0x31u, 78}}) {
            auto original = Packet(static_cast<std::uint16_t>(pid), shape.first, shape.second);
            auto game = original;
            Check(!cap::SonyControlsNeutral(static_cast<std::uint16_t>(pid), game.data(), game.size()), "Physical pressed packet unexpectedly neutral");
            Check(cap::NeutralizeSonyReport(static_cast<std::uint16_t>(pid), game.data(), game.size()), "Known Sony report rejected");
            Check(cap::SonyControlsNeutral(static_cast<std::uint16_t>(pid), game.data(), game.size()), "Neutralized report still contains controls");
            const unsigned base = shape.first == 0x11 ? 3 : shape.first == 0x31 ? 2 : 1;
            for (unsigned axis = 0; axis < 4; ++axis) Check(game[base + axis] == 128, "Stick not centered");
            std::uint32_t buttons = 0;
            Check(sm2debug::controller::ParseSonyReport(static_cast<std::uint16_t>(pid), game.data(), game.size(), buttons) && buttons == 0,
                  "Independent menu parser does not accept neutral report/CRC");
            Check(sm2debug::controller::ParseSonyReport(static_cast<std::uint16_t>(pid), original.data(), original.size(), buttons) && buttons != 0,
                  "Independent menu read lost physical controls");
            if (shape.first == 0x11 || shape.first == 0x31) {
                std::uint32_t actual = 0; std::memcpy(&actual, game.data() + game.size() - 4, 4);
                Check(actual == BluetoothCrc(game.data(), game.size() - 4), "Recomputed Bluetooth CRC wrong");
                auto broken = original; broken.at(77) ^= 1; const auto before = broken;
                Check(!cap::NeutralizeSonyReport(static_cast<std::uint16_t>(pid), broken.data(), broken.size()) && broken == before,
                    "Invalid CRC packet modified");
            }
            ++cases;
        }
    }
    auto invalid = Packet(0x0ce6, 1, 64); const auto before = invalid;
    Check(!cap::NeutralizeSonyReport(0xffff, invalid.data(), invalid.size()) && invalid == before, "Unknown product changed");
    Check(!cap::NeutralizeSonyReport(0x0ce6, invalid.data(), 9) && invalid == before, "Truncated packet changed");
    invalid[8] = 15; const auto badHat = invalid;
    Check(!cap::NeutralizeSonyReport(0x0ce6, invalid.data(), invalid.size()) && invalid == badHat, "Invalid hat changed"); ++cases;
}
void CompletionCases() {
    Reset(); physical = Packet(product, 1, 64); auto buffer = physical; DWORD count = 0; OVERLAPPED op{};
    visible = true; asynchronous = true;
    Check(!cap::Read(reinterpret_cast<HANDLE>(0x1234), buffer.data(), 64, &count, &op) && GetLastError() == ERROR_IO_PENDING,
          "Async status changed");
    Check(buffer == physical, "Pending I/O buffer changed before completion");
    Check(!cap::Result(reinterpret_cast<HANDLE>(0x1234), &op, &count, FALSE) && GetLastError() == ERROR_IO_INCOMPLETE,
          "Incomplete result changed"); Check(buffer == physical, "Incomplete buffer changed");
    complete = true;
    Check(cap::ResultEx(reinterpret_cast<HANDLE>(0x1234), &op, &count, 0, FALSE) && GetLastError() == 654 && count == 64,
          "Completion return/count/LastError changed");
    Check(cap::SonyControlsNeutral(product, buffer.data(), 64), "Completed async report leaked physical state"); ++cases;
    asynchronous = false; visible = false; buffer = physical;
    Check(cap::Read(reinterpret_cast<HANDLE>(0x1234), buffer.data(), 64, &count, nullptr) && GetLastError() == 321,
          "Synchronous result changed"); Check(cap::SonyControlsNeutral(product, buffer.data(), 64), "Closing button leaked before release");
    cap::NeutralizeSonyReport(product, physical.data(), 64); buffer = physical;
    cap::Read(reinterpret_cast<HANDLE>(0x1234), buffer.data(), 64, &count, nullptr);
    physical = Packet(product, 1, 64); buffer = physical;
    cap::Read(reinterpret_cast<HANDLE>(0x1234), buffer.data(), 64, &count, nullptr);
    Check(buffer == physical, "Physical controller did not resume after neutral/release"); ++cases;
    Reset(); visible = true; buffer = physical;
    cap::Read(reinterpret_cast<HANDLE>(0x4321), buffer.data(), 64, &count, nullptr);
    Check(buffer == physical, "Unrelated file read modified"); ++cases;
    Reset(); visible = true; asynchronous = true; buffer = physical;
    cap::Read(reinterpret_cast<HANDLE>(0x1234), buffer.data(), 64, &count, &op);
    resultError = ERROR_OPERATION_ABORTED;
    Check(!cap::Result(reinterpret_cast<HANDLE>(0x1234), &op, &count, FALSE) && GetLastError() == ERROR_OPERATION_ABORTED,
        "Cancellation status changed");
    Check(buffer == physical && std::none_of(cap::pending.begin(), cap::pending.end(), [](const auto& p) {return p.operation != nullptr;}),
        "Cancelled buffer mutated or retained"); ++cases;
    Reset(); visible = true; asynchronous = true; buffer = physical;
    cap::Read(reinterpret_cast<HANDLE>(0x1234), buffer.data(), 64, &count, &op); complete = true; overrideTransferred = 129;
    cap::Result(reinterpret_cast<HANDLE>(0x1234), &op, &count, FALSE);
    Check(buffer == physical, "Oversized transferred count caused buffer write"); ++cases;
}
std::vector<std::uint8_t> Burst(const std::vector<std::uint8_t>& report, unsigned records) {
    std::vector<std::uint8_t> result;
    for (unsigned i = 0; i < records; ++i) result.insert(result.end(), report.begin(), report.end());
    return result;
}
void CheckNeutralBurst(const std::vector<std::uint8_t>& buffer, unsigned width, unsigned records) {
    for (unsigned i = 0; i < records; ++i) {
        const auto* report = buffer.data() + i * width;
        std::uint32_t buttons = 0;
        Check(cap::SonyControlsNeutral(product, report, width), "SDK batch leaked a held control");
        Check(sm2debug::controller::ParseSonyReport(product, report, width, buttons) && buttons == 0,
              "SDK batch was not neutral to the independent menu parser");
        if (report[0] == 0x11 || report[0] == 0x31) {
            std::uint32_t actual = 0; std::memcpy(&actual, report + width - 4, 4);
            Check(actual == BluetoothCrc(report, width - 4), "SDK Bluetooth batch has an invalid per-record CRC");
        }
    }
}
void SdkCompletionCases() {
    const auto device = reinterpret_cast<HANDLE>(0x1234);
    // SDK reserves 32 records even when the completed read contains one.
    Reset(); product = 0x0ce6; visible = true; physical = Packet(product, 1, 64);
    std::vector<std::uint8_t> buffer(2048 + 16, 0xcd); DWORD count = 0;
    Check(cap::Read(device, buffer.data(), 2048, &count, nullptr) && count == 64 && GetLastError() == 321,
          "SDK short synchronous completion changed API result/count/error");
    Check(cap::SonyControlsNeutral(product, buffer.data(), 64),
          "SDK read capacity was mistaken for transferred report length");
    Check(std::all_of(buffer.begin() + 64, buffer.end(), [](auto value) { return value == 0xcd; }),
          "SDK short completion modified bytes beyond transferred count"); ++cases;

    for (const auto pid : {std::uint16_t(0x05c4), std::uint16_t(0x0ce6)}) {
        for (const bool bluetooth : {false, true}) {
            Reset(); product = pid; visible = true;
            const unsigned width = bluetooth ? 78 : 64;
            const unsigned reportId = bluetooth ? (pid == 0x05c4 ? 0x11 : 0x31) : 1;
            physical = Burst(Packet(product, reportId, width), 32);
            const auto original = physical;
            buffer.assign(physical.size() + 16, 0xcd);
            Check(cap::Read(device, buffer.data(), static_cast<DWORD>(physical.size()), &count, nullptr) &&
                  count == physical.size() && GetLastError() == 321, "SDK full batch changed API shape");
            CheckNeutralBurst(buffer, width, 32);
            Check(physical == original && std::all_of(buffer.begin() + count, buffer.end(), [](auto value) { return value == 0xcd; }),
                  "SDK batch changed original input or its output guard"); ++cases;
        }
    }
    // 2496 is divisible by both strides; 39 USB records exceed the SDK batch limit.
    Reset(); product = 0x0ce6; visible = true; physical = Burst(Packet(product, 1, 64), 39);
    buffer = physical; cap::Read(device, buffer.data(), 2496, &count, nullptr);
    Check(buffer == physical, "SDK batch over 32 records was reinterpreted with the wrong stride"); ++cases;

    for (const bool extended : {false, true}) {
        for (const unsigned records : {1u, 32u}) {
            Reset(); product = 0x0ce6; visible = asynchronous = true;
            physical = Burst(Packet(product, 1, 64), records);
            buffer.assign(2048 + 16, 0xcd); OVERLAPPED operation{}; count = 17;
            Check(!cap::Read(device, buffer.data(), 2048, &count, &operation) && GetLastError() == ERROR_IO_PENDING,
                  "SDK pending read changed API status");
            const auto pendingBuffer = buffer;
            Check(std::equal(physical.begin(), physical.end(), buffer.begin()), "SDK pending buffer was modified");
            Check(!cap::Result(device, &operation, &count, FALSE) && GetLastError() == ERROR_IO_INCOMPLETE && buffer == pendingBuffer,
                  "SDK incomplete completion modified input");
            complete = true;
            const BOOL ok = extended ? cap::ResultEx(device, &operation, &count, 0, FALSE) :
                                       cap::Result(device, &operation, &count, FALSE);
            Check(ok && count == records * 64 && GetLastError() == 654, "SDK completion changed result/count/error");
            CheckNeutralBurst(buffer, 64, records);
            Check(std::equal(buffer.begin() + count, buffer.end(), pendingBuffer.begin() + count),
                  "SDK completion wrote outside the transferred range"); ++cases;
        }
    }
    Reset(); product = 0x0ce6; visible = asynchronous = true; physical = Burst(Packet(product, 0x31, 78), 32);
    buffer = physical; OVERLAPPED operation{};
    cap::Read(device, buffer.data(), 2496, &count, &operation);
    Check(buffer == physical, "SDK Bluetooth pending buffer changed"); complete = true;
    Check(cap::ResultEx(device, &operation, &count, 0, FALSE) && count == 2496 && GetLastError() == 654,
          "SDK Bluetooth completion changed API shape");
    CheckNeutralBurst(buffer, 78, 32); ++cases;

    // All records must pass before the first record is changed.
    for (const unsigned malformed : {0u, 1u, 2u, 3u}) {
        Reset(); product = 0x0ce6; visible = true;
        physical = malformed == 2 ? Burst(Packet(product, 0x31, 78), 32) : Burst(Packet(product, 1, 64), 32);
        if (malformed == 0) physical[31 * 64 + 8] = 15;
        if (malformed == 1) physical[31 * 64] = 0x7f;
        if (malformed == 2) physical.back() ^= 1;
        if (malformed == 3) physical.pop_back();
        buffer = physical;
        cap::Read(device, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr);
        Check(buffer == physical, "SDK malformed batch was partially neutralized"); ++cases;
    }
    for (const DWORD transferred : {2049u, 2497u, 129u}) {
        Reset(); product = 0x0ce6; visible = asynchronous = true; physical = Burst(Packet(product, 1, 64), 2);
        buffer.assign(2048 + 16, 0xcd); operation = {};
        cap::Read(device, buffer.data(), 2048, &count, &operation); const auto before = buffer;
        complete = true; overrideTransferred = transferred;
        Check(cap::Result(device, &operation, &count, FALSE) && count == transferred && GetLastError() == 654,
              "SDK invalid transferred count changed API result");
        Check(buffer == before, "SDK invalid transferred length modified output"); ++cases;
    }
    Reset(); product = 0x0ce6; visible = true; physical = Burst(Packet(product, 1, 64), 2);
    buffer.assign(2497 + 16, 0xcd);
    cap::Read(device, buffer.data(), 2497, &count, nullptr);
    Check(std::equal(physical.begin(), physical.end(), buffer.begin()) &&
          std::all_of(buffer.begin() + physical.size(), buffer.end(), [](auto value) { return value == 0xcd; }),
          "SDK unsupported read capacity modified input"); ++cases;
}
void SdkDrainAndMenuCases() {
    const auto device = reinterpret_cast<HANDLE>(0x1234);
    Reset(); product = 0x0ce6; visible = true;
    const auto pressed = Packet(product, 1, 64);
    auto neutral = pressed; Check(cap::NeutralizeSonyReport(product, neutral.data(), 64), "SDK neutral fixture invalid");
    physical = Burst(pressed, 2); auto buffer = physical; DWORD count = 0;
    cap::Read(device, buffer.data(), 2048, &count, nullptr); CheckNeutralBurst(buffer, 64, 2);
    visible = false; buffer = physical;
    cap::Read(device, buffer.data(), 2048, &count, nullptr); CheckNeutralBurst(buffer, 64, 2);
    physical = Burst(neutral, 2); buffer = physical; cap::Read(device, buffer.data(), 2048, &count, nullptr);
    physical = Burst(pressed, 2); buffer = physical; cap::Read(device, buffer.data(), 2048, &count, nullptr);
    Check(buffer == physical, "SDK controls did not resume after a neutral batch"); ++cases;

    visible = true; buffer = physical; cap::Read(device, buffer.data(), 2048, &count, nullptr);
    visible = false; physical = pressed; physical.insert(physical.end(), neutral.begin(), neutral.end());
    physical.insert(physical.end(), pressed.begin(), pressed.end()); buffer = physical;
    cap::Read(device, buffer.data(), 2048, &count, nullptr);
    Check(cap::SonyControlsNeutral(product, buffer.data(), 64) &&
          std::equal(buffer.begin() + 64, buffer.end(), physical.begin() + 64),
          "SDK close drain did not preserve sequential release then fresh press"); ++cases;

    // A malformed later record cannot prematurely release the device gate.
    visible = true; physical = Burst(pressed, 2); buffer = physical; cap::Read(device, buffer.data(), 2048, &count, nullptr);
    visible = false; physical = Burst(neutral, 2); physical[64 + 8] = 15; buffer = physical;
    cap::Read(device, buffer.data(), 2048, &count, nullptr);
    Check(buffer == physical, "SDK malformed close batch changed");
    physical = Burst(pressed, 2); buffer = physical; cap::Read(device, buffer.data(), 2048, &count, nullptr);
    CheckNeutralBurst(buffer, 64, 2); ++cases;

    Reset(); product = 0x0ce6; visible = true; auto navigation = pressed;
    navigation[8] = 0x20; navigation[9] = navigation[10] = 0; // Up and Cross; PS released.
    physical = Burst(navigation, 2); const auto original = physical; buffer = physical;
    cap::Read(device, buffer.data(), 2048, &count, nullptr); CheckNeutralBurst(buffer, 64, 2);
    std::uint32_t buttons = 0;
    Check(physical == original && sm2debug::controller::ParseSonyReport(product, navigation.data(), 64, buttons) &&
          (buttons & sm2debug::controller::Cross) && (buttons & sm2debug::controller::Up),
          "SDK capture removed the independent menu input");
    sm2debug::controller::ButtonTracker tracker; tracker.Feed(0, 100, true, true);
    const auto actions = tracker.Feed(buttons, 101, true, true);
    Check(std::find(actions.values.begin(), actions.values.begin() + actions.count, sm2debug::MenuInput::Accept) != actions.values.begin() + actions.count &&
          std::find(actions.values.begin(), actions.values.begin() + actions.count, sm2debug::MenuInput::Up) != actions.values.begin() + actions.count,
          "SDK capture blocked original menu navigation"); ++cases;
}
void KeyboardMouseCases() {
    Reset(); visible = true; held['W'] = true;
    Check(cap::Key('W') == 0 && GetLastError() == 345, "Game keyboard polling leaked");
    Check(Async('W') < 0, "Menu keyboard polling changed");
    LRESULT result = 123;
    Check(cap::HandleWindowMessage(nullptr, WM_KEYDOWN, 'W', 0, reinterpret_cast<WNDPROC>(1), result) && result == 0,
          "Visible keyboard message not consumed");
    Check(!releases.empty() && releases[0] == WM_KEYUP, "Held key did not get game-thread release");
    Check(cap::HandleWindowMessage(nullptr, WM_MOUSEMOVE, 0, 0, nullptr, result), "Mouse motion not consumed");
    Check(!cap::HandleWindowMessage(nullptr, WM_INPUT, 0, 0, nullptr, result), "WM_INPUT cleanup path swallowed");
    visible = false; Check(cap::Key('W') == 0, "Held key leaked on close"); held['W'] = false;
    Check(cap::Key('W') == static_cast<SHORT>(0x8001), "Released key not restored to real API"); ++cases;
    Reset(); visible = true; raw = {}; raw.header.dwSize = sizeof(raw); raw.header.dwType = RIM_TYPEMOUSE;
    raw.data.mouse.lLastX = 123; raw.data.mouse.lLastY = -456; raw.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL;
    RAWINPUT filtered{}; UINT bytes = sizeof(filtered);
    Check(cap::Raw(nullptr, RID_INPUT, &filtered, &bytes, sizeof(RAWINPUTHEADER)) == sizeof(raw) && GetLastError() == 987,
          "Raw mouse API shape changed");
    Check(!filtered.data.mouse.lLastX && !filtered.data.mouse.lLastY && filtered.data.mouse.usButtonFlags == RI_MOUSE_LEFT_BUTTON_UP,
          "Mouse delta/buttons not neutral"); ++cases;
    visible = false; held[VK_LBUTTON] = true;
    cap::Raw(nullptr, RID_INPUT, &filtered, &bytes, sizeof(RAWINPUTHEADER));
    Check(!filtered.data.mouse.lLastX && !filtered.data.mouse.lLastY, "Held mouse drag leaked on close");
    held[VK_LBUTTON] = false;
    cap::Raw(nullptr, RID_INPUT, &filtered, &bytes, sizeof(RAWINPUTHEADER));
    Check(filtered.data.mouse.lLastX == 123 && filtered.data.mouse.lLastY == -456, "Mouse did not resume after release"); ++cases;
    visible = true;
    raw.header.dwType = RIM_TYPEKEYBOARD; raw.data.keyboard.VKey = 'A'; raw.data.keyboard.Message = WM_KEYDOWN;
    cap::Raw(nullptr, RID_INPUT, &filtered, &bytes, sizeof(RAWINPUTHEADER));
    Check((filtered.data.keyboard.Flags & RI_KEY_BREAK) && filtered.data.keyboard.Message == WM_KEYUP, "Raw keyboard not released"); ++cases;
    raw.header.dwSize = sizeof(RAWINPUTHEADER); const auto malformed = raw;
    cap::Raw(nullptr, RID_INPUT, &filtered, &bytes, sizeof(RAWINPUTHEADER));
    Check(std::memcmp(&filtered, &malformed, sizeof(raw)) == 0, "Malformed short raw packet mutated"); ++cases;
    Reset(); visible = true;
    rawBytes.resize(offsetof(RAWINPUT, data.hid.bRawData) + 128);
    auto* hid = reinterpret_cast<RAWINPUT*>(rawBytes.data()); hid->header.dwSize = static_cast<DWORD>(rawBytes.size());
    hid->header.dwType = RIM_TYPEHID; hid->header.hDevice = reinterpret_cast<HANDLE>(0x5678);
    hid->data.hid.dwSizeHid = 64; hid->data.hid.dwCount = 2;
    physical = Packet(product, 1, 64);
    std::memcpy(hid->data.hid.bRawData, physical.data(), 64); std::memcpy(hid->data.hid.bRawData + 64, physical.data(), 64);
    auto output = rawBytes; UINT capacity = static_cast<UINT>(output.size());
    cap::Raw(nullptr, RID_INPUT, output.data(), &capacity, sizeof(RAWINPUTHEADER));
    auto* gameHid = reinterpret_cast<RAWINPUT*>(output.data());
    Check(cap::SonyControlsNeutral(product, gameHid->data.hid.bRawData, 64) &&
        cap::SonyControlsNeutral(product, gameHid->data.hid.bRawData + 64, 64), "Multi-report raw HID packet leaked controls"); ++cases;
}
void PatchFaultCases() {
    Reset(); patchSlot = reinterpret_cast<void*>(0x1111); modelProtection = PAGE_READONLY; failRestores = 1;
    cap::Patch patch{&patchSlot, reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222)};
    Check(!cap::ApplyPatch(patch) && patch.applied && patch.protectionPending && patchSlot == reinterpret_cast<void*>(0x2222),
        "CAS-success/protection-failure ownership lost");
    Check(patch.originalProtection == PAGE_READONLY && modelProtection == PAGE_READWRITE, "Original RO protection lost");
    failRestores = 1;
    Check(!cap::RestorePatch(patch) && !patch.applied && patch.protectionPending && patchSlot == reinterpret_cast<void*>(0x1111),
        "Failed rollback did not retain protection obligation");
    cap::patches[0] = patch; cap::patchCount = 1;
    cap::Shutdown();
    Check(modelProtection == PAGE_READONLY && cap::patchCount == 0 && !cap::patches[0].protectionPending,
        "Shutdown retry failed to restore true original protection"); ++cases;
    Reset(); patchSlot = reinterpret_cast<void*>(0x3333); modelProtection = PAGE_READONLY;
    patch = {&patchSlot, reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222)};
    Check(!cap::ApplyPatch(patch) && !patch.applied && !patch.protectionPending && patchSlot == reinterpret_cast<void*>(0x3333) &&
        modelProtection == PAGE_READONLY, "Foreign slot overwritten or protection changed"); ++cases;
    patchSlot = reinterpret_cast<void*>(0x1111);
    Check(cap::ApplyPatch(patch) && patch.applied && !patch.protectionPending, "Clean patch failed");
    patchSlot = reinterpret_cast<void*>(0x3333);
    Check(cap::RestorePatch(patch) && patchSlot == reinterpret_cast<void*>(0x3333), "Rollback overwrote later foreign hook"); ++cases;
    Reset(); patchSlot = reinterpret_cast<void*>(0x1111); modelProtection = PAGE_READONLY;
    cap::sdkPatches[0] = {&patchSlot, reinterpret_cast<void*>(0x1111), reinterpret_cast<void*>(0x2222)};
    Check(cap::ApplyPatch(cap::sdkPatches[0]), "SDK restoration fixture patch failed");
    cap::sdkInstalled = true; cap::Shutdown();
    Check(patchSlot == reinterpret_cast<void*>(0x1111) && modelProtection == PAGE_READONLY &&
          !cap::sdkInstalled && !cap::sdkPatches[0].applied && !cap::sdkPatches[0].protectionPending,
          "SDK shutdown did not restore import ownership and protection"); ++cases;
}
void XInputCases() {
    Reset(); held['A'] = true; XINPUT_STATE state{}, cached{};
    bool hasCachedState = false;
    // The game ingests controls only when dwPacketNumber differs from its cache.
    const auto poll = [&] {
        Check(cap::XState<0>(0, &state) == ERROR_SUCCESS && GetLastError() == 876,
              "XInput connection/status changed");
        if (hasCachedState && cached.dwPacketNumber == state.dwPacketNumber) return false;
        cached = state; hasCachedState = true; return true;
    };
    Check(poll() && cached.Gamepad.wButtons == XINPUT_GAMEPAD_A && cached.Gamepad.sThumbLX == 30000,
          "XInput hidden controls did not reach game cache");
    visible = true;
    Check(poll() && !cached.Gamepad.wButtons && !cached.Gamepad.sThumbLX,
          "XInput opening did not invalidate cached held controls");
    Check(!poll(), "XInput stable captured state changed packet number"); ++cases;
    visible = false;
    Check(!poll() && !cached.Gamepad.wButtons, "XInput held button leaked on close");
    held['A'] = false;
    Check(!poll() && !cached.Gamepad.wButtons, "XInput neutral release changed neutral game cache");
    held['A'] = true;
    Check(poll() && cached.Gamepad.wButtons == XINPUT_GAMEPAD_A && cached.Gamepad.sThumbLX == 30000,
          "XInput resumed controls did not invalidate neutral game cache");
    Check(!poll(), "XInput unchanged resumed state changed packet number"); ++cases;
    Reset(); visible = true;
    Check(cap::XState<0>(0, &state) == ERROR_SUCCESS && GetLastError() == 876 && state.dwPacketNumber != 456 &&
          !state.Gamepad.wButtons && !state.Gamepad.sThumbLX,
          "XInput first captured read retained a possible pre-hook packet");
    const auto firstPacket = state.dwPacketNumber;
    cap::XState<0>(0, &state);
    Check(state.dwPacketNumber == firstPacket, "XInput first captured packet was not stable"); ++cases;
    Reset(); held['A'] = true;
    cap::XState<0>(0, &state); const auto userZeroPacket = state.dwPacketNumber;
    cap::XState<0>(1, &state); const auto userOnePacket = state.dwPacketNumber;
    visible = true; cap::XState<0>(0, &state);
    Check(state.dwPacketNumber != userZeroPacket && !state.Gamepad.wButtons,
          "XInput first user did not enter capture");
    visible = false; cap::XState<0>(1, &state);
    Check(state.dwPacketNumber == userOnePacket && state.Gamepad.wButtons == XINPUT_GAMEPAD_A,
          "XInput packet or release gate leaked between users");
    cap::XState<0>(0, &state);
    Check(!state.Gamepad.wButtons, "XInput other user cleared first user's release gate"); ++cases;
    Reset(); held['A'] = true;
    cap::XState<0>(0, &state); const auto connectedPacket = state.dwPacketNumber;
    const auto beforeFailure = state;
    cap::xFunctions[0] = +[](DWORD, XINPUT_STATE*) -> DWORD {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED); return ERROR_DEVICE_NOT_CONNECTED;
    };
    Check(cap::XState<0>(0, &state) == ERROR_DEVICE_NOT_CONNECTED && GetLastError() == ERROR_DEVICE_NOT_CONNECTED &&
          std::memcmp(&state, &beforeFailure, sizeof(state)) == 0,
          "XInput disconnected status/output changed");
    cap::xFunctions[0] = &XState;
    Check(cap::XState<0>(0, &state) == ERROR_SUCCESS && GetLastError() == 876 &&
          state.dwPacketNumber != connectedPacket && state.Gamepad.wButtons == XINPUT_GAMEPAD_A,
          "XInput reconnect did not invalidate cached controls"); ++cases;
    cap::xFunctions[0] = +[](DWORD, XINPUT_STATE* target) -> DWORD {
        if (!target) { SetLastError(ERROR_BAD_ARGUMENTS); return ERROR_BAD_ARGUMENTS; }
        *target = {}; target->dwPacketNumber = MAXDWORD; target->Gamepad.wButtons = XINPUT_GAMEPAD_B;
        SetLastError(987); return ERROR_SUCCESS;
    };
    visible = true;
    Check(cap::XState<0>(4, &state) == ERROR_SUCCESS && GetLastError() == 987 && state.dwPacketNumber == MAXDWORD &&
          state.Gamepad.wButtons == XINPUT_GAMEPAD_B, "XInput out-of-range user was modified");
    Check(cap::XState<0>(0, nullptr) == ERROR_BAD_ARGUMENTS && GetLastError() == ERROR_BAD_ARGUMENTS,
          "XInput invalid output status changed");
    Check(cap::XState<0>(2, &state) == ERROR_SUCCESS && GetLastError() == 987 && state.dwPacketNumber != MAXDWORD &&
          !state.Gamepad.wButtons, "XInput packet wrap failed to invalidate capture"); ++cases;
    cap::installed = false;
    Check(!cap::Initialize(GetModuleHandleW(nullptr), false, &Visible), "Unsupported process initialized");
    Check(!cap::Initialize(GetModuleHandleW(nullptr), true, &Visible), "Synthetic host incorrectly admitted as retail"); ++cases;
}
}
int main() {
    try {
        fixture::ProtocolCases(); fixture::CompletionCases(); fixture::SdkCompletionCases(); fixture::SdkDrainAndMenuCases();
        fixture::KeyboardMouseCases(); fixture::PatchFaultCases(); fixture::XInputCases();
        std::cout << "{\"passed\":true,\"cases\":" << fixture::cases << ",\"assertions\":" << fixture::assertions
            << ",\"production_imports_patched\":false,\"game_launched\":false,\"physical_input_used\":false}";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"passed\":false,\"error\":\"" << error.what() << "\"}"; return 1;
    }
}
