"""Load and validate the definitions in protocol/defs/.

load_defs() reads registry.yaml, diag_codes.yaml, the optional system.yaml (the
system items, type 0x00) and one YAML file per module type. It returns plain dicts in a normalized form (every default filled in),
which the emitters and the descriptor hash use. Every error is a SpecError
whose message names the file and the item.
"""

from __future__ import annotations

import keyword
import math
import re
import struct
from pathlib import Path
from typing import Any

import yaml

REGISTRY_FILE = "registry.yaml"
DIAG_FILE = "diag_codes.yaml"
SYSTEM_FILE = "system.yaml"
# Files that are not module types. system.yaml is the system type (0x00),
# loaded by load_system().
RESERVED_FILES = {REGISTRY_FILE, DIAG_FILE, SYSTEM_FILE}


class SpecError(Exception):
    pass


def fail(where: str, msg: str) -> None:
    raise SpecError(f"{where}: {msg}")


# -- small helpers -------------------------------------------------------------

SNAKE = re.compile(r"^[a-z][a-z0-9_]*$")
DOTTED = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*$")
PASCAL_IDENT = re.compile(r"^[A-Z][A-Za-z0-9]*$")
IDENT = re.compile(r"^[A-Za-z][A-Za-z0-9]*$")


def is_int(x: Any) -> bool:
    return isinstance(x, int) and not isinstance(x, bool)


def is_num(x: Any) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def pascal(snake: str) -> str:
    """snake_case or dotted.snake_case to PascalCase."""
    return "".join(part.capitalize() for part in re.split(r"[._]", snake) if part)


