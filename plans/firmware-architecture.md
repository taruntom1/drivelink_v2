# Plan: firmware architecture v3 (self-describing core)

Status: draft, 2026-11-09. Nothing here is built yet.

**Baseline.** This plan assumes:
- `main` at `b72655b`
- the whole `diag` plan, phases 1–3 (error codes over UART, the `Boot` event, `GetInfo`, `diag_codes.yaml` with its generator, the `detail` byte, motor and IMU configure protocols at v4)
- the repo restructure (phase 1 of [host-architecture.md](host-architecture.md)): the firmware lives in `firmware/`

All paths below are relative to the repo root.

The firmware is rebuilt around a **core** that describes itself, so the host software can be generic. The host is written only after this plan is done. [host-architecture.md](host-architecture.md) describes the host side that is built on top of it.

## Goals

1. **The host learns everything from the device.** At connect it reads which modules exist, their params, signals, inputs and actions (name, type, unit, range), and their current values. No host code is specific to a module.
2. **Adding a module is local.** A new sensor, actuator or control law means one firmware module plus its YAML. No protocol handler, GUI or ROS code.
3. **The configuration lives on the ESP**, survives reboots, and is edited live from the host.
4. **Safe by default**: the device boots disarmed, a watchdog guards against host loss, and faults disarm the outputs.
5. **Good tuning tools**: on-device capture at the full loop rate, and built-in test signals.
6. **Keep the existing rules.** The real-time rules, the error rules and the "no allocation after configuration" rule in `CLAUDE.md` all still apply.

## Decisions

| Topic | Decision |
|---|---|
| Structure | A **core** component and a **module registry**. The core owns all protocol handling, the loops, the layout, persistence, arming, groups, capture, test signals, time and OTA. Modules register a factory, never touch the UART, and declare what they expose. |
| Data model | Self-describing. For each module type: **params** (configuration), **signals** (readable values), **inputs** (values the host commands) and **actions** (operations with arguments). The device serves the descriptors. |
| Definitions | YAML per module type in `protocol/defs/`. One generator turns them into pure C++ headers for `protocol/` (used by the firmware and the host) and into the docs. It also takes over the MSG_TYPE registry and `diag_codes.yaml`. |
| Ids | Stable `u16` ids per module type, never reused (retired ids stay in the YAML). An item is addressed as `Ref = (type u8, instance u8, id u16)`. |
| Values | Every param, signal and input value is a 4-byte slot: bool, u8, u16, u32, i32, f32 or enum (u8 with named values in the descriptor). Larger items go through actions. |
| Param writes | Atomic batches. If any entry is invalid, nothing is applied, and the response names the failing index with result + detail. Valid batches are staged and swapped in at the start of the owning loop's next tick. |
| Layout | Structural params are flagged `reboot_required`. `ADD_INSTANCE` and `REMOVE_INSTANCE` are system ops. Layout changes are written to NVS and take effect at the next boot. Each instance has a 16-byte label. |
| Persistence | Explicit `SAVE`, `REVERT` and `FACTORY_RESET`. NVS stores (id, value) pairs per instance. `SAVE` is refused while armed. |
| Loops | A few named loops. `control` uses an esp_timer, runs on core 1, and follows the real-time rules. `sensor` runs on core 0 and allows bus I/O. Each module type declares which loop classes it may run in, the layout assigns each instance a loop, and loop rates are params. |
| Control | Motor control is split into **Actuator**, **Feedback** and **Controller** module types, wired together by the layout. Today's code becomes `actuator.pwm_dir`, `feedback.quadrature` and `controller.cascade_pid`. |
| Telemetry | **Subscription groups.** Each group is bound to one loop, runs at a divider of that loop's rate, and packs its values with no per-value ids. Each group has its own stream channel. |
| Commands | **Command groups**, packed the same way. Host → ESP stream channels. They are applied at the start of the loop tick. While armed, they feed the watchdog. |
| Actions | A generic action table: up to 4 scalar args, plus an optional blob of up to 512 B in either direction. Actions run on the config task. |
| Safety | System arm/disarm. The device boots disarmed. While disarmed, every actuator output is forced to zero. A watchdog timeout or a `disarms` fault disarms. A `blocks_arm` fault blocks `ARM`. Both flags are set per code in the YAML. |
| Status | A system status stream (default 10 Hz) that also serves as the heartbeat. |
| Time | Every timestamp is esp_timer µs since boot. The ESP never sets its clock. The host sends pings and maps ESP time to its own clock. |
| Tuning | Generic capture into PSRAM (octal PSRAM on the target boards), with triggers and pre-trigger samples. A test signal generator drives velocity setpoints, position setpoints or raw duty. |
| Link | `LEN` becomes a `u16`. `MAX_PAYLOAD` is a Kconfig option, 1024 by default. Stream slots are allocated per group at that group's size. Bulk transfers use chunks of 512 B or less. The default baud rate is 921600, still fixed at compile time. |
| Updates | OTA over the protocol, allowed only while disarmed, with a dual OTA partition table and rollback. |
| Testing | The core's logic is pure, behind thin OS adapters. The whole core also builds natively as a **virtual device** with mock modules, which serves as the host's simulator. |
| Migration | Clean break. Each manager is ported to the core, and its typed protocol is deleted in the same PR. |

