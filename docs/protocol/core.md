# Core protocol v10 (H2e-1)

F4b adds arming, faults, the independent safety monitor and runtime limits.
F4c adds source params and their generated value/age signals. F7a (v6) adds
the capture family, the core event stream and the status capture state.
F7b (v7) adds test generation, its completion event and capture trigger.
F9 (v8) adds OTA, its status/identity query and the OTA ARM gate.
H1c (v9) adds factory device identity and a live, persisted device name.
H2e-1 (v10) widens the item descriptor flags to u16 and adds the `Tune` flag.
Firmware, host library, CLI and vdev use v10 together; older peers are
incompatible. Diagnostic framing remains v3 and time remains v1. NVS layout
format 3 includes the name; LP format 2 loads with an empty name.
F8 adds the independent [time protocol v1](time.md) on 0x02/0x42; core stays v7.

Commands use registry MSG_TYPE `core_command` (0x01); responses use
`core_response` (0x41). All integers are little-endian. Version is 9.
Request: `version u8 | seq u8 | family u8 | op u8 | body`.
Response: `version u8 | seq u8 | family u8 | op u8 | result u8 | detail u8 | body`.
A malformed header uses family/op 0xFF; seq is echoed if parsed with a valid
version. Fixed bodies have exact lengths. Rejected operations have no body,
except OTA WRITE's documented OtaOffset error body.
The [OTA family, state machine and boot contract](ota.md) is family 9.
Result, detail, family and operation numbers come from `registry.yaml`'s
`wire_enums`, exported by generated `registry.hpp`; never renumber them.
`Ok` and `PendingReboot` are accepted outcomes.

Strings below are `length u16 | UTF-8 bytes` without a terminator, except
device names (below) and layout command labels (u8 length, 1..16 non-NUL bytes).
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
Both signals work with ordinary subscriptions and capture.
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

Requests have empty bodies except SET_NAME and the bench stall below. GET_INFO (1) answers:
`protocol_version u8 | firmware_version string | build_id string |
descriptor_hash u32 | capture_capacity u32 | max_payload u16 |
running_instances u8 | flags u8 | loop_count u8`, followed by
`loop_id u8 | name string | rate_hz u32 | loop_flags u8` per running loop,
then `device_id[6] | name_len u8 | name bytes`. The id is the read-only factory
MAC in native byte order, not a little-endian integer. The name is 0..16 UTF-8
bytes without a terminator, empty by default; display the id when unnamed.
CLI JSON formats the id as 12 lowercase hex digits. Names need not be unique.
Loop flags: bit 0 `realtime`; the other bits are reserved and zero. A loop id
can have gaps and is not its position in this list. Safe mode has zero running loops.
`running_instances` includes running loop instances, excluding the system.
The build id is the first 8 ELF SHA-256 bytes as 16 hex characters.
Flags: pending_reboot 1, layout_invalid 2, config_loaded_from_nvs 4,
safe_mode 8. Capture capacity is the largest capture ring in bytes (see Capture).

SAVE (5), REVERT (6), FACTORY_RESET (7), REBOOT (8), EXIT_SAFE_MODE (9)
wrap the core operations. Reset, reboot and exit safe mode reply, wait for
command TX to drain, then reboot. A drain timeout cancels the reboot and
logs the failure; a host may retry REBOOT. BenchCrash (254) responds and aborts only in an
explicit `CONFIG_CORE_BENCH_CRASH=y` test build. Normal builds reject it.

SET_NAME (10): `name_len u8 | name bytes`, empty success body. The body must
match the length exactly. Names are valid UTF-8 scalar sequences without
C0/C1 controls (U+0000..001F, U+007F..009F); empty clears the name. The limit
is bytes, not characters. No trimming, normalization or truncation. Invalid
names return InvalidArgument/BadValue without mutation. SET_NAME applies live
in RAM, including while armed, and sets `nvs_dirty`. SAVE persists it with the
configuration in the same A/B layout commit; REVERT restores it after its
parameter batches succeed. NoSavedConfig leaves it unchanged. FACTORY_RESET
clears it. Identity remains readable in safe mode.

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
  value_type u8 | flags u16 | limits_present u8 | min u32 | max u32 |
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
values in `defs.hpp`, including GPIO 0x20, RefTicksAfter 0x40 and, since v10,
Tune 0x0100: a live numeric or enum param that the host's Tune screen shows
(gains, and the enum that selects them; see
[adding-a-module.md](../adding-a-module.md)). Bits 9..15 are zero.
Retired definitions are absent. Module diag source IDs are resolved from
module type YAMLs when referenced by the diagnostic definitions; the retired
manager sources (`motor_manager` 0x10, `time_sync` 0x11) stay reserved.

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
CORE_SUBSCRIPTION_GROUPS (8, 1..31), CORE_COMMAND_GROUPS (8, 1..32), and CORE_GROUP_ENTRIES (64, 1..64). The effective entry
limit is min(configured limit, 64, floor((max_payload - 19)/4)), so both a
complete LIST record and telemetry fit. A 128-byte build advertises 27. The channel is
registry SubGroupChannelFirst + subscription ID, or CmdGroupChannelFirst +
command ID.

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

