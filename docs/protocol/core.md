# Core protocol v1 (F3b)

F3c adds `libdrivelink`, `drivelink-cli` and `drivelink-vdev` as consumers of
this same contract, with no wire-format change. The CLI supports readable and
JSON output, generic descriptors and typed scalar values; see `host/README.md`.
Vdev has no ESP CPU-load sampler (status loads are 0xFF) and models crash resets
through its persistent running marker. Its build id is the source commit's first
16 hex characters, or 16 zeros when Git metadata is unavailable; an explicit
`DRIVELINK_VDEV_BUILD_ID` CMake override is available for source archives.

Commands use registry MSG_TYPE `core_command` (0x01); responses use
`core_response` (0x41). All integers are little-endian. Version is 1.
Request: `version u8 | seq u8 | family u8 | op u8 | body`.
Response: `version u8 | seq u8 | family u8 | op u8 | result u8 | detail u8 | body`.
A malformed header uses family/op 0xFF; seq is echoed if parsed with a valid
version. Fixed bodies have exact lengths. Rejected operations have no body.
Result, detail, family and operation numbers come from `registry.yaml`'s
`wire_enums`, exported by generated `registry.hpp`; never renumber them.
`Ok` and `PendingReboot` are accepted outcomes.

Strings below are `length u16 | UTF-8 bytes` without a terminator, except
layout command labels, which use a u8 length and 1..16 non-NUL bytes.
Ref: `type u8 | instance u8 | item u16`. A scalar value is a 4-byte slot,
with the type declared by its descriptor (IEEE binary32 for f32).

## System (family 1)

All requests have empty bodies. GET_INFO (1) answers:
`protocol_version u8 | firmware_version string | build_id string |
descriptor_hash u32 | capture_capacity u32 | max_payload u16 |
running_instances u8 | flags u8 | loop_count u8`, followed by
`loop_id u8 | name string | rate_hz u32` per loop.
The build id is the first 8 ELF SHA-256 bytes as 16 hex characters.
Flags: pending_reboot 1, layout_invalid 2, config_loaded_from_nvs 4,
safe_mode 8. Capture capacity is 0 until F7.

SAVE (5), REVERT (6), FACTORY_RESET (7), REBOOT (8), EXIT_SAFE_MODE (9)
wrap the core operations. Reset, reboot and exit safe mode reply, wait for
command TX to drain, then reboot. A drain timeout cancels the reboot and
logs the failure; a host may retry REBOOT. ARM, DISARM and ACK_FAULT remain
Unsupported until F4b. BenchCrash (254) responds and aborts only in an
explicit `CONFIG_CORE_BENCH_CRASH=y` test build. Normal builds reject it.

## Layout (family 2)

- LIST_INSTANCES (1): `view u8 | start u16`. View 0 is configured records,
  including failed/offline records; view 1 is the pending layout, falling
  back to configured records when no pending copy exists.
- ADD_INSTANCE (2): `type u8 | loop u8 | label_length u8 | label bytes`.
  Loop 0xFF selects the type's first loop (control before sensor).
  Reply: allocated instance u8. No instance is created until SAVE/reboot.
- REMOVE_INSTANCE (3): `type u8 | instance u8`.
- SET_LABEL (4): `type u8 | instance u8 | label_length u8 | label bytes`.
- DISCARD_PENDING (5): empty body.

LIST response: page prefix, then `type u8 | instance u8 | loop u8 |
state u8 | label string` entries. States: running 0, failed 1, invalid 2,
dependency_missing 3, offline 4; pending entries use 0xFF. Comparing the
views reveals additions, removals and label changes. SAVE retains the pending
view until reboot or discard. A pending view contains the effective values
that SAVE will write, including live values from original records.

## Describe (family 3)

Every response starts with `total u16 | next u16 | count u8`. The cursor is
an index, 0xFFFF means complete. Cursors past total are rejected. A page
contains whole entries only; an entry that cannot fit returns BufferTooSmall.
The generator rejects complete descriptor responses over 1024 bytes and
exports `kDescriptorMinPayload`; firmware statically checks its receive limit.
No descriptor is truncated or split. Names/descriptions use UTF-8 byte lengths.

- GET_TYPES (1): `start u16`. Entry: `type u8 | loop_mask u8 | name string`.
- GET_ITEMS (2): `type u8 | start u16`. Entry: `id u16 | kind u8 |
  value_type u8 | flags u8 | limits_present u8 | min u32 | max u32 |
  default u32 | name string | unit string | description string | ref string |
  action_placement u8 | blob_direction u8 | nargs u8 | nresults u8`, then
  each argument/result as `value_type u8 | name string | unit string`.
- GET_ENUM (3): `type u8 | item u16 | start u16`.
  Entry: `value u8 | name string`. Non-enum items have an empty list.
- GET_CODES (4): `start u16`. Entry: `source u8 | code u16 | severity u8 |
  kind u8 | flags u8 | name string`. Flags: clears 1, blocks_arm 2, disarms 4.

Kinds/types, loop masks, action placements and item flags use the explicit
values in `defs.hpp`, including GPIO 0x20 and RefTicksAfter 0x40.
Retired definitions are absent. Module diag source IDs are resolved from
module type YAMLs when referenced by the diagnostic definitions; legacy
manager sources remain until their porting phases.

## Param (family 4)

GET (1) and GET_PENDING (3): `count u8 | count * Ref`.
SET (2): `count u8 | count * (Ref, value u32)`.
Accepted responses: `count u8 | count * applied_value u32` in request order.
At most 64 entries. GET reads configured parameter shadows; GET_PENDING
reads the effective pending layout. SET applies the existing atomic batch
and domain rules. Pending writes answer PendingReboot. Values outside numeric
limits are clamped and returned; invalid types, NaN/Inf, enums and wiring
are rejected. A response must fit before any SET is attempted.

## Action (family 8)

RUN (1): `Ref | args[4] u32 | blob_length u16 | blob bytes`.
Accepted response: `out[4] u32 | blob_length u16 | blob bytes`.
Unused argument slots must be zero. Blobs are at most 512 bytes and must
follow the descriptor direction. The response buffer must accommodate the
maximum output before execution; small-payload builds can reject blob-output
actions with BufferTooSmall. Inputs and outputs are preallocated.

Loop actions run after the staged parameter batch at the tick boundary.
Paused actions obtain a loop acknowledgement, execute on the config task,
and release the pause. A ready action can be withdrawn on timeout; once
claimed the caller waits for definite completion. Paused control-loop actions
are refused while armed. Modules must keep loop actions bounded and avoid
allocation, blocking and logging.

## Status (MSG_TYPE 0x80)

`version u8 | epoch u8 | counter u16 | monotonic_timestamp_us u64 |
arm_state u8 | flags u8 | active_error_count u8 | cpu_load[2] u8 |
overruns[2] u32 | last_disarm_reason u8` (26 bytes).
Epoch is 0. Counter wraps at 65536. The two overrun fields are control then
sensor. Flags follow the architecture plan. Until F4b the device is disarmed
and safety-only fields are zero. CPU utilization is measured from per-core idle runtime deltas (0..100 percent).
It is 0xFF until the first interval, or when measurement is disabled or the
runtime clock is not ESP timer. CORE_STATUS_CPU_LOAD defaults on and selects
runtime statistics; reads take constant work without stack scans. Active errors come from instance-aware diag state.
`system.status.rate_hz` defaults to 10 (1..100). UART streams use latest-wins
slots; skips are reported by uart_comm.StreamSkipped.