## Concepts

| Term | Meaning |
|---|---|
| Module type | A kind of module, such as `actuator.pwm_dir` or `imu.bno055`. It has a `u8` type id and one YAML file. |
| Instance | One configured module of a type. It has a `u8` instance id (unique within its type) and a 16-byte label. |
| Param | Configuration, read and write. Flags: `reboot_required`, `persist` (default on), `read_only`. |
| Signal | A value read in a loop tick: measurements, estimates and controller internals (error, P/I/D terms, integrator). |
| Input | A value commanded by the host through a command group, such as a velocity setpoint or a duty. While disarmed, inputs are ignored and outputs are zero. |
| Action | An operation with arguments and a result, such as `zero_position`, `reset_integrator` or `calibration_read`. |
| Loop | A periodic task owned by the core. Instances run in a loop, and groups attach to one. |
| Group | A subscription group (signals, ESP → host) or a command group (inputs, host → ESP). Each is bound to one loop. |
| `Ref` | `type u8 \| instance u8 \| id u16`, 4 bytes on the wire. |

Wiring between instances (which controller uses which actuator and feedback) uses `Ref`-typed params. These are `u32` params flagged `reboot_required`, which hold a packed type and instance.

## Firmware structure

| Component | Role | Comes from |
|---|---|---|
| `uart_comm` | The transport: driver, RX/TX tasks, command FIFO, stream slots, callbacks. Framing and the scanner come from `protocol/`. | today's `uart_comm` |
| `protocol` (in `/protocol`) | Framing, CRC, `wire_codec`, generated definitions and codecs. Pure C++. | extracted from `uart_comm` and the managers |
| `diag` | Fault codes, counters, latching, reporter. Moves to v2 (see below). | the diag plan |
| `core` | Registry, loops, layout and NVS, the param service, groups, capture, test signals, arming, status, time ping, OTA, and every core protocol handler. | new; absorbs the protocol parts of `motor_manager`, `imu_bmo_055` and `time_sync` |
| `core_os` | Thin adapters for tasks, timers, queues, mutexes, NVS and the clock: an ESP-IDF version and a native version. | new; builds on `rtos_util` |
| `mod_motor` | The `actuator.pwm_dir`, `feedback.quadrature` and `controller.cascade_pid` module types. | `motor_controller`, plus the module parts of `motor_manager` |
| `mod_imu_bno055` | The `imu.bno055` module type. | `imu_bmo_055` without its protocol and task code. This also ends the `bmo` misspelling. |
| `motor_hal`, `rotary_encoder_hal`, `pid` | Unchanged hardware wrappers and math. | unchanged |
| `mod_mock` | Mock actuator, feedback and IMU types for the virtual device and for tests on hardware without motors. | the mock modes of today's managers |

`motor_manager`, `imu_bmo_055` and `time_sync` are removed once their phases are done.

### Module API (sketch)

