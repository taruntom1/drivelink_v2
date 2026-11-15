# core

The self-describing firmware core: module registry, configurable loop instances,
params and actions, validated pending layouts, NVS A/B persistence and safe mode.
F4a adds preallocated groups on the configurable loops. The wire contract is
[core v9](../../../docs/protocol/core.md).

**The core is pure.** It includes no ESP-IDF or FreeRTOS header, no `sdkconfig.h` and no `CONFIG_` symbol. It uses only `core_os` (the OS adapters, NVS included), `protocol/` and the C++ standard library, so all of it but `src/idf/` also builds natively (`firmware/test/native`). Kconfig reaches it through `CoreConfig`, which `src/idf/core_config_idf.cpp` fills.

## API (namespace `core`, headers in `include/core/`)

| Header | What |
|---|---|
| `core.hpp` | `Core`: the facade. `init(module list)`, `boot(KvStore&)`, `start()` / `stop()`, the layout ops, `save()` / `revert()` / `factoryReset()` / `exitSafeMode()`, the state getters, `params()`, `findLoop(id)`, `loopAt(slot)`, `loopCount()`, `loop(id)` (known ids only). `kInitStage*` for `core.InitFailed`. |
| `core_config.hpp` | `CoreConfig` (`max_loops`; `max_instances`; `gpio_allowed_mask`; `crash_clear_delay_us`) and `coreConfigFromKconfig()` (IDF only) |
| `gpio_policy.hpp` | The ESP32-S3 pin policy (`kEsp32s3Allowed`) |
| `module_type.hpp` | `ModuleType {desc, create}` and `InstanceConfig` |
| `registry.hpp` | `Registry` (reserves system 0 and loop 5; rejects duplicates, a null `create`) and `descIsInTable()` |
| `instance.hpp` | `Instance` (`start`, `tick`), `InstanceContext` (the bind API and `ref()`), `InstanceState`, `TickContext`, `OnChange`, `kStartStage*` |
| `instance_table.hpp` | The records, one per configured instance plus the system's |
| `value.hpp` | `checkValue()`, `storeValue()` / `loadValue()`, `valueTypeOf<T>()` |
| `loop.hpp` | `Loop`: the instance list, `runTick()`, the heartbeat, the stats and the staging slot; `realtime()` |
| `param_service.hpp` | `ParamService`: `set()` (`stage()` + `complete()`), `get()`, `Ref`, `SetEntry`, `ParamDomain`, `nvsDirty()` |
| `pending_layout.hpp` | `PendingLayout`: the pending copy and its ops |
| `layout_validation.hpp` | `validateLayout()`, `LayoutEntry`, `refLinks()` (pure) |
| `wiring_order.hpp` | `creationOrder()`, `tickOrder()` (pure) |
| `config_blob.hpp` | The NVS blob codec and `crc32()` |
| `persistence.hpp` | `persist::loadConfig()`, `saveConfig()`, `eraseAll()`, `applyPairs()`, the NVS names |
| `safe_mode.hpp` | `decideSafeMode()` (pure) |
| `status.hpp` | `Result`, `Detail` and `Status {result, detail, index}` with `ok()` and `accepted()` |

## Boot

`main` constructs the `Core`, calls `init(types)`, then `boot(store)`, then `start()`. `boot`:
1. **Safe mode.** The crash counter (RTC no-init memory, `core_os/boot_info.hpp`) goes +1 on a crash reset (panic, interrupt or task watchdog) and back to 0 on any other. At 3 the device boots in **safe mode**: every configured record is `Offline`, nothing is created, no loop task starts, and `core.SafeModeEntered` is reported. Params can still be read and edited, and SAVE keeps the layout. Outside safe mode the counter is cleared `crash_clear_delay_us` (30 s) after the boot; in safe mode it is not, so the device stays safe while the crash loop goes on. `exitSafeMode()` clears it; the reboot is the caller's job, and any non-crash reset ends safe mode too.
2. **Load** the newest complete generation from NVS (below), or the factory layout: control and sensor loops, or control alone when max_loops=1. A valid saved empty layout stays empty. The system params apply at once.
3. **Validate** the layout (below). An entry that fails is left out (`Invalid`, `core.LayoutInvalid` with the detail, `layoutInvalid()`).
4. **Records.** Every entry gets a record, running or not, holding its shadow (the loaded values). An unknown type, a duplicate (type, inst) or an entry beyond the module or loop capacity gets none, so the next SAVE drops it.
5. **Create loops first.** Allocate their runtime and start tasks with tick callbacks gated. A failed task leaves its loop Failed and modules in it DependencyMissing. Other loops remain available. Then create modules in dependency order (a stable Kahn sort: dependencies first, then by layout index). A failed `create` / `start` is `Failed` (`core.InstanceStartFailed`); an instance wired to one that is not running is `DependencyMissing` (`core.DependencyMissing`, arg1 the missing one), transitively.
6. **Tick order.** Each loop gets its running instances in the wiring's tick order: a ref's target ticks before the instance, unless the ref is flagged `RefTicksAfter` (YAML `ref_order: after`). A controller with `feedback` and `actuator` (`after`) gives feedback → controller → actuator, whatever the layout order.

