// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "flame_slot_contract.h"

namespace sm2debug::mj_weapon_switch {
struct Identity {
    std::uint64_t itemAsset, weaponAsset;
    const char* name;
    std::uintptr_t weaponVtable;
};
inline constexpr Identity M2{flame_slot::ItemAsset,flame_slot::WeaponAsset,"MJFlamethrower",0x5323378};
inline constexpr Identity Sonic{0xb78058f8a2db4bd8ull,0x9928218d0d67f43eull,"SonicShotgun",0x54da178};
enum class Target { None, M2, Sonic };

inline bool Admits(const flame_slot::ItemContract& item,const Identity& identity) {
    return item.asset==identity.itemAsset && item.weaponAsset==identity.weaponAsset &&
        item.category==3 && item.quantity==1 && item.id!=0 && item.slotType==0 &&
        item.anyHeldSlot==0 && item.heldSlot==flame_slot::HeldSlot;
}
// Runtime item IDs come from the two verified owned items. The selected ID
// captured with the menu snapshot prevents a queued toggle changing its target.
inline Target Plan(const flame_slot::ItemContract& m2,const flame_slot::ItemContract& sonic,
                   const flame_slot::Slots& slots,std::uint16_t selected,std::uint16_t captured) {
    if (!Admits(m2,M2) || !Admits(sonic,Sonic) || m2.id==sonic.id || !captured ||
        selected!=captured || !flame_slot::Valid(slots,m2.id) || !flame_slot::Valid(slots,sonic.id) ||
        slots.selected!=flame_slot::HeldSlot || slots.items[flame_slot::HeldSlot]!=selected) return Target::None;
    return selected==m2.id ? Target::Sonic : selected==sonic.id ? Target::M2 : Target::None;
}
inline bool Preserved(const flame_slot::ItemContract& m2Before,const flame_slot::ItemContract& sonicBefore,
                      const flame_slot::ItemContract& m2After,const flame_slot::ItemContract& sonicAfter,
                      const flame_slot::Slots& before,const flame_slot::Slots& after,Target target) {
    if (!Admits(m2Before,M2) || !Admits(sonicBefore,Sonic) || !Admits(m2After,M2) || !Admits(sonicAfter,Sonic) ||
        m2Before.id!=m2After.id || sonicBefore.id!=sonicAfter.id || m2Before.id==sonicBefore.id) return false;
    const auto id=target==Target::M2 ? m2Before.id : target==Target::Sonic ? sonicBefore.id : 0;
    return id && flame_slot::Preserved(before,after,id);
}
} // namespace sm2debug::mj_weapon_switch
