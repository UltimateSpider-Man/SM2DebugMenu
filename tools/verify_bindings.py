"""Read-only signature checks against the actual retail PE; never loads the game.

Run: python SM2DebugMenu/tools/verify_bindings.py Spider-Man2.exe
Source of baseline signatures: https://github.com/hbgda/SM2ScriptTemplate
The installed switchcharacter/ragesettings DLLs independently contain the core
actor/component signatures. A unique match is not proof of a valid function:
the old Transform::SetPosition signature is explicitly rejected below.
"""

import argparse
import bisect
import hashlib
import json
import mmap
import re
import struct
from pathlib import Path
from supported_builds import CANONICAL_SHA256, is_supported_hash


EXPECTED_SHA256 = CANONICAL_SHA256  # Immutable provenance for original research.

BINDINGS = {
    "actor_lookup": {
        "signature": "44 8B 01 41 8B D0 C1 EA 14 81 E2 FF 07 00 00 74 ?? 41 81 E0 FF FF 0F 00 4B 8D 0C ?? 48 C1 E1 06 48 03 0D ?? ?? ?? ?? 44 3B 05 ?? ?? ?? ?? 73 ?? 0F B7 41 ?? 3B D0 74 ?? 33 C9 48 8B C1",
        "kind": "function",
        "prototype": "void* actor_lookup(const uint32_t* actor_handle)",
    },
    "hero_system_object": {
        "signature": "48 89 05 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? 48 89 35",
        "kind": "rip_reference",
        "displacement_offset": 3,
        "instruction_length": 7,
        "note": "Inline global object, not a pointer slot. Installed mods use *(object+0x150), then actor_lookup(entry+4) for the current hero.",
    },
    "component_registry_object": {
        "signature": "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 8B 4E ?? 48 8D 54 24 ?? 48 83 C1 58",
        "kind": "rip_reference",
        "displacement_offset": 3,
        "instruction_length": 7,
    },
    "component_info_lookup": {
        "signature": "E8 ?? ?? ?? ?? 49 8B 4E ?? 48 8D 54 24 ?? 48 83 C1 58",
        "kind": "relative_call",
        "displacement_offset": 1,
        "instruction_length": 5,
        "prototype": "void* lookup(void* registry, uint32_t component_name_crc)",
    },
    "actor_component_lookup_small": {
        "signature": "48 89 5C 24 ?? 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 0F 84 ?? ?? ?? ?? 66 83 79 ?? 00",
        "kind": "function",
        "prototype": "void* lookup(void* actor_plus_0x58, void* component_info)",
    },
    "actor_component_lookup_large": {
        "signature": "44 0F B7 49 ?? 4C 8B C2",
        "kind": "function",
        "prototype": "void* lookup(void* actor_plus_0x80, void* component_info)",
    },
    "character_switch_by_name_crc": {
        "signature": "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC 90 00 00 00 49 8B E9",
        "kind": "function",
        "prototype": "void switch_character(void* HeroCharacterManager, uint32_t name_crc, int mode_4, void* null)",
    },
    "heroes_do_not_take_damage_live_flag": {
        "signature": "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 39 0F B7 51 08 45 33 C0 8B 41 0C",
        "kind": "rip_reference",
        "displacement_offset": 6,
        "instruction_length": 11,
        "note": "Actual live byte read by Health::CanTakeDamage. The nearby DDL default instance is a different address. This setting applies to all hero actors.",
    },
    "transform_set_position": {
        "signature": "48 89 5C 24 08 57 48 83 EC 30 80 79 5F 00 48 8B FA C5 F8 29 74 24 20 48 8B D9 C5 F8 10 71 30 75 ?? 45 33 C0 48 8B D1 E8 ?? ?? ?? ?? 8B 07",
        "kind": "function",
        "prototype": "void set_position(void* transform, const float* xyz)",
        "note": "Verified argument reads at xyz+0/+4/+8, position writes at transform+0x30/+0x34/+0x38, w=1, and dirty-state updates. Source: ExtCan/SM64-SpiderMan2/package/resources/sm2mario/bindings.ini.",
    },
    "old_template_transform_set_position": {
        "signature": "0F 10 51 ?? 4C 8B C1",
        "kind": "function",
        "reject": "On the inspected retail build this matches inside a matrix function, not a Vector3 setter. Do not call it.",
    },
}


