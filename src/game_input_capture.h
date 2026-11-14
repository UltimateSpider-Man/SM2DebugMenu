// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace sm2debug::input_capture {
struct Diagnostics {
    bool installed = false;
    unsigned patchedImports = 0;
    std::uint64_t sonyReports = 0, rawPackets = 0, xinputStates = 0;
    std::uint64_t keyboardQueries = 0, windowMessages = 0, unsupportedReports = 0;
    bool sonySdkInstalled = false;
};
// Patch the supported executable and verified Sony SDK input imports. Calling
// again handles delayed SDK loading. This DLL's independent menu reads keep
// their original APIs; no controller output or feature report is modified.
bool Initialize(HMODULE self, bool supported, bool (*menuVisible)());
void Shutdown();
Diagnostics Observe();
bool HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                         WNDPROC previous, LRESULT& result);

// Pure protocol boundary for fixtures. Unknown/truncated/invalid-CRC reports
// are refused without modification; transport/sequence/battery data survives.
bool NeutralizeSonyReport(std::uint16_t product, std::uint8_t* report, std::size_t bytes);
bool SonyControlsNeutral(std::uint16_t product, const std::uint8_t* report, std::size_t bytes);
}
