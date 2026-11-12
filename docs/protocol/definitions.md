# Definitions

`protocol/defs/` is the single source of the wire's names and numbers. One generator, `tools/gen/gen.py`, turns it into pure C++ headers for `protocol/` and into the generated docs. The generated files are checked in, and CI fails if they are stale.

| Source file | Defines | Generated |
|---|---|---|
| `registry.yaml` | MSG_TYPEs and stream channel ranges | `gen/registry.hpp`, [registry.md](registry.md) |
| `diag_codes.yaml` | diag sources and codes | `gen/diag_codes.hpp`, [diag-codes.md](diag-codes.md) |
| `system.yaml` | the system items (type `0x00`) | `gen/types.hpp`, `gen/type_system.hpp`, [types/system.md](types/system.md) |
| `<type>.yaml`, one per module type | the items a module type exposes | `gen/types.hpp`, `gen/type_<type>.hpp`, `types/<type>.md` |

No module type ships yet: the type path is exercised by the generator's test fixtures, and the core's native tests use the fixture types as test modules.

The headers are in `protocol/include/drivelink/protocol/gen/`, namespace `drivelink::protocol::defs`. Never edit them by hand.

## Running the generator

```bash
python tools/gen/gen.py          # regenerate
python tools/gen/gen.py --check  # exit 1 if a generated file is stale (CI)
python -m unittest discover -s tools/gen/tests   # the generator's own tests
```

It needs Python 3 and PyYAML. `--check` compares with CRLF read as LF, and it also reports generated files that no definition produces any more (a removed type). A plain run rewrites stale files and deletes those.

Every validation error names the file and the item. The exit code is 2 for an invalid definition, 1 for a stale file under `--check`.

## registry.yaml

```yaml
system_type_id: 0x00
msg_types:
  - {name: diag_query, value: 0x23, direction: host_to_esp, description: "..."}
  - {name: imu_stream, value: 0xBF, direction: esp_to_host, legacy: F6}
channel_ranges:
  - {name: cmd_groups, symbol: CmdGroup, first: 0xA0, last: 0xBF, direction: host_to_esp}
```

Rules:
- A MSG_TYPE value is unique, between `0x01` and `0xFE`, and falls in a class of [framing.md](framing.md). Names are unique and snake_case. The generated constant is `msg::k<PascalName>`.
- `direction` is `host_to_esp`, `esp_to_host` or `both`.
- `legacy: <step>` marks the old protocol of a manager. The step that ports the manager removes the entry.
- A channel range lies inside the stream class (`0x80–0xBF`) and overlaps no other range.
- A range holds no entries, except legacy ones filling its tail without gaps. They cut the usable channel count: it is the run from `first` to the first occupied channel. `kCmdGroupChannelCount` is 29 while the legacy streams live, and grows when they are removed.
- `symbol` names the constants `k<symbol>ChannelFirst` and `k<symbol>ChannelCount`.
- `system_type_id` is `0x00`. Module type ids are in each type's own file.

The header's `static_assert`s repeat the checks in C++: unique values, each value in its class range, ranges inside the stream class and clear of every entry.

## Module type files

One file per module type, named `<type>.yaml`. The shape follows the architecture plan:

```yaml
type: controller.cascade_pid
type_id: 0x20
description: Optional text.
loops: [control]
items:
  - {id: 0x0001, kind: param,  name: actuator, type: u32, flags: [reboot_required], ref: actuator}
  - {id: 0x0010, kind: param,  name: mode, type: enum, values: {open_loop: 0, velocity: 1}, default: open_loop}
  - {id: 0x0020, kind: param,  name: vel.kp, type: f32, unit: "1/(rad/s)", min: 0, max: 1000, default: 0}
  - {id: 0x0100, kind: signal, name: position, type: f32, unit: rad}
  - {id: 0x0200, kind: input,  name: velocity_sp, type: f32, min: -1000, max: 1000, testgen: true}
  - {id: 0x0300, kind: action, name: reset_integrators, runs_in: loop}
  - {id: 0x0400, name: old_item, retired: true}
```

### Type

- `type`: snake_case segments joined by dots. The file must be named `<type>.yaml`.
- `type_id`: `0x01–0xFE`, unique across types. `0x00` is the system.
- `loops`: `control` and/or `sensor`, the loop classes the type may run in.

### Items

Ids are `u16` and unique within the type, retired ids included. Ids are never reused. The id blocks per kind (`0x00xx`, `0x01xx`, ...) are a readability convention only.

Names are snake_case segments joined by dots (`vel.kp`). They must be unique among the live items, and so must the constants made from them: `vel.kp` becomes `kVelKp`, so `vel_kp` would collide.

A retired item has only `id`, `name` and `retired: true`. It keeps its id in the YAML, the hash and the docs, and gets no descriptor.