`start()` releases the tick gates after boot. It is idempotent while running;
after `stop()` reconstruct the Core to start again. A stopped Core does not
report a successful restart with no tasks.

Getters for F3b's `GET_INFO`: `safeMode()`, `layoutInvalid()`, `configLoadedFromNvs()`, `pendingReboot()`, `nvsDirty()`, `instanceCount()` (running), `configuredCount()`, `instancesFailed()`, `findRecord()`.

**Instances** bind their storage in `start(ctx)`:
- `ctx.bindParam(id, T*, OnChange = nullptr)`, `ctx.bindSignal(id, const T*)`, `ctx.bindInput(id, T*)`, where `T` is `bool`, `uint8_t`, `uint16_t`, `uint32_t`, `int32_t`, `float` or an enum class with a `uint8_t` base. Each bind is checked against the descriptor; `bindParam` writes the boot value into the storage.
- `ctx.param(id)` reads a boot value (structural params), `ctx.ref(id)` returns the instance a wiring param points at (nullptr when unwired), `ctx.fail(stage, err)` says why `start` failed.
- After `start`, every param, signal and input must be bound, or the instance failed (`core.InstanceStartFailed`, arg1 the stage).

**The system instance** is the core's own storage for the system params (`protocol/defs/system.yaml`, type `0x00`, instance 0). It runs in no loop and is not a layout entry.

## Loops and concurrency

- **Placement.** `loop.core` is the byte-sized enum `core_0=0`, `core_1=1`,
  `any=2`; new loops default to `any`. `Core::createLoop` explicitly maps it
  to OS affinity 0, 1 or `core_os::kNoAffinity`; invalid internal choices fail
  loop startup. Factory loops retain their explicit cores 1/0, and saved 0/1
  values keep their meaning. A missing stored pair follows the new descriptor
  default. Selection is reboot-required: SET stages, SAVE/reboot applies, and
  DISCARD cancels pending edits (REVERT restores live params only). Any core
  delegates initial placement to FreeRTOS; ESP-IDF's FPU use pins the task early
  because `runTick` computes float `dt_s`. It does not rebalance running loops.

- **The loops.** Each built-in loop instance owns config-task params and tick-owned statistic signals. Its id is the loop id, its label the name, and its own loop is kNoLoop. `core_loop_<id>` tasks use persisted rate, core, priority, stack and real-time settings. Configured loop count defaults to at most 4, configurable up to 8; sparse ids are supported. Capacity for modules (default 16) is separate. The factory control loop is 1 kHz, real-time, core 1, priority 20; sensor is 100 Hz, non-real-time, core 0, priority 10. Each stack defaults to 4096 bytes. Live rate changes restart the timer from the config task. Every real-time task subscribes to the Task WDT; paused actions on it are refused while armed. A tick longer than its period counts core.LoopOverBudget using the loop id as diagnostic instance. All loop stalls remain subject to F4b's unchanged disarm rule.
- **A tick:** the heartbeat, then the staged param batch, then a queued action, then group swaps/commands, each instance's `tick()` with `dt = elapsed × period`, subscription latching, then the stats. Instances tick in the wiring's tick order, fixed at boot (see Boot, step 6). Creation is always dependencies first.
- **The staging slot.** A param batch reaches a loop only through its one staging slot, an atomic state (Empty → Filling → Ready → Applying → Applied) that the config task and the loop move with stores and compare-and-swaps. The config task waits on a `core_os::Event` for up to 3 periods plus 5 ms, then withdraws the batch (Ready → Empty). The loop never waits for the config task.
- **The shadow.** The config task reads and returns param values from its own shadow copy, never from the storage the loop owns.
- **The config task.** `CoreLink`'s `core_cfg` task is the one caller of the core's configuration calls. Its large buffers live in `Impl`, not on its stack.
- **The status stream** is sent at `system.status.rate_hz`.

