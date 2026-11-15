// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cmath>

namespace sm2debug::flame_loop {
// This gate consumes validated native shot observations. It never generates
// shots or damage, and a held OS button alone cannot keep the loop alive.
struct Gate {
    static constexpr std::uint64_t GraceMs = 200;
    std::uint32_t frame = 0;
    double shotTime = 0;
    std::uint64_t observedAt = 0, firedAt = 0;
    bool primed = false, active = false;
    void Reset() noexcept { *this = {}; }
    bool Observe(bool permitted, std::uint32_t nextFrame, double nextTime,
                 std::uint64_t now) noexcept {
        const bool valid = std::isfinite(nextTime) && nextTime >= 0;
        const bool fresh = valid && primed && nextFrame != frame && nextTime > shotTime &&
            now >= observedAt && now - observedAt <= GraceMs;
        if (!permitted || !valid || now < observedAt) { firedAt = 0; active = false; }
        else {
            if (fresh) { firedAt = now; active = true; }
            if (!firedAt || now - firedAt > GraceMs) active = false;
        }
        frame = nextFrame; shotTime = valid ? nextTime : 0;
        observedAt = now; primed = valid;
        return active;
    }
};
} // namespace sm2debug::flame_loop
