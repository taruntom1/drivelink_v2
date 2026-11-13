# Plan: firmware architecture v3 (self-describing core)

Status: draft, 2026-11-10 (revised after review). Nothing here is built yet.

**Baseline.** This plan assumes:
- `main` at `b72655b`
- the whole `diag` plan, phases 1–3 (error codes over UART, the `Boot` event, `GetInfo`, `diag_codes.yaml` with its generator, the `detail` byte, motor and IMU configure protocols at v4)
- the repo restructure (step S1, detailed in [host-architecture.md](host-architecture.md)): the firmware lives in `firmware/`

The work runs on the `firmware_overhaul` branch, one session and one PR per step. [overhaul-status.md](overhaul-status.md) holds the step list, the current state and the decision log.

All paths below are relative to the repo root.

The firmware is rebuilt around a **core** that describes itself, so the host software can be generic. The host is written only after this plan is done, except for a minimal C++ client that grows alongside the firmware phases (see Phases). [host-architecture.md](host-architecture.md) describes the host side.

## Goals

1. **The host learns everything from the device.** At connect it reads which modules exist, their params, signals, inputs, actions and diag codes (name, type, unit, range), and their current values. No host code is specific to a module.
2. **Adding a module is local.** A new sensor, actuator or control law means one firmware module plus its YAML. No protocol handler, GUI or ROS code.
3. **The configuration lives on the ESP**, survives reboots, and is edited live from the host.
4. **Safe by default:**
   - the device boots disarmed
   - a watchdog and a monitor that is independent of the loops stop the outputs when the host or a loop is lost
   - faults disarm the outputs
   - a layout that crashes at boot cannot brick the board
5. **Good tuning tools**: on-device capture at the full loop rate, and built-in test signals.
6. **Keep the existing rules.** The real-time rules, the error rules and the "no allocation after configuration" rule in `firmware/CLAUDE.md` all still apply.

## Decisions

