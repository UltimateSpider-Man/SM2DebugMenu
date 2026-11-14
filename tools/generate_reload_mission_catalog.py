# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate immutable Reload selection data from pinned retail evidence.

Reads the known PE/TOC and existing bounded extracts only. No archive extraction,
process access, native calls, inferred checkpoint ownership or gameplay writes.
Use --check to verify generated output, and --self-test for refusal regressions.
"""
import argparse
import collections
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys

import numpy as np
from supported_builds import CANONICAL_SHA256, checked_file_hash

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parent
EVIDENCE = PROJECT / "verification"
OUTPUT = PROJECT / "src/reload_mission_catalog.cpp"
CATALOG_SHA = "bc4a51372727bfdb5d53a765cb81b1a90525632e3fdf55cfc6f8d1b87da4891c"
NAMES_SHA = "2fbbcfcb92d954c7ccb2de0c61d23902b4c7e1dfb50a43f3dabb62aa601d0bea"
EXE_SHA = CANONICAL_SHA256  # Immutable provenance of the original asset research.
TOC_SHA = "e36e4d28b9f2594ba8ed3d52e1565d3a5a995120d65d38331b7d9791009255d3"
DECODER_SHA = "eef02b4642917bd95b8546580f346dd2a4d02222ff30299f6bfd1a5a010a8a6a"
NORMALIZER_SHA = "b26655b0d919e60e25d06247b2b5edf5a7c018449c963957bfe020f91ac69298"
DEV_MANIFEST_SHA = "82c05f07cf92d0789fbb50dc373c4c4ec73d6fd2c0a4a731f11d536012960675"


def require(condition, explanation):
    if not condition:
        raise ValueError(explanation)


def file_sha(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def pinned(path, expected):
    require(file_sha(path) == expected, "Evidence hash differs: " + str(path))
    return path


def unsigned(value, width, label):
    # IDs remain exact integers throughout; accepting JSON floats here could
    # silently round 64-bit graph and node identities.
    if type(value) is str:
        require(value.startswith("0x") or len(value) == width // 4,
                "Expected exact hexadecimal " + label)
        try:
            value = int(value, 16)
        except ValueError as error:
            raise ValueError("Invalid hexadecimal " + label) from error
    require(type(value) is int and 0 <= value < (1 << width), "Out-of-range exact integer: " + label)
    return value


def ascii_text(value, label, nonempty=True):
    require(type(value) is str and (bool(value) or not nonempty), "Invalid " + label)
    require(all(0x20 <= ord(char) < 0x7F for char in value), "Non-ASCII/control character in " + label)
    return value


def load_inputs():
    checked_file_hash(ROOT / "Spider-Man2.exe")
    pinned(ROOT / "toc", TOC_SHA)
    pinned(EVIDENCE / "dev_config_read.py", DECODER_SHA)
    pinned(EVIDENCE / "dev_normalize_menus.py", NORMALIZER_SHA)
    pinned(EVIDENCE / "dev_retail_menu_manifest.json", DEV_MANIFEST_SHA)
    catalog = json.loads(pinned(EVIDENCE / "reload-reference-and-data.json", CATALOG_SHA).read_text(encoding="utf-8"))
    names = json.loads(pinned(EVIDENCE / "reload-override-name-hashes.json", NAMES_SHA).read_text(encoding="utf-8"))
    require(catalog["schema"] == names["schema"] == 1, "Unsupported evidence schema")
    require(names["source_catalog_sha256"] == CATALOG_SHA, "Name hashes refer to another catalog")
    require(catalog["provenance"]["retail_exe_sha256"] == EXE_SHA, "Catalog executable provenance differs")
    require(catalog["provenance"]["toc_sha256"] == TOC_SHA, "Catalog TOC provenance differs")
    require(catalog["provenance"]["format_decoder_sha256"] == DECODER_SHA, "Catalog decoder provenance differs")
    require(names["serialized_string_headers_validated"] == 28218 and names["raw_assets_validated"] == 228,
            "Unexpected verified name-header baseline")
    sys.path.insert(0, str(EVIDENCE))
    from dev_config_read import ConfigReader
    from dev_normalize_menus import asset_id, crc32, normalize
    return catalog, names, ConfigReader, asset_id, crc32, normalize


class Toc:
    def __init__(self, asset_id, normalize):
        self.data = (ROOT / "toc").read_bytes()
        require(hashlib.sha256(self.data).hexdigest() == TOC_SHA, "TOC changed during generation")
        self.asset_id, self.normalize = asset_id, normalize
        count = struct.unpack_from("<H", self.data, 20)[0]
        self.sections = {tag: (offset + 8, length) for tag, offset, length in
                         (struct.unpack_from("<III", self.data, 24 + i * 12) for i in range(count))}
        offset, size = self.sections[0x506D7B8A]
        self.ids = np.frombuffer(self.data, "<u8", size // 8, offset)
        self.cache = {}

    def describe(self, path):
        ascii_text(path, "asset path")
        key = self.normalize(path)
        if key not in self.cache:
            ident = self.asset_id(path)
            rows = []
            for index in np.flatnonzero(self.ids == ident):
                size, archive, offset, header = struct.unpack_from("<IIIi", self.data, self.sections[0x65BCF461][0] + int(index) * 16)
                start = self.sections[0x398ABFF0][0] + archive * 66
                name = self.data[start:start + 40].split(b"\0", 1)[0].decode("ascii")
                rows.append({"toc_index": int(index), "archive": name, "offset": offset,
                             "size": size, "header_offset": header})
            self.cache[key] = {"asset_id": ident, "entries": rows}
        return self.cache[key]

    def validate(self, evidence, present=True):
        actual = self.describe(evidence["asset_path"])
        require(actual["asset_id"] == unsigned(evidence["asset_id"], 64, "asset ID"), "Asset ID does not hash from path")
        require(bool(actual["entries"]) == evidence["toc_present"], "TOC membership differs from evidence")
        require(actual["entries"] == evidence["entries"], "TOC archive/offset/size evidence differs")
        require(not present or bool(actual["entries"]), "Required asset absent from TOC")
        return actual["asset_id"]


class RawAssets:
    def __init__(self, decoder, crc32, toc):
        self.decoder, self.crc32, self.toc = decoder, crc32, toc
        self.cache = {}
        self.string_header_count = 0

    def get(self, source):
        self.toc.validate(source)
        path = Path(source["extracted_path"]).resolve()
        require(path.is_relative_to(EVIDENCE.resolve()), "Extract path is outside verification directory")
        if path not in self.cache:
            data = path.read_bytes()
            # The retained main mission graph is980464 bytes; objective graphs
            # are smaller. No newly extracted or unbounded archive data is read.
            require(len(data) == source["bytes"] and len(data) <= 1024 * 1024, "Extract size differs or exceeds budget")
            require(hashlib.sha256(data).hexdigest() == source["sha256"], "Extract hash differs")
            headers = collections.defaultdict(list)
            crc32 = self.crc32

            class Reader(self.decoder):
                def node(self, kind, depth):
                    offset = self.position
                    header = struct.unpack_from("<IIQ", self.data, offset) if kind == 10 else None
                    value = super().node(kind, depth)
                    if header is not None:
                        require((crc32(value) if value else 0) == header[1], "Serialized native name CRC mismatch")
                        headers[value].append({"offset": offset, "crc32": header[1], "crc64": header[2]})
                    return value

            reader = Reader(data)
            sections, order, classes = {}, {}, {}
            for tag, (offset, length) in reader.sections.items():
                if data[offset:offset + 8] != b"\0\0\0\0D\0\x15\x03":
                    continue
                reader.position = offset
                value = reader.object()
                require(reader.position == offset + length, "Serialized section extent mismatch")
                sections[tag] = value
                for i, node in enumerate(value.get("Nodes", [])):
                    key = node["BuiltNodeDataBufferNameHash"]
                    require(key not in order, "Duplicate built-node section identity")
                    order[key], classes[key] = i, node["FieldNameHash"]
            self.string_header_count += sum(len(values) for values in headers.values())
            self.cache[path] = {"headers": headers, "sections": sections, "order": order, "classes": classes,
                                "sourceSha": source["sha256"], "sourceBytes": len(data)}
        require(self.cache[path]["sourceSha"] == source["sha256"] and self.cache[path]["sourceBytes"] == source["bytes"],
                "Aliased extract references disagree about source identity")
        return self.cache[path]

    def name(self, asset, name, expected=None, header=None):
        ascii_text(name, "name", nonempty=False)
        if not name:
            require(expected in (None, 0), "Empty name cannot have a nonzero native ID")
            return {"name": "", "nameCrc": 0}
        values = asset["headers"].get(name, [])
        require(values, "Name lacks a typed serialized string header: " + name)
        calculated = self.crc32(name)
        require(all(value["crc32"] == calculated for value in values), "Inconsistent serialized name hashes")
        if expected is not None:
            require(calculated == unsigned(expected, 32, "name CRC"), "Catalog/native name CRC differs")
        if header is not None:
            require(any(value["offset"] == header["string_header_offset"] and value["crc32"] == unsigned(header["crc32"], 32, "header CRC")
                        and value["crc64"] == unsigned(header["crc64"], 64, "header CRC64") for value in values),
                    "Recorded typed string-header location differs")
        return {"name": name, "nameCrc": calculated}

    def node(self, asset, record, field):
        tag = unsigned(record["section_tag"], 32, "section tag")
        require(tag in asset["sections"] and tag in asset["order"], "Node section is missing from typed Nodes array")
        node = asset["sections"][tag]
        catalog_field = "mission_name" if field == "MissionName" else "objective_name"
        require(node.get(field) == record[catalog_field],
                "Catalog name does not match exact typed node")
        require(unsigned(record["node_id"], 64, "node ID") == unsigned(node["Id"], 64, "raw node ID"), "Node ID mismatch")
        order = unsigned(record["asset_node_order"], 32, "Nodes array index")
        require(order == asset["order"][tag], "Catalog asset Nodes index differs")
        require(unsigned(record["node_class_field_hash"], 32, "node class hash") == asset["classes"][tag], "Typed node class differs")
        return node


def unique(records, key, explanation):
    seen = set()
    for record in records:
        value = key(record)
        require(value not in seen, explanation)
        seen.add(value)


def build_model(catalog, names, toc, raw):
    unique(catalog["graphs"], lambda g: toc.normalize(g["asset_path"]), "Duplicate graph path")
    unique(catalog["graphs"], lambda g: unsigned(g["asset_id"], 64, "graph ID"), "Duplicate graph asset ID")
    unique(names["records"], lambda m: (m["mission_graph_asset_id"], m["mission_name"]), "Duplicate name-hash mission record")
    hashed = {(m["mission_graph_asset_id"], m["mission_name"]): m for m in names["records"]}
    loadout_evidence = {toc.normalize(v["asset_path"]): v for v in catalog["loadout_assets"]}
    for loadout in loadout_evidence.values():
        toc.validate(loadout)
    excluded, model = [], []

    def loadouts(record, node):
        actual = []
        for wrapper in node.get("ActivationOverrides", {}).get("ActivationOverrides", []):
            override = wrapper.get("ActivationOverride", {})
            if override.get("Type") == "ObjectiveActivationOverrideDebugLoadout":
                path = override.get("Obj", {}).get("DebugStartLoadout")
                require(type(path) is str, "Typed debug loadout is not a native string")
                if path:
                    actual.append(path)
        expected = [row["asset_path"] for row in record["debug_loadouts"]]
        require(actual == expected, "Debug loadout does not follow typed activation override")
        unique(expected, lambda value: toc.normalize(value), "Duplicate debug loadout path in one node")
        result = []
        for path in sorted(expected):
            key = toc.normalize(path)
            require(key in loadout_evidence, "Loadout lacks verified TOC evidence")
            result.append({"assetPath": path, "assetId": toc.validate(loadout_evidence[key])})
        return result

    for graph in sorted(catalog["graphs"], key=lambda value: ascii_text(value["asset_path"], "graph path")):
        graph_id = toc.validate(graph)
        graph_raw = raw.get(graph)
        unique(graph["missions"], lambda m: m["mission_name"], "Duplicate mission name in graph")
        missions = []
        for mission in sorted(graph["missions"], key=lambda value: value["mission_name"]):
            source_node = raw.node(graph_raw, mission, "MissionName")
            key = (graph["asset_id"], mission["mission_name"])
            require(key in hashed, "Mission lacks typed native hash evidence")
            hash_record = hashed[key]
            require(hash_record["mission_graph"] == graph["asset_path"], "Hash record belongs to another graph")
            named = raw.name(graph_raw, mission["mission_name"], hash_record["mission_crc32"], hash_record["mission_string_header"])
            dependency = mission.get("objective_graph")
            require(dependency is not None, "Mission has no objective graph evidence")
            require(source_node.get("ObjectiveGraph", {}).get("AssetPath") == dependency["asset_path"], "Mission/objective graph typed relationship differs")
            toc.validate(dependency, present=False)
            if not dependency["toc_present"]:
                require(not mission["catalog_selectable"] and not mission["objectives"], "Absent objective graph was admitted")
                excluded.append({"missionGraph": graph["asset_path"], "mission": mission["mission_name"], "absentObjectiveGraph": dependency["asset_path"]})
                continue
            require(mission["catalog_selectable"], "Present mission was unexpectedly excluded")
            objective_raw = raw.get(dependency)
            require(source_node.get("AvailableCheckpoint", "") == mission.get("available_checkpoint", ""), "Mission available checkpoint differs from typed field")
            available = raw.name(graph_raw, mission.get("available_checkpoint", ""))
            unique(mission["objectives"], lambda value: value["objective_name"], "Duplicate objective name in mission")
            objective_hashes = {o["objective_name"]: o for o in hash_record["objectives"]}
            require(len(objective_hashes) == len(hash_record["objectives"]), "Duplicate objective hash evidence")
            require(set(objective_hashes) == {o["objective_name"] for o in mission["objectives"]}, "Objective set differs from typed name-hash evidence")
            objectives = []
            for objective in sorted(mission["objectives"], key=lambda value: value["objective_name"]):
                child = raw.node(objective_raw, objective, "ObjectiveName")
                require(objective["catalog_selectable"], "Unselectable objective was admitted")
                h = objective_hashes[objective["objective_name"]]
                obj = raw.name(objective_raw, objective["objective_name"], h["objective_crc32"], h["objective_string_header"])
                checkpoint_name = objective.get("checkpoint", "")
                require(child.get("Checkpoint", "") == checkpoint_name, "Checkpoint ownership differs from typed objective field")
                require(h.get("checkpoint", "") == checkpoint_name, "Checkpoint differs from validated header evidence")
                checkpoint = raw.name(objective_raw, checkpoint_name, h.get("checkpoint_crc32"))
                obj.update(assetNodeId=unsigned(objective["node_id"], 64, "objective node ID"),
                           assetNodeOrder=unsigned(objective["asset_node_order"], 32, "objective Nodes index"),
                           checkpoint=checkpoint, debugLoadouts=loadouts(objective, child))
                objectives.append(obj)
            unique(objectives, lambda value: value["nameCrc"], "Scoped objective CRC collision")
            named.update(assetNodeId=unsigned(mission["node_id"], 64, "mission node ID"),
                         assetNodeOrder=unsigned(mission["asset_node_order"], 32, "mission Nodes index"),
                         objectiveGraphPath=dependency["asset_path"], objectiveGraphAssetId=toc.validate(dependency),
                         availableCheckpoint=available, debugLoadouts=loadouts(mission, source_node), objectives=objectives)
            missions.append(named)
        unique(missions, lambda value: value["nameCrc"], "Scoped mission CRC collision")
        model.append({"graphAssetId": graph_id, "graphPath": graph["asset_path"], "missions": missions})
    require(len(hashed) == sum(len(g["missions"]) for g in catalog["graphs"]), "Unmatched mission hash records")
    return model, excluded


def quoted(value):
    return json.dumps(value, ensure_ascii=True)


def checkpoint_cpp(value):
    return "{" + quoted(value["name"]) + f", 0x{value['nameCrc']:08x}u" + "}"


def loadouts_cpp(values):
    return "{" + ", ".join("{" + quoted(v["assetPath"]) + f", 0x{v['assetId']:016x}ull" + "}" for v in values) + "}"


def render(model):
    lines = ["// SPDX-License-Identifier: GPL-3.0-or-later",
             "// Generated by tools/generate_reload_mission_catalog.py. Do not edit.",
             "// Pinned source catalog SHA256: " + CATALOG_SHA,
             "// Pinned native name-header evidence SHA256: " + NAMES_SHA,
             '#include "reload_mission_catalog.h"', "", "namespace sm2debug {",
             "const std::vector<ReloadMissionGraph>& ReloadMissionCatalog() {",
             "    static const std::vector<ReloadMissionGraph> catalog = {"]
    for graph in model:
        lines.append(f"        {{0x{graph['graphAssetId']:016x}ull, {quoted(graph['graphPath'])}, {{")
        for mission in graph["missions"]:
            lines.append(f"            {{{quoted(mission['name'])}, 0x{mission['nameCrc']:08x}u, 0x{mission['assetNodeId']:016x}ull, {mission['assetNodeOrder']}u,")
            lines.append(f"                {quoted(mission['objectiveGraphPath'])}, 0x{mission['objectiveGraphAssetId']:016x}ull, {checkpoint_cpp(mission['availableCheckpoint'])}, {loadouts_cpp(mission['debugLoadouts'])}, {{")
            for objective in mission["objectives"]:
                lines.append(f"                    {{{quoted(objective['name'])}, 0x{objective['nameCrc']:08x}u, 0x{objective['assetNodeId']:016x}ull, {objective['assetNodeOrder']}u, {checkpoint_cpp(objective['checkpoint'])}, {loadouts_cpp(objective['debugLoadouts'])}}},")
            lines.append("                }},")
        lines.append("        }},")
    lines += ["    };", "    return catalog;", "}", "",
              "const ReloadMissionGraph* FindReloadMissionGraph(std::uint64_t graphAssetId) {",
              "    for (const auto& graph : ReloadMissionCatalog())",
              "        if (graph.graphAssetId == graphAssetId) return &graph;",
              "    return nullptr;", "}", "",
              "const ReloadMission* FindReloadMission(std::uint64_t graphAssetId, std::uint32_t missionNameCrc) {",
              "    const auto* graph = FindReloadMissionGraph(graphAssetId);",
              "    if (graph) for (const auto& mission : graph->missions)",
              "        if (mission.nameCrc == missionNameCrc) return &mission;",
              "    return nullptr;", "}", "",
              "const ReloadMissionObjective* FindReloadMissionObjective(std::uint64_t graphAssetId,",
              "    std::uint32_t missionNameCrc, std::uint32_t objectiveNameCrc) {",
              "    const auto* mission = FindReloadMission(graphAssetId, missionNameCrc);",
              "    if (mission) for (const auto& objective : mission->objectives)",
              "        if (objective.nameCrc == objectiveNameCrc) return &objective;",
              "    return nullptr;", "}", "}", ""]
    return "\n".join(lines)


def self_test(catalog, names, toc, raw):
    results = []

    def denied(label, mutation):
        source, hashes = copy.deepcopy(catalog), copy.deepcopy(names)
        mutation(source, hashes)
        try:
            build_model(source, hashes, toc, raw)
        except (ValueError, KeyError, TypeError) as error:
            results.append({"case": label, "passed": True, "reason": str(error)})
        else:
            raise ValueError("Mutation was incorrectly admitted: " + label)

    denied("duplicate graph", lambda c, n: c["graphs"].append(copy.deepcopy(c["graphs"][0])))
    denied("duplicate scoped mission", lambda c, n: c["graphs"][0]["missions"].append(copy.deepcopy(c["graphs"][0]["missions"][0])))
    denied("duplicate scoped objective", lambda c, n: c["graphs"][0]["missions"][0]["objectives"].append(copy.deepcopy(c["graphs"][0]["missions"][0]["objectives"][0])))
    denied("rounded floating point graph ID", lambda c, n: c["graphs"][0].update(asset_id=float(0xBACA0CDE336B5792)))
    denied("rounded floating point node ID", lambda c, n: c["graphs"][0]["missions"][0].update(node_id=float(0xFB73D3674181BE28)))
    denied("overflow node ID", lambda c, n: c["graphs"][0]["missions"][0].update(node_id="0x10000000000000000"))
    denied("mismatched mission native CRC", lambda c, n: n["records"][0].update(mission_crc32="0x12345678"))
    denied("mismatched objective native CRC", lambda c, n: n["records"][0]["objectives"][0].update(objective_crc32="0x12345678"))
    denied("mismatched checkpoint native CRC", lambda c, n: n["records"][0]["objectives"][0].update(checkpoint_crc32="0x12345678"))
    denied("invented available checkpoint", lambda c, n: c["graphs"][0]["missions"][0].update(available_checkpoint="CHK_NOT_FROM_THIS_MISSION"))
    denied("invented objective checkpoint ownership", lambda c, n: c["graphs"][0]["missions"][0]["objectives"][0].update(checkpoint="CHK_NOT_FROM_THIS_OBJECTIVE"))
    denied("mismatched asset Nodes order", lambda c, n: c["graphs"][0]["missions"][0].update(asset_node_order=0))
    denied("mismatched objective graph relationship", lambda c, n: c["graphs"][0]["missions"][0]["objective_graph"].update(asset_path="nodegraph/objectives/absent.objectives.nodegraph"))
    denied("forged typed string-header offset", lambda c, n: n["records"][0]["mission_string_header"].update(string_header_offset=0))
    denied("invented debug loadout", lambda c, n: c["graphs"][0]["missions"][0]["debug_loadouts"].append({"asset_path": "configs/gamestart_loadout/ctns_usability_loadout.config"}))
    denied("changed archive membership", lambda c, n: c["graphs"][0]["entries"][0].update(offset=0))
    denied("changed raw extract identity", lambda c, n: c["graphs"][0].update(sha256="0" * 64))
    denied("foreign graph hash record", lambda c, n: n["records"][0].update(mission_graph="nodegraph/missions/other.missions.nodegraph"))
    missing = next(m for g in catalog["graphs"] for m in g["missions"] if not m["objective_graph"]["toc_present"])
    denied("missing objective graph falsely selectable", lambda c, n: next(m for g in c["graphs"] for m in g["missions"] if m["mission_name"] == missing["mission_name"] and not m["objective_graph"]["toc_present"]).update(catalog_selectable=True))
    require(not toc.describe("configs/gamestart_loadout/ctns_usability_loadout.config")["entries"], "Known absent loadout unexpectedly present")
    results.append({"case": "known absent loadout has no admitted TOC membership", "passed": True})
    for value in [float(0xBACA0CDE336B5792), True, -1, "0x10000000000000000"]:
        try:
            unsigned(value, 64, "test ID")
        except ValueError:
            pass
        else:
            raise ValueError("Inexact/out-of-range ID admitted")
    results.append({"case": "64-bit IDs reject float Boolean negative and overflow", "passed": True})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Compare output without rewriting generated source")
    parser.add_argument("--self-test", action="store_true", help="Exercise evidence/data-integrity refusal cases")
    args = parser.parse_args()
    catalog, names, decoder, asset_id, crc32, normalize = load_inputs()
    toc = Toc(asset_id, normalize)
    raw = RawAssets(decoder, crc32, toc)
    model, excluded = build_model(catalog, names, toc, raw)
    totals = {"graphs": len(model), "missions": sum(len(g["missions"]) for g in model),
              "objectives": sum(len(m["objectives"]) for g in model for m in g["missions"])}
    require(totals == {"graphs": 5, "missions": 316, "objectives": 2648}, "Known exact-build catalog completeness changed")
    require(len(raw.cache) == 228 and raw.string_header_count == 28218, "Raw asset/header verification baseline differs")
    output = render(model)
    if args.check:
        require(OUTPUT.read_text(encoding="utf-8") == output, "Generated C++ differs; regenerate explicitly")
    else:
        OUTPUT.write_text(output, encoding="utf-8", newline="\n")
    tests = self_test(catalog, names, toc, raw) if args.self_test else []
    report = {"schema": 1, "passed": True, "counts": totals,
              "objective_checkpoints": sum(bool(o["checkpoint"]["name"]) for g in model for m in g["missions"] for o in m["objectives"]),
              "mission_available_checkpoints": sum(bool(m["availableCheckpoint"]["name"]) for g in model for m in g["missions"]),
              "distinct_verified_debug_loadouts": len({l["assetId"] for g in model for m in g["missions"] for l in m["debugLoadouts"]} |
                                                     {l["assetId"] for g in model for m in g["missions"] for o in m["objectives"] for l in o["debugLoadouts"]}),
              "raw_assets_revalidated": len(raw.cache), "typed_string_headers_revalidated": raw.string_header_count,
              "excluded_missions": excluded, "negative_cases": tests,
              "source_catalog_sha256": CATALOG_SHA, "source_names_sha256": NAMES_SHA,
              "canonical_research_executable_sha256": EXE_SHA,
              "actual_input_executable_sha256": checked_file_hash(ROOT / "Spider-Man2.exe"),
              "output_sha256": hashlib.sha256(output.encode("utf-8")).hexdigest(),
              "output": str(OUTPUT), "check_mode": args.check,
              "no_process_access": True, "no_native_calls": True,
              "boundaries": ["ASCII sorting is local and deterministic.", "Asset Nodes indices are not native Mission Order.",
                             "Name CRCs require actual current native graph/node/checkpoint admission before execution.",
                             "Unassigned checkpoints, missing assets and native Dev presets are not imported."]}
    report_path = EVIDENCE / "reload-mission-catalog-generation.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"passed": True, **totals, "negative_cases": len(tests), "report": str(report_path)}))


if __name__ == "__main__":
    main()