```cpp
namespace core {

// One per module type, registered at startup (static registration table).
struct ModuleType {
    const defs::TypeDesc& desc;          // generated: params, signals, inputs, actions, loop classes
    Instance* (*create)(const InstanceConfig& cfg);   // called at boot only; may allocate
};

class Instance {
public:
    virtual ~Instance() = default;
    // Boot: read structural params from cfg, acquire hardware. Report failures through DIAG_*.
    virtual esp_err_t start(const InstanceContext& ctx) = 0;
    // Loop tick (in the instance's loop). Real-time rules apply in `control`.
    virtual void tick(const TickContext& t) = 0;
    // Param, signal and input access is table-driven: the instance binds generated ids
    // to its storage in start() (ctx.bindParam(defs::motor::kVelKp, &m_vel.kp, onChange)),
    // so the core can copy staged values in, and sample signals out, without virtual calls per item.
    // Actions run on the config task.
    virtual ActionResult runAction(uint16_t action_id, const ActionArgs& in, ActionOut& out) = 0;
    // Called when the system disarms: force outputs to zero.
    virtual void disarm() {}
};

}
```

**The order of work in each loop tick**, all bounded and fixed-size:
1. Swap in staged param batches for this loop, with a try-lock and a copy.
2. Apply the latest command group values (mailbox) to the bound inputs, if armed.
3. The test signal generator overrides its target input, if it's running.
4. `tick()` each instance in this loop, in dependency order (feedback, controller, actuator).
5. Latch the subscription groups whose divider is due, and record the capture sample.
6. Update the loop statistics (overruns, run time).

Instance tick order comes from the wiring (`Ref` params) and is computed once at boot.

## Definitions (YAML)

`protocol/defs/` holds:
- `registry.yaml`: module type ids, MSG_TYPEs, stream channel ranges
- `diag_codes.yaml` (moved from the diag component)
- `system.yaml`: system params such as the watchdog timeout, the status rate and loop rates
- one file per module type

`tools/gen/` replaces `tools/gen_diag_codes.py`. It writes three things:
- `protocol/include/drivelink/protocol/gen/*.hpp`: constexpr descriptor tables, id constants, enums, and `static_assert`s for unique ids and MSG_TYPE ranges
- `docs/protocol/*.md`: the generated tables, between marker comments
- the Python table that diag's host tooling uses today, kept only while `firmware/test/uart_comm` needs it

The generated files are checked in, and CI fails if they are stale. The **descriptor hash** is a CRC32 computed at build time over all the generated tables. It is reported in device info, and the host caches descriptors by it.

```yaml
# protocol/defs/controller.cascade_pid.yaml
type: controller.cascade_pid
type_id: 0x20
loops: [control]
params:
  - {id: 0x0001, name: actuator,      type: u32, flags: [reboot_required], ref: actuator}
  - {id: 0x0002, name: feedback,      type: u32, flags: [reboot_required], ref: feedback}
  - {id: 0x0010, name: mode,          type: enum, values: {open_loop: 0, velocity: 1, position: 2}, default: open_loop}
  - {id: 0x0020, name: vel.kp,        type: f32, unit: "1/(rad/s)", min: 0, max: 1000, default: 0}
  - {id: 0x0021, name: vel.ki,        type: f32, unit: "1/rad",     min: 0, max: 1000, default: 0}
  - {id: 0x0022, name: vel.kd,        type: f32, min: 0, max: 1000, default: 0}
  - {id: 0x0023, name: vel.d_cutoff,  type: f32, unit: Hz, min: 0, max: 5000, default: 0}
signals:
  - {id: 0x0001, name: position,      type: f32, unit: rad}
  - {id: 0x0002, name: velocity,      type: f32, unit: rad/s}
  - {id: 0x0010, name: vel.error,     type: f32, unit: rad/s}
  - {id: 0x0011, name: vel.p_term,    type: f32}
  - {id: 0x0012, name: vel.i_term,    type: f32}
inputs:
  - {id: 0x0001, name: velocity_sp,   type: f32, unit: rad/s, min: -1000, max: 1000}
  - {id: 0x0002, name: position_sp,   type: f32, unit: rad}
  - {id: 0x0003, name: duty_sp,       type: f32, min: -1, max: 1}
actions:
  - {id: 0x0001, name: reset_integrators}
```

**Value rules.**
- Range checks follow the conventions: a non-finite float or an invalid enum is rejected; a valid value out of range is clamped, and the applied value is reported back.
- A param can have an `on_change` hook. Its rules:
  - It runs in the loop. It must be bounded and must not allocate.
  - If it needs to do more (for example retune a filter), it does that from the param's new value inside the tick.

