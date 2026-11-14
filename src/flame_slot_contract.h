// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace sm2debug::flame_slot {
inline constexpr std::uint64_t ItemAsset = 0x929e50931c3da053ull;
inline constexpr std::uint64_t WeaponAsset = 0xaff67d91d723dd1dull;
inline constexpr int HeldSlot = 2; // Native kSlot3 is zero based.
// Native 14ad240 constructor, with occupied-slot replacement and selection
// enabled. The second word is the constructor's mode, not a held-slot index.
struct Options {
    std::int32_t slot = -1, mode = 5;
    std::uint8_t replaceOccupied = 1, selectCurrent = 1, reserved[2]{};
};
static_assert(sizeof(Options) == 12 && offsetof(Options,replaceOccupied) == 8 &&
              offsetof(Options,selectCurrent) == 9);
struct ItemContract {
    std::uint64_t asset = 0, weaponAsset = 0;
    std::uint32_t category = 0, quantity = 0;
    std::uint16_t id = 0;
    std::int32_t slotType = -1, heldSlot = -1;
    std::uint8_t anyHeldSlot = 1;
};
inline bool Admits(const ItemContract& item) {
    return item.asset == ItemAsset && item.weaponAsset == WeaponAsset &&
        item.category == 3 && item.quantity == 1 && item.id != 0 &&
        item.slotType == 0 && item.anyHeldSlot == 0 && item.heldSlot == HeldSlot;
}
struct Slots {
    std::array<std::uint16_t,11> items{};
    std::int32_t selected = -1;
};
inline bool Valid(const Slots& slots,std::uint16_t item) {
    if (!item || slots.selected < -1 || slots.selected >= static_cast<int>(slots.items.size())) return false;
    unsigned matches = 0;
    for (auto id:slots.items) if (id == item) ++matches;
    return matches <= 1;
}
inline bool Assigned(const Slots& slots,std::uint16_t item) {
    return Valid(slots,item) && slots.selected == HeldSlot && slots.items[HeldSlot] == item;
}
inline bool Preserved(const Slots& before,const Slots& after,std::uint16_t item) {
    if (!Valid(before,item) || !Assigned(after,item)) return false;
    for (std::size_t i=0;i<before.items.size();++i) {
        if (i == HeldSlot) continue;
        const auto expected = before.items[i] == item ? 0 : before.items[i];
        if (after.items[i] != expected) return false;
    }
    return true;
}
} // namespace sm2debug::flame_slot