class PEImage:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("Not a PE image")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("Bad PE signature")
        self.machine, count = struct.unpack_from("<HH", data, pe + 4)
        optional = pe + 24
        if struct.unpack_from("<H", data, optional)[0] != 0x20B:
            raise ValueError("Expected PE32+")
        self.image_base = struct.unpack_from("<Q", data, optional + 24)[0]
        self.image_size = struct.unpack_from("<I", data, optional + 56)[0]
        self.sections = []
        section_table = optional + struct.unpack_from("<H", data, pe + 20)[0]
        for i in range(count):
            entry = section_table + i * 40
            name = data[entry:entry + 8].rstrip(b"\0").decode("ascii")
            virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, entry + 8)
            self.sections.append((name, rva, virtual_size, raw, raw_size))
        exception_rva, exception_size = struct.unpack_from("<II", data, optional + 112 + 3 * 8)
        self.functions = []
        if exception_rva:
            start = self.offset(exception_rva)
            for pos in range(start, start + exception_size, 12):
                begin, end, _ = struct.unpack_from("<III", data, pos)
                self.functions.append((begin, end))
        self.starts = [entry[0] for entry in self.functions]

    def offset(self, rva):
        for _, start, virtual_size, raw, raw_size in self.sections:
            if start <= rva < start + max(virtual_size, raw_size):
                if rva - start >= raw_size:
                    raise ValueError("RVA points into uninitialized data")
                return raw + rva - start
        raise ValueError(f"RVA outside sections: {rva:#x}")

    def enclosing_function(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.functions[i][0] <= rva < self.functions[i][1]:
            return self.functions[i]
        return None

    def matches(self, signature):
        pattern = b"".join(b"." if "?" in token else re.escape(bytes.fromhex(token))
                           for token in signature.split())
        result = []
        for name, rva, _, raw, raw_size in self.sections:
            if name != ".text":
                continue
            for match in re.finditer(pattern, self.data[raw:raw + raw_size], re.DOTALL):
                result.append(rva + match.start())
        return result


def verify(image, spec):
    matches = image.matches(spec["signature"])
    result = {"matches": [hex(value) for value in matches], "count": len(matches)}
    if len(matches) != 1:
        result["status"] = "missing-or-ambiguous"
        return result
    rva = matches[0]
    if spec["kind"] in ("rip_reference", "relative_call"):
        displacement = struct.unpack_from("<i", image.data,
                                          image.offset(rva) + spec["displacement_offset"])[0]
        rva += spec["instruction_length"] + displacement
    result["target_rva"] = hex(rva)
    result["target_va_at_preferred_base"] = hex(image.image_base + rva)
    function = image.enclosing_function(rva)
    if function:
        result["enclosing_function"] = [hex(value) for value in function]
    callable_target = spec["kind"] in ("function", "relative_call")
    if spec.get("reject"):
        result["status"] = "rejected"
        result["reason"] = spec["reject"]
    elif callable_target and function and function[0] != rva:
        result["status"] = "rejected-interior-function-match"
    else:
        result["status"] = "static-match"
    if "prototype" in spec:
        result["prototype"] = spec["prototype"]
    if "note" in spec:
        result["note"] = spec["note"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", default="Spider-Man2.exe", type=Path)
    args = parser.parse_args()
    with args.executable.open("rb") as source:
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            image = PEImage(data)
            sha = hashlib.sha256(data).hexdigest()
            if not is_supported_hash(sha):
                print(json.dumps({
                    "path": str(args.executable.resolve()),
                    "sha256": sha,
                    "known_build": False,
                    "reason": "Known-build field/vtable checks are unavailable for this executable.",
                }, indent=2))
                return 1
            result = {
                "path": str(args.executable.resolve()),
                "sha256": sha,
                "known_build": is_supported_hash(sha),
                "canonical_research_sha256": EXPECTED_SHA256,
                "machine": hex(image.machine),
                "image_size": hex(image.image_size),
                "bindings": {name: verify(image, spec) for name, spec in BINDINGS.items()},
                "known_build_health_layout": {
                    "max_health_offset": "0xa0",
                    "current_health_offset": "0xd0",
                    "health_vtable_rva": "0x530eff0",
                    "hero_health_vtable_rva": "0x5412a30",
                    "getter_slot": 13,
                    "setter_slot": 14,
                    "getter_rva": "0x43e020",
                    "health_setter_rva": "0x34703d0",
                    "hero_health_setter_rva": "0xee5580",
                    "setter_prototype": "void set_health(void* component, float value)",
                    "setter_behavior": "Clamp to [0,max_health], update current health, and publish state changes. HeroHealth adds a subclass clamp.",
                },
                "limits": [
                    "Static evidence only. No process was started and no runtime behavior was tested.",
                    "Health offsets are confirmed by the known build's RTTI-associated vtable getter/setters; validate finite, plausible values and component ownership before each call.",
                    "HeroHealth+0x230 bit0x80 is cleared by the installed RageSettings disable-invincibility option; restoring a saved bit must only occur on the same current component.",
                    "Invoke gameplay functions from the game thread and guard missing hero/components during loading and character changes.",
                ],
            }
            # Additional independent evidence: validate the virtual getter and
            # subclass setter targets against the actual known-build vtables.
            layout = result["known_build_health_layout"]
            tables = {}
            for label, rva in (("Health", 0x530EFF0), ("HeroHealth", 0x5412A30)):
                table = image.offset(rva)
                getter, setter = struct.unpack_from("<QQ", data, table + 13 * 8)
                tables[label] = {
                    "getter_rva": hex(getter - image.image_base),
                    "setter_rva": hex(setter - image.image_base),
                }
            layout["actual_vtable_targets"] = tables
            layout["getter_bytes_match"] = data[image.offset(0x43E020):image.offset(0x43E020) + 9] == bytes.fromhex("C5 FA 10 81 D0 00 00 00 C3")
            flags = []
            for name, instruction_rva in (
                ("HeroesDoNotTakeDamage", 0x34720A4),
                ("HeroesDoNotDie", 0x34720FA),
                ("EnemyBotsDoNotDie", 0x3472149),
                ("EnemyBotsDontTakeDamage", 0x3472054),
            ):
                instruction = image.offset(instruction_rva)
                raw = data[instruction:instruction + 7]
                target = instruction_rva + 7 + struct.unpack_from("<i", raw, 2)[0]
                flags.append({
                    "name": name,
                    "cmp_instruction_rva": hex(instruction_rva),
                    "cmp_bytes": raw.hex(" "),
                    "is_cmp_rip_byte_with_zero": raw[:2] == b"\x80\x3d" and raw[6] == 0,
                    "live_byte_rva": hex(target),
                })
            result["live_health_flag_read_evidence"] = flags
            print(json.dumps(result, indent=2))
            required = [value for name, value in result["bindings"].items()
                        if name != "old_template_transform_set_position"]
            health_ok = layout["getter_bytes_match"] and tables == {
                "Health": {"getter_rva": "0x43e020", "setter_rva": "0x34703d0"},
                "HeroHealth": {"getter_rva": "0x43e020", "setter_rva": "0xee5580"},
            }
            flags_ok = all(flag["is_cmp_rip_byte_with_zero"] for flag in flags)
            return 0 if all(value["status"] == "static-match" for value in required) and health_ok and flags_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