## Wire protocol

### Framing

`AA AA AA | MSG_TYPE u8 | LEN u16 | payload | CRC16`
- The CRC covers MSG_TYPE, LEN and the payload.
- Overhead is 8 bytes.
- A candidate whose LEN is above `MAX_PAYLOAD` is noise: drop one byte and rescan, as today.
- MSG_TYPE ranges are unchanged.

### MSG_TYPEs (generated registry)

| MSG_TYPE | Direction | Use |
|---|---|---|
| `0x01` / `0x41` | cmd / resp | Core commands (families below) |
| `0x02` / `0x42` | cmd / resp | Time ping. Answered in the RX callback, with the RX time latched on arrival. |
| `0x23` / `0x63` | cmd / resp | diag query (unchanged) |
| `0xC0` | event | diag events |
| `0xC1` | event | Core events: capture done, test signal done, layout applied, OTA state |
| `0x80` | stream ESP → host | System status |
| `0x81–0x9F` | stream ESP → host | Subscription groups 0–30 |
| `0xA0–0xBF` | stream host → ESP | Command groups 0–31 |

The Kconfig MSG_TYPE options of motor_manager, the IMU and time_sync are removed.

### Core command header

- **Request:** `version u8 | seq u8 | family u8 | op u8 | body`
- **Response:** `version u8 | seq u8 | family u8 | op u8 | result u8 | detail u8 | body`

The rules follow the protocol conventions:
- The response echoes `seq`.
- A header that can't be parsed gets `family = op = 0xFF`.
- Bodies have exact lengths, or carry a count that must match the remaining length.

`result` and `detail` reuse the enums from diag phase 3, with these additions: `Armed` (the op is not allowed while armed), `NotArmed`, `PendingReboot`, `NoMemory` and `Exists`.

| Family | Ops |
|---|---|
| `0x01` system | `GET_INFO`, `ARM`, `DISARM`, `SAVE`, `REVERT`, `FACTORY_RESET`, `REBOOT` |
| `0x02` layout | `LIST_INSTANCES` (paged), `ADD_INSTANCE(type, label)`, `REMOVE_INSTANCE(type, inst)`, `SET_LABEL`, `DISCARD_PENDING` |
| `0x03` describe | `GET_TYPES` (paged), `GET_ITEMS(type, kind, start)` (paged: params, signals, inputs, actions with their descriptors), `GET_ENUM(type, item)` |
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
  - capture capacity, `MAX_PAYLOAD` and the loop list (id, name, rate)
  - an instance count and flags: `pending_reboot`, `layout_invalid` and `config_loaded_from_nvs`
- **Groups** are device resources, configured at runtime:
  - Configuring one allocates its stream slot or its command mailbox (configure time, so allocation is allowed).
  - Group limits come from Kconfig. The defaults are 8 subscription groups and 8 command groups, each with at most 64 entries and within `MAX_PAYLOAD`.
  - Groups are not persisted. The host sets them up after each connect, and the Boot event tells it when to do that again.
- **Ops refused while armed:** `SAVE`, `FACTORY_RESET`, `REBOOT`, `ota.*`, all layout ops, and `CMD_CONFIG` for a group that targets an armed loop.
- **Capture `trigger`:**
  - `manual`
  - `testgen_start`
  - `input_change(Ref)`
  - `threshold(Ref, rising|falling, value)`

### Streams

All stream payloads start with `version u8 | counter u16 | timestamp_us u64`. The counter wraps, so a gap means packets were lost.

| Stream | Body after the common prefix |
|---|---|
| System status `0x80` | `arm_state u8 \| flags u8 (fault_active, blocks_arm_active, pending_reboot, nvs_dirty, testgen_running, capture_state×2) \| active_error_count u8 \| cpu_load u8 × 2 \| overruns u32 per loop \| last_disarm_reason u8` |
| Subscription group | `n × value(4)`, in the order given at configuration |
| Command group (host → ESP) | the prefix without a timestamp, then `n × value(4)` |

Overhead per frame: 8 for the frame, plus 11 for the prefix (3 for command groups).

### Time ping

- **Request:** `version u8 | id u16`
- **Response:** `version u8 | id u16 | esp_rx_us u64 | esp_tx_us u64`