def load_yaml(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as f:
            return yaml.safe_load(f)
    except yaml.YAMLError as e:
        raise SpecError(f"{path.name}: invalid YAML: {e}") from e


def check_keys(where: str, d: dict, allowed: set[str], required: set[str]) -> None:
    for k in d:
        if k not in allowed:
            fail(where, f"unknown key '{k}' (allowed: {', '.join(sorted(allowed))})")
    for k in required:
        if k not in d:
            fail(where, f"missing '{k}'")


def opt_str(where: str, d: dict, key: str) -> str:
    v = d.get(key, "")
    if not isinstance(v, str):
        fail(where, f"'{key}' must be a string")
    return v


# -- registry ------------------------------------------------------------------

# MSG_TYPE classes, as in docs/protocol/framing.md.
CLASS_RANGES = (("command", 0x01, 0x3F), ("response", 0x40, 0x7F), ("stream", 0x80, 0xBF), ("event", 0xC0, 0xFE))
STREAM_MIN, STREAM_MAX = 0x80, 0xBF
DIRECTIONS = ("host_to_esp", "esp_to_host", "both", "host_to_agent", "agent_to_host")


def msg_class(value: int) -> str | None:
    for name, lo, hi in CLASS_RANGES:
        if lo <= value <= hi:
            return name
    return None


def load_registry(path: Path) -> dict:
    f = path.name
    doc = load_yaml(path)
    if not isinstance(doc, dict):
        fail(f, "must be a mapping")
    check_keys(f, doc, {"system_type_id", "loop_type_id", "msg_types", "channel_ranges", "wire_enums"}, {"system_type_id", "msg_types", "channel_ranges"})

    system_id = doc["system_type_id"]
    if not is_int(system_id) or system_id != 0:
        fail(f, "system_type_id must be 0x00")

    loop_id = doc.get("loop_type_id", 5)
    if not is_int(loop_id) or not 1 <= loop_id <= 254 or loop_id in (1, 2, 3, 4, 16, 17, 18):
        fail(f, "loop_type_id must be 0x01..0xFE and clear of fixed diag sources")
    msgs: list[dict] = []
    by_value: dict[int, str] = {}
    by_name: set[str] = set()
    if not isinstance(doc["msg_types"], list):
        fail(f, "msg_types must be a list")
    for e in doc["msg_types"]:
        where = f"{f}: msg_type {e.get('name') if isinstance(e, dict) else e!r}"
        if not isinstance(e, dict):
            fail(where, "must be a mapping")
        check_keys(where, e, {"name", "value", "direction", "description"}, {"name", "value", "direction"})
        name, value = e["name"], e["value"]
        if not isinstance(name, str) or not SNAKE.match(name):
            fail(where, "name must be snake_case")
        if not is_int(value) or not 0x01 <= value <= 0xFE:
            fail(where, "value must be 0x01..0xFE (0x00 and 0xFF are reserved)")
        cls = msg_class(value)
        if cls is None:  # not reachable while the classes cover 0x01..0xFE
            fail(where, f"value 0x{value:02X} is in no MSG_TYPE class")
        if e["direction"] not in DIRECTIONS:
            fail(where, f"direction must be one of {', '.join(DIRECTIONS)}")
        if value in by_value:
            fail(where, f"value 0x{value:02X} already used by '{by_value[value]}'")
        if name in by_name:
            fail(where, "name already used")
        by_value[value] = name
        by_name.add(name)
        msgs.append(
            {
                "name": name,
                "value": value,
                "class": cls,
                "direction": e["direction"],
                "description": opt_str(where, e, "description"),
            }
        )
    msgs.sort(key=lambda m: m["value"])

    ranges: list[dict] = []
    if not isinstance(doc["channel_ranges"], list):
        fail(f, "channel_ranges must be a list")
    for r in doc["channel_ranges"]:
        where = f"{f}: channel range {r.get('name') if isinstance(r, dict) else r!r}"
        if not isinstance(r, dict):
            fail(where, "must be a mapping")
        check_keys(where, r, {"name", "symbol", "first", "last", "direction", "description"},
                   {"name", "symbol", "first", "last", "direction"})
        if not isinstance(r["name"], str) or not SNAKE.match(r["name"]):
            fail(where, "name must be snake_case")
        if not isinstance(r["symbol"], str) or not PASCAL_IDENT.match(r["symbol"]):
            fail(where, "symbol must be PascalCase")
        first, last = r["first"], r["last"]
        if not is_int(first) or not is_int(last):
            fail(where, "first and last must be integers")
        if not (STREAM_MIN <= first <= last <= STREAM_MAX):
            fail(where, f"first and last must satisfy 0x{STREAM_MIN:02X} <= first <= last <= 0x{STREAM_MAX:02X} (the stream class)")
        if r["direction"] not in DIRECTIONS:
            fail(where, f"direction must be one of {', '.join(DIRECTIONS)}")
        ranges.append(
            {
                "name": r["name"],
                "symbol": r["symbol"],
                "first": first,
                "last": last,
                "direction": r["direction"],
                "description": opt_str(where, r, "description"),
            }
        )

    for i, a in enumerate(ranges):
        for b in ranges[:i]:
            if a["name"] == b["name"]:
                fail(f"{f}: channel range {a['name']}", "name already used")
            if a["symbol"] == b["symbol"]:
                fail(f"{f}: channel range {a['name']}", f"symbol already used by '{b['name']}'")
            if a["first"] <= b["last"] and b["first"] <= a["last"]:
                fail(f"{f}: channel range {a['name']}", f"overlaps range '{b['name']}'")

    # A range holds no msg_type: every channel in it is usable.
    for r in ranges:
        where = f"{f}: channel range {r['name']}"
        for m in msgs:
            if r["first"] <= m["value"] <= r["last"]:
                fail(where, f"msg_type '{m['name']}' (0x{m['value']:02X}) lies inside the range")
        r["usable_count"] = r["last"] - r["first"] + 1

    wire_enums = doc.get("wire_enums", {})
    if not isinstance(wire_enums, dict):
        fail(f, "wire_enums must be a mapping")
    for name, values in wire_enums.items():
        if not isinstance(name, str) or not PASCAL_IDENT.fullmatch(name) or not isinstance(values, dict):
            fail(f, "wire enum needs a PascalCase name and mapping")
        seen = set()
        for key, value in values.items():
            if not isinstance(key, str) or not PASCAL_IDENT.fullmatch(key) or not is_int(value) or not 0 <= value <= 254 or value in seen:
                fail(f, f"invalid or duplicate wire enum {name}.{key}")
            seen.add(value)
    return {"system_type_id": system_id, "loop_type_id": loop_id, "msg_types": msgs, "channel_ranges": ranges, "wire_enums": wire_enums}


# -- diag codes ----------------------------------------------------------------

SEVERITIES = {"error": ("Error", 1), "warning": ("Warning", 2)}
KINDS = {"record": "Record", "counter": "Counter", "latched": "Latched"}
MAX_SLOTS = 254  # 0xFF is the "no next page" marker in GetEntries


def load_diag(path: Path, module_sources: dict | None = None) -> dict:
    f = path.name
    doc = load_yaml(path)
    if not isinstance(doc, dict) or "sources" not in doc or "codes" not in doc:
        fail(f, "needs top-level 'sources' and 'codes'")

    doc["sources"] = dict(doc["sources"])
    for name, value in (module_sources or {}).items():
        if any(c.get("source") == name for c in doc["codes"] or []):
            doc["sources"][name] = value
    sources: list[dict] = []
    seen_ids: dict[int, str] = {}
    for name, sid in doc["sources"].items():
        if not SNAKE.match(str(name)) or keyword.iskeyword(name):
            fail(f, f"source name '{name}' must be snake_case")
        if not is_int(sid) or not 0x01 <= sid <= 0xFE:
            fail(f, f"source '{name}': id must be 0x01..0xFE")
        if sid in seen_ids:
            fail(f, f"source '{name}': id 0x{sid:02X} already used by '{seen_ids[sid]}'")
        seen_ids[sid] = name
        sources.append({"name": name, "id": sid})
    source_names = {s["name"] for s in sources}

    codes: list[dict] = []
    seen_codes: set[tuple[str, int]] = set()
    seen_names: set[tuple[str, str]] = set()
    for c in doc["codes"] or []:
        where = f"{f}: code {c.get('source')}/{c.get('name')}"
        for key in ("source", "code", "name", "severity", "kind", "description"):
            if key not in c:
                fail(where, f"missing '{key}'")
        if c["source"] not in source_names:
            fail(where, "unknown source")
        if not is_int(c["code"]) or not 0x0001 <= c["code"] <= 0xFFFF:
            fail(where, "code must be 0x0001..0xFFFF")
        if not isinstance(c["name"], str) or not IDENT.match(c["name"]) or not c["name"][0].isupper():
            fail(where, "name must be PascalCase")
        if c["severity"] not in SEVERITIES:
            fail(where, f"severity must be one of {sorted(SEVERITIES)}")
        if c["kind"] not in KINDS:
            fail(where, f"kind must be one of {sorted(KINDS)}")
        for flag in ("blocks_arm", "disarms"):
            if not isinstance(c.get(flag, False), bool):
                fail(where, f"{flag} must be true/false")
        clears = c.get("clears", False)
        retired = c.get("retired", False)
        if not isinstance(clears, bool) or not isinstance(retired, bool):
            fail(where, "'clears' and 'retired' must be true/false")
        if c["kind"] in ("counter", "latched"):
            if c["kind"] == "counter" and clears:
                fail(where, "counter codes cannot clear")
            if "arg0" in c or "arg1" in c:
                fail(where, f"{c['kind']} codes carry no args")
            if c["kind"] == "latched":
                clears = True
        else:
            for arg in ("arg0", "arg1"):
                if arg not in c:
                    fail(where, f"missing '{arg}' (use 'unused')")
        key = (c["source"], c["code"])
        if key in seen_codes:
            fail(where, f"code 0x{c['code']:04X} already used in source '{c['source']}'")
        seen_codes.add(key)
        name_key = (c["source"], c["name"])
        if name_key in seen_names:
            fail(where, f"name already used in source '{c['source']}'")
        seen_names.add(name_key)
        codes.append(
            {
                "source": c["source"],
                "code": c["code"],
                "name": c["name"],
                "severity": c["severity"],
                "kind": c["kind"],
                "clears": clears,
                "blocks_arm": c.get("blocks_arm", False),
                "disarms": c.get("disarms", False),
                "retired": retired,
                "arg0": c.get("arg0"),
                "arg1": c.get("arg1"),
                "description": c["description"],
            }
        )

    live = [c for c in codes if not c["retired"]]
    if len(live) > MAX_SLOTS:
        fail(f, f"{len(live)} live codes; at most {MAX_SLOTS} fit the slot index")
    return {"sources": sources, "codes": codes}


# -- module types --------------------------------------------------------------

LOOP_CLASSES = ("realtime", "non_realtime")
KIND_NAMES = ("param", "signal", "input", "action")
SCALAR_TYPES = ("bool", "u8", "u16", "u32", "i32", "f32")
VALUE_TYPES = SCALAR_TYPES + ("enum",)
NUMERIC_TYPES = ("u8", "u16", "u32", "i32", "f32")
INT_RANGES = {"u8": (0, 0xFF), "u16": (0, 0xFFFF), "u32": (0, 0xFFFFFFFF), "i32": (-(2**31), 2**31 - 1)}
# Flag names in descriptor order. `persist`, `testgen` and `tune` are set from their own keys.
# `gpio` and `ref_ticks_after` come from the `gpio` and `ref_order` keys.
FLAG_ORDER = ("reboot_required", "persist", "read_only", "locked_while_armed", "testgen", "gpio", "ref_ticks_after", "source", "tune")
LISTED_FLAGS = {"param": {"reboot_required", "read_only", "locked_while_armed"}, "signal": set(), "input": set(), "action": set()}
PLACEMENTS = ("loop", "paused")
BLOBS = ("none", "in", "out", "both")
MAX_ACTION_VALUES = 4
RESERVED_CONSTANTS = {"kTypeId", "kItems", "kDesc"}
REF_ORDERS = ("before", "after")
GPIO_NONE = 0xFF  # the value of a gpio param that names no pin (its default)

COMMON_KEYS = {"id", "kind", "name", "description", "retired"}
KIND_KEYS = {
    "param": {"type", "unit", "min", "max", "default", "flags", "values", "ref", "ref_order", "persist", "gpio", "tune"},
    "signal": {"type", "unit", "min", "max", "values"},
    "input": {"type", "unit", "min", "max", "values", "testgen"},
    "action": {"runs_in", "args", "results", "blob"},
}


def f32(where: str, what: str, v: Any) -> tuple[float, int]:
    """Round a number to f32. Returns (value as f32, raw bits)."""
    if not is_num(v):
        fail(where, f"{what} must be a number")
    if not math.isfinite(v):
        fail(where, f"{what} must be finite")
    try:
        bits = struct.unpack("<I", struct.pack("<f", float(v)))[0]
    except OverflowError:
        fail(where, f"{what} {v!r} does not fit in f32")
    return struct.unpack("<f", struct.pack("<I", bits))[0], bits


def slot_value(where: str, what: str, vtype: str, v: Any) -> tuple[Any, int]:
    """Check `v` against `vtype` (not enum). Returns (value, raw 4-byte slot bits)."""
    if vtype == "bool":
        if not isinstance(v, bool):
            fail(where, f"{what} must be true or false")
        return v, int(v)
    if vtype == "f32":
        return f32(where, what, v)
    lo, hi = INT_RANGES[vtype]
    if not is_int(v):
        fail(where, f"{what} must be an integer")
    if not lo <= v <= hi:
        fail(where, f"{what} {v} is outside the {vtype} range {lo}..{hi}")
    return v, v & 0xFFFFFFFF


def load_enum_values(where: str, raw: Any) -> list[dict]:
    if not isinstance(raw, dict) or not raw:
        fail(where, "'values' must be a non-empty mapping of name: number")
    out: list[dict] = []
    seen_values: dict[int, str] = {}
    seen_pascal: set[str] = set()
    for name, value in raw.items():
        if not isinstance(name, str) or not SNAKE.match(name):
            fail(where, f"enum value name {name!r} must be snake_case")
        if not is_int(value) or not 0 <= value <= 0xFF:
            fail(where, f"enum value '{name}' must be 0..255")
        if value in seen_values:
            fail(where, f"enum value {value} used by both '{seen_values[value]}' and '{name}'")
        if pascal(name) in seen_pascal:
            fail(where, f"enum value '{name}' collides with another name after PascalCase conversion")
        seen_values[value] = name
        seen_pascal.add(pascal(name))
        out.append({"name": name, "value": value})
    return out


def load_action_values(where: str, what: str, raw: Any) -> list[dict]:
    if raw is None:
        return []
    if not isinstance(raw, list):
        fail(where, f"'{what}' must be a list")
    if len(raw) > MAX_ACTION_VALUES:
        fail(where, f"'{what}' has {len(raw)} entries; at most {MAX_ACTION_VALUES} scalars")
    out: list[dict] = []
    seen: set[str] = set()
    for a in raw:
        if not isinstance(a, dict):
            fail(where, f"each entry of '{what}' must be a mapping")
        check_keys(f"{where}: {what}", a, {"name", "type", "unit"}, {"name", "type"})
        if not isinstance(a["name"], str) or not SNAKE.match(a["name"]):
            fail(where, f"{what} name {a['name']!r} must be snake_case")
        if a["name"] in seen:
            fail(where, f"{what} name '{a['name']}' used twice")
        seen.add(a["name"])
        if a["type"] not in SCALAR_TYPES:
            fail(where, f"{what} '{a['name']}': type must be one of {', '.join(SCALAR_TYPES)}")
        out.append({"name": a["name"], "type": a["type"], "unit": opt_str(where, a, "unit")})
    return out


def load_item(file: str, type_name: str, raw: Any) -> dict:
    ident = raw.get("name") if isinstance(raw, dict) else raw
    where = f"{file}: item {ident!r}"
    if not isinstance(raw, dict):
        fail(where, "must be a mapping")
    if "id" not in raw:
        fail(where, "missing 'id'")
    if not is_int(raw["id"]) or not 0x0000 <= raw["id"] <= 0xFFFF:
        fail(where, "id must be 0x0000..0xFFFF")
    where = f"{file}: item 0x{raw['id']:04X} {ident}"
    name = raw.get("name")
    if not isinstance(name, str) or not DOTTED.match(name):
        fail(where, "name must be snake_case segments joined by dots, such as vel.kp")
    if not isinstance(raw.get("retired", False), bool):
        fail(where, "'retired' must be true/false")

    if raw.get("retired", False):
        check_keys(where, raw, {"id", "name", "retired"}, {"id", "name", "retired"})
        return {"id": raw["id"], "name": name, "retired": True}

    kind = raw.get("kind")
    if kind not in KIND_NAMES:
        fail(where, f"kind must be one of {', '.join(KIND_NAMES)}")
    check_keys(where, raw, COMMON_KEYS | KIND_KEYS[kind], {"id", "kind", "name"})
    item: dict[str, Any] = {
        "id": raw["id"],
        "kind": kind,
        "name": name,
        "retired": False,
        "description": opt_str(where, raw, "description"),
        "unit": opt_str(where, raw, "unit"),
        "type": "none",
        "flags": [],
        "values": [],
        "ref": None,
        "min": None,
        "max": None,
        "default": None,
        "min_bits": 0,
        "max_bits": 0,
        "default_bits": 0,
        "has_min": False,
        "has_max": False,
        "runs_in": None,
        "args": [],
        "results": [],
        "blob": "none",
    }

    if kind == "action":
        placement = raw.get("runs_in")
        if placement not in PLACEMENTS:
            fail(where, f"runs_in must be one of {', '.join(PLACEMENTS)}")
        item["runs_in"] = placement
        item["args"] = load_action_values(where, "args", raw.get("args"))
        item["results"] = load_action_values(where, "results", raw.get("results"))
        blob = raw.get("blob", "none")
        if blob not in BLOBS:
            fail(where, f"blob must be one of {', '.join(BLOBS)}")
        item["blob"] = blob
        return item

    vtype = raw.get("type")
    if vtype not in VALUE_TYPES:
        fail(where, f"type must be one of {', '.join(VALUE_TYPES)}")
    item["type"] = vtype

    if vtype == "enum":
        item["values"] = load_enum_values(where, raw.get("values"))
        for k in ("min", "max"):
            if k in raw:
                fail(where, f"'{k}' is not allowed on an enum")
    else:
        if "values" in raw:
            fail(where, "'values' is only for type enum")
        if vtype == "bool":
            for k in ("min", "max"):
                if k in raw:
                    fail(where, f"'{k}' is not allowed on a bool")
    if "min" in raw:
        item["min"], item["min_bits"] = slot_value(where, "min", vtype, raw["min"])
        item["has_min"] = True
    if "max" in raw:
        item["max"], item["max_bits"] = slot_value(where, "max", vtype, raw["max"])
        item["has_max"] = True
    if item["has_min"] and item["has_max"] and item["min"] > item["max"]:
        fail(where, f"min {item['min']} is above max {item['max']}")

    if "default" in raw:
        d = raw["default"]
        if vtype == "enum":
            names = {v["name"]: v["value"] for v in item["values"]}
            if d not in names:
                fail(where, f"default {d!r} is not one of the enum values ({', '.join(names)})")
            item["default"], item["default_bits"] = d, names[d]
        else:
            item["default"], item["default_bits"] = slot_value(where, "default", vtype, d)
            if item["has_min"] and item["default"] < item["min"]:
                fail(where, f"default {item['default']} is below min {item['min']}")
            if item["has_max"] and item["default"] > item["max"]:
                fail(where, f"default {item['default']} is above max {item['max']}")
    elif kind == "param" and vtype == "enum":
        fail(where, "an enum param needs a 'default'")

    flags: set[str] = set()
    listed = raw.get("flags", [])
    if not isinstance(listed, list) or any(not isinstance(x, str) for x in listed):
        fail(where, "'flags' must be a list of names")
    for fl in listed:
        if fl not in LISTED_FLAGS[kind]:
            allowed = ", ".join(sorted(LISTED_FLAGS[kind])) or "none"
            fail(where, f"flag '{fl}' is not allowed on a {kind} (allowed: {allowed})")
        if fl in flags:
            fail(where, f"flag '{fl}' listed twice")
        flags.add(fl)

    if kind == "param":
        persist = raw.get("persist", True)
        if not isinstance(persist, bool):
            fail(where, "'persist' must be true/false")
        if persist and "read_only" not in flags:
            flags.add("persist")  # a param persists unless it is read-only or says persist: false
    if "tune" in raw:
        if not isinstance(raw["tune"], bool):
            fail(where, "'tune' must be true/false")
        if raw["tune"]:
            if "reboot_required" in flags or "read_only" in flags:
                fail(where, "'tune' is only for a live param (not reboot_required or read_only)")
            if vtype not in NUMERIC_TYPES and vtype != "enum":
                fail(where, "'tune' is only for a numeric or enum param")
            flags.add("tune")  # the Tune screen shows it (docs/adding-a-module.md)
    if kind == "input" and "testgen" in raw:
        if not isinstance(raw["testgen"], bool):
            fail(where, "'testgen' must be true/false")
        if raw["testgen"]:
            flags.add("testgen")

    if "ref" in raw:
        ref = raw["ref"]
        if kind != "param" or vtype != "u32" or "reboot_required" not in flags:
            fail(where, "'ref' is only for a u32 param flagged reboot_required")
        if not isinstance(ref, str) or not SNAKE.match(ref):
            fail(where, "'ref' must be a snake_case category name")
        item["ref"] = ref
    if "ref_order" in raw:
        if "ref" not in raw:
            fail(where, "'ref_order' is only for a 'ref' param")
        if raw["ref_order"] not in REF_ORDERS:
            fail(where, f"ref_order must be one of {', '.join(REF_ORDERS)}")
        if raw["ref_order"] == "after":
            flags.add("ref_ticks_after")  # the referenced instance ticks after this one

    if "gpio" in raw:
        if not isinstance(raw["gpio"], bool):
            fail(where, "'gpio' must be true/false")
        if raw["gpio"]:
            if vtype != "u8" or "reboot_required" not in flags:
                fail(where, "'gpio' is only for a u8 param flagged reboot_required")
            for k in ("min", "max", "default"):
                if k in raw:
                    fail(where, f"'{k}' is not allowed on a gpio param (the default is 0xFF, no pin; the pin policy decides the rest)")
            flags.add("gpio")
            item["default"], item["default_bits"] = GPIO_NONE, GPIO_NONE

    item["flags"] = [fl for fl in FLAG_ORDER if fl in flags]
    return item


def load_items(f: str, name: str, raw_items: Any, allowed_kinds: tuple[str, ...] = KIND_NAMES) -> list[dict]:
    """Load the items of type `name` from file `f`, with the per-type id and name checks."""
    where = f"{f}: type {name}"
    if not isinstance(raw_items, list):
        fail(where, "items must be a list")
    items = []
    for raw in raw_items:
        if not isinstance(raw, dict) or raw.get("kind") != "source":
            items.append(load_item(f, name, raw))
            continue
        where = f"{f}: source {raw.get('name')!r}"
        if name == SYSTEM_TYPE or name == "loop" or name.split(".")[0] in ("actuator", "controller"):
            fail(where, "sources are not allowed on this type")
        check_keys(where, raw,
                   {"id", "kind", "name", "type", "value_id", "age_id", "unit", "description", "values"},
                   {"id", "kind", "name", "type", "value_id", "age_id"})
        param = load_item(f, name, {"id": raw["id"], "kind": "param", "name": raw["name"],
                                  "type": "u32", "flags": ["reboot_required"],
                                  "description": raw.get("description", "")})
        param["flags"].append("source")
        value = {"id": raw["value_id"], "kind": "signal", "name": str(raw["name"]) + ".value",
                 "type": raw["type"], "unit": raw.get("unit", "")}
        if "values" in raw:
            value["values"] = raw["values"]
        age = {"id": raw["age_id"], "kind": "signal", "name": str(raw["name"]) + ".age_us",
               "type": "u32", "unit": "us"}
        items.extend((param, load_item(f, name, value), load_item(f, name, age)))

    seen_ids: dict[int, str] = {}
    live_names: dict[str, int] = {}
    live_consts: dict[str, str] = {}
    for it in items:
        iw = f"{f}: item 0x{it['id']:04X} {it['name']}"
        if it["id"] in seen_ids:
            fail(iw, f"id already used by '{seen_ids[it['id']]}' (retired ids count too)")
        seen_ids[it["id"]] = it["name"]
        if it["retired"]:
            continue
        if it["kind"] not in allowed_kinds:
            fail(iw, f"kind {it['kind']} is not allowed here (allowed: {', '.join(allowed_kinds)})")
        if it["name"] in live_names:
            fail(iw, "name already used by another item of this type")
        live_names[it["name"]] = it["id"]
        const = "k" + pascal(it["name"])
        if const in RESERVED_CONSTANTS:
            fail(iw, f"name would generate the reserved constant {const}")
        if const in live_consts:
            fail(iw, f"name generates the constant {const}, which '{live_consts[const]}' already generates")
        live_consts[const] = it["name"]
    return items


def load_type(path: Path, system_id: int, loop_id: int = 5) -> dict:
    f = path.name
    doc = load_yaml(path)
    if not isinstance(doc, dict):
        fail(f, "must be a mapping")
    builtin = path.stem == "loop"
    check_keys(f, doc, {"type", "type_id", "description", "realtime", "items"},
               {"type", "type_id", "items"} | (set() if builtin else {"realtime"}))
    name = doc["type"]
    if not isinstance(name, str) or not DOTTED.match(name):
        fail(f, "type must be snake_case segments joined by dots, such as controller.cascade_pid")
    if name == SYSTEM_TYPE:
        fail(f, f"type '{SYSTEM_TYPE}' is reserved for {SYSTEM_FILE}")
    if path.stem != name:
        fail(f, f"the file must be named '{name}.yaml'")
    where = f"{f}: type {name}"
    tid = doc["type_id"]
    if not is_int(tid) or not 0x01 <= tid <= 0xFE:
        fail(where, "type_id must be 0x01..0xFE (0x00 is reserved for system)")
    if tid == system_id:
        fail(where, "type_id is the system type id")

    if (tid == loop_id) != builtin:
        fail(where, "loop_type_id is reserved for the built-in loop type")
    requirement = doc.get("realtime")
    if builtin:
        if requirement is not None:
            fail(where, "the loop type runs in no loop: remove 'realtime'")
        loops = []
    else:
        if requirement not in ("required", "forbidden", "any"):
            fail(where, "realtime must be required, forbidden or any")
        loops = {"required": ["realtime"], "forbidden": ["non_realtime"], "any": list(LOOP_CLASSES)}[requirement]

    items = load_items(f, name, doc["items"])

    return {
        "type": name,
        "ns": name.replace(".", "_"),
        "type_id": tid,
        "description": opt_str(where, doc, "description"),
        "loops": [lp for lp in LOOP_CLASSES if lp in loops],
        "items": items,
    }


SYSTEM_TYPE = "system"
SYSTEM_KINDS = ("param", "signal")


def load_system(path: Path, system_id: int) -> dict:
    """system.yaml: the system items, type 0x00. Like a module type, but it
    runs in no loop (the core applies its params on the config task) and has
    only params and signals."""
    f = path.name
    doc = load_yaml(path)
    if not isinstance(doc, dict):
        fail(f, "must be a mapping")
    if "loops" in doc:
        fail(f, "the system type runs in no loop: remove 'loops'")
    check_keys(f, doc, {"type", "type_id", "description", "items"}, {"type", "type_id", "items"})
    if doc["type"] != SYSTEM_TYPE:
        fail(f, f"type must be '{SYSTEM_TYPE}'")
    where = f"{f}: type {SYSTEM_TYPE}"
    tid = doc["type_id"]
    if not is_int(tid) or tid != system_id:
        fail(where, f"type_id must be the system type id 0x{system_id:02X}")

    items = load_items(f, SYSTEM_TYPE, doc["items"], SYSTEM_KINDS)

    return {
        "type": SYSTEM_TYPE,
        "ns": SYSTEM_TYPE,
        "type_id": tid,
        "description": opt_str(where, doc, "description"),
        "loops": [],
        "items": items,
    }


# -- everything ----------------------------------------------------------------


def load_defs(defs_dir: Path) -> dict:
    registry = load_registry(defs_dir / REGISTRY_FILE)

    types: list[dict] = []
    for path in sorted(defs_dir.glob("*.yaml")):
        if path.name in RESERVED_FILES:
            continue
        types.append(load_type(path, registry["system_type_id"], registry["loop_type_id"]))
    system_path = defs_dir / SYSTEM_FILE
    if system_path.exists():  # optional: without it there is no system type
        types.append(load_system(system_path, registry["system_type_id"]))
    seen: dict[int, str] = {}
    for t in types:
        if t["type_id"] in seen:
            fail(f"{t['type']}.yaml", f"type_id 0x{t['type_id']:02X} already used by '{seen[t['type_id']]}'")
        seen[t["type_id"]] = t["type"]
    types.sort(key=lambda t: t["type_id"])
    diag = load_diag(defs_dir / DIAG_FILE, {t["ns"]:t["type_id"] for t in types if t["type_id"] != 0})
    result = {"registry": registry, "diag": diag, "types": types}
    descriptor_min_payload(result)  # validate complete descriptor responses
    return result


def descriptor_min_payload(defs: dict) -> int:
    """Complete core descriptor response sizes, including header and page fields."""
    largest = 128
    def text(s):
        n = len((s or "").encode("utf-8"))
        if n > 65535:
            fail("descriptors", "string exceeds u16 length")
        return n
    for t in defs["types"]:
        largest = max(largest, 6 + 5 + 4 + text(t["type"]))
        for item in t["items"]:
            if item.get("retired"):
                continue
            n = 31 + sum(text(item.get(k)) for k in ("name", "unit", "description", "ref"))
            for a in item.get("args", []) + item.get("results", []):
                n += 5 + text(a["name"]) + text(a["unit"])
            largest = max(largest, 6 + 5 + n)
            for e in item.get("values", []):
                largest = max(largest, 6 + 5 + 3 + text(e["name"]))
    for code in defs["diag"]["codes"]:
        largest = max(largest, 6 + 5 + 8 + text(code["name"]))
    if largest > 1024:
        fail("descriptors", f"complete response needs {largest} bytes, limit 1024")
    return largest