## Layout and the pending copy

A layout entry is `{type, inst, loop, label}` plus the instance's params. Changes go to the **pending layout** (`PendingLayout`), which SAVE writes and a reboot applies:
- `addInstance(type, loop, label, &inst)`: the device picks the lowest free instance id of the type (0..254). Params start at their defaults.
- `removeInstance(type, inst)`: refused while another entry is wired to it (`Referenced`).
- `setLabel`, `discardPending`, `getPending(refs, values)` (added instances and `reboot_required` params from the pending values, other params from the records).
- A `reboot_required` param SET, and any SET on an instance that exists only in the pending layout, goes to the pending values and answers `PendingReboot` with the applied values (`Status::accepted()`).
- A `reboot_required` **system** param is refused (`Unsupported`): the system instance is not a layout entry and has no pending copy. None exist today.
- The pending copy is made at the first op from the configured records. The whole layout is validated on every op; an op is undone and refused when an entry that passed before fails after it, or the entry it added or changed fails.
- Layout ops are refused while armed (`Armed`) and before `boot()`.
- `pendingReboot()`: the pending copy is active, or the last SAVE wrote it.

**Validation** (`validateLayout`, pure; the same code at boot and on every op): the type is registered (`UnknownType`); room (`Capacity`); (type, inst) unique (`Exists`); the loop is one the type allows (`LoopMismatch`); the label non-empty, at most 16 characters and unique (`BadLabel`, `LabelInUse`); every pin (`gpio` param, `0xFF` = none) allowed by `CoreConfig::gpio_allowed_mask` (`GpioNotAllowed`) and not used twice on the device (`GpioConflict`); every wiring value 0 (unwired) or `type << 8 | inst` naming an entry whose type is in the ref's category, the part of the type name before the dot (`BadRef`), in the same loop (`LoopMismatch`); no cycle in the ref graph or the tick graph (`WiringCycle`); and every entry it is wired to valid (`DependencyMissing`).

## Params