The RX time is taken in the RX callback, and the TX time just before the response is queued. The host:
1. timestamps its own send and receive
2. keeps the pings with the smallest round trip
3. fits offset and drift (a linear fit over a sliding window)

## Arming and safety

- **States:** `Disarmed` (the boot state), `Armed`, and `Fault`. `Fault` is `Disarmed` with a latched reason, and it needs an explicit `DISARM` (acknowledge) before `ARM` is accepted again.
- **`ARM` is refused** when:
  - any `blocks_arm` code is active
  - the layout is invalid or a reboot is pending
  - an instance failed to start
  - no command group has been configured since boot
- **Disarm triggers:**
  - `DISARM`
  - a watchdog timeout (no command group frame, and no empty keepalive frame, for `system.watchdog_ms`, default 200)
  - recording any diag code flagged `disarms`
  - a reboot
- **Disarm path:**
  1. An atomic flag is set. This is real-time safe: `DIAG_COUNT` of a `disarms` code sets it.
  2. In the next tick, each loop calls `disarm()` on its instances. The actuators drive zero duty (coast).
  3. The test signal generator stops. A running capture keeps recording, so the event is captured.
- **diag YAML:** `blocks_arm: bool` and `disarms: bool` per code, generated into the slot table. Examples:
  - a feedback loss or a stalled control timer both disarm
  - a sync failure does neither
- **Future hardware e-stop:** a GPIO input module records an `estop` code with `disarms` and `blocks_arm`.

## Layout, persistence and boot

- **NVS layout:**
  - One namespace per instance, `t<type>i<inst>`.
  - Each key holds one id → value pair. In practice the values are packed into one blob of `n × (id u16, value u32)` per instance, with a header (format version, count, CRC32).
  - A `layout` blob lists the instances, with their type, id and label.
- **Unknown ids are ignored, and missing ids take their YAML default.** Adding or removing params in a firmware update therefore needs no migration. A retired id is skipped.
- **Ops:**
  - `SAVE` writes every `persist` param of every instance, plus the layout, then clears `nvs_dirty`.
  - `REVERT` reloads the live (non-structural) params from NVS through the normal staged path.
  - `FACTORY_RESET` erases the namespaces and reboots.
- **Pending changes:** a `reboot_required` param write or a layout op goes to a pending copy. Device info and the status stream show `pending_reboot`. The change takes effect after `SAVE` and `REBOOT`. `DISCARD_PENDING` drops it.
- **Flash stalls the loops.** NVS writes disable the cache on both cores, so the loops can stall for several ms. That's why `SAVE` is refused while armed. The status stream counts the overruns this causes while disarmed. This is acceptable for now; it can be revisited by putting the control path in IRAM.
- **Boot sequence:**
  1. `diag::init()`
  2. `UartComm`
  3. core init: load the layout and params from NVS, or the defaults
  4. create the instances in dependency order. An instance that fails is reported (`<source>.InitFailed`) and left out, and anything wired to it is left out too (`core.DependencyMissing`).
  5. start the loops
  6. the diag `Reporter`
  7. the `Boot` event goes out

  The device stays up in a degraded state, as in diag phase 1.
- **No layout yet** (first boot, or after a factory reset): the device boots with zero instances and waits for the host. The board is then configured entirely from the GUI.

## diag v2

- **Timestamps are monotonic.** The `time_synced` flag and `diag::setTimeSynced()` are removed.
- **`Boot` body:** `timestamp_us u64 | reset_reason u8 | instances_ok u8 | instances_failed u8`. `started_mask` goes away; the instance details come from `LIST_INSTANCES`.
- **Sources become module types** (generated from `registry.yaml`), plus `system`, `uart_comm`, `diag` and `core`. Records about one instance carry `instance` in `arg0`, so the existing per-manager stage numbers move to `arg1`.
- **`blocks_arm` and `disarms`** are added to each code (see above).
- **GetInfo** stays as a diagnostic view. Device identity is in `system.GET_INFO`.
- The YAML moves to `protocol/defs/`, and the generator becomes `tools/gen/`.

## Capture and test signals