| Topic | Decision |
|---|---|
| Structure | A **core** component and a **module registry**. The core owns all protocol handling, the loops, the layout, persistence, arming, groups, capture, test signals, time and OTA. Modules register a factory, never touch the UART, and declare what they expose. |
| Data model | Self-describing. For each module type: **params** (configuration), **signals** (readable values), **inputs** (values the host commands) and **actions** (operations with arguments). The device serves the descriptors, including the descriptors of the diag codes. |
| Definitions | YAML per module type in `protocol/defs/`. One generator turns them into pure C++ headers for `protocol/` (used by the firmware and the host) and into the docs. It also takes over the MSG_TYPE registry and `diag_codes.yaml`. |
| Ids | **One `u16` id space per module type**, shared by params, signals, inputs and actions. The generator enforces unique ids, and the descriptor says which kind each item is. Ids are never reused (retired ids stay in the YAML). An item is addressed as `Ref = (type u8, instance u8, id u16)`. System items use type `0x00`, instance 0. |
| Values | Every param, signal and input value is a 4-byte slot: bool, u8, u16, u32, i32, f32 or enum (u8 with named values in the descriptor). Larger items go through actions. |
| Param writes | Atomic batches. If any entry is invalid, nothing is applied, and the response names the failing index with result + detail. Live params in one batch must belong to a single loop. Each loop has **one staging slot**. The response is sent only after the loop has swapped the batch in, or after the batch was withdrawn. |
| Layout | Structural params are flagged `reboot_required`. `ADD_INSTANCE` and `REMOVE_INSTANCE` are system ops. Layout changes are written to NVS and take effect at the next boot. Each instance has a 16-byte label, unique on the device. |
| Persistence | Explicit `SAVE`, `REVERT` and `FACTORY_RESET`. NVS stores (id, value) pairs per instance, in A/B slots with a generation number. `SAVE` is refused while armed. |
| Loops | Loops are part of the layout. Each loop is an instance of the built-in `loop` type, and its rate, core, priority, stack and `realtime` flag are its params (at most 4 loops by default). The factory layout has `control` (1 kHz, core 1, real-time) and `sensor` (100 Hz, core 0, bus I/O allowed). Each module type declares whether it needs a real-time loop (`realtime: required`, `forbidden` or `any`), and the layout assigns each instance a loop (stored in its layout entry). An instance reads a signal from another loop through a **source**: a sampled copy that carries its age. See "Configurable loops, the real-time flag and sources". |
| Control | Motor control is split into **Actuator**, **Feedback** and **Controller** module types, wired together by the layout. Today's code becomes `actuator.pwm_dir`, `feedback.quadrature` and `controller.cascade_pid`. |
| Mode changes | Allowed while armed, **bumpless**. Hold previous duty for the transition tick within current limits; initialize from measured feedback and preset integrators. Require an active-input value frame received strictly after mode application; keepalives and old queued values cannot release the wait. |
| Telemetry | **Subscription groups.** Each group is bound to one loop, runs at a divider of that loop's rate, and packs its values with no per-value ids. Each group has its own stream channel and a config epoch. |
| Commands | **Command groups**, packed the same way. Host → ESP stream channels, applied at the start of the loop tick. Only valid frames received after `ARM`, carrying the current epoch, feed the watchdog. |
| Group memory | All groups are allocated at their maximum size at boot and never freed. A new group configuration is swapped in through the staged path. |
| Actions | A generic action table: up to 4 scalar args, plus an optional blob of up to 512 B in either direction. The YAML says where each action runs: `loop` (handed to the loop through a mailbox and run at the start of the tick, so bounded and real-time safe) or `paused` (on the config task, with that instance's loop paused). |
| Safety | System arm/disarm. The device boots disarmed. `DISARM` is handled in the RX callback. A **safety monitor** that is independent of the loops checks the loop heartbeats and the command watchdog, and forces the actuators to zero. On every arm and disarm transition, inputs and controllers are reset. Faults latch, watchdog trips do not (see Arming and safety). |
| Boot safety | After repeated crash resets, the device boots into **safe mode**: no instances, reported in `GET_INFO`. |
| Status | A system status stream (default 10 Hz) that also serves as the heartbeat. |
| Time | Every timestamp is esp_timer µs since boot. The ESP never sets its clock. The host sends pings and maps ESP time to its own clock. The ping's TX time is stamped by the TX task when the frame is written. |
| Tuning | Generic capture into PSRAM, allocated only by explicit request. A test signal generator drives velocity setpoints, position setpoints or raw duty. |
| Link | `LEN` becomes a `u16`, followed by a **header CRC8** over MSG_TYPE and LEN. `MAX_PAYLOAD` is 1024: a protocol constant, and also the hard upper bound for the host's scanner. Bulk transfers use chunks of 512 B or less. The default baud rate is 921600, still fixed at compile time. The UART ISR runs from IRAM. |
| Updates | OTA over the protocol, allowed only while disarmed, with sequential writes, idempotent chunks, a dual OTA partition table and rollback. The NVS partition keeps its offset and size. |
| Testing | The core's logic is pure, behind thin OS adapters. The whole core also builds natively as a **virtual device** with mock modules, which serves as the host's simulator. |
| Dev client | A minimal C++ `libdrivelink` slice and `drivelink-cli` start in F3 and grow with each phase. H1 completes them for the GUI and ROS. |
| Migration | Clean break. Each manager is ported to the core, and its typed protocol is deleted in the same PR. |
| Docs | Every phase updates `docs/protocol/`, `CLAUDE.md`, `firmware/CLAUDE.md`, `firmware/components/CLAUDE.md`, the READMEs of the components it adds, and `plans/overhaul-status.md`, in the same PR as the change. The legacy component READMEs and `review/` were removed in `f4de709` and are not recreated. |

## Concepts

| Term | Meaning |
|---|---|
| Module type | A kind of module, such as `actuator.pwm_dir` or `imu.bno055`. It has a `u8` type id (`0x00` is reserved for system) and one YAML file. |
| Instance | One configured module of a type. It has a `u8` instance id (unique within its type) and a 16-byte label (unique on the device). |
| Item | A param, signal, input or action of a module type, with an id unique within that type. |
| Param | Configuration, read and write. Flags: `reboot_required`, `persist` (default on), `read_only`, `locked_while_armed`. |
| Signal | A value read in a loop tick: measurements, estimates and controller internals (error, P/I/D terms, integrator). |
| Input | A value commanded by the host through a command group, such as a velocity setpoint or a duty. Inputs can also be captured and subscribed to like signals. |
| Action | An operation with arguments and a result, such as `zero_position`, `reset_integrators` or `calibration_read`. |
| Loop | A periodic task owned by the core, configured as an instance of the built-in `loop` type. Its instance id is the loop id. Instances run in a loop, and groups attach to one. |
| Real-time loop | A loop whose `realtime` param is true. The real-time rules apply to every tick in it, and only it may hold types that are `realtime: required` (actuators, feedback, controllers). |
| Source | A param of a consumer instance that names a signal (or input) of another instance, in any loop. The core gives the consumer a sampled copy with its age and a valid flag. |
| Group | A subscription group (signals or inputs, ESP → host) or a command group (inputs, host → ESP). Each is bound to one loop. |
| `Ref` | `type u8 \| instance u8 \| id u16`, 4 bytes on the wire. The id alone says which kind of item it is. |

Wiring between instances (which controller uses which actuator and feedback) uses `Ref`-typed params. These are `u32` params flagged `reboot_required`, which hold `type << 8 | instance` in the low 16 bits with the upper 16 bits 0; `0` means unwired, which validation allows (the module's `start()` decides whether it needs the ref). A ref's `ref:` key names a category, the part of the type name before the dot (`actuator` holds `actuator.pwm_dir`). **Wired instances must run in the same loop**, and layout validation enforces that. Creation is always dependencies first; the tick order follows each ref's `ref_order` (`before`, the default: the target ticks first; `after`: it ticks after), so a controller's `actuator` ref is `ref_order: after`.

Wiring gives a module a pointer to the other instance, so it only works inside one loop. To read a value from an instance in **another loop**, a type declares a **source** param instead (F4c; see "Configurable loops, the real-time flag and sources").

A pin is a `u8` param flagged `reboot_required` with `gpio: true` (`ItemFlag::Gpio`). `0xFF` means no pin; it is the default and claims nothing.

## Firmware structure

| Component | Role | Comes from |
|---|---|---|
| `uart_comm` | The transport: driver, RX/TX tasks, command FIFO, stream slots, callbacks. Framing and the scanner come from `protocol/`. | today's `uart_comm` |
| `protocol` (in `/protocol`) | Framing, CRCs, `wire_codec`, generated definitions and codecs. Pure C++. | extracted from `uart_comm` and the managers |
| `diag` | Fault codes, counters, latching, reporter. Moves to v2 (see below). | the diag plan |
| `core` | Registry, loops, the safety monitor, layout and NVS, the param service, groups, capture, test signals, arming, status, time ping, OTA, and every core protocol handler. | new; absorbs the protocol parts of `motor_manager`, `imu_bmo_055` and `time_sync` |
| `core_os` | Thin adapters for tasks, timers, queues, mutexes, NVS and the clock: an ESP-IDF version and a native version. | new; builds on `rtos_util` |
| `mod_motor` | The `actuator.pwm_dir`, `feedback.quadrature` and `controller.cascade_pid` module types. | `motor_controller`, plus the module parts of `motor_manager` |
| `mod_imu_bno055` | The `imu.bno055` module type. | `imu_bmo_055` without its protocol and task code. This also ends the `bmo` misspelling. |
| `motor_hal`, `rotary_encoder_hal`, `pid` | Unchanged hardware wrappers and math. | unchanged |
| `mod_mock` | Mock actuator, feedback and IMU types for the virtual device and for tests on hardware without motors. | the mock modes of today's managers |

`motor_manager`, `imu_bmo_055` and `time_sync` are removed once their phases are done. The GPIO policy from `motor_manager` (`gpio_policy`) moves into the core as a GPIO registry (see Layout).

### Module API (sketch)

```cpp
namespace core {

// One per module type. main passes the list to the core explicitly (no static
// self-registration, which the linker would drop).
struct ModuleType {
    const defs::TypeDesc& desc;          // generated: items, real-time requirement, action placement
    Instance* (*create)(const InstanceConfig& cfg);   // called at boot only; may allocate
};

class Instance {
public:
    virtual ~Instance() = default;
    // Boot: bind params, signals and inputs, read structural params through ctx,
    // claim GPIOs through ctx, acquire hardware. On failure call ctx.fail(stage, err)
    // and return false; the core reports core.InstanceStartFailed.
    virtual bool start(InstanceContext& ctx) = 0;
    // Loop tick (in the instance's loop). Real-time rules apply in every real-time loop.
    virtual void tick(const TickContext& t) = 0;
    // Param, signal and input access is table-driven: the instance binds generated ids
    // to its storage in start() (ctx.bindParam(defs::controller_cascade_pid::kVelKp, &m_vel.kp, onChange)),
    // so the core can copy staged values in, and sample signals out, without virtual calls per item.
    // Actions: `loop` actions run here, at the start of the tick; `paused` actions run on the
    // config task while this instance's loop is paused.
    virtual ActionResult runAction(uint16_t action_id, const ActionArgs& in, ActionOut& out) = 0;
    // Arm and disarm transitions, called in the loop at a tick boundary. Reset inputs to hold
    // values (velocity and duty 0, position = measured position), reset integrators and filters.
    virtual void onArmTransition(bool armed) {}
    // Actuators only. Called by the safety monitor from the esp_timer task, never from the loop.
    // Must be non-blocking, take no lock, and force the output to zero in hardware
    // (for pwm_dir: a continuous force-low of its one PWM generator; DIR is left alone).
    virtual void emergencyStop() {}
};

}
```

**The order of work in each loop tick**, all bounded and fixed-size:
1. Increment the loop heartbeat, which the safety monitor reads.
2. Handle the arm state:
   - On a transition, call `onArmTransition()` on every instance in this loop, and clear the command mailboxes.
   - While disarmed, actuators output zero.
3. Swap in the staged param batch for this loop, if there is one, and signal the config task. A `mode` change goes through the controller's bumpless switch here.
4. Run the queued `loop` actions.
5. Apply the latest command group values from the mailbox to the bound inputs, if armed and the frame arrived after `ARM` with the current epoch. Then take the latest snapshot of every source link that ends in this loop (F4c).
6. The test signal generator overrides its target input, if it's running.
7. `tick()` each instance in this loop, in dependency order (feedback, controller, actuator).
8. Latch the subscription groups whose divider is due, record the capture sample, and publish this loop's snapshot for every source link that starts in it (F4c).
9. Update the loop statistics (overruns, run time).

Instance tick order comes from the wiring (`Ref` params) and is computed once at boot.

## Definitions (YAML)

`protocol/defs/` holds:
- `registry.yaml`: MSG_TYPEs and stream channel ranges. Module type ids live in each type's own YAML (`type_id`); the generator checks they are unique, and `0x00` is reserved for `system`.
- `diag_codes.yaml` (moved from the diag component)
- `system.yaml`: system items such as the watchdog timeout and the status rate (type `0x00`), and from F4b the read-only `limits.*` params (see "Limits" under the notes on the ops). The loop rates were here until F3d; their ids are retired.
- `loop.yaml` (from F3d): the built-in `loop` type, whose instances are the loops
- one file per module type

`tools/gen/` replaces `tools/gen_diag_codes.py`. It writes three things:
- `protocol/include/drivelink/protocol/gen/*.hpp`: constexpr descriptor tables, id constants, enums, and `static_assert`s for unique ids per type and for MSG_TYPE ranges
- `docs/protocol/*.md`: the generated tables, between marker comments
- the Python table that diag's host tooling uses today, kept only while `firmware/test/uart_comm` needs it

The generated files are checked in, and CI fails if they are stale. The **descriptor hash** is a CRC32 that the generator computes over the canonical form of all the definitions (registry, diag codes and module types), so it changes exactly when a definition changes. It is emitted as `kDescriptorHash` (see `docs/protocol/definitions.md`). It is reported in `GET_INFO`, so the host can tell when the firmware's definitions changed.

```yaml
# protocol/defs/controller.cascade_pid.yaml
type: controller.cascade_pid
type_id: 0x20
realtime: required        # F3d; `loops: [control]` before it
items:
  # params
  - {id: 0x0001, kind: param,  name: actuator,      type: u32, flags: [reboot_required], ref: actuator, ref_order: after}
  - {id: 0x0002, kind: param,  name: feedback,      type: u32, flags: [reboot_required], ref: feedback}
  - {id: 0x0010, kind: param,  name: mode,          type: enum, values: {open_loop: 0, velocity: 1, position: 2}, default: open_loop}
  - {id: 0x0020, kind: param,  name: vel.kp,        type: f32, unit: "1/(rad/s)", min: 0, max: 1000, default: 0}
  - {id: 0x0021, kind: param,  name: vel.ki,        type: f32, unit: "1/rad",     min: 0, max: 1000, default: 0}
  - {id: 0x0022, kind: param,  name: vel.kd,        type: f32, min: 0, max: 1000, default: 0}
  - {id: 0x0023, kind: param,  name: vel.d_cutoff,  type: f32, unit: Hz, min: 0, max: 5000, default: 0}
  # signals
  - {id: 0x0100, kind: signal, name: position,      type: f32, unit: rad}
  - {id: 0x0101, kind: signal, name: velocity,      type: f32, unit: rad/s}
  - {id: 0x0102, kind: signal, name: duty,          type: f32}
  - {id: 0x0110, kind: signal, name: vel.error,     type: f32, unit: rad/s}
  - {id: 0x0111, kind: signal, name: vel.p_term,    type: f32}
  - {id: 0x0112, kind: signal, name: vel.i_term,    type: f32}
  # inputs
  - {id: 0x0200, kind: input,  name: velocity_sp,   type: f32, unit: rad/s, min: -1000, max: 1000, testgen: true}
  - {id: 0x0201, kind: input,  name: position_sp,   type: f32, unit: rad, testgen: true}
  - {id: 0x0202, kind: input,  name: duty_sp,       type: f32, min: -1, max: 1, testgen: true}
  # actions
  - {id: 0x0300, kind: action, name: reset_integrators, runs_in: loop}
```

The id blocks per kind (`0x00xx`, `0x01xx`, …) are a readability convention only. The rule the generator enforces is that ids are unique within a type.

`feedback.quadrature` exposes both `position` (f32, rad) and `count` (i32, raw encoder counts). An f32 position loses encoder resolution after a few thousand revolutions; the count does not, which matters for odometry.

**Value rules.**
- Range checks follow the conventions: a non-finite float or an invalid enum is rejected; a valid value out of range is clamped, and the applied value is reported back.
- A param can have an `on_change` hook. Its rules:
  - It runs in the loop. It must be bounded and must not allocate.
  - If it needs to do more (for example retune a filter), it does that from the param's new value inside the tick.

## Wire protocol

### Framing

`AA AA AA | MSG_TYPE u8 | LEN u16 | HCRC u8 | payload | CRC16`
- `HCRC` is a CRC-8 (polynomial 0x07, init 0x00) over MSG_TYPE and LEN.
- The CRC16 covers MSG_TYPE, LEN, HCRC and the payload.
- Overhead is 9 bytes.
- **The scanner rejects a candidate before waiting for its payload** if HCRC doesn't match or LEN is above `MAX_PAYLOAD`. It then drops one byte and rescans, as today. A false preamble therefore almost never holds up RX.
- `MAX_PAYLOAD` is the protocol constant 1024. A firmware may be built with a smaller receive limit, which `GET_INFO` reports, but never a larger one.
- MSG_TYPE ranges are unchanged.

### MSG_TYPEs (generated registry)

| MSG_TYPE | Direction | Use |
|---|---|---|
| `0x01` / `0x41` | cmd / resp | Core commands (families below) |
| `0x02` / `0x42` | cmd / resp | Time ping. Answered in the RX callback, with the RX time latched on arrival. |
| `0x03` | cmd | `DISARM`, fast path. Handled in the RX callback and never queued. It is answered through `0x41` (family system, op `DISARM`). |
| `0x23` / `0x63` | cmd / resp | diag query (unchanged) |
| `0xC0` | event | diag events |
| `0xC1` | event | Core events: capture done, test signal done, layout applied, OTA state, arm state change |
| `0x80` | stream ESP → host | System status |
| `0x81–0x9F` | stream ESP → host | Subscription groups 0–30 |
| `0xA0–0xBF` | stream host → ESP | Command groups 0–31 |

The Kconfig MSG_TYPE options of motor_manager, the IMU, time_sync and diag were removed in F2; `protocol/defs/registry.yaml` holds every assignment, and `docs/protocol/registry.md` is its generated table. The remaining legacy entries are time_sync `0x20` / `0x60`, until F8. F5b removed the motor entries (`0x21` / `0x61`, streams `0xBD` / `0xBE`); F6 removed IMU `0x22` / `0x42` and stream `0xBF`. Command groups now have 32 channels (`0xA0`–`0xBF`). **The time ping's `0x02` / `0x42` enter the registry in F8**; F6 leaves `0x42` free.

### Core command header

- **Request:** `version u8 | seq u8 | family u8 | op u8 | body`
- **Response:** `version u8 | seq u8 | family u8 | op u8 | result u8 | detail u8 | body`

The rules follow the protocol conventions:
- The response echoes `seq`.
- A header that can't be parsed gets `family = op = 0xFF`.
- Bodies have exact lengths, or carry a count that must match the remaining length.

**Result and detail codes.**
- The core defines one shared `Result` enum and one shared `Detail` enum in `registry.yaml`.
- `Result` adds: `Armed` (the op is not allowed while armed), `NotArmed`, `PendingReboot`, `NoMemory`, `Exists`, `Busy` and `Fault`.
- `Detail` adds: `CrossLoopBatch`, `StagingBusy`, `WrongKind`, `LockedWhileArmed`, `GpioConflict`, `LabelInUse`, `LoopMismatch` and `SafeMode`.
- The per-manager detail enums from diag phase 3 go away with their managers.

| Family | Ops |
|---|---|
| `0x01` system | `GET_INFO`, `ARM`, `DISARM`, `ACK_FAULT`, `SAVE`, `REVERT`, `FACTORY_RESET`, `REBOOT`, `EXIT_SAFE_MODE` |
| `0x02` layout | `LIST_INSTANCES` (paged), `ADD_INSTANCE(type, loop, label)` (the loop is a loop instance id; it defaults to the lowest-numbered loop the type may run in; a loop itself is added with type `loop` and no loop; the device picks the lowest free instance id of the type and returns it), `REMOVE_INSTANCE(type, inst)`, `SET_LABEL`, `DISCARD_PENDING` |
| `0x03` describe | `GET_TYPES` (paged), `GET_ITEMS(type, start)` (paged: every item with its kind and descriptor), `GET_ENUM(type, item)`, `GET_CODES(start)` (paged diag code descriptors: source, code, name, severity, kind, flags) |
| `0x04` param | `GET(n × Ref)`, `SET(n × (Ref, value))` (atomic batch), `GET_PENDING` (staged `reboot_required` values) |
| `0x05` group | `SUB_CONFIG(group, loop, divider, n × Ref)`, `SUB_ENABLE`, `SUB_CLEAR`, `CMD_CONFIG(group, loop, n × Ref)`, `CMD_CLEAR`, `LIST` |
| `0x06` capture | `CONFIG(loop, divider, samples, pre_trigger, trigger, n × Ref)`, `ARM`, `ABORT`, `STATUS`, `READ(offset, len)` |
| `0x07` testgen | `START(target Ref, waveform, params)`, `STOP`, `STATUS` |
| `0x08` action | `RUN(Ref, args[4], blob)` → `result, detail, out[4], blob` |
| `0x09` ota | `BEGIN(size, sha256)`, `WRITE(offset, chunk)`, `FINISH`, `ABORT`, `CONFIRM` |

**Notes on the ops:**
- **`GET_INFO`** returns:
  - the protocol version and the firmware version (`esp_app_desc`)
  - the build id and the descriptor hash
  - capture capacity, the receive limit and the list of running loops (id, name, rate, and from F3d a flags byte with `realtime`)
  - an instance count and flags: `pending_reboot`, `layout_invalid`, `config_loaded_from_nvs` and `safe_mode`
- **Limits** (from F4b). The device reports the limits of its build as read-only system params, not in `GET_INFO`:
  - F4b adds `limits.loops` (`CORE_MAX_LOOPS`), `limits.instances` (`CORE_MAX_INSTANCES`), and the group limits that F4a built (`limits.sub_groups`, `limits.cmd_groups`, `limits.group_entries`). F4c adds `limits.link_values`. A later step that adds a limit adds its param.
  - Each is a `u16` param of `system.yaml` flagged `read_only` (so it is not persisted). The core writes its value at boot from `CoreConfig`. A `SET` is refused (`ReadOnly`).
  - The host reads them with the normal param `GET` and learns their names from the describe family, so this needs no new op and no change to a wire format. Only the descriptor hash changes.
  - A host uses them to say "4 of 4 loops" before it sends an op. The device still refuses one over the limit (`Capacity`).
  - `GET_INFO` keeps the two it already has: the capture capacity and the receive limit.
- **Param `SET`**
  - **Staging rules:**
    - `reboot_required` params go to the pending layout, and so does any param of an instance that exists only there. Such a batch must be all pending (else `CrossLoopBatch`); accepted, it answers `PendingReboot` with the applied values.
    - Params of an instance that isn't running (failed, invalid, missing a dependency, or safe mode) are validated and written to its stored values on the config task; a batch can't mix them with system or loop params.
    - Live params must all belong to instances in one loop, or the batch is rejected (`CrossLoopBatch`).
    - If that loop's staging slot is taken, the batch is rejected (`Busy`, `StagingBusy`).
    - Params flagged `locked_while_armed` are refused while armed: loop rates, `system.watchdog_ms`, and wiring.
  - **Response timing.** The config task stages the batch and waits for the loop to take it, for up to 3 loop periods plus 5 ms.
    - If the loop takes it, the response is `Ok` with the applied values (after clamping), and a `GET` right after returns the new values.
    - If it times out, the config task withdraws the batch with a compare-and-swap on the slot and answers `Busy`. Either way the outcome is definite.
  - **Batches for other loops.** A batch for the instances of any loop is applied the same way, at that loop's next tick. The params of a loop instance itself (its rate) are applied on the config task, like system params.
- **Groups** are device resources:
  - All of them are allocated at boot at the maximum size (64 entries; packed values alone are about 4.3 KB for 8 + 8 groups, with additional RAM for resolved bindings, staged configurations and mailboxes) and never freed.
  - `SUB_CONFIG` and `CMD_CONFIG` write a new configuration that the loop swaps in at a tick boundary, and they bump the group's **epoch** (u8). `CMD_CLEAR` and `SUB_CLEAR` disable a group, also through the swap.
  - Group limits come from Kconfig. The defaults are 8 subscription groups and 8 command groups, each with at most 64 entries and within `MAX_PAYLOAD`.
  - Groups are not persisted. The host sets them up after each connect, and the Boot event tells it when to do that again.
  - A subscription group may contain signals and inputs. A command group may contain only inputs (otherwise `WrongKind`).
- **Ops refused while armed:** `SAVE`, `FACTORY_RESET`, `REBOOT`, `ota.*`, all layout ops, `CMD_CONFIG` and `CMD_CLEAR` for a group in an armed loop, and `paused` actions on instances in a real-time loop.
- **Capture `trigger`:**
  - `manual`
  - `testgen_start`
  - `input_change(Ref)`
  - `threshold(Ref, rising|falling, value)`

### Streams

All stream payloads start with `version u8 | epoch u8 | counter u16`. ESP → host streams then add `timestamp_us u64`.
- The counter wraps, so a gap means packets were lost.
- `epoch` is the group's configuration epoch, and is 0 for the status stream.

| Stream | Body after the prefix |
|---|---|
| System status `0x80` | `arm_state u8 \| flags u8 (fault_latched, blocks_arm_active, pending_reboot, nvs_dirty, testgen_running, safe_mode, capture_state×2) \| active_error_count u8 \| cpu_load u8 × 2 \| overruns u32 per loop \| last_disarm_reason u8`. From F3d the loop part is `loop_count u8 \| loop_count × (loop_id u8, overruns u32)`, for the running loops. |
| Subscription group | `n × value(4)`, in the order given at configuration |
| Command group (host → ESP) | `n × value(4)`. A frame with `n = 0` (prefix only) is a keepalive. |

Overhead per frame: 9 for the frame, plus 12 for the ESP → host prefix, or 4 for the command group prefix.

**Command frame rules.** The frame is rejected and counted (diag counter `core.CommandFrameRejected`), does not feed the watchdog, and applies nothing, if any of these hold:
- its epoch doesn't match the group's current one
- its length doesn't match the group
- it contains a non-finite value

A valid frame feeds the watchdog only if it arrived after the current `ARM`.

### Time ping

- **Request:** `version u8 | id u16`
- **Response:** `version u8 | id u16 | esp_rx_us u64 | esp_tx_us u64`

The RX time is taken in the RX callback. `esp_tx_us` is written into the frame by the TX task just before the frame goes to the UART driver. `uart_comm` gets a "stamp at send" flag on TX items for this, which also recomputes the CRC. The host:
1. timestamps its own send and receive
2. keeps the pings with the smallest round trip
3. fits offset and drift (a linear fit over a sliding window)

## Arming and safety

**States:**
- `Disarmed`: the boot state.
- `Armed`.
- `Fault`: disarmed with a latched reason.

**What latches.**
- A watchdog timeout, a lost lease (the agent sends `DISARM`), an explicit `DISARM` or a reboot leaves the device plain `Disarmed`. The host can re-arm as soon as the conditions allow.
- Recording a diag code flagged `disarms` (with `DIAG_RAISE`, `DIAG_ERROR` or `DIAG_COUNT`) disarms **and latches `Fault`**. `ARM` is refused until the host sends `ACK_FAULT`.
- `ACK_FAULT` is itself refused while any `disarms` code is still active.

**`ARM` is refused** when any of these hold:
- the device is in `Fault`
- any `blocks_arm` or `disarms` code is active
- the layout is invalid or a reboot is pending
- an instance failed to start
- the device is in safe mode
- no command group has been configured since boot

**`ARM` and faults can't race.** Every disarm increments a disarm epoch (an atomic u32). `ARM` reads the epoch, checks its conditions, then sets `Armed` with a compare-and-swap that succeeds only if the epoch hasn't changed. A fault that arrives during `ARM` always wins.

**Disarm triggers:**
- `DISARM`, handled in the RX callback (MSG_TYPE `0x03`, or system op `DISARM`)
- a watchdog timeout, evaluated by the safety monitor
- a stalled loop heartbeat, detected by the safety monitor
- any `disarms` code
- a reboot

**Disarm path:**
1. An atomic flag is set and the disarm epoch is incremented. This is real-time safe.
2. In the next tick, each loop sees the transition and calls `onArmTransition(false)`. The actuators drive zero duty (coast), and inputs and controllers are reset.
3. The test signal generator stops. A running capture keeps recording, so the event is captured.

**The safety monitor** does not depend on the loops working:
- **What it is.** An esp_timer callback, task dispatch, every 10 ms. It never blocks or takes a lock. It reads only atomics.
- **What it checks, while armed:**
  - each loop's heartbeat: every running loop of the layout, real-time or not. If it hasn't advanced for 3 periods, plus 5 ms, it records `core.LoopStalled` (flagged `disarms`).
  - the time of the last valid command frame against `system.watchdog_ms`. On a timeout it records `core.WatchdogTimeout`, which disarms but does not latch.
- **What it does on either condition:** it sets the disarm flag and calls `emergencyStop()` on every actuator instance. For `actuator.pwm_dir` that forces its one PWM generator low (`mcpwm_generator_set_force_level`, continuous; DIR is left alone), so the output stops even if the loop never runs again. Each `actuator.pwm_dir` has an MCPWM operator of its own, because the two generators of an operator share one force register (F5a-2).
- **Releasing the force.** On the next `ARM`, the loop releases the force level through `onArmTransition(true)`.
- **Backstop.** Every real-time loop task is also subscribed to the Task WDT, with panic-reset, for when even the esp_timer task is stuck.
- **Priority.** A loop's priority is at most 20, below the esp_timer task (22), so the monitor always preempts a loop.
- **`system.watchdog_ms`:** default 200, range 20–1000, `locked_while_armed`.

**Faults in the control tick.**
- A new macro `DIAG_RAISE(code)` is real-time safe: an atomic update that sets `occurred` and `active`, plus a dirty bit. The reporter then sends the record or the Cleared event from its own task.
- Codes that the control tick raises, such as `feedback.SignalLost`, are declared `kind: latched` in the YAML. That kind has an `active` bit like `clears: true` record codes, but no args.

**diag YAML flags:** `blocks_arm: bool` and `disarms: bool` per code, generated into the slot table. Examples:
- `feedback.SignalLost`, `core.LoopStalled` and a stalled control timer set `disarms`.
- `core.WatchdogTimeout` sets `disarms`, but is exempt from latching.
- a time sync failure sets neither.

**Future hardware e-stop:** a GPIO input module raises an `estop` code with `disarms` and `blocks_arm`.

## Layout, persistence and boot

**NVS layout:**
- One namespace per instance, `t<type>i<inst>`.
- Each instance's params are stored as one blob of `n × (id u16, value u32)`, with a header: format version, generation u32, count and CRC32.
- Each blob has two slots, A and B (keys `p_a` / `p_b`). `SAVE` writes generation `newest + 1` to slot `gen & 1`, which is the slot with the older generation. Loading takes the valid slot with the highest generation. A power loss during `SAVE` therefore leaves the previous configuration intact.
- The `layout` blob (instances, with their type, id, loop and label; namespace `core`, keys `layout_a` / `layout_b`) uses the same A/B scheme and is written last, as the commit record. One `SAVE` gives every blob the same generation, and loading uses only blobs of the newest generation that is complete across the layout.

**Ids across firmware versions.** Unknown ids are ignored, and missing ids take their YAML default. Adding or removing params in a firmware update therefore needs no migration. A retired id is skipped.

**Ops:**
- `SAVE` writes every `persist` param of every instance, plus the layout, then clears `nvs_dirty`.
- `REVERT` reloads the live (non-structural) params from NVS through the normal staged path, one batch per loop.
- `FACTORY_RESET` erases the namespaces and reboots.
- `SAVE` erases the namespace of every instance that is not in the layout it wrote (after the commit), so a removed instance's params go then. A later `ADD_INSTANCE` that reuses the id starts from the YAML defaults.

**Pending changes.** A `reboot_required` param write or a layout op goes to a pending copy. Device info and the status stream show `pending_reboot`. The change takes effect after `SAVE` and `REBOOT`. `DISCARD_PENDING` drops it.

**Layout validation** runs when a layout op or a `reboot_required` write is staged, and again at boot:
- **GPIOs:** every pin param (`gpio: true`, other than `0xFF`) is checked against the ESP32-S3 pin policy that `motor_manager` uses today (no USB pins, no flash/PSRAM pins 26–37, no uart_comm pins; `GpioNotAllowed`). It is also checked against every other pin on the device (`GpioConflict`).
- **Wiring:** a `Ref` param must be 0 or point to an existing instance of its category (`BadRef`), in the same loop (`LoopMismatch`). Neither the ref graph nor the tick graph may have a cycle (`WiringCycle`). An instance that another one is wired to can't be removed (`Referenced`).
- **Labels:** must be unique on the device (`LabelInUse`) and non-empty.
- **Loops (F3d):** an instance's loop must be an existing loop instance whose `realtime` flag the type allows (`LoopMismatch`). There are at most `CORE_MAX_LOOPS` loops (`Capacity`). A loop that still holds an instance can't be removed (`Referenced`).
- **Sources (F4c):** a source param must be 0 or name an existing signal or input of its value type (`BadRef`). The instance it names can't be removed (`Referenced`).

**Flash stalls the loops.**
- NVS and OTA writes disable the cache on both cores, so the loops can stall for several ms. That is why `SAVE` and OTA are refused while armed.
- The status stream counts the overruns this causes while disarmed.
- `CONFIG_UART_ISR_IN_IRAM` keeps the UART receiving during those stalls (set in F1).
- `CONFIG_SPIRAM_XIP_FROM_PSRAM` could remove the stalls altogether by running code from PSRAM. F7 evaluates it against its cost in control-loop latency.

**Boot sequence:**
1. `diag::init()`
2. `UartComm`
3. the safe-mode check (below)
4. core init: load the layout and params from NVS, or the defaults
5. validate the layout, then create the instances in dependency order. An instance that fails is reported (`<source>.InitFailed`) and left out, and anything wired to it is left out too (`core.DependencyMissing`).
6. start the loops and the safety monitor
7. the diag `Reporter`
8. the `Boot` event goes out

The device stays up in a degraded state, as in diag phase 1.

**No layout yet** (first boot, or after a factory reset): the device boots with the factory layout, which is the `control` and `sensor` loops and no module instance, and waits for the host. The board is then configured entirely from the GUI.

**Safe mode:**
- A counter in RTC no-init memory counts consecutive boots whose reset reason is a panic, an interrupt WDT or a task WDT. It is cleared after 30 s of uptime.
- When it reaches 3, the device boots with zero instances, starts no loop task (a loop's rate, core or priority may be what crashes it), and sets `safe_mode` in `GET_INFO` and the status stream, plus the diag code `core.SafeModeEntered`.
- In safe mode the host can read and edit the layout and params, `FACTORY_RESET`, or run OTA, then `EXIT_SAFE_MODE`, which clears the counter and reboots.

## Configurable loops, the real-time flag and sources

Decided by the user on 2026-11-13, after F3c. F3d builds the loops and the flag; F4c builds the sources. Where older text in this plan says `control` and `sensor`, it means the two loops of the factory layout.

### Loops are instances (F3d)

- **The type.** `protocol/defs/loop.yaml` defines the built-in type `loop`. Its type id is reserved in `registry.yaml` beside `system_type_id`, clear of the diag source ids. The core registers it itself, like `system`; it is not in main's module list. The reserved type id is 0x05.
- **Identity.** A loop's instance id is its loop id, used wherever a loop is named: a layout entry's `loop`, `ADD_INSTANCE`, group and capture configuration, the status stream. Its label is its name. A loop instance itself runs in no loop (`kNoLoop`).
- **Items:**

  | Item | Type | Rules |
  |---|---|---|
  | `rate_hz` | u16, 1–2000, default 100 | Live and `locked_while_armed`. Applied on the config task by restarting the loop's timer, as the system rate params were. |
  | `realtime` | bool, default false | `reboot_required`. See the next section. |
  | `core` | u8, 0–1, default 0 | `reboot_required`. |
  | `priority` | u8, 1–20, default 10 | `reboot_required`. The ceiling keeps every loop below the esp_timer task (22), where the safety monitor runs. |
  | `stack_bytes` | u16, 3072–16384, default 4096 | `reboot_required`. |
  | `ticks`, `overruns`, `last_run_us`, `max_run_us` | u32 signals | The loop statistics, so a group bound to the loop can stream them. |

- **Managing loops.** A loop is added and removed with `ADD_INSTANCE` and `REMOVE_INSTANCE` of type `loop`, through the pending layout, `SAVE` and a reboot, like any instance. `REMOVE_INSTANCE` is refused while a layout entry still runs in the loop (`Referenced`). A loop's params are in the system domain of a param batch.
- **Limits.** `CORE_MAX_LOOPS` (Kconfig, default 4, range 1–8). A loop costs about 7 KB of RAM: its stack, one staging slot and one action slot. The Kconfig options for the control and sensor task stack, priority and core go away.
- **The factory layout.** With no saved layout (first boot, factory reset) the device has `control` (id 0: 1000 Hz, real-time, core 1, priority 20) and `sensor` (id 1: 100 Hz, not real-time, core 0, priority 10), and no module instance. With CORE_MAX_LOOPS=1 the factory layout contains only control (user decision for F3d). A saved layout may hold any set of loops, including none. Loop capacity is separate from the module capacity CORE_MAX_INSTANCES.
- **Boot.** Loops are created before the module instances. If a loop's task can't be created, the loop is `Failed` and its instances are `DependencyMissing`. Loop tasks start with their callbacks gated until module creation and tick lists are complete; Core::start releases the gates. In safe mode no loop task is started.
- **Defaults.** `ADD_INSTANCE` with loop `0xFF` picks the lowest-numbered loop the type may run in (`LoopMismatch` if there is none).
- **Persistence.** Loops are ordinary layout entries with their own param blobs. The layout blob's format version is bumped, so a layout saved before F3d is dropped and the factory layout is used. `system.control.rate_hz` (`0x0001`) and `system.sensor.rate_hz` (`0x0002`) are retired.
- **RX priority.** The UART RX task (priority 5) stays below the loops, as it is below `sensor` today. A loop that starves it stops the command frames, and the watchdog disarms.

### The real-time flag replaces the loop classes (F3d)

- **In the YAML,** `realtime: required | forbidden | any` replaces `loops: [...]`. `TypeDesc::loops` and the `loop_mask` byte of `GET_TYPES` keep their encoding: bit 0 means the type may run in a real-time loop, bit 1 in the other loops.
  - `required`: actuator, feedback and controller types. Every loop that drives an output is therefore real-time.
  - `forbidden`: types whose tick does bus I/O or may block (`imu.bno055`).
  - `any`: bounded computation with no I/O.
- **What the flag does to a loop:**
  - The real-time rules of `firmware/CLAUDE.md` apply to every tick in it.
  - `paused` actions on its instances are refused while armed. This check is the only thing that keeps such a loop from being paused while armed: a paused loop keeps counting its heartbeat, so the monitor doesn't see it as stalled.
  - Its task is subscribed to the Task WDT.
- **What enforces the rules:**
  1. **Layout validation** checks the type's requirement against the loop's flag (`LoopMismatch`).
  2. **The module API.** Services that block (bus handles, from F6) are given only to instances in loops that are not real-time.
  3. **A measured budget.** A tick that runs longer than its period is counted per loop (`core.LoopOverBudget`, a counter). It doesn't disarm; `core.LoopStalled` does.
  4. **The real-time guard in the native tests and the vdev.** `Loop::runTick` of a real-time loop sets a per-thread flag in the native `core_os` backend. While it is set, a heap allocation or a blocking `core_os` call (`Mutex::lock`, `Event::wait`, the `KvStore`) fails the test; the vdev counts it. Every type that is `required` or `any` has a test that ticks it under the guard.
- **What stays a convention:** a module that calls a blocking IDF function directly from a real-time tick is not caught on the device. Reviews and the budget counter cover that.

### The stall rule is unchanged

The safety monitor watches the heartbeat of every running loop, and a stall of any of them disarms and latches (`core.LoopStalled`), whether the loop is real-time or not. The monitor walks the fixed loop table and reads only atomics.

### Sources: reading a value across loops (F4c)

- **Declaration.** A source is one YAML item of `kind: source`. It gives the value type of its target and two more ids:

  ```yaml
  - {id: 0x0010, kind: source, name: left, type: i32, value_id: 0x0110, age_id: 0x0111}
  ```

  The generator expands it into three items of the type. On the wire they are an ordinary param and two ordinary signals:
  - the param `left` (id `0x0010`): a `u32` flagged `reboot_required` and `Source` (`ItemFlag` `0x80`, the last free bit). Its value is a packed `Ref` to a signal or input of the declared value type, in any loop (its own included); 0 means not connected. The packed value is `type << 24 | instance << 16 | item_id`, with helpers in `defs.hpp`.
  - the signal `left.value` (id `0x0110`), of the declared value type: the sampled copy.
  - the signal `left.age_us` (id `0x0111`), a `u32` in µs: the age of that copy.
- **What the host sees.** There is no op for sources. The host reads a source's state as it reads any signal:
  - It knows a param is a source by the `Source` flag. It finds the two signals by name (`<param name>.value` and `<param name>.age_us`; the generator makes them, so the names can't drift), and takes the target's value type from the `.value` signal.
  - `age_us` is `0xFFFFFFFF` while there is no valid sample. A real age saturates at `0xFFFFFFFE` (about 71 minutes).
  - The host works out why a source has no valid sample from what it already has: the param is 0 (not connected), the target instance is not running (missing), or nothing has arrived yet.
  - "Stale" is a host rule (for example an age above three periods of the producer's loop), or the consumer's own limit param if its type has one.
  - Both signals can go into a subscription group of the consumer's loop and into a capture, so the age can be streamed and plotted.
  - The new flag bit changes what `GET_ITEMS` can send, so F4c bumps the core wire version.
- **In the module.** `start()` binds the three items with one call, `ctx.bindSource(id, &m_x)`, where `m_x` is a `core::Sampled<T>`. The core fills it at step 5 of the consumer's tick:
  - `value`: the copy taken at the producer's tick. It is the storage of the `.value` signal.
  - `age_us`: this tick's start time minus the producer tick's start time, or `0xFFFFFFFF`. It is the storage of the `.age_us` signal.
  - `valid`: false while the source is not connected, the producer is not running, or nothing was published yet.
  - `fresh`: true if a new snapshot arrived since this instance's last tick.
- **A missing producer does not stop the consumer.** The consumer starts and sees `valid == false`, so a fusion module can go on with what it has. This differs from wiring, where a missing target leaves the instance out (`DependencyMissing`).
- **The transfer** is done by the core; a module never touches it.
  - There is one link per ordered pair of loops (producer loop, consumer loop). It holds every value that the instances of the consumer loop read from the producer loop.
  - A link is a **triple buffer**: three value arrays and one atomic index. At the end of its tick (step 8) the producer loop copies the bound signal storage and its tick timestamp into the back array and swaps the index. At step 5 the consumer loop takes the latest array with one atomic exchange and copies the values into the bound `Sampled<T>`s.
  - Both sides are wait-free. Neither waits, retries or takes a lock, whatever the two loops' cores and priorities are. A seqlock was rejected: a reader that preempts the writer on the same core would spin.
  - One snapshot is one producer tick, so values from one loop are coherent with each other (a whole quaternion, both wheel positions).
  - The links are allocated at boot from the layout. Identical target refs share a value slot within a pair. A link holds at most `CORE_MAX_LINK_VALUES` distinct values (Kconfig, default 32); a layout that needs more is invalid (`Capacity`). The work per tick is fixed after boot.
- **The same loop.** A source in the producer's own loop is copied directly just before the consumer's tick, with age 0. It adds a tick-order edge (the producer ticks first), and a cycle through such edges is rejected (`WiringCycle`).
- **Latest wins.** There is no queue. A consumer slower than its producer misses samples; one that needs every sample runs in the producer's loop.
- **Staleness is the consumer's business.** The age grows while the producer loop is paused, slow or stalled. The core sets no maximum age; a consumer that needs one has its own param.
- **Sources are not inputs.** Inputs are written only by command groups and the test signal generator, and arm transitions don't touch sources. A source on an actuator or controller type would close a loop on the device outside the command watchdog, so adding one needs a user decision.
- **Cost in a real-time loop:** a copy of at most `CORE_MAX_LINK_VALUES` 4-byte values and one atomic exchange per link and tick.
- **The case this is for.** `feedback.quadrature` runs in `control`, wired to its controller as before. `imu.bno055` runs in `sensor`. A fusion type, in `sensor` or in a loop of its own, has sources on the encoders' `count` or `position` and on the IMU's signals.

### What it changes in the other steps

- **F4a.** A group is bound to a loop id, for any loop. Its "latch a list of bound signals at the end of the tick" code is written so that F4c's links can use it.
- **F4b.** The monitor walks the loop table. "Armed loop" and the `paused` rule use the `realtime` flag. It also adds the read-only `limits.*` system params (see "Limits" under the notes on the ops).
- **F5 and F6.** The motor types are `realtime: required`; `imu.bno055` is `forbidden` and runs in the factory `sensor` loop.
- **F7.** Capture takes a loop id. F7b decides whether the test signal generator stays one per loop, now that the number of loops varies.
- **Host.** `libdrivelink` and the CLI see loops as instances. The GUI's layout screen (H2) edits them.

## diag v2

"diag v2" is the name of this redesign. Its wire version byte is `0x03`: F1b already bumped diag's version to `0x02` with the new framing, without changing a payload.

- **Timestamps are monotonic.** The `time_synced` flag and `diag::setTimeSynced()` are removed.
- **`Boot` body:** `timestamp_us u64 | reset_reason u8 | instances_ok u8 | instances_failed u8 | safe_mode u8`. `started_mask` goes away; the instance details come from `LIST_INSTANCES`.
- **Records gain an `instance u8`.** It comes after `source`, and is `0xFF` when the record isn't about one instance. That makes the Record body 25 bytes. `arg0` and `arg1` keep their current meaning per code, so nothing collides with codes that already use `arg1` (motor `InitFailed`, IMU `InitFailed`, `SyncFailed`).
- **Sources become module types** (generated from the module type YAMLs), plus `system`, `uart_comm`, `diag` and `core`.
- **New YAML fields per code:** `blocks_arm` and `disarms`, plus the new `kind: latched` and `DIAG_RAISE` (see above).
- **Code descriptors** are served by `describe.GET_CODES` and covered by the descriptor hash, so the host can name a new module's faults without being rebuilt.
- **GetInfo** stays as a diagnostic view. Device identity is in `system.GET_INFO`.
- The YAML moves to `protocol/defs/`, and the generator becomes `tools/gen/`.

## Capture and test signals

**Capture**
- Up to `capture.max_refs` signals or inputs (default 16) from one loop, at a divider of its rate. Capturing the input next to the response is what a step-response plot needs.
- Stored in a ring of `samples × n × 4` bytes, plus a `u64` start timestamp; the samples are evenly spaced.
- The ring is allocated at `CONFIG` time, which is configure time, with `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. The reconfiguration frees and reallocates it only while no capture is armed.
- **PSRAM config:**
  - Octal PSRAM, 80 MHz, with `CONFIG_SPIRAM_USE_CAPS_ALLOC`. Nothing else lands in PSRAM by accident, which plain `malloc` could cause with `SPIRAM_USE_MALLOC`.
  - `CONFIG_SPIRAM_IGNORE_NOTFOUND=y`, so a board without PSRAM boots and falls back to an internal ring of 32 KB.
  - `GET_INFO` reports the capacity.
- **Access from the loop.** The control loop writes `n` values per due tick into PSRAM through the cache. That is fixed work, and it never allocates. During a flash write the cache is disabled anyway, and the loop is stalled.
- States: `Idle → Armed → Triggered → Done`. A `Done` event goes out on `0xC1`. `READ` pages through the samples in chunks of 512 B or less.
- Capture may run while disarmed (sensors only), as long as no testgen is running.

**Test signal generator**
- One generator for each loop.
- Waveforms: `step`, `square`, `ramp`, `sine`, and `chirp` (linear or log). Parameters: offset, amplitude, period or frequency start and end, duration, repeat.
- Targets: inputs flagged `testgen: true` in the YAML. For now those are the `velocity_sp`, `position_sp` and `duty_sp` inputs of `controller.cascade_pid`.
- Every generated value is clamped to the input's range.
- The generator runs only while armed and stops on disarm. While it runs, command group values for its target are ignored, but command frames still feed the watchdog.
- **Plant identification** uses `duty_sp` in `open_loop` mode with capture triggered by `testgen_start`. The host fits the model; the firmware only generates and records.

## OTA

- **Partition table:** `nvs` (kept at `0x9000`, size `0x6000`, as in the default table, so the configuration saved in F3–F8 survives), `otadata`, `phy_init`, `ota_0` (4 MB), `ota_1` (4 MB), and spare space for later (16 MB flash).
- **Bootloader:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
- **Sequence:**
  1. `BEGIN(size, sha256)` checks that the device is disarmed and calls `esp_ota_begin` with `OTA_WITH_SEQUENTIAL_WRITES`, so the image region is erased while it's written instead of all at once (which would block the config task for seconds).
  2. `WRITE(offset, chunk ≤ 512)` is **idempotent**:
     - `offset` equal to the bytes written so far: append the chunk.
     - `offset` below that, with the data already there: acknowledge it as a duplicate (a retry).
     - anything else: return `InvalidArgument` with the expected offset in the body.
     - The host keeps a few chunks in flight, within the config queue depth.
  3. `FINISH` checks the SHA-256 and sets the boot partition.
  4. `REBOOT`.
- **Confirmation:** the new image confirms itself (`esp_ota_mark_app_valid_cancel_rollback`) only after the host sends `CONFIRM`. Without it, the next reset rolls back. `CONFIRM` is sent after the host has reconnected and checked `GET_INFO`.
- **Flash writes.** They run on the config task, so the loops stall as with NVS, which is why OTA requires the device to be disarmed. The UART keeps receiving because its ISR is in IRAM.

## Virtual device

- `core`, `protocol` and the pure parts of the modules build natively against `core_os`'s native backend: std::thread, a timer thread, a file-backed key-value store, and steady_clock.
- `mod_mock` provides `actuator.mock` and `feedback.mock`, which model a first-order motor, and `imu.mock`. From F5a the mock motor also has encoder quantization, a deadband, duty saturation, a one-tick delay and optional noise, each set by a param, because it is the only plant the controller meets before the hardware campaign.
- A `drivelink-vdev` executable (in `host/vdev/`) serves the protocol on a TCP port, or a pty on Linux. It is the simulator for the host and for CI integration tests. It builds with the host superbuild (MinGW and Linux).
- It also runs the real modules' pure logic: PID, velocity estimator and controller. Only the HALs are mocked.
- The safety monitor, the safe-mode counter and the NVS A/B logic run in the vdev too. Crashes and power loss can be simulated by killing the process.

## Testing

| Layer | Tests |
|---|---|
| `protocol/` | Codec round trips for every family, stream and event; exact lengths; paging; HCRC rejection of false preambles; the generator's static checks (unique ids per type) |
| `core` (native) | Batch validation, atomicity, cross-loop rejection, staging busy and withdrawal; group swap and epoch; command frame rules; capture ring and triggers; test signal waveforms; the arming state machine including the epoch race, latching and `ACK_FAULT`; the monitor's stall and watchdog detection; NVS A/B generations and torn writes; layout validation; safe mode; boot with failing instances; loop instances (one loop up to the maximum, the factory layout, the real-time check, removal refused while in use); the real-time guard (an allocation or a blocking `core_os` call inside a real-time tick fails the test); source links (never published, coherence under a writer thread, age, a paused or failed producer) |
| Modules (native) | Controller cascade, bumpless mode switch (against the mock motor's quantization, deadband, saturation and delay), the order of the actuator, feedback and controller ticks, `onArmTransition` resets; `imu.bno055` against a fake bus that plays the sensor from its datasheet (init, mode switches, calibration, NACK, timeout) |
| `vdev` | End-to-end over TCP with `drivelink-cli`: connect, describe, configure, arm, command, watchdog trip, capture, save, reboot, reload |
| Hardware | `firmware/test/uart_comm` HIL tests, updated for the new framing (with `--baud` defaulting to 921600), plus a smoke test on hardware: flash, connect, arm, run a capture, and a loop-stall test that forces the monitor to stop the motors. Checks that need motors, an IMU or the robot are written in their phase and run in the HW phase (see below) |

CI adds three jobs: the native core and module tests, a vdev integration job, and the generator staleness check. The existing IDF builds and the nolog variant stay.

## Phases

Each phase is one PR, or a short series of PRs. The steps are listed in [overhaul-status.md](overhaul-status.md). Each PR targets `firmware_overhaul`, builds with zero warnings, keeps CI green, and updates its docs (`docs/protocol/`, `CLAUDE.md`, `firmware/CLAUDE.md`, `firmware/components/CLAUDE.md`, the READMEs of new components, the status file) in the same PR. The repo restructure (S1) comes before F1.

| Phase | Content | Done when |
|---|---|---|
| F1 Link and `protocol/` | Extract framing, CRCs, scanner and `wire_codec` into `protocol/`. New framing: u16 `LEN` + HCRC8, `MAX_PAYLOAD` 1024, default baud 921600, `CONFIG_UART_ISR_IN_IRAM`. `uart_comm` fixes for the bigger frames: buffers move from the stacks into `Impl`, task stacks are resized and checked, stream slots are allocated with their own size, and `sendData` gets a non-blocking slot write (double buffer plus try-lock, with a counter for skipped writes) so the control tick never waits. Bump every module protocol version. The codec tests move from `firmware/test/host` to `protocol/test`, and `conftest.py` defaults `--baud` to 921600. | HIL tests pass with the new framing, the managers still work over it, and stack high-water marks are logged |
| F2 Definitions and generator | `tools/gen/`, `registry.yaml`, and `diag_codes.yaml` moved to `protocol/defs/`. The generated registry replaces the Kconfig MSG_TYPEs, and the "IDs from Kconfig" convention in `firmware/CLAUDE.md` is replaced in the same PR. The descriptor table format and hash. The legacy manager streams move out of the way: motor setpoint `0xBD`, motor telemetry `0xBE`, IMU `0xBF`. They are marked `legacy`, and each is removed when its manager is ported. Until then the registry's `static_assert`s keep the group channel limits below them. motor_manager and the IMU go to protocol v6 with the move. | Generated headers are used by diag and the managers, and the CI staleness check passes |
| F3 Core skeleton and dev client | `core_os` (IDF and native), registry, loops (`control`, `sensor`), layout and NVS (A/B, validation, GPIO registry), safe mode, the system/layout/describe/param families, the action family, the status stream, `GET_INFO`, `mod_mock`, `drivelink-vdev` (in `host/vdev/`). diag v2. The first slice of `libdrivelink` (serial and TCP transport, session, the core families) and `drivelink-cli`, built by a first `host/superbuild`. `firmware/test/host` becomes `firmware/test/native`, which builds the core against `core_os`'s native backend. | With the CLI, on vdev and on hardware: mock instances can be added, described, set, saved, rebooted and reloaded |
| F3d Configurable loops | Loops as instances of the built-in `loop` type, the `realtime` flag in place of the loop classes, the factory layout, the real-time guard in the native tests, core wire v2, and the host library, CLI and vdev updated with it. | With the CLI, on vdev and on hardware: a third loop can be added, given instances, saved, rebooted and reloaded; safe mode starts no loop |
| F4 Groups and arming | The read-only `limits.*` system params (F4b). Subscription and command groups (preallocated, epochs), arming and the arm state machine, `DISARM` fast path, the safety monitor, the watchdog, `DIAG_RAISE`, the `blocks_arm` / `disarms` flags and `kind: latched`, all working on mock instances. The CLI gains arm, stream and plot-to-CSV commands. | vdev: mock motors are commanded and streamed through the CLI; the watchdog, a simulated loop stall and faults disarm |
| F4c Sources | Source items (`kind: source`: the param, its generated value and age signals, the `Source` item flag), `limits.link_values`, the per-loop-pair snapshot buffers, `Sampled<T>`, and a `fusion.mock` type that reads `feedback.mock` and `imu.mock` across loops. | vdev: the fusion mock's output is streamed with the expected ages; on hardware the control loop's longest tick is measured with and without a 16-value link |
| F5 Motor stack as modules | Split `motor_controller` into the actuator, feedback and controller types, with YAML, `emergencyStop()`, `onArmTransition()` and bumpless mode switching. The mock motor gains quantization, deadband, saturation, delay and noise. Delete `motor_manager`. | Velocity and position control through groups on vdev and on the bare board with mocks; the monitor stops the mock motors in the loop-stall test; nothing remains of the old motor protocol. Deferred to HW: the same on real motors |
| F6 IMU module | `imu.bno055` on the `sensor` loop, behind a bus interface with a fake BNO055 in the native tests, with calibration actions (`calibration_read`, `calibration_write`, `calibration_apply`, all `runs_in: paused`), persistent typed calibration committed by SAVE/restored at boot, and a paused live mode action. Delete `imu_bmo_055`. | Native tests pass against the fake bus; on the bare board a missing sensor fails cleanly and the calibration actions work on `imu.mock`. Deferred to HW: IMU signals in subscription groups and calibration on a real BNO055 |
| F7 Capture and test signals | The capture and testgen families, PSRAM enabled (caps alloc only). Evaluate `SPIRAM_XIP_FROM_PSRAM`. | A step response and a chirp, captured at 1 kHz on the bare board with the mock motor and their inputs. Deferred to HW: the same on a real motor |
| F8 Time | The ping on `0x02`, with TX-time stamping in the TX task. Delete `time_sync`. | The host's offset and drift estimate is stable over the bench's USB-UART adapter (USB-UART adapters add up to 1 ms of USB frame jitter; their target is a stable estimate, not a jitter bound). Deferred to HW: jitter below 100 µs over the Pi's GPIO UART |
| F9 OTA | The partition table (NVS unchanged), the OTA family with sequential and idempotent writes, and rollback. | Updating with the CLI, a retried chunk is acknowledged, and rollback when `CONFIRM` isn't sent |
| F10 Cleanup | Remove the leftover Kconfig and legacy registry entries. Rewrite `firmware/CLAUDE.md` for the new architecture and remove the temporary overhaul section from the root `CLAUDE.md`. A full docs consistency pass. Tag `main`'s last legacy commit `legacy-final`, then merge `firmware_overhaul` into `main`, marked as not yet run on motors or an IMU. | Every rule in the docs matches the code, and `main` holds the new firmware |
| HW Hardware campaign | Run every deferred hardware check its phase wrote (see "Deferred hardware checks" in the status file). Fixes go to `main` as PRs. | Every deferred check has passed on real motors, a real BNO055, the Pi and the robot |

Arming lands (F4) before any real motor is driven through the core, so the board is never driven by the new code without the watchdog and the monitor. From F3 to F5 the old `motor_manager` keeps working alongside the core. Each manager is deleted in the phase that ports it, so no module ever exists in both forms.

**Deferred hardware checks.** No motors or IMU are on the bench while the overhaul is built, so a phase whose check needs them doesn't wait for it. It merges on its native, vdev and bare-board checks, writes a script for the hardware check and dry-runs it against vdev or the bare board with mocks. The scripts run in the HW phase, or as soon as the hardware arrives, before any further phase. `motor_manager` and `imu_bmo_055` are still deleted in their phases: on this branch they ran only in mock mode, and the `legacy-final` tag keeps them. The first run on a real motor follows a procedure, not new firmware: wheels off the ground, a current-limited supply, a low duty cap and a hand on the power switch.

## Decisions made while writing this plan (review these)

- **Time ping on its own MSG_TYPE, answered in the RX callback**, and `DISARM` on its own fast-path MSG_TYPE. Both avoid the config queue.
- **Groups are not persisted.** They describe what one host session wants. The host sets them up after each connect, and the Boot event tells it when to do so again.
- **Signals and inputs use the 4-byte slot, as params do.** IMU data that is `i16` today becomes `f32` in SI units, which doubles its bandwidth. A 10-value IMU group at 100 Hz is still only about 7% of 921600 baud.
- **One test signal generator per loop.** That covers tuning one controller at a time. More than one would need ownership rules.
- **`ARM` requires a configured command group**, so the watchdog always has a feed to watch.
- **The NVS format is (id, value) pairs in A/B slots, not the wire encoding of a struct.** It tolerates params being added or removed between firmware versions, and it survives power loss during `SAVE`.
- **`imu_bmo_055` becomes `mod_imu_bno055`.** The rewrite is the moment to fix the spelling.
- **Batches that span loops are rejected, not applied in two phases.** A host that wants to change both loops sends two batches. Two-phase commit across loops isn't worth its complexity.
- **The safety monitor runs at 10 ms** as an esp_timer callback with task dispatch. Running it as an ISR would allow a faster check, but `emergencyStop()` would then have to be ISR-safe for every future actuator type.
- **Safe mode triggers after 3 crash resets in a row**, each within 30 s of boot.

## Out of scope

- USB transport. The transport stays behind `uart_comm`, so USB can be added later.
- Runtime baud switching.
- Strings and arrays as param types, apart from the instance label.
- Wiring between instances that changes at runtime. Wiring is `reboot_required`.
- Encrypting or signing the OTA image. The SHA-256 is only an integrity check.
- A hardware driver-enable GPIO driven by the monitor. It can be added as an optional `actuator.pwm_dir` param when boards have one.

## Open questions

- Safety monitor period: is 10 ms (worst case about 13 ms plus 3 control periods to stop the motors after a loop stall) fast enough for your robots?

## F3b implementation decisions (2026-11-12)

User-approved: one F3b PR; separate current/pending layout views; deterministic
first-order motor and IMU mocks with actions; complete descriptors with
generator-enforced size limits rather than continuation; test-only BenchCrash
for hardware safe-mode coverage. The concrete core v2 and diag v3 encodings
are specified in docs/protocol/core.md and docs/protocol/diag.md.

CPU load uses idle runtime deltas, with 0xFF for an unavailable sample. Status rate defaults to 10 Hz (1..100).
The diagnostic runtime uses 254 fixed sparse slots, including generated
non-instance slots; exhaustion is counted. Safety flag metadata is present,
while arming/latching/DIAG_RAISE behavior stays in F4b.

### F4a implementation decisions

Core wire v3 adds group operations/streams; NVS formats do not change. Group
wire bodies and epoch behavior are specified in docs/protocol/core.md. An input
has one command-group owner (AlreadyBound on overlap). Group staging uses a
bounded atomic ownership claim and a tick-boundary acknowledgement; a rebind
to another loop is committed by the old loop before the new one accesses it.
The device stays disarmed through F4a. F4b connects command application and
receipt times to its arm state and watchdog. NaN is rejected on the wire;
host NaN-to-hold conversion waits for arming/hold semantics.


### F4b implementation decisions

Core wire v4 removes GROUP LIST's F4a capacity prefix: the five limits are
read-only system params, and host convenience results obtain capacities by GET.
The user approved this consolidation and core v4 with the F4b plan.
DISARM on MSG_TYPE 0x03 uses the ordinary four-byte system DISARM header and
responds on core_response. Both DISARM encodings bypass the firmware queue.

The ARM commit word packs state/fault/stopping bits and the disarm epoch in a
32-bit lock-free atomic, with a coherent split-word ARM timestamp. The monitor
uses a prebuilt actuator list and repeats emergency forces while disarmed;
ARM cannot commit while forces are being applied. Native/vdev share the real
atomic diagnostic state. Latched delivery coalesces repeats, retaining raise
and clear when both precede a reporter scan. Fixed overflow bitmaps (32 bytes per generated safety code) retain active
conditions and their recovery even if the sparse reporting table is exhausted.

Mock diagnostic actions and a default-off BenchStall (guarded by the existing
bench crash option) exercise these paths without motors. There is no NVS
format change. Automatic NaN-to-hold mapping remains for later host modeling,
as permitted by the host plan; the explicit command API still rejects NaN.

## F5a motor implementation boundary

F5a is split into two PRs/sessions. F5a-1 provides shared pure motor interfaces,
`controller.cascade_pid` and realistic mocks; F5a-2 adds the real PWM+DIR actuator,
quadrature feedback and real-pin setup checks. F5b still removes the legacy motor
stack/streams and adds the loop-stall campaign. Real motor checks remain deferred.
The PWM+DIR HAL has one PWM generator; F5a-2 reconciled the safety section's
earlier two-generator wording with it (below).

The new controller implements a true position → bounded velocity target →
velocity PID → bounded duty cascade, unlike the legacy position-to-duty PID.
Each actuator has at most one controller owner. A duplicate fails startup and
blocks ARM. Standalone mock commands cannot override a claimed actuator. A
shared control-port lifetime makes claim release safe regardless of record
teardown order; its mutable fields remain owned by one loop.

A mode switch captures measured velocity/position or previous duty once,
tracks previous output in the integrators and suppresses derivative kick.
Only an active-input value frame received strictly after mode application
releases `waiting_for_command`; equal timestamps are stale. A fresh frame
during the transition tick is used on the next tick. An unchanged mode write
creates no new boundary. Watchdog semantics are unchanged, including while
waiting. ARM/DISARM clear controller histories and internal input receipt
metadata, hold zero duty/velocity or measured position, and preserve encoder
position and source storage. These are module semantics over existing encodings.
The old PID wrapper retains its original integration policy while sharing pure
math with the new conditional-integration kernel.

### F5a-2 implementation decisions

`actuator.pwm_dir` (0x21) and `feedback.quadrature` (0x22) are pure module
logic over a small driver seam (`mod_motor/hardware.hpp`); the device backend
wraps `motor_hal` and `rotary_encoder_hal`, and the native tests use a fake.
The vdev does not register them.

- **Force.** The actuator forces its one PWM generator low (continuous force)
  and leaves DIR alone. It is forced from creation, before its PWM actions are
  set, and released only by `onArmTransition(true)` after a zero-duty write.
  An atomic gate mirrors the force. Every stop bumps an atomic stop epoch, and
  a release that a monitor stop raced stops again, so a stale release never
  undoes a monitor stop (found by the independent review).
- **One MCPWM operator per actuator** (6 at most). Both generators of an
  operator share one force register that the driver updates read-modify-write;
  sharing it would let one output's release undo another's monitor force.
- **Reversal.** A nonzero duty is written only when the previous request had
  the same sign or was zero, and `Motor::write` leaves DIR alone on a zero
  write: the compare value applies at the next PWM period and DIR at once, so
  DIR never flips under a live compare value of the other sign. A request that
  alternates sign each tick writes zero.
- **Failures.** Start failures use each type's `InitFailed` (stage 1 a pin
  unset, 2 a driver error with its esp_err). Tick-time driver errors raise the
  latched, disarming `actuator_pwm_dir.DriverFault` / `feedback_quadrature.ReadFailed`
  (user decision). A failed read reports NaN, so the controller zeroes its duty.
- **Params.** `invert` on both (locked while armed), `duty_limit` on the
  actuator, `pullup` on the encoder (user decision).
- **PWM resolution.** The `MOTOR_HAL_TIMER_RESOLUTION_HZ` default is 10 MHz
  (400 steps at 25 kHz, 0.25% duty) instead of 1 MHz (2.5%).

## F6 implementation decisions (2026-11-13)

User-approved details are in `f6-implementation.md`. Calibration persists as
ordinary typed params through explicit SAVE; boot applies a valid saved blob
before startup mode selection. The core commits bounded own-instance, hookless
live-param updates from successful paused actions while the loop is still
paused, preserving its existing shadows and A/B transaction formats.

The register driver is instance-owned over a read/write/delay bus seam; device
ports and pins are exclusive. Supported bus settings are 100–400 kHz and
10–20 ms requested timeouts, tick-rounded per IDF wait. FIFO progress/recovery
can exceed one such wait and still trigger the ordinary monitor. No safety
exemption is added. Full mode/signal coverage is retained with explicit units,
coordinate convention, per-mode availability and sample validity/age/count.

Runtime health/read faults invalidate samples and report a warning without
directly disarming; consumers choose their response. Internal initialization
failure fails startup and therefore gates ARM. A sensor-only reset never
produces fresh valid data until configuration is restored. See
`docs/protocol/imu.md` for health/recovery and blob/SIC details.

Real NDOF calibration testing waits for levels 3/3/3/3 and performs exact
application/boot-readback checks in CONFIG, because fusion automatically
recalibrates offsets/radii. The bare-board missing-sensor and mock checks
suffice for F6; physical signal/calibration quality remains deferred to HW.