| Key | Allowed on | Meaning |
|---|---|---|
| `kind` | all | `param`, `signal`, `input` or `action` |
| `type` | param, signal, input | `bool`, `u8`, `u16`, `u32`, `i32`, `f32` or `enum` (a u8 with named values) |
| `unit`, `description` | all | free text |
| `min`, `max` | numeric param, signal, input | limits, checked against the type's range; `min <= default <= max` |
| `default` | param | an enum default is a value name. An enum param needs one. Other params default to 0. |
| `values` | enum items | a mapping of name to `0–255`, unique values |
| `flags` | param | `reboot_required`, `read_only`, `locked_while_armed` |
| `persist` | param | `false` keeps the param out of NVS. Otherwise it persists, unless it is `read_only`. |
| `ref` | u32 param with `reboot_required` | the category of a wiring param, such as `actuator` |
| `testgen` | input | `true` lets a test signal drive it |
| `runs_in` | action (required) | `loop` or `paused` |
| `args`, `results` | action | up to 4 scalars each: `{name, type, unit}` with a scalar type (no enum) |
| `blob` | action | `none` (default), `in`, `out` or `both` |

Every param, signal and input value is a 4-byte slot. Larger data goes through actions.

## system.yaml

The system items: device-wide settings that the core owns, addressed as type `0x00`, instance 0. The file has the shape of a module type file, with three differences:

```yaml
type: system
type_id: 0x00
items:
  - {id: 0x0001, kind: param, name: control.rate_hz, type: u16, unit: Hz, min: 100, max: 2000, default: 1000, flags: [locked_while_armed]}
```

- `type` is `system` and `type_id` is `0x00` (the registry's `system_type_id`). No module type may be named `system`.
- There is no `loops` key: the system type runs in no loop. The core applies its params on the config task (a loop rate change restarts that loop's timer), so `TypeDesc::loops` is 0.
- Items are params and signals only. Inputs and actions are rejected.

Everything else follows the module type rules above: ids, names, value types, limits, flags. The file is optional for the generator (its test fixtures can leave it out); the firmware needs it. The items are listed in [types/system.md](types/system.md). It holds only the items in use; later steps add theirs (the watchdog timeout, the status rate).

## The descriptor format

`protocol/include/drivelink/protocol/defs.hpp` (hand-written) defines what the generated tables hold.

- Enums: `ItemKind`, `ValueType`, `ItemFlag` (bits `RebootRequired`, `Persist`, `ReadOnly`, `LockedWhileArmed`, `Testgen`), `LoopClass` (bits `Control`, `Sensor`), `ActionPlacement` and `BlobDir`. Their numeric values are not a wire encoding yet. The describe protocol (F3) fixes that.
- `ItemDesc`: `id`, `kind`, `type`, `flags`, `name`, `unit`, `description`, `min_bits`, `max_bits`, `default_bits`, `has_min`, `has_max`, `enum_values`, `ref`, `runs_in`, `args`, `results` and `blob`.
- `TypeDesc`: `name`, `type_id`, `loops` and `items`.
- `findItem(type, id)`, `findType(table, type_id)`, `hasFlag`, `runsIn`, `f32Bits` and `f32FromBits`.

`min_bits`, `max_bits` and `default_bits` are the raw slot bits, read according to `type`: a bool is 0 or 1, an unsigned integer is its value, an i32 is its two's complement, an f32 is its IEEE-754 bits and an enum is the value of the named entry. A limit that the YAML leaves out has its `has_*` flag false and its bits 0. The generator writes f32 values as hex bits taken from `struct.pack`, with the decimal in a comment.

For each type, `gen/type_<type with . replaced by _>.hpp` holds, in namespace `defs::<type with . replaced by _>`:
- `kTypeId` and one `uint16_t` id constant per live item (`defs::controller_cascade_pid::kVelKp`)
- `kItems` (the `ItemDesc` table) and `kDesc` (the `TypeDesc`)
- a `static_assert` that the ids in `kItems` are unique

`gen/types.hpp` holds `kTypeTable`, a table of pointers to every `kDesc`, sorted by type id, so the system type (`defs::system::kDesc`) comes first. Today it holds only the system type.

## The descriptor hash

`kDescriptorHash` in `gen/registry.hpp` is a CRC-32 (IEEE, `zlib.crc32`) over the canonical JSON of the normalized registry, diag codes and module types: sorted keys, no whitespace, every default filled in. The generator computes it. It changes when a definition changes, including a description, and does not change for a comment or for the order of keys in the YAML. Msg types are sorted by value and types by type id before hashing. Diag codes, items and enum values keep their YAML order, which is part of their meaning.

`GET_INFO` (F3b) reports the hash, so the host can tell that the firmware's definitions changed.
