// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "overlay.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace sm2debug::controller {
// The report parser and edge tracker are independent of a physical controller.
// Values describe only the menu controls, not the controller's complete state.
enum Button : std::uint32_t {
    Up = 1u << 0, Down = 1u << 1, Left = 1u << 2, Right = 1u << 3,
    Cross = 1u << 4, Circle = 1u << 5, L1 = 1u << 6, R1 = 1u << 7,
    PS = 1u << 8
};

bool IsSupportedProduct(std::uint16_t product);
// Preview windows are eligible only in a build explicitly marked as preview.
bool IsControllerWindowClass(const wchar_t* className);
bool ParseSonyReport(std::uint16_t product, const std::uint8_t* report,
                     std::size_t length, std::uint32_t& buttons);

struct InputBatch {
    std::array<MenuInput, 9> values{};
    std::size_t count = 0;
};

class ButtonTracker {
public:
    InputBatch Feed(std::uint32_t buttons, std::uint64_t nowMs,
                    bool focused, bool menuVisible);
    void LoseFocus();
    void Reset();
private:
    bool initialized_ = false;
    bool focused_ = false;
    bool toggled_ = false;
    std::uint32_t previous_ = 0;
    std::uint64_t lastToggle_ = 0;
    std::array<std::uint64_t, 4> nextRepeat_{};
};

struct Diagnostics {
    std::uint64_t enumerationRuns = 0;
    std::uint64_t sonyCandidates = 0;
    std::uint64_t openedDevices = 0;
    std::uint64_t reportsParsed = 0;
    std::uint64_t malformedReports = 0;
    std::uint64_t readErrors = 0;
    std::size_t activeDevices = 0;
    std::size_t retiringDevices = 0;
    std::size_t pendingReads = 0;
    std::array<std::uint16_t, 8> products{};
};

// Contains counts and product IDs only; no device paths or controller serials.
Diagnostics GetDiagnostics();
}
