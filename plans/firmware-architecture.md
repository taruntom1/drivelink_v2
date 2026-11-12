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
| Loops | A few named loops. `control` uses an esp_timer, runs on core 1, and follows the real-time rules. `sensor` runs on core 0 and allows bus I/O. Each module type declares which loop classes it may run in, the layout assigns each instance a loop (stored in its layout entry), and loop rates are params: `system.control.rate_hz` (default 1 kHz, 100–2000 Hz) and `system.sensor.rate_hz` (default 100 Hz, 10–500 Hz), both `locked_while_armed`. |
| Control | Motor control is split into **Actuator**, **Feedback** and **Controller** module types, wired together by the layout. Today's code becomes `actuator.pwm_dir`, `feedback.quadrature` and `controller.cascade_pid`. |
| Mode changes | Allowed while armed, **bumpless**. They apply at a tick boundary. The new mode's setpoint starts from the current measurement, and its integrator is preset so that the output doesn't jump. |
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
| Loop | A periodic task owned by the core. Instances run in a loop, and groups attach to one. |
| Group | A subscription group (signals or inputs, ESP → host) or a command group (inputs, host → ESP). Each is bound to one loop. |
| `Ref` | `type u8 \| instance u8 \| id u16`, 4 bytes on the wire. The id alone says which kind of item it is. |

Wiring between instances (which controller uses which actuator and feedback) uses `Ref`-typed params. These are `u32` params flagged `reboot_required`, which hold a packed type and instance. **Wired instances must run in the same loop**, and layout validation enforces that.

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
    const defs::TypeDesc& desc;          // generated: items, loop classes, action placement
    Instance* (*create)(const InstanceConfig& cfg);   // called at boot only; may allocate
};