## Capture (family 6, F7a)

One capture records up to `limits.capture_refs` (system 0x0016, read-only u16,
`CORE_CAPTURE_MAX_REFS`, default 16, capped so STATUS fits the payload) signals
or inputs of one loop into a ring of `samples × count × 4` bytes. The ring is
allocated at CONFIG from PSRAM (internal RAM, at most 32 KB, on a board without
PSRAM) and kept until the next CONFIG. GET_INFO's `capture_capacity` is the
largest ring: the smaller of `CORE_CAPTURE_MAX_KB` and the largest free PSRAM
block at boot. Captures are not persisted, survive DISARM and record while
disarmed; a reboot leaves the capture unconfigured.

| Op | Request body | Reply body |
|---|---|---|
| CONFIG (1) | `loop u8, divider u16, samples u32, pre_trigger u32, trigger, count u8, count * Ref` | empty |
| ARM (2) | empty | empty |
| ABORT (3) | empty | empty |
| STATUS (4) | empty | see below |
| READ (5) | `first u32, count u16` | `first u32, count u16, count * n * value u32` |
| TRIGGER (6) | empty | empty |

The trigger is always 10 bytes: `kind u8, Ref, edge u8, value u32`. Kinds:
manual 0, testgen_start 1, value_change 2, threshold 3. Edges: rising 0,
falling 1. Fields a kind does not use must be zero. `testgen_start` fires on
the next due sample after generation starts on the capture's loop. `value_change`
fires when the trigger value's 4-byte slot differs from the previous sample's;
`threshold` fires when the previous sample is below (rising) or above
(falling) `value` and the current one reaches it, compared in the item's type
(f32, i32, otherwise unsigned). `value` must be a valid encoding of that type
(NaN/Inf and out-of-type values are rejected); it is not clamped to the item's
limits. The trigger ref is a bound signal or input of a running instance in the
capture loop, and need not be captured. Neither trigger can fire on the first
sample after ARM. Both are evaluated only on recorded samples, so at a divider
above 1 a value that changes and returns between two recorded samples is not
seen. TRIGGER forces the next sample to be the trigger in any kind.

CONFIG validates everything before it changes anything: the loop must be
running (`LoopMismatch`); divider, samples and count at least 1, `pre_trigger <
samples` (`BadValue`); count at most the limit (`BatchTooLarge`); refs unique
(`DuplicateRef`; more than 64 on the wire is also `BatchTooLarge`) bound signals or inputs of running instances in that loop
(group rules: `UnknownRef`, `WrongKind`, `LoopMismatch`, `DependencyMissing`);
the ring within the capacity (`Capacity`). A failed allocation answers
`NoMemory` and leaves the capture unconfigured. Captures in state Armed or
Triggered refuse CONFIG and ARM; ARM needs a configured capture; TRIGGER needs
Armed; READ needs Done. These state refusals answer InvalidArgument with detail
`WrongState` (46), without disturbing the recording. CONFIG, ARM and ABORT
answer `Busy/StagingBusy` in the unlikely case the capture's loop never lets go
of the capture between ticks; nothing changed. ARM in Done re-records into the same ring. ABORT returns any
state to Idle, keeping the configuration and discarding the recording.

States: Idle 0, Armed 1, Triggered 2, Done 3. While Armed the ring keeps the
latest samples. The loop records at the end of its tick (after the groups'
latch), one sample every `divider` loop executions, starting with the first
tick after ARM. The trigger sample is the first post-trigger sample. After
`samples - pre_trigger` post-trigger samples the capture is Done and the
CaptureDone event is sent. A trigger is accepted before `pre_trigger` samples
exist: the capture then holds fewer samples, and STATUS reports the real
trigger index. READ sample 0 is the oldest; at most 512 bytes of values per
reply (and within the payload limit, else `BufferTooSmall`); a range past the
recorded samples is `BadValue`.