**Capture**
- Up to `capture.max_refs` signals (default 16) from one loop, at a divider of its rate.
- Stored in a PSRAM ring of `samples × n × 4` bytes, plus a `u64` start timestamp; the samples are evenly spaced.
- The buffer is allocated at `CONFIG` time, which is configure time.
- PSRAM is enabled in `sdkconfig.defaults` (octal, 80 MHz). `GET_INFO` reports the capacity.
- States: `Idle → Armed → Triggered → Done`. A `Done` event goes out on `0xC1`. `READ` pages through the samples in chunks of 512 B or less.
- The loop only writes `n` values into the ring per due tick. That is fixed work, and it never allocates.

**Test signal generator**
- One generator for each loop.
- Waveforms: `step`, `square`, `ramp`, `sine`, and `chirp` (linear or log). Parameters: offset, amplitude, period or frequency start and end, duration, repeat.
- Targets for now: the `velocity_sp`, `position_sp` and `duty_sp` inputs of a `controller.cascade_pid` instance. The YAML flags each input that may be targeted (`testgen: true`).
- Every generated value is clamped to the input's range.
- The generator runs only while armed and stops on disarm. While it runs, command group values for its target are ignored.
- **Plant identification** uses `duty_sp` in `open_loop` mode with capture triggered by `testgen_start`. The host fits the model; the firmware only generates and records.

## OTA

- **Partition table:** `nvs`, `otadata`, `phy_init`, `ota_0` (4 MB), `ota_1` (4 MB), and spare space for later (16 MB flash).
- **Bootloader:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.
- **Sequence:**
  1. `BEGIN(size, sha256)` checks that the device is disarmed and calls `esp_ota_begin`.
  2. `WRITE(offset, chunk ≤ 512)` writes in order and is answered for each chunk. The host keeps a few chunks in flight, within the config queue depth.
  3. `FINISH` checks the SHA-256 and sets the boot partition.
  4. `REBOOT`.
- **Confirmation:** the new image confirms itself (`esp_ota_mark_app_valid_cancel_rollback`) only after the host sends `CONFIRM`. Without it, the next reset rolls back. `CONFIRM` is sent after the host has reconnected and checked `GET_INFO`.
- The flash writes run on the config task, so the loops stall as with NVS, which is why OTA requires the device to be disarmed.

## Virtual device

- `core`, `protocol` and the pure parts of the modules build natively against `core_os`'s native backend: std::thread, a timer thread, a file-backed key-value store, and steady_clock.
- `mod_mock` provides `actuator.mock` and `feedback.mock`, which model a first-order motor, and `imu.mock`.
- A `drivelink-vdev` executable serves the protocol on a TCP port, or a pty on Linux. It is the simulator for the host phases and for CI integration tests. It builds with the host superbuild (MinGW and Linux).
- It also runs the real modules' pure logic: PID, velocity estimator and controller. Only the HALs are mocked.

## Testing

| Layer | Tests |
|---|---|
| `protocol/` | Codec round trips for every family, stream and event; exact lengths; paging; the generator's static checks |
| `core` (native) | Batch validation and atomicity, staging and swap, group packing, capture ring and triggers, test signal waveforms, arming state machine, NVS blob codec (unknown and missing ids), boot with failing instances |
| Modules (native) | Controller cascade, the order of the actuator, feedback and controller ticks, disarm behavior |
| `vdev` | End-to-end over TCP: connect, describe, configure, arm, command, capture, save, reboot, reload |
| Hardware | `firmware/test/uart_comm` HIL tests, updated for the new framing, plus a smoke test on hardware that flashes, connects and runs a capture |

CI adds three jobs: the native core and module tests, a vdev integration job, and the generator staleness check. The existing IDF builds and the nolog variant stay.

## Phases

Each phase is one PR, or a short series of PRs. Each builds with zero warnings and keeps CI green. The repo restructure comes before F1.

