# Core protocol v5 (F4c)

F4b adds arming, faults, the independent safety monitor and runtime limits.
F4c adds source params and their generated value/age signals.
Firmware, host library, CLI and vdev use v5 together; older peers are
incompatible. Diagnostic framing remains v3; NVS formats are unchanged.

Commands use registry MSG_TYPE `core_command` (0x01); responses use
`core_response` (0x41). All integers are little-endian. Version is 5.
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

## Sources (F4c)

Item flag `Source` is 0x80. A source is an ordinary persisted u32 param,
`reboot_required`. Its value is `type << 24 | instance << 16 |
item_id`; zero disconnects it. This packed numeric representation differs
from both a Ref's serialized field order and existing instance wiring.
The target must be a signal or input of the source's value type. Unknown
targets, params/actions and type mismatches return `BadRef`. Removing a
referenced instance returns `Referenced`.

The generator adds signals `<param>.value` (the target's value type) and
`<param>.age_us` (u32, microseconds). Hosts discover them by name; the CLI
and host descriptor JSON also expose `is_source`. There is no source op.
Both signals work with ordinary subscriptions and, from F7, capture.
`age_us == 0xFFFFFFFF` means no valid sample; real ages saturate at
0xFFFFFFFE. An invalid sample's value is not meaningful. Failed producers
leave their consumers running, with invalid samples.

Across loops, one snapshot contains all requested values from the same
producer tick; the consumer gets the latest complete snapshot and its age
at consumer tick start. A paused producer leaves its last snapshot in place
and age increases. Same-loop sources sample just before the consumer ticks,
after their producer, with age zero. Same-loop cycles return `WiringCycle`;
cross-loop cycles are permitted. ARM/DISARM preserve sources. A consumer
decides how much staleness is acceptable.

`limits.link_values` (system 0x0015, read-only u16) is the maximum number of
distinct target values per ordered pair of different loops (default 32).
Repeated targets within a pair share one slot. Exceeding it returns
`Capacity` and leaves the pending layout unchanged. This limit is not saved.

## System (family 1)

Requests have empty bodies except the bench stall described below. GET_INFO (1) answers:
`protocol_version u8 | firmware_version string | build_id string |
descriptor_hash u32 | capture_capacity u32 | max_payload u16 |
running_instances u8 | flags u8 | loop_count u8`, followed by
`loop_id u8 | name string | rate_hz u32 | loop_flags u8` per running loop.
Loop flags: bit 0 `realtime`; the other bits are reserved and zero. A loop id
can have gaps and is not its position in this list. Safe mode has zero running loops.
`running_instances` includes running loop instances, excluding the system.
The build id is the first 8 ELF SHA-256 bytes as 16 hex characters.
Flags: pending_reboot 1, layout_invalid 2, config_loaded_from_nvs 4,
safe_mode 8. Capture capacity is 0 until F7.

SAVE (5), REVERT (6), FACTORY_RESET (7), REBOOT (8), EXIT_SAFE_MODE (9)
wrap the core operations. Reset, reboot and exit safe mode reply, wait for
command TX to drain, then reboot. A drain timeout cancels the reboot and
logs the failure; a host may retry REBOOT. BenchCrash (254) responds and aborts only in an
explicit `CONFIG_CORE_BENCH_CRASH=y` test build. Normal builds reject it.

## Layout (family 2)

- LIST_INSTANCES (1): `view u8 | start u16`. View 0 is configured records,
  including failed/offline records; view 1 is the pending layout, falling
  back to configured records when no pending copy exists.
- ADD_INSTANCE (2): `type u8 | loop u8 | label_length u8 | label bytes`.
  For modules, loop 0xFF selects the lowest-numbered compatible loop in the
  effective pending layout; no match returns LoopMismatch. For the built-in
  `loop` type (0x05), loop must be 0xFF: the loop's instance id is its loop id.
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

GET_TYPES loop_mask bit 0 allows real-time loops, bit 1 allows other loops;
system and loop have mask zero because they run in no loop.

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
and domain rules. Live system and loop params share the config-task domain;
module batches must name a single actual loop ID. Reboot-required params and
pending-only instances use the pending domain. Pending writes answer PendingReboot. Values outside numeric
limits are clamped and returned; invalid types, NaN/Inf, enums and wiring
are rejected. A response must fit before any SET is attempted.

## Group (family 5)

All groups are boot-allocated resources, with independent subscription and
command IDs. They survive reconnects but are never persisted. Limits are
CORE_SUBSCRIPTION_GROUPS (8, 1..31), CORE_COMMAND_GROUPS (8, 1..29 while legacy
channels are reserved), and CORE_GROUP_ENTRIES (64, 1..64). The effective entry
limit is min(configured limit, 64, floor((max_payload - 19)/4)), so both a
complete LIST record and telemetry fit. A 128-byte build advertises 27. The channel is
registry SubGroupChannelFirst + subscription ID, or CmdGroupChannelFirst +
command ID. No legacy channel is used.

| Op | Request body |
|---|---|
| SUB_CONFIG (1) | `group u8, loop u8, divider u16, count u8, count * Ref` |
| SUB_ENABLE (2) | `group u8, enabled u8` (exactly 0 or 1) |
| SUB_CLEAR (3) | `group u8` |
| CMD_CONFIG (4) | `group u8, loop u8, count u8, count * Ref` |
| CMD_CLEAR (5) | `group u8` |
| LIST (6) | `start u16` |

Successful mutations return `epoch u8, channel u8`. CONFIG enables the group;
SUB_ENABLE requires a configured group. Divider is 1..65535 loop executions
(skipped timer periods do not advance it). Zero-entry groups are allowed,
including a command group used only for keepalives. References are unique,
bound signals or inputs from running instances on the named loop; command
groups accept only inputs. A loop instance's statistic signals belong to its
own ID, despite its record having no parent loop. Offline and pending-only
instances cannot be bound. An input can belong to only one command group
(`AlreadyBound`); subscriptions may overlap.

Each mutation swaps at a tick boundary, increments a wrapping u8 epoch, resets
the divider phase, and discards old command and telemetry mailboxes. CLEAR
leaves an unconfigured slot with its advanced epoch. For a cross-loop rebind,
the old loop commits the handoff and relinquishes access before the new loop
uses the group. The caller waits up to three owning-loop periods plus 5 ms;
if the loop has not claimed the swap it is withdrawn (`Busy/Withdrawn`), with
no change. A claimed swap completes definitely. A paused/stalled loop can
therefore refuse a mutation. Command CONFIG/CLEAR are refused while armed;
subscription changes are allowed.

LIST starts with the usual `total u16, next u16, count u8` page header.
Read capacities through the read-only system params `limits.sub_groups`,
`limits.cmd_groups` and `limits.group_entries`. There is no capacity prefix.
It lists every slot, subscriptions first, then commands, each in ascending ID:
`kind u8 (0 subscription, 1 command), group u8, flags u8, loop u8, epoch u8,
divider u16, n u8, n * Ref`. Flags are configured=1, enabled=2. Empty slots
have flags=0, loop=0xFF, divider=1, n=0. Command divider is always 1. Pages
contain whole entries and never truncate a reference list.

### Group streams

Subscription payload: `version u8, epoch u8, counter u16, timestamp_us u64,
n * value u32` (12 + 4*n bytes). Values follow configuration order and descriptor
types. Samples are latched together at the end of the owning loop's tick.
The timestamp is that tick's start time. Loop statistics reflect the previous
completed tick. Latest samples replace unsent ones. Counter increments for
each published sample, wraps at 65536, and stays continuous across mutations.
The host drops a frame with a stale epoch before interpreting its values.
An 8-bit epoch cannot distinguish frames retained across 256 mutations;
clients must not retain frames across epoch reuse or device reboot.

Command payload: `version u8, epoch u8, counter u16, n * value u32`
(4 + 4*n bytes); a prefix-only frame is a keepalive for any configured group.
Counters are informational and do not enforce order. CONFIG order defines
value types. All values are validated before publishing the mailbox: invalid
bool/narrow integer/enum, non-finite f32, wrong version, epoch or exact length,
unconfigured ID or a busy mailbox rejects the whole frame and increments
core.CommandFrameRejected. Valid numeric values outside descriptor limits
are clamped, as with params. Keepalives update the receipt time without
replacing pending values. RX never writes module storage.

The loop consumes the latest accepted values at its start boundary only when
armed and their receipt time is strictly after the current ARM instant. Old
values are discarded while disarmed; a keepalive never revives them.
The low-level host API uses typed slot bits; NaN remains rejected. Host
NaN-to-hold conversion remains for later semantic device modeling.

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
claimed the caller waits for definite completion. Paused actions in any real-time loop
are refused while armed. Modules must keep loop actions bounded and avoid
allocation, blocking and logging.

## Status (MSG_TYPE 0x80)

`version u8 | epoch u8 | counter u16 | monotonic_timestamp_us u64 |
arm_state u8 | flags u8 | active_error_count u8 | cpu_load[2] u8 |
loop_count u8 | loop_count * (loop_id u8, overruns u32) | last_disarm_reason u8`
(19 + 5 * loop_count bytes, at most 59).
Epoch is 0. Counter wraps at 65536. Only running loops are included; safe mode
has loop_count zero. Loop IDs may have gaps. Flags: fault_latched 1, blocks_arm_active 2, pending_reboot 4, nvs_dirty 8,
testgen_running 16 (reserved until F7), safe_mode 32. Capture state occupies
bits 6..7 (reserved until F7). CPU utilization is measured from per-core idle runtime deltas (0..100 percent).
It is 0xFF until the first interval, or when measurement is disabled or the
runtime clock is not ESP timer. CORE_STATUS_CPU_LOAD defaults on and selects
runtime statistics; reads take constant work without stack scans. Active errors come from instance-aware diag state.
`system.status.rate_hz` defaults to 10 (1..100). UART streams use latest-wins
slots; skips are reported by uart_comm.StreamSkipped.

## Built-in loop and factory layout

Type 0x05 is registered by the core. Its [items](types/loop.md) are discovered
normally. It uses ADD_INSTANCE, REMOVE_INSTANCE, params, SAVE and reboot; removal
is Refused with Referenced while another layout entry runs in it. rate_hz is live,
locked while armed, and applies on the config task with rollback on timer failure.
The other settings require reboot. A running real-time loop refuses paused actions
while armed and subscribes to the Task WDT. A stall of any loop still disarms in F4b.

A blank store, factory reset or incompatible old layout creates control (id 0,
1000 Hz, real-time, core 1, priority 20) and sensor (id 1, 100 Hz, non-real-time,
core 0, priority 10), both with 4096-byte stacks. CORE_MAX_LOOPS=1 creates only
control. A valid saved layout can contain zero loops. CORE_MAX_LOOPS bounds loop
instances separately from CORE_MAX_INSTANCES, which bounds module instances.
The layout blob format is 2; instance blobs retain format 1. Pre-F3d layouts
are discarded in favor of the factory layout, without migrating their modules.
Loop task startup failure leaves that loop Failed and dependent modules
DependencyMissing; unrelated loops and the config link stay available.


## Arming and safety (F4b)

ARM (system op 2), DISARM (3), ACK_FAULT (4) have empty bodies and empty
successful responses. States in status are Disarmed 0, Armed 1, Fault 2.
ARM requires a running, valid layout with no failed instances or pending
reboot, no safe mode, no active `blocks_arm`/`disarms` code, no fault latch,
and at least one currently configured command group. An empty group counts.
Repeated ARM while already Armed succeeds without extending watchdog grace.

Refusals use Fault with FaultLatched (38), BlocksArm (39), LayoutInvalid (40),
StartFailed (32), SafeMode (29), or RebootPending (45). NoCommandGroup (41)
uses InvalidArgument. A concurrent disarm/stop uses Busy/DisarmRaced (42).
A monitor that cannot start fails Core startup with Fault/MonitorUnavailable (44).
ACK_FAULT is refused Armed/LockedWhileArmed while armed, or Fault/FaultActive
(43) while a disarming condition remains active. Recovery alone never clears
the fault latch. DISARM never acknowledges a fault.

Fast DISARM uses MSG_TYPE 0x03 with exactly the same four-byte core header:
`version=4 | seq | family=1 | op=3`, with no body. Its response is the normal
six-byte core response on 0x41. Both this form and system DISARM on 0x01 are
handled directly on UART RX, bypassing the config queue. Malformed requests
apply nothing. A lost response never undoes the stop.

Every disarm invalidates an in-flight ARM ticket, even when already disarmed.
At the next tick each loop resets its bound inputs and calls
`onArmTransition(false)`; on ARM it also calls `onArmTransition(true)`.
An intervening disarm is preserved when disarm/re-arm both occur between
loop ticks. Command values must have arrived strictly after the ARM instant;
a prefix-only keepalive updates receipt time without reviving older values.

The 10 ms monitor runs independently on esp_timer's task. All running loops,
including non-real-time loops and sparse loop ids, are monitored. No heartbeat
advance for more than three periods plus 5 ms raises `core.LoopStalled` for
that loop, disarms and latches Fault. Heartbeat recovery clears the active
condition even while disarmed, after which ACK_FAULT is possible. Paused
loops continue their heartbeat; real-time paused actions are refused armed.

`system.watchdog_ms` (item 4, u16, persisted, locked armed) defaults to 200 ms
and clamps to 20..1000. Any valid command-group frame received after ARM feeds
the watchdog. Until the first such frame, ARM grants one watchdog interval.
An unavailable nonblocking receipt snapshot defers the check. Expiry raises
and clears `core.WatchdogTimeout`, disarms without Fault and permits re-arm.
Rejected frames never feed it. Status `last_disarm_reason` is Boot 0,
Explicit 1, Watchdog 2, Fault 3, LoopStalled 4, Shutdown 5.

On a detected stall or timeout the monitor calls `emergencyStop()` on every
actuator. It repeats the force while disarmed, without depending on a loop
transition. A stop gate prevents a concurrent ARM commit during those calls.
Real-time loop tasks also feed the panic-reset Task WDT after each completed
tick, providing a backstop when esp_timer itself cannot run.

The five read-only, non-persisted u16 system params are `limits.loops` (16),
`limits.instances` (17), `limits.sub_groups` (18), `limits.cmd_groups` (19)
and `limits.group_entries` (20). Values come from the normalized runtime
configuration, overriding YAML defaults and stored data at boot. Instance
capacity counts modules separately from loop capacity. SET returns ReadOnly;
SAVE omits all five.

BenchStall (system op 253) takes `loop u8 | duration_ms u16`, 1..10000 ms.
Like BenchCrash it requires the default-off `CONFIG_CORE_BENCH_CRASH` test
build (vdev: `--allow-crash`). It schedules deliberate busy work in that loop,
withholding its Task WDT feed. It is for bare-board monitor/WDT verification;
normal builds answer Unsupported. No layout or NVS format changes are involved.

## Motor controller commands (F5a-1)

`controller.cascade_pid` (type 0x20) is discovered through the existing module
and item descriptors; there is no new family or encoding. Position control
cascades position PID to a velocity target to velocity PID to duty. The live
`mode`, tuning and limits, signals and command inputs are in its generated type
table. References use ordinary structural same-loop wiring.

An armed mode change holds the previous duty for the transition tick, within
current limits, and starts the new mode from measured feedback. The controller
then requires an active-input value frame received strictly after mode
application. Equal timestamps, preexisting inputs and keepalives cannot release
`waiting_for_command`. A new frame during the transition tick takes effect on
the next tick. Rewriting an unchanged mode does not reset this boundary.
The watchdog remains active while waiting; keepalives still feed it.

Only one controller can claim an actuator. Duplicate ownership fails startup
and blocks ARM using the existing failed-instance rule. A claimed mock ignores
its standalone duty input and refuses its `drive` action with Busy/AlreadyBound.
ARM/DISARM reset controller state and receipts, holding zero duty/velocity or
measured position; they preserve encoder position and source storage. See
`firmware/components/mod_motor/README.md` for implementation contracts. These
are discovered module behaviors over core v5; diag remains v3 and NVS unchanged.