STATUS reply (44 + 4 * count bytes): `state u8, flags u8 (configured 1,
irregular 2), loop u8, divider u16, samples u32, pre_trigger u32, trigger (10),
recorded u32, trigger_index u32, period_us u32, trigger_timestamp_us u64,
count u8, count * Ref`. An unconfigured capture has loop 0xFF, divider 1 and
no refs. `recorded` is the valid samples in the ring (0 when Idle).
`trigger_index` is 0xFFFFFFFF until triggered; while Triggered it is the
trigger sample's current index among the `recorded` samples, final at Done. `period_us` is the loop period
at ARM; samples are `period_us × divider` apart. `trigger_timestamp_us` is the
start time of the trigger sample's tick (0 until triggered): sample i is at
`trigger_timestamp_us + (i - trigger_index) × period_us × divider`.
`irregular` means that spacing may not hold: some tick since ARM started more
than 1.5 periods after the previous one (an overrun, a paused loop, a late
wake), the loop's rate changed, or a tick was lost to a concurrent ABORT. It
covers the whole time since ARM, including history no longer in the ring.

## Core events (0xC1, F7a)

Core events are reliable frames from the device: `version u8, event u8, body`.
Event ids come from the generated `CoreEvent` enum. The events are:
CaptureDone (1), once per completed capture: `recorded u32, trigger_index u32,
flags u8` (the STATUS flags). CONFIG, ARM and ABORT drop an unsent CaptureDone. A host
that was not connected learns the result from STATUS.

TestgenDone (2), once per terminated run: `loop u8, run u32, reason u8`
(8 bytes including the event header). Reasons: Completed 1, Stopped 2,
Disarmed 3. Each loop retains one pending terminal event; START is refused
with WrongState until the config task has consumed it, so a new run cannot
overwrite an unsent result. Transport failure is counted as ResponseDropped;
STATUS remains authoritative. Completion of one loop does not stop others.

## Test generator (family 7, F7b)

One generator per running loop. Only running f32 inputs flagged Testgen may
be targeted (currently cascade-controller setpoints and the mock duty input).
The generator configuration is volatile. START requires Armed and captures
the exact arm generation; DISARM, faults and the watchdog invalidate it.
Re-arming never resumes an old test. No generation feeds the watchdog.

| Op | Request body | Accepted response body |
|---|---|---|
| START (1) | `loop u8, Ref, waveform u8, offset f32, amplitude f32, frequency_start f32, frequency_end f32, duration_ms u32, repeat u32` (30 bytes) | `run u32` |
| STOP (2) | `loop u8` | empty |
| STATUS (3) | `loop u8` | 48-byte body below |

STATUS: `loop u8, state u8, reason u8, run u32, elapsed_ms u32,
repetitions u32, value f32, Ref, waveform u8, offset f32, amplitude f32,
frequency_start f32, frequency_end f32, duration_ms u32, repeat u32`.
States: Idle 0, Running 1, Done 2. Reason is None 0 until Done. Run ids are
per-loop u32 counters (wrap modulo 2^32), scoped to the current boot; correlate
events with loop and run. Idle has run/value/progress zero and default
configuration. Elapsed time and completed repetitions saturate at UINT32_MAX.
After disarm, STATUS reports Done/Disarmed immediately; the owning loop
publishes the terminal event when it next runs, which can be later if stalled.

Waveforms: Step 0, Square 1, Ramp 2, Sine 3, ChirpLinear 4, ChirpLog 5.
All values are offset plus amplitude times the following shape:

- Step: 1 from the first emitted sample through the run.
- Square: +1 for the first half-period and -1 for the second; 50% duty.
- Ramp: t/D over one duration D, starting at zero.
- Sine: sin(2*pi*f0*t), zero phase at start.
- Linear chirp: sin(2*pi*(f0*t + (f1-f0)*t*t/(2*D))).
- Log chirp: sin(2*pi*f0*D*expm1(log(f1/f0)*t/D)/log(f1/f0));
  equal frequencies use the ordinary sine limit.