| Phase | Content | Done when |
|---|---|---|
| F1 Link and `protocol/` | Extract framing, CRC, scanner and `wire_codec` into `protocol/`. LEN becomes `u16`, `MAX_PAYLOAD` defaults to 1024, stream slots are allocated at registration with their own size, the default baud becomes 921600. Bump every module protocol version for the framing change. The codec tests move from `firmware/test/host` to `protocol/test`. | HIL tests pass with the new framing, and the managers still work over it |
| F2 Definitions and generator | `tools/gen/`, `registry.yaml`, and `diag_codes.yaml` moved to `protocol/defs/`. The generated registry replaces the Kconfig MSG_TYPEs. The descriptor table format and hash. The legacy manager streams move out of the way: motor setpoint `0xBD`, motor telemetry `0xBE`, IMU `0xBF`. They are marked `legacy`, and each is removed when its manager is ported. Until then the registry's `static_assert`s keep the group channel limits below them. | Generated headers are used by diag and the managers, and the CI staleness check passes |
| F3 Core skeleton | `core_os` (IDF and native), registry, loops (`control`, `sensor`), layout and NVS, the system/layout/describe/param families, the action family, the status stream, `GET_INFO`, `mod_mock`, and `drivelink-vdev` (in `host/vdev/`). diag v2. `firmware/test/host` becomes `firmware/test/native`, which builds the core against `core_os`'s native backend. | vdev and hardware: mock instances can be added, described, set, saved, rebooted and reloaded |
| F4 Groups and arming | Subscription and command groups, arming, the watchdog, and the `blocks_arm` / `disarms` flags, all working on mock instances. | vdev: mock motors are commanded and streamed; the watchdog and faults disarm |
| F5 Motor stack as modules | Split `motor_controller` into the actuator, feedback and controller types, with YAML. Delete `motor_manager`. | Velocity and position control on hardware through groups; nothing remains of the old motor protocol |
| F6 IMU module | `imu.bno055` on the `sensor` loop, with calibration actions (`calibration_read`, `calibration_write`, `calibration_apply`). Delete `imu_bmo_055`. | IMU signals in subscription groups, and calibration works through actions |
| F7 Capture and test signals | The capture and testgen families, PSRAM enabled. | A step response and a chirp captured at 1 kHz on hardware |
| F8 Time | The ping on `0x02`. Delete `time_sync`. | The host's offset and drift estimate is stable, with jitter below 100 µs on the bench |
| F9 OTA | The partition table, the OTA family and rollback. | Updating from the vdev host tooling, and rollback when `CONFIRM` isn't sent |
| F10 Cleanup | Docs (`CLAUDE.md`, `components/CLAUDE.md`, `docs/protocol/*`), `review/` statuses, and removing the leftover Kconfig. | Every rule in the docs matches the code |

Arming lands (F4) before any real motor is driven through the core (F5), so the board is never driven by the new code without a watchdog. From F3 to F5 the old `motor_manager` keeps working alongside the core. Each manager is deleted in the phase that ports it, so no module ever exists in both forms.

## Decisions made while writing this plan (review these)

- **Time ping on its own MSG_TYPE, answered in the RX callback.** Going through the config queue would add jitter to the timestamps.
- **`Fault` needs an explicit `DISARM` before re-arming**, so a disarm after a fault is never silently cleared by a fresh `ARM`.
- **Groups are not persisted.** They describe what one host session wants. The host sets them up after each connect, and the Boot event tells it when to do so again.
- **Signals and inputs use the 4-byte slot, as params do.** IMU data that is `i16` today becomes `f32` in SI units, which doubles its bandwidth. A 10-value IMU group at 100 Hz is still only about 6% of 921600 baud.
- **One test signal generator per loop.** That covers tuning one controller at a time. More than one would need ownership rules.
- **`ARM` requires a configured command group**, so the watchdog always has a feed to watch.
- **The NVS format is (id, value) pairs, not the wire encoding of a struct.** It tolerates params being added or removed between firmware versions.
- **`imu_bmo_055` becomes `mod_imu_bno055`.** The rewrite is the moment to fix the spelling.

## Out of scope

- USB transport. The transport stays behind `uart_comm`, so USB can be added later.
- Runtime baud switching.
- Strings and arrays as param types, apart from the instance label.
- Wiring between instances that changes at runtime. Wiring is `reboot_required`.
- Encrypting or signing the OTA image. The SHA-256 is only an integrity check.

## Open questions

- Should capture be allowed while disarmed, to record sensors only? Proposed: yes, as long as no testgen is running.
- `control` loop default rate: 1 kHz as today? And the `sensor` default: 100 Hz?
- Should a removed instance's NVS namespace be erased at once, or kept until `FACTORY_RESET`?
- How big is the keepalive frame? An empty command-group frame (prefix only, `n = 0`) is the proposal.