class Instance {
public:
    virtual ~Instance() = default;
    // Boot: bind params, signals and inputs, read structural params through ctx,
    // claim GPIOs through ctx, acquire hardware. On failure call ctx.fail(stage, err)
    // and return false; the core reports core.InstanceStartFailed.
    virtual bool start(InstanceContext& ctx) = 0;
    // Loop tick (in the instance's loop). Real-time rules apply in `control`.
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
    // (for pwm_dir: mcpwm_generator_set_force_level on both generators).
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
5. Apply the latest command group values from the mailbox to the bound inputs, if armed and the frame arrived after `ARM` with the current epoch.
6. The test signal generator overrides its target input, if it's running.
7. `tick()` each instance in this loop, in dependency order (feedback, controller, actuator).
8. Latch the subscription groups whose divider is due, and record the capture sample.
9. Update the loop statistics (overruns, run time).

Instance tick order comes from the wiring (`Ref` params) and is computed once at boot.

## Definitions (YAML)

`protocol/defs/` holds:
- `registry.yaml`: MSG_TYPEs and stream channel ranges. Module type ids live in each type's own YAML (`type_id`); the generator checks they are unique, and `0x00` is reserved for `system`.
- `diag_codes.yaml` (moved from the diag component)
- `system.yaml`: system items such as the watchdog timeout, the status rate and loop rates (type `0x00`)
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
loops: [control]
items:
  # params
  - {id: 0x0001, kind: param,  name: actuator,      type: u32, flags: [reboot_required], ref: actuator}
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

The Kconfig MSG_TYPE options of motor_manager, the IMU, time_sync and diag were removed in F2; `protocol/defs/registry.yaml` holds every assignment, and `docs/protocol/registry.md` is its generated table. Until their managers are ported, the legacy entries stay there: time_sync `0x20` / `0x60` (until F8), motor `0x21` / `0x61` and streams `0xBD` / `0xBE` (until F5b), IMU `0x22` / `0x42` and stream `0xBF` (until F6). **The time ping's `0x02` / `0x42` enter the registry in F8**, because the legacy IMU configure response holds `0x42` until F6.

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
| `0x02` layout | `LIST_INSTANCES` (paged), `ADD_INSTANCE(type, loop, label)` (the loop defaults to the type's first loop class), `REMOVE_INSTANCE(type, inst)`, `SET_LABEL`, `DISCARD_PENDING` |
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
  - capture capacity, the receive limit and the loop list (id, name, rate)
  - an instance count and flags: `pending_reboot`, `layout_invalid`, `config_loaded_from_nvs` and `safe_mode`
- **Param `SET`**
  - **Staging rules:**
    - `reboot_required` params go to the pending layout.
    - Live params must all belong to instances in one loop, or the batch is rejected (`CrossLoopBatch`).
    - If that loop's staging slot is taken, the batch is rejected (`Busy`, `StagingBusy`).
    - Params flagged `locked_while_armed` are refused while armed: loop rates, `system.watchdog_ms`, and wiring.
  - **Response timing.** The config task stages the batch and waits for the loop to take it, for up to 3 loop periods plus 5 ms.
    - If the loop takes it, the response is `Ok` with the applied values (after clamping), and a `GET` right after returns the new values.
    - If it times out, the config task withdraws the batch with a compare-and-swap on the slot and answers `Busy`. Either way the outcome is definite.
  - **Batches for `sensor`.** A batch for `sensor` instances is applied the same way at the sensor loop's next tick.
- **Groups** are device resources:
  - All of them are allocated at boot at the maximum size (64 entries, so about 270 B each; about 4.3 KB for 8 + 8 groups) and never freed.
  - `SUB_CONFIG` and `CMD_CONFIG` write a new configuration that the loop swaps in at a tick boundary, and they bump the group's **epoch** (u8). `CMD_CLEAR` and `SUB_CLEAR` disable a group, also through the swap.
  - Group limits come from Kconfig. The defaults are 8 subscription groups and 8 command groups, each with at most 64 entries and within `MAX_PAYLOAD`.
  - Groups are not persisted. The host sets them up after each connect, and the Boot event tells it when to do that again.
  - A subscription group may contain signals and inputs. A command group may contain only inputs (otherwise `WrongKind`).
- **Ops refused while armed:** `SAVE`, `FACTORY_RESET`, `REBOOT`, `ota.*`, all layout ops, `CMD_CONFIG` and `CMD_CLEAR` for a group in an armed loop, and `paused` actions on instances in the `control` loop.
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
| System status `0x80` | `arm_state u8 \| flags u8 (fault_latched, blocks_arm_active, pending_reboot, nvs_dirty, testgen_running, safe_mode, capture_state×2) \| active_error_count u8 \| cpu_load u8 × 2 \| overruns u32 per loop \| last_disarm_reason u8` |
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
  - each loop's heartbeat. If it hasn't advanced for 3 periods, plus 5 ms, it records `core.LoopStalled` (flagged `disarms`).
  - the time of the last valid command frame against `system.watchdog_ms`. On a timeout it records `core.WatchdogTimeout`, which disarms but does not latch.
- **What it does on either condition:** it sets the disarm flag and calls `emergencyStop()` on every actuator instance. For `actuator.pwm_dir` that forces both MCPWM generators low (`mcpwm_generator_set_force_level`), so the outputs stop even if the loop never runs again.
- **Releasing the force.** On the next `ARM`, the loop releases the force level through `onArmTransition(true)`.
- **Backstop.** The control task is also subscribed to the Task WDT, with panic-reset, for when even the esp_timer task is stuck.
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
- Each blob has two slots, A and B. `SAVE` writes the slot with the older generation. Loading takes the valid slot with the highest generation. A power loss during `SAVE` therefore leaves the previous configuration intact.
- The `layout` blob (instances, with their type, id, loop and label) uses the same A/B scheme. One `SAVE` gives every blob the same generation, and loading uses only blobs of the newest generation that is complete across the layout.

**Ids across firmware versions.** Unknown ids are ignored, and missing ids take their YAML default. Adding or removing params in a firmware update therefore needs no migration. A retired id is skipped.

**Ops:**
- `SAVE` writes every `persist` param of every instance, plus the layout, then clears `nvs_dirty`.
- `REVERT` reloads the live (non-structural) params from NVS through the normal staged path, one batch per loop.
- `FACTORY_RESET` erases the namespaces and reboots.
- `REMOVE_INSTANCE`, once applied at reboot, erases that instance's namespace. A later `ADD_INSTANCE` that reuses the id starts from the YAML defaults.

**Pending changes.** A `reboot_required` param write or a layout op goes to a pending copy. Device info and the status stream show `pending_reboot`. The change takes effect after `SAVE` and `REBOOT`. `DISCARD_PENDING` drops it.

**Layout validation** runs when a layout op or a `reboot_required` write is staged, and again at boot:
- **GPIOs:** the core keeps a GPIO registry. Every pin param is checked against the ESP32-S3 pin policy that `motor_manager` uses today (no USB pins, no flash/PSRAM pins 26–37, no uart_comm pins). It is also checked against every other instance's pins (`GpioConflict`).
- **Wiring:** a `Ref` param must point to an existing instance of the right type, in the same loop (`LoopMismatch`).
- **Labels:** must be unique on the device (`LabelInUse`) and non-empty.

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

**No layout yet** (first boot, or after a factory reset): the device boots with zero instances and waits for the host. The board is then configured entirely from the GUI.

**Safe mode:**
- A counter in RTC no-init memory counts consecutive boots whose reset reason is a panic, an interrupt WDT or a task WDT. It is cleared after 30 s of uptime.
- When it reaches 3, the device boots with zero instances and sets `safe_mode` in `GET_INFO` and the status stream, plus the diag code `core.SafeModeEntered`.
- In safe mode the host can read and edit the layout and params, `FACTORY_RESET`, or run OTA, then `EXIT_SAFE_MODE`, which clears the counter and reboots.

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
- `mod_mock` provides `actuator.mock` and `feedback.mock`, which model a first-order motor, and `imu.mock`.
- A `drivelink-vdev` executable (in `host/vdev/`) serves the protocol on a TCP port, or a pty on Linux. It is the simulator for the host and for CI integration tests. It builds with the host superbuild (MinGW and Linux).
- It also runs the real modules' pure logic: PID, velocity estimator and controller. Only the HALs are mocked.
- The safety monitor, the safe-mode counter and the NVS A/B logic run in the vdev too. Crashes and power loss can be simulated by killing the process.

## Testing

| Layer | Tests |
|---|---|
| `protocol/` | Codec round trips for every family, stream and event; exact lengths; paging; HCRC rejection of false preambles; the generator's static checks (unique ids per type) |
| `core` (native) | Batch validation, atomicity, cross-loop rejection, staging busy and withdrawal; group swap and epoch; command frame rules; capture ring and triggers; test signal waveforms; the arming state machine including the epoch race, latching and `ACK_FAULT`; the monitor's stall and watchdog detection; NVS A/B generations and torn writes; layout validation; safe mode; boot with failing instances |
| Modules (native) | Controller cascade, bumpless mode switch, the order of the actuator, feedback and controller ticks, `onArmTransition` resets |
| `vdev` | End-to-end over TCP with `drivelink-cli`: connect, describe, configure, arm, command, watchdog trip, capture, save, reboot, reload |
| Hardware | `firmware/test/uart_comm` HIL tests, updated for the new framing (with `--baud` defaulting to 921600), plus a smoke test on hardware: flash, connect, arm, run a capture, and a loop-stall test that forces the monitor to stop the motors |

CI adds three jobs: the native core and module tests, a vdev integration job, and the generator staleness check. The existing IDF builds and the nolog variant stay.

## Phases

Each phase is one PR, or a short series of PRs. The steps are listed in [overhaul-status.md](overhaul-status.md). Each PR targets `firmware_overhaul`, builds with zero warnings, keeps CI green, and updates its docs (`docs/protocol/`, `CLAUDE.md`, `firmware/CLAUDE.md`, `firmware/components/CLAUDE.md`, the READMEs of new components, the status file) in the same PR. The repo restructure (S1) comes before F1.

| Phase | Content | Done when |
|---|---|---|
| F1 Link and `protocol/` | Extract framing, CRCs, scanner and `wire_codec` into `protocol/`. New framing: u16 `LEN` + HCRC8, `MAX_PAYLOAD` 1024, default baud 921600, `CONFIG_UART_ISR_IN_IRAM`. `uart_comm` fixes for the bigger frames: buffers move from the stacks into `Impl`, task stacks are resized and checked, stream slots are allocated with their own size, and `sendData` gets a non-blocking slot write (double buffer plus try-lock, with a counter for skipped writes) so the control tick never waits. Bump every module protocol version. The codec tests move from `firmware/test/host` to `protocol/test`, and `conftest.py` defaults `--baud` to 921600. | HIL tests pass with the new framing, the managers still work over it, and stack high-water marks are logged |
| F2 Definitions and generator | `tools/gen/`, `registry.yaml`, and `diag_codes.yaml` moved to `protocol/defs/`. The generated registry replaces the Kconfig MSG_TYPEs, and the "IDs from Kconfig" convention in `firmware/CLAUDE.md` is replaced in the same PR. The descriptor table format and hash. The legacy manager streams move out of the way: motor setpoint `0xBD`, motor telemetry `0xBE`, IMU `0xBF`. They are marked `legacy`, and each is removed when its manager is ported. Until then the registry's `static_assert`s keep the group channel limits below them. motor_manager and the IMU go to protocol v6 with the move. | Generated headers are used by diag and the managers, and the CI staleness check passes |
| F3 Core skeleton and dev client | `core_os` (IDF and native), registry, loops (`control`, `sensor`), layout and NVS (A/B, validation, GPIO registry), safe mode, the system/layout/describe/param families, the action family, the status stream, `GET_INFO`, `mod_mock`, `drivelink-vdev` (in `host/vdev/`). diag v2. The first slice of `libdrivelink` (serial and TCP transport, session, the core families) and `drivelink-cli`, built by a first `host/superbuild`. `firmware/test/host` becomes `firmware/test/native`, which builds the core against `core_os`'s native backend. | With the CLI, on vdev and on hardware: mock instances can be added, described, set, saved, rebooted and reloaded |
| F4 Groups and arming | Subscription and command groups (preallocated, epochs), arming and the arm state machine, `DISARM` fast path, the safety monitor, the watchdog, `DIAG_RAISE`, the `blocks_arm` / `disarms` flags and `kind: latched`, all working on mock instances. The CLI gains arm, stream and plot-to-CSV commands. | vdev: mock motors are commanded and streamed through the CLI; the watchdog, a simulated loop stall and faults disarm |
| F5 Motor stack as modules | Split `motor_controller` into the actuator, feedback and controller types, with YAML, `emergencyStop()`, `onArmTransition()` and bumpless mode switching. Delete `motor_manager`. | Velocity and position control on hardware through groups; the monitor stops the motors in the loop-stall test; nothing remains of the old motor protocol |
| F6 IMU module | `imu.bno055` on the `sensor` loop, with calibration actions (`calibration_read`, `calibration_write`, `calibration_apply`, all `runs_in: paused`). Delete `imu_bmo_055`. | IMU signals in subscription groups, and calibration works through actions |
| F7 Capture and test signals | The capture and testgen families, PSRAM enabled (caps alloc only). Evaluate `SPIRAM_XIP_FROM_PSRAM`. | A step response and a chirp, captured at 1 kHz on hardware with their inputs |
| F8 Time | The ping on `0x02`, with TX-time stamping in the TX task. Delete `time_sync`. | The host's offset and drift estimate is stable, with jitter below 100 µs over the Pi's GPIO UART (USB-UART adapters add up to 1 ms of USB frame jitter; their target is a stable estimate, not a jitter bound) |
| F9 OTA | The partition table (NVS unchanged), the OTA family with sequential and idempotent writes, and rollback. | Updating with the CLI, a retried chunk is acknowledged, and rollback when `CONFIRM` isn't sent |
| F10 Cleanup | Remove the leftover Kconfig and legacy registry entries. Rewrite `firmware/CLAUDE.md` for the new architecture and remove the temporary overhaul section from the root `CLAUDE.md`. A full docs consistency pass. Merge `firmware_overhaul` into `main`. | Every rule in the docs matches the code, and `main` holds the new firmware |

Arming lands (F4) before any real motor is driven through the core (F5), so the board is never driven by the new code without the watchdog and the monitor. From F3 to F5 the old `motor_manager` keeps working alongside the core. Each manager is deleted in the phase that ports it, so no module ever exists in both forms.

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