`ParamService::set` takes atomic batches of at most 64 entries (`kMaxBatchEntries`):
- Every entry is validated first: the Ref exists, it is a param (`WrongKind`), not `read_only`, not `locked_while_armed` while armed, and the value passes `checkValue` (clamped). A Ref may appear once.
- Every entry must have the same **domain** (else `CrossLoopBatch`): **system and live loop params** (applied on the caller; a rate change restarts the loop's timer, rolled back on failure with `core.TimerRestartFailed`), **one actual loop id** (live params of running instances: staged into the loop's slot, which the config task waits up to 3 periods plus 5 ms for, then withdraws), **offline** (an instance that isn't running: written to its shadow on the caller) or **pending** (above).
- `get()` and the applied values come from the shadow the config task keeps, never from loop-owned storage. `stage()` and `complete(timeout)` are public, so tests drive a batch without threads.
- `nvsDirty()` is set by an applied `persist` param or a pending op, and cleared by SAVE.

## Device identity

IDF CoreLink reads the immutable factory MAC using `esp_efuse_mac_get_default`,
checks errors, and injects six bytes into pure CommandService. Native callers
inject a stable id. `Core::setName` and `name()` belong to the config caller,
never loops/RX. SET_NAME changes RAM only, also armed, validates UTF-8 (0..16
bytes, no C0/C1 controls), and marks dirty. GET_INFO appends the id/name.
REVERT restores the saved name after parameter batches succeed; reset clears it.

## NVS A/B persistence

Layout format 3 includes the name in its CRC-protected commit record. LP
format 2 loads with an empty name, preserving records/params. New writes use
format 3; wire compatibility requires core v9.

- **Blobs** (`config_blob.hpp`, little-endian through `wire::Reader` / `Writer`, closed by a CRC-32):
  - instance: `fmt u8 | gen u32 | count u16 | count × (id u16, value u32) | crc32`, in namespace `t<type>i<inst>` (`t00i00` is the system), keys `p_a` / `p_b`
  - layout: `fmt u8 | gen u32 | count u8 | count × (type u8, inst u8, loop u8, label[16]) | name_len u8 | name bytes | crc32`, in namespace `core`, keys `layout_a` / `layout_b`
- **SAVE** (`save()`): refused while armed. Generation `newest + 1` goes to slot `gen & 1`: every instance blob (every `persist` param of every record, or of the pending layout when there is one), the system's, then the layout blob as the commit record. Then the namespaces of instances no longer in the layout are erased. A failed write reports `core.NvsWriteFailed` and answers `Fault` / `StorageFailed`; the previous generation stays intact, so a power loss at any point leaves it loadable.
- **Load**: the valid layout slot with the highest generation, with every instance blob (and the system's) of that same generation; else the other slot's generation; else the defaults with `core.ConfigLoadFailed` (not on a blank store). Unknown and retired ids and non-`persist` params are ignored, a missing id takes its default, an invalid value its default, and a value out of range is clamped.
- **REVERT** (`revert()`): reloads every live, persisted param of the configured instances and the system from the newest complete generation, through `ParamService::set`, per domain in batches of at most 64. `NoSavedConfig` when there is none.
- **FACTORY_RESET** (`factoryReset()`): erases `core` and every `t??i??` namespace. Refused while armed; the reboot is the caller's job.

## Kconfig

| Option | Default | Range |
|---|---|---|
| `CORE_MAX_INSTANCES` | 16 | 1–64 |
| `CORE_MAX_LOOPS` | 4 | 1–8 |
| `CORE_CAPTURE_MAX_REFS` | 16 | 1–64 (also capped so a capture STATUS fits the payload) |
| `CORE_CAPTURE_MAX_KB` | 4096 | 32–16384 (the largest capture ring) |

`CoreConfig::gpio_allowed_mask` is the ESP32-S3 policy minus the uart_comm TX/RX pins (`CONFIG_UART_COMM_TX_PIN` / `_RX_PIN`). `crash_clear_delay_us` is 30 s.

## Sources (F4c)

Modules bind each source with `ctx.bindSource(id, &sample)`, where sample is
`Sampled<T>` (`core/sampled.hpp`): value, age_us, valid and fresh. This also
binds the generated value/age signals. Binding the three items individually
is insufficient. Failed/disconnected producers yield invalid samples.

`SourceService` resolves and allocates after module startup, before loop
tasks are enabled. Same-loop edges affect tick order, not creation or
dependency order; sources sample immediately before the consumer ticks.
Each tick entry caches its consumer index at boot, so it visits only that
instance's same-loop bindings, never bindings belonging to other loops.
Cross-loop edges group by ordered loop pair and deduplicate target refs.
The producer uses `BoundSignal`/`latchSignals` for end-of-tick snapshots.

Each pair owns three arrays and one lock-free atomic middle index with a
dirty bit. Producer and consumer exclusively own their back/front arrays;
acquire/release exchanges transfer complete arrays through the middle.
Neither side retries, waits or locks. The consumer retains the last snapshot
and updates age until a new one arrives. Fresh is true only when taking a
new snapshot; same-loop copies are fresh every tick. Producer tick-start time
defines age; valid ages saturate at 0xFFFFFFFE, invalid age is 0xFFFFFFFF.
Arm transitions preserve sample storage.

`CORE_MAX_LINK_VALUES` and `CoreConfig::link_values` cap distinct values per
pair, default 32 (range 1 to 64), advertised by `limits.link_values`.
Same-loop copies consume no pair capacity. Three packed arrays cost 12 bytes
per value, plus timestamps, resolved bindings and consumer mappings. All
storage is allocated at boot. Sources may target built-in loop statistics,
sampled on the owning loop as for subscriptions.

## Rules

- **Pure.** No IDF, FreeRTOS, `sdkconfig.h` or `CONFIG_` outside `src/idf/`. Anything OS-specific goes through `core_os`.
- **Real-time paths:** the loop tick, the staging swap and the `OnChange` hooks take no lock, don't log, allocate or block, and do bounded work.
- **Allocate only at setup or configure time:** the `Core` constructor, `boot()`, the layout ops, SAVE, REVERT and FACTORY_RESET. A live param write allocates nothing.
- **Flash stalls both cores.** SAVE and FACTORY_RESET write NVS, which is why they are refused while armed.
- **No logging.** The core reports through the `core_os` diag sink; `main` logs the startup summary.
- Diag args are wire contract: the stages and causes (`kInitStage*`, `kStartStage*`, `persist::kNvsOp*`, `persist::kLoad*`) are documented in `protocol/defs/diag_codes.yaml` and never renumbered. `core.LayoutInvalid` carries a `Detail`, which F3b numbers.

## F3b command and action services

`CommandService` is pure and config-task-owned, with fixed scratch storage.
`CoreLink` is its IDF UART adapter: a preallocated command queue and core_cfg
worker, bounded RX callback, and latest-wins status stream. Its Kconfig settings
are CORE_COMMAND_QUEUE_DEPTH (8), CORE_COMMAND_TASK_STACK (6144) and
CORE_COMMAND_TASK_PRIO (5). See docs/protocol/core.md for the wire contract.

`Core::runAction` validates metadata and transfers ownership through a fixed
per-loop action slot. A paused action waits for acknowledgement before touching
module state. All core configuration calls require one caller at a time.

CPU utilization sampling uses CORE_STATUS_CPU_LOAD (default on), selecting
FreeRTOS runtime stats. ESP timer idle counters are sampled once per status
packet with constant work per core; no kernel stack scan occurs. A first or
unavailable sample is 0xFF. CpuLoadSampler handles unsigned counter wrap and
clamps delayed idle-accounting deltas. The pure diagnostic count bridge accepts
an explicit instance, just like record reporting.

## Native real-time guard

Every real-time runTick enters a per-thread guard, covering staging hooks and
loop actions too. C++ new/new[] (aligned/nothrow included), project C allocator
calls (malloc/calloc/realloc), and blocking core_os Mutex/Event/KvStore calls
count violations. Native tests install a failure callback invoked outside the
outer guard; tests deliberately provoking violations opt out explicitly. Test
recorders reserve their logs during setup. Vdev counts violations and reports
changes on stderr. IDF uses a no-op guard; direct IDF blocking calls remain a
review responsibility. Native Event::signal remains an allowed wake hint.
Instance blobs retain format 1; layout format 2 retires pre-F3d layouts.

## Groups (F4a)

`groups()` exposes GroupService: configure/enable/clear, config-owned resource
metadata, bounded RX command mailboxes, and latest subscription samples.
Defaults are eight subscription groups and eight command groups, each with up
to 64 refs. Kconfig CORE_SUBSCRIPTION_GROUPS (1..31), CORE_COMMAND_GROUPS
(1..32) and CORE_GROUP_ENTRIES (1..64) flow through CoreConfig. Registry counts
are statically checked. The effective entry limit is capped by the payload
(22 + 4*n bytes for a complete LIST record), so 128-byte builds advertise 26
entries. All storage is allocated at construction, including
two resolved binding tables per slot and one config scratch table. Memory is
larger than just the packed wire values: bindings, staged configurations,
metadata and transport mailboxes also consume RAM.

Loop::runTick swaps group config and consumes command mailboxes after actions,
then latches due subscriptions after module ticks and before updating stats.
Loop-statistic storage is sampled only by that loop. `latchSignals` is the
bounded scalar-list helper intended for F4c's source snapshots.

Each slot uses a single-attempt atomic ownership claim for tick/RX/transport
access; contenders drop work without waiting. Configuration uses a separate
Ready/Applying/Applied handshake, with definite withdrawal like params. Loop
routing is rechecked after a claim; a cross-loop rebind hands off through the
old owner. No steady-state path allocates, spins or takes an OS lock. The
config caller owns LIST shadows; readers never touch loop-owned scalar storage.
Telemetry can skip a tick when its slot is busy, and pending samples overwrite
older samples. Unsent packets already copied to a transport can retain the old
epoch; consumers must filter it.

F4b connects the device arm state and ARM instant to beginTick. Transitions
reset inputs and reject pre-arm values. lastCommandUs uses one nonblocking slot claim and returns
false when unavailable, with the output unchanged. A monitor must distinguish
that unavailable snapshot from a successful timestamp of zero (no frame), and
must not conclude timeout from contention. No 64-bit atomic is required on ESP32.


## F4b safety ownership

`Core::arm()`, `disarm()`, `acknowledgeFault()`, `armStatus()` and
`lastDisarmReason()` implement the arming contract in docs/protocol/core.md.
`ArmState` packs the commit state, fault latch and disarm epoch in lock-free
32-bit atomics; ParamService reads the same armed bit. The ARM instant is
published as two atomic words and used only with a coherent state snapshot.
The config caller serializes ARM and ACK; RX and diag can disarm concurrently.

Loop-owned transition hooks reset bound inputs before commands are consumed.
Module `onArmTransition` hooks run on their owning loop, without allocation,
blocking or logging. `emergencyStop` is callable concurrently from the monitor
and shutdown: actuators must use a hardware force or atomic state, never a
write to ordinary loop-owned storage. The mock's force is atomic and independent
of its duty input. A monitor stop gate serializes ARM commits with applying
forces, and the force is repeated while disarmed to cover stale loop releases.

The boot-built actuator list and loop table are immutable while monitoring.
The monitor owns heartbeat observation times; all other inputs it reads are
atomic or a bounded GroupService receipt snapshot. It does no OS wait and
allocates nothing. The timer's stop fences callbacks before Core storage is
released. The single-device diagnostic observer is installed before enabling
loops and detached after stopping them, with in-flight callbacks fenced.

The core initializes read-only limit shadows from its normalized configuration
both before system binding and after NVS loading. They are never persisted.
BenchStall deliberately violates the tick budget only when a bench-enabled
CommandService admits the test request; it is not a deployment operation.

Diagnostic report-slot exhaustion does not change safety recovery: fixed
per-code/instance overflow bits retain active conditions until cleared.

Before raising a timed-out or stalled condition, the monitor CAS-claims the
exact arm generation it evaluated. A concurrent disarm/re-arm discards the old
observation, so the new ARM always receives its own watchdog grace interval.

## F5a-1 optional interfaces and input receipts

`Instance::queryInterface(uint32_t)` is a startup-only C++ extension point;
unknown IDs return null. Interface IDs are not wire values. Motor modules use
it to resolve compatible actuator/feedback APIs without RTTI or core coupling.

`bindInput` optionally accepts an `int64_t*` for the last applied input's
receipt timestamp. GroupService copies validated post-ARM value-frame RX times
on the owning loop. Testgen writes its actual generated-sample write time on
that loop. Keepalives do not update it.
ARM/DISARM reset the timestamp to zero with the input; sources are unaffected.
No RX/monitor thread accesses this plain binding storage. The existing monitor
receipt API and exact-generation stop claim are unchanged. Motor controllers
compare the timestamp strictly against their mode-application instant, so an
old queued input cannot bypass a mode switch's fresh-command requirement.

## Paused-action parameter updates (F6)

`ActionOut` may request up to 24 `(own item id, value bits)` updates. They are
internal C++ metadata and are never serialized. Only a successful acknowledged
paused action can commit them. `ParamService::applyPaused` verifies ownership,
all IDs/types/ranges, uniqueness, writability and armed restrictions before any
mutation; structural, read-only and hooked params are rejected. The caller
holds the owning loop paused, updates bindings plus mutex-protected shadows
and sets NVS dirty before releasing it. Hooks remain owning-loop operations.
Loop actions cannot request these updates.

Calibration uses ordinary persistent typed params, so SAVE's existing complete-
generation A/B transaction, pending-layout origin values, REVERT, removal and
factory reset require no new blob format. Details: `docs/protocol/imu.md`.

## Capture (F7a)

`CaptureService` (`core/capture_service.hpp`, `Core::capture()`) is the one
capture of the device; the wire contract is in `docs/protocol/core.md`.

- **Memory.** Bindings are fixed arrays for 64 refs; `capture_refs` only
  limits a capture. The ring is allocated at CONFIG through `core_os::captureAlloc` (PSRAM,
  or 32 KB internal without it) and freed only by the next CONFIG or the
  destructor. `capacityBytes()` is fixed at construction from
  `core_os::captureCapacity(capture_max_bytes)`.
- **Recording.** `Loop::runTick` calls `sample(loop, tick_start, period)` after
  the groups' latch. The capture's loop records only in Armed/Triggered, under
  a single-attempt claim: it never waits, allocates or logs, and a lost claim
  (only a concurrent ABORT) marks the capture irregular. The loop alone moves
  Armed → Triggered → Done.
- **Config side.** CONFIG and ARM refuse an active capture before touching the
  claim, so a refusal never costs the loop a tick; ARM publishes Armed after
  releasing it. CONFIG, ARM and ABORT take the claim by spinning, then with a
  few bounded sleeps (`Busy/StagingBusy` if the loop never lets go). The configuration, ring and
  loop-owned counters are written only while the loop leaves them alone (Idle
  or Done) and are published by the Armed release store. TRIGGER only sets an
  atomic request. STATUS reads atomics; the trigger timestamp is read only
  after an acquire of Triggered/Done. READ is allowed only in Done.
- **Events.** `takeDone()` returns a completed capture once (Done is stored
  before the pending flag); `captureDoneEvent()` encodes it and
  `captureStatusBits()` gives status bits 6..7. `CoreLink` and the vdev send
  the event on 0xC1 from their config task.
- `bindSignal()` (group_service) is the shared ref resolution of groups and
  capture.

## Test generation (F7b)

`Core::testgen()` exposes `TestgenService`, one preallocated slot per loop
capacity entry, routed by the running loop id (including sparse ids). Targets
are bound f32 inputs flagged Testgen. Wire fields and waveform equations live
in `docs/protocol/core.md`.

- The single config caller validates/binds START, captures the arm generation,
  and publishes a fixed request with a Ready release store. The owning loop
  claims Ready, applies START/STOP before command consumption, and acknowledges
  Applied after the generation step. A Ready timeout can withdraw with a
  definite no-effect result; a claimed request waits for definite completion.
- Inputs, receipts, active waveform state and each binding's handoff gate
  belong only to the owning loop. Groups suppress the active target's value
  and receipt while still applying other entries and feeding the watchdog.
  After STOP/completion, commands received at or before termination remain
  suppressed, even if their slot was busy; switching targets preserves the
  previous target's boundary. Generation follows groups and precedes module
  ticks. Its receipt uses the actual write time to honor controller mode
  boundaries. ARM/DISARM reset inputs/gates; old arm generations never resume.
- Runtime snapshots contain only internal-RAM, word-sized atomics. A bounded
  sequence read returns Busy/StagingBusy if it cannot obtain a coherent view.
  Effective state/running flags also check the current arm generation, so a
  stalled loop cannot advertise an old run as active after disarm.
- Each terminal snapshot is immutable while its pending flag is set. The loop
  sets that flag before publishing live Done; START refuses until the config
  caller consumes the event. `testgenDoneEvent` sends loop/run/reason from the
  config task. STATUS remains authoritative if transport delivery fails.
  Repetitions restart waveform phase without retriggering capture. The first
  emitted sample requests capture's same-loop `testgen_start` trigger, and the
  next due recorded sample consumes it. Capture continues after termination.

No steady-state allocation, OS wait, spin, logging or watchdog feed occurs in
generation. Disarm retains the monitor's independent emergency force. XiP is
an isolated evaluation profile, off in normal firmware; all shared state stays
internal even when code/rodata are mapped into PSRAM.

## Time (F8)

CoreLink answers time protocol v1 directly on the UART RX task, including in
safe mode. A valid ping queues an exact reply with RX time and requests a
TX-task timestamp/CRC patch. Malformed/version-mismatched requests increment
`core.BadTimeRequest`; an exhausted command pool increments `ResponseDropped`.
Neither path blocks, feeds the watchdog, allocates or starts a task. Callback
registration is reset before captured state is destroyed. The pure codecs and
wire boundaries are in [time.md](../../../docs/protocol/time.md).

## OTA (F9)

`Core::ota()` exposes the config-caller-owned `OtaService` (`ota_service.hpp`).
BEGIN starts the inactive slot, WRITE appends or checks exact duplicates,
FINISH verifies the complete-file SHA-256 and prepares a validated image, and
ABORT releases it without writing boot metadata. Only `commit()` selects it,
called by explicit SYSTEM REBOOT after its reply drains and followed by an
immediate restart. EXIT_SAFE_MODE and other reset paths do not commit.

The ARM gate covers Receiving/Ready/Error, selected-boot uncertainty and a
pending running candidate. Only successful cleanup/confirmation or reset can
resolve it. ABORT refuses already selected or pending images. STATUS remains
readable on error; all other OTA state belongs to the config caller. The
separate atomic gate is safe for status queries, stays in internal RAM and
does not add work to loop ticks. No new task is created.

During a transaction, SAVE/REVERT/FACTORY_RESET, layout and wire PARAM SET
mutations, capture ARM and testgen START are refused. BEGIN refuses active
capture/testgen. The OS adapter owns flash/image validation; fixed upload/read
scratch lives on the heap-owned service, not a task stack. Native tests cover
preparation/cancellation/reset, uncertain commit before/after metadata change,
rollback/confirmation and exact wire/error behavior. The authoritative
[wire contract](../../../docs/protocol/ota.md) includes offset error bodies.