t is device elapsed time within the current repetition; D is duration_ms/1000.
Duration must be positive; repeat 1 means once, 0 repeats until STOP/DISARM.
At a repeat boundary, phase restarts at t=0. At the final duration boundary
the run ends without emitting an extra endpoint sample: the last emitted
value is held. Missed ticks advance elapsed time, so they do not slow a
waveform. All parameters must be finite. Square/sine require 0 < f0 < half
the loop rate and f1=0; chirps require both frequencies positive and below
half the loop rate. Step/ramp require f0=f1=0. Clamp each output to descriptor
limits (and finite f32 range for an unlimited input).

START refuses an active run or pending completion with InvalidArgument/
WrongState, invalid target kind with WrongKind, an unflagged/non-f32 input
with Unsupported, cross-loop/unknown loop with LoopMismatch and invalid
parameters with BadValue. Disarmed START is NotArmed/WrongState. A disarm
racing activation is NotArmed/DisarmRaced. Refusals do not mutate the run.
STOP is idempotent. START/STOP apply on the loop through bounded staging;
Busy/Withdrawn guarantees no effect. Once claimed, wait for a definite result.
Unknown operation is InvalidArgument/Unsupported; fixed lengths are exact.

The loop suppresses command-group writes and input receipts for the active
target, retaining other entries and the frame's watchdog feed. Generation
runs before module ticks, updating the target and its receipt from the actual
write time. This participates in the controller's fresh-input mode contract.
On STOP/completion, the last emitted value remains until a command received
strictly after termination. Earlier mailboxes are discarded for that target
even if consumed later. A subsequent run on another target preserves the
old target's handoff boundary. Keepalives never change or refresh setpoints.
ARM/DISARM reset inputs as before; monitor emergency force is unchanged.

A testgen_start capture trigger requests the next due sample when the first
generated value is emitted on that capture's loop. A capture divider may
delay the recorded trigger. Other loops, repeat boundaries and captures
armed after the start do not trigger it. Capture keeps recording after STOP,
completion or DISARM.

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
testgen_running 16 (any loop effectively generating), safe_mode 32. Bits 6..7 are the
capture state (Idle 0, Armed 1, Triggered 2, Done 3). CPU utilization is measured from per-core idle runtime deltas (0..100 percent).
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

`core` is a discovered enum: `core_0` (0), `core_1` (1), `any` (2). New loop
instances default to `any`. This is the configured placement policy, not the
current CPU: Any core starts the task without affinity and lets FreeRTOS choose
initial placement. On ESP32-S3, ESP-IDF pins a task when it uses the hardware
FPU; the ordinary loop tick computes floating-point `dt_s`, so it normally pins
early. Automatic placement does not promise load balancing or continued migration,
and the selected core can change after reboot. SET stages a selection; SAVE and
reboot apply it. DISCARD clears pending edits; REVERT restores live params only.
Existing saved 0/1 values retain their placement. A saved record missing this
param uses the descriptor default, as other missing params do. The descriptor
type, enum choices, default and hash change; the existing enum/scalar wire
encoding and core version do not.

A blank store, factory reset or incompatible old layout creates control (id 0,
1000 Hz, real-time, core 1, priority 20) and sensor (id 1, 100 Hz, non-real-time,
core 0, priority 10), both with 4096-byte stacks. CORE_MAX_LOOPS=1 creates only
control. A valid saved layout can contain zero loops. CORE_MAX_LOOPS bounds loop
instances separately from CORE_MAX_INSTANCES, which bounds module instances.
The layout blob format is 3 (H1c); LP format 2 loads with an empty name.
Instance blobs retain format 1. Pre-F3d layouts
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
`version=10 | seq | family=1 | op=3`, with no body. Its response is the normal
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

The read-only, non-persisted u16 system params are `limits.loops` (16),
`limits.instances` (17), `limits.sub_groups` (18), `limits.cmd_groups` (19)
and `limits.group_entries` (20), plus `limits.link_values` (21, F4c) and
`limits.capture_refs` (22, F7a). Values come from the normalized runtime
configuration, overriding YAML defaults and stored data at boot. Instance
capacity counts modules separately from loop capacity. SET returns ReadOnly;
SAVE omits them all.

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
are discovered module behaviors with no wire encoding of their own.

## IMU module (F6)

The new [IMU contract](imu.md) uses ordinary descriptors, subscription groups,
paused actions and persistent parameters. The internal paused-action parameter
transaction changes no wire encoding. Core stays v5 and diag stays v3. The
legacy IMU messages/stream are removed and all 32 command channels are usable.
