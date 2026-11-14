"""Read-only static proof for the exact local executable and photo entry gate."""
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from datetime import datetime, timezone
from supported_builds import is_supported_hash

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parent
sys.path.insert(0, str(ROOT / "LTSC2019Compatibility/verification"))
from read_minidump import PE


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    executable = ROOT / "Spider-Man2.exe"
    source = PROJECT / "src/photo_mode_gate.cpp"
    source_text = source.read_text()
    executable_hash = digest(executable)
    if not is_supported_hash(executable_hash):
        raise ValueError("Unsupported executable")
    pe = PE(executable)
    checks = []
    def check(name, actual, expected):
        checks.append({"name": name, "actual": actual, "expected": expected, "passed": actual == expected})
    for rva, hexadecimal in re.findall(r'Bytes\(base,(0x[0-9a-f]+),"([0-9a-f]+)"\)', source_text):
        check("code_" + rva, pe.at(int(rva, 16), len(hexadecimal)//2).hex(), hexadecimal)
    for start, end, unwind in re.findall(r'Function\(base,nt,(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+)\)', source_text):
        check("pdata_" + start, list(pe.function(int(start,16))), [int(x,16) for x in (start,end,unwind)])
    check("derived_can_enter_vtable_slot", struct.unpack("<Q",pe.at(0x549a4d8,8))[0], pe.preferred_base+0x15a66b0)
    check("derived_rtti_locator_pointer", struct.unpack("<Q",pe.at(0x549a490,8))[0], pe.preferred_base+0x8e138b0)
    check("derived_rtti_locator", list(struct.unpack("<6I",pe.at(0x8e138b0,24))), [1,0,0,0xab4f4c0,0x8e138d8,0x8e138b0])
    check("derived_type_name", pe.at(0xab4f4d0,21).split(b"\0")[0].decode(), ".?AVPhotomodeSystem@@")
    callers = []
    for virtual_size,rva,raw_size,raw_offset in pe.sections:
        if rva != 0x1000:
            continue
        data = pe.data[raw_offset:raw_offset+raw_size]
        for opcode in (b"\xe8",b"\xe9"):
            offset = 0
            while True:
                offset = data.find(opcode,offset)
                if offset < 0:
                    break
                if offset+5 <= len(data) and rva+offset+5+struct.unpack_from("<i",data,offset+1)[0] == 0x359a640:
                    address = rva+offset
                    callers.append({"rva":address,"bytes":data[offset:offset+5].hex(),"function":list(pe.function(address))})
                offset += 1
    check("all_direct_native_open_callers", sorted(x["rva"] for x in callers), [0x380365,0x38542f,0xe9ebc9,0x15a6bd3,0x15aabb2])
    check("controller_route_success_branch",pe.at(0x15a6bd0,25).hex(),"488bcbe8683aff010fb6d884c0740ab9f0a5ce77e8e70f4a01")
    check("system_menu_route_success_branch",pe.at(0xe9ebc2,20).hex(),"488d0d97b9810ae872ba6f0284c00f85e8040000")
    report = {
        "schema":1,"generated_utc":datetime.now(timezone.utc).isoformat(),
        "exe_sha256":executable_hash,"source_sha256":digest(source),
        "header_sha256":digest(PROJECT/"src/photo_mode_gate.h"),
        "passed":all(item["passed"] for item in checks),"checks":checks,"native_open_callers":callers,
        "contract":{
            "object_rva":"0xb6ba560","singleton_pointer_rva":"0xc237f90",
            "vtable_rva":"0x549a498","hook_slot_rva":"0x549a4d8","original_rva":"0x15a66b0",
            "native_open_rva":"0x359a640",
            "open":"Virtual +0x40 false returns before either native state DWORD +0x34/+0x38 is written.",
            "wrapper":"Visible Debug or Dev refuses immediately; hidden overlay continues exact original on its native caller thread/TLS context.",
            "native_restrictions":"Original starts with shared3597790: initialized byte+0x3c must be nonzero and native lock byte+0x46 must be zero. Cinematic/UI native lock writes remain untouched.",
            "existing_photo":"No forced close or state rewrite of an already active/suspended photo session.",
            "active_observation":"ReadProcessMemory only; initialized singleton with exact derived vtable; state<=14. Active predicate is exact3599490: state outside10..12 and not14.",
            "lifecycle":"One process vtable pointer, owned CAS rollback, original page protection retained across failures, module pinned.",
            "limits":"No game launched or physical button tested by this static proof; previously active camera and arbitrary external code calling entry side-effects directly are outside this Open gate."
        }
    }
    destination=PROJECT/"verification/photo-gate-native-proof.json"
    destination.write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"passed":report["passed"],"checks":len(checks),"report":str(destination)}))
    if not report["passed"]:
        for item in checks:
            if not item["passed"]:
                print(json.dumps(item))
        raise SystemExit(1)


if __name__ == "__main__":
    main()
