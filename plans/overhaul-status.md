# Firmware overhaul status

The working state of the overhaul described in [firmware-architecture.md](firmware-architecture.md) and [host-architecture.md](host-architecture.md). **Every PR into `firmware_overhaul` updates this file.** A new session reads it first, and `/overhaul-step` does that for you.

## Current

- **Branch:** `firmware_overhaul`. `main` is frozen except for critical fixes, which are merged into `firmware_overhaul` at once. `firmware_overhaul` is merged into `main` after F10.
- **Next step:** F4a: subscription and command groups (preallocated, epochs). Start after F3c is merged.
- **Handoff (F3c):**
  - `host/README.md` has Linux/Qt-MinGW builds and CLI/vdev usage. The host
    superbuild is `host/superbuild`; colcon still discovers the protocol and
    host library packages separately. Installed find_package consumers are tested.
  - `Session` owns one Asio I/O thread; synchronous calls serialize. Timed-out
    seqs retire until reconnect; only reads retry. Mutation timeouts have unknown
    outcomes. Event queues drop oldest with a visible count. F4a can add group
    services and stream decoding without changing the transport/session facade.
  - Services use device descriptors and nlohmann JSON 3.11.3 (public ABI, exact
    version required). No module-specific host code or full cached device/time
    model yet. Core v1 and diag v3 have no F3c wire-format changes.
  - Vdev runs real core/mock loops, one config caller, bounded diagnostics,
    TCP or Linux PTY, per-blob durable KV and a crash running-marker file.
    Idle PTY backpressure retains the master; reply drain failures cancel effects
    and recover with handler/composed-write generation guards. SIGTERM is clean
    on Linux; killing a process counts as a crash. Native unit tests remain
    in-memory unless file storage is explicitly attached before boot.
  - Windows Asio flow_control(none) reasserts DTR/RTS and resets the board.
    Keep the disabled bootstrap lines supplied by open(). Transient native
    Win32 AccessDenied/FileNotFound at reopen retry before any request is sent;
    Asio's similarly named socket error is not the native serial code.
  - Self-review and independent high-effort Codex review completed. All three
    findings were fixed and rechecked: idle PTY closure, premature diag pages and
    exported JSON ABI version mismatch. Hardware-discovered bootstrap-line and
    native error-code fixes were independently checked too. None were rejected.
  - CLI bare-board smoke passed on 2026-11-13 (COM3 protocol, COM4 console), with
    temporary mock layout, typed params, SAVE/reboot/reload, actions, status/diag
    and restored original zero-instance layout/rates 1000/100/10 Hz.
- **Handoff (F3b):**
  - Core v1 is specified in `docs/protocol/core.md`; diag redesign is wire v3
    (`docs/protocol/diag.md`). Enums come from `registry.yaml::wire_enums`.
  - `CommandService` is pure, single-config-caller and owns fixed scratch storage.
    F3c can feed framed requests into it and execute `takeAfterReply()` effects.
    Supply device version, build id, receive cap and keep bench_crash false.
  - Mock type list is `mod_mock::kTypes` (0x70..0x72). The exact first-order
    motor and wrapped-yaw IMU are deterministic; feedback uses the previous
    actuator tick. Mock reset exercises paused actions and bounded blob echo.
  - Layout LIST takes current/pending view plus cursor. GET_PENDING reads effective
    staged values. Descriptors use whole-record paging and generated size checks.
  - Action Event signals are hints: the config caller tests slot state and deadline;
    only Ready can be withdrawn. Once claimed it waits for definite completion.
    Paused control-loop actions and all reboot paths refuse an armed device.
  - Diagnostic state has 254 sparse, fixed identities. The pure record and counter
    bridges accept instance IDs; native counters also retain those identities.
    F4b adds latch/arming semantics; metadata flags exist with false defaults.
  - Status defaults to 10 Hz (1..100); CPU utilization samples idle runtime deltas,
    without stack scans. Runtime stats default on; first/unsupported samples are
    0xFF. SMP/unicore builds skip the current two-core sampling adapter.
  - BenchCrash is system op254 and exists only with CORE_BENCH_CRASH enabled.
    Normal builds reject it. `smoke_core_firmware.py --safe-mode` is for the bench
    test build; normal firmware must be restored after testing.
  - The self-review and independent Codex reviewer found and fixed an armed reboot
    bypass via EXIT_SAFE_MODE and stale action signals causing premature Busy.
    Follow-up review covered CPU sampling and the instance counter bridge.
- **Still true from F3a-2:**
  - **The C++ API F3b wraps** (all on `core::Core`, config-task side):
    - ADD_INSTANCE is `addInstance(type, loop, label, &inst)`. The device picks the lowest free instance id (0..254). The default loop (the type's first loop class) belongs in the F3b handler.
    - REMOVE_INSTANCE is `removeInstance`; it is refused while another entry refs it (`Referenced`). SET_LABEL is `setLabel`, DISCARD_PENDING is `discardPending`, GET_PENDING is `getPending`.
    - SAVE is `save`, REVERT is `revert`, FACTORY_RESET is `factoryReset`, EXIT_SAFE_MODE is `exitSafeMode`. After FACTORY_RESET and EXIT_SAFE_MODE the handler reboots (`core_os::reboot()`).
    - LIST_INSTANCES uses `findRecord` and the record states (`Running`, `Failed`, `Invalid`, `DependencyMissing`, `Offline`).
    - GET_INFO uses `safeMode`, `layoutInvalid`, `configLoadedFromNvs`, `pendingReboot`, `nvsDirty`, `instanceCount` (running), `configuredCount` and `instancesFailed`.
    - Layout ops answer `Ok`, are refused while armed, and need a booted core.
  - **Param SET answers `PendingReboot`** for a batch of `reboot_required` params, or for params of instances that exist only in the pending layout. Callers test `Status::accepted()`, not `ok()`. A batch has one domain: system, one loop, offline (an instance that isn't running: written to its shadow) or pending. Otherwise the answer is `CrossLoopBatch`.
  - **Boot** is `init()`, then `boot(store)`, then `start()`. `createInstance` is gone; instances come only from the NVS layout. Native tests write a layout with `writeLayout`, then boot (`bootWith` / `bootCore`); a reboot is a new `Core` on the same native `KvStore`.
  - **NVS:**
    - Namespaces are `core` (`layout_a` / `layout_b`) and `t<type>i<inst>` (`p_a` / `p_b`). The system params are in `t00i00`.
    - A SAVE writes generation `gen` into slot `gen & 1`, the instance blobs first and the layout blob last (the commit record). It never writes the slot of the complete generation it loaded or last saved.
    - Each distinct `t??i??` uses one of NVS's 254 namespace slots for good, even after an erase.
  - **Result / Detail** grew: `GpioNotAllowed`, `BadRef`, `WiringCycle`, `Referenced`, `DependencyMissing`, `NoSavedConfig`, `StorageFailed`. F3b numbers them in `registry.yaml`. `core.LayoutInvalid` arg1 carries the raw `Detail`, so F3b's numbering fixes its meaning.
  - **The wiring and pin helpers** are in `defs.hpp`: `kNoGpio`, `kUnwired`, `wiringRef`, `isWiringRef`, `wiringRefType`, `wiringRefInstance`, `inCategory`. F3b's describe must send the new flag bits (`Gpio` 0x20, `RefTicksAfter` 0x40).
  - **`core_config_idf.cpp`** reads `CONFIG_UART_COMM_TX_PIN` / `RX_PIN` for the GPIO policy, so an app that builds `core` needs `uart_comm` in its config.
  - **`CONFIG_CORE_BOOT_SELFTEST`** (main's Kconfig) is a bench-only check: every boot flips `control.rate_hz` between 1000 and 500 and SAVEs. Never commit it on. F3b can drop it once SAVE is on the wire.
  - F3b has now run the legacy main smoke over COM3 and safe-mode crash/recovery on the board. The previous lack of a CH343 adapter and crash hook is resolved.
- **Still true from F3a-1:**
  - **The core** is `firmware/components/core` (namespace `core`). It is pure and builds natively; it uses only `core_os`, `protocol/` and std, and reports through the `core_os` diag sink. Kconfig reaches it only through `CoreConfig` (`src/idf/core_config_idf.cpp`). Its READMEs: `firmware/components/core/README.md`, `firmware/components/core_os/README.md`.
  - **`core_os`** has the clock, `Mutex`, `Event`, `PeriodicTask`, `OneShotTimer`, the diag sink, the `KvStore` (NVS; native: in-memory with fault injection) and the boot info (reset reason, RTC crash counter, reboot). The backend is picked by include dir.
  - **Instances** must bind **every** param (structural ones included), signal and input in `start()`, or they fail (`core.InstanceStartFailed`, arg1 the stage: below 0x100 from the instance, 0x100–0x103 from the core). Boot values come from the shadow, filled from NVS.
  - **Thread tests** keep loose timing bounds (MinGW wakes up to ~15 ms late). The system-param rollback on a failed timer restart (`core.TimerRestartFailed`) can't be triggered natively.
  - F3b supersedes the empty module list: main now registers `mod_mock::kTypes` beside the legacy managers.
- **Still true from F2:**
  - **Definitions** live in `protocol/defs/` (`registry.yaml`, `diag_codes.yaml`, `system.yaml`, later one YAML per module type). `python tools/gen/gen.py` writes `protocol/include/drivelink/protocol/gen/*.hpp` and the generated tables in `docs/protocol/` (`registry.md`, `diag-codes.md`, `types/`). `--check` is what CI runs; the generator's own tests are `python -m unittest discover -s tools/gen/tests`. The YAML schema, the descriptor format and the hash are specified in [docs/protocol/definitions.md](../docs/protocol/definitions.md).
  - **`system.yaml`** is type `0x00` (namespace `defs::system`, first in `kTypeTable`): no loops, params and signals only. It holds the loop rates; F3b and F4b add the status rate and `watchdog_ms` when they use them.
  - **Descriptor format** (`protocol/include/drivelink/protocol/defs.hpp`): `TypeDesc`, `ItemDesc` (min, max and default as raw 4-byte slot bits), `findItem`, `findType`. Its enum values are not a wire encoding yet: F3b's describe family fixes that. Generated per-type namespaces are `drivelink::protocol::defs::<type with . → _>`.
  - **`kDescriptorHash`** (in `gen/registry.hpp`) is a CRC-32 over the canonical JSON of all definitions, descriptions included. F3b reports it in `GET_INFO`.
  - **MSG_TYPEs:** every constant is `drivelink::protocol::defs::msg::k<Name>`. The legacy streams are on `0xBD` (motor setpoints), `0xBE` (motor telemetry) and `0xBF` (IMU), and motor_manager and the IMU are at v6. While they live, the command groups have 29 channels (`kCmdGroupChannelCount`); F4a's Kconfig group count must `static_assert` against it. **The time ping (`0x02` / `0x42`) is not in the registry yet:** F8 adds it.
  - **diag** call sites use `diag::codes::<source>::k<Name>` through the hand-written shim `firmware/components/diag/include/diag_codes.hpp`. The `core` source is `0x04`.
  - The native test build runs the generator on `tools/gen/tests/fixtures` at build time, so it needs Python 3 with PyYAML.
- **Still true from F1b:**
  - The frame is `AA AA AA | MSG_TYPE | LEN u16 | HCRC | payload | CRC16`, specified in [docs/protocol/framing.md](../docs/protocol/framing.md) (spec, CRC table, scanner rules, golden frames, version history). `kMaxPayload` is 1024, the default baud is 921600, the UART ISR runs from IRAM, and the driver rings are 4096 bytes. Legacy versions are motor_manager v6, IMU v6, time_sync v2, diag v2. **F3b's diag redesign ships as wire version 3**, not 2; `plans/firmware-architecture.md` says so.
  - `firmware/test/uart_comm/protocol.py` has a hardware-free self-test (`python protocol.py`) that CI runs; it holds the golden frames. `smoke_main_firmware.py` checks the types and versions of diag, motor, IMU and time_sync, and the IMU stream channel: update it with any change to those.
  - **Stale `sdkconfig` trap:** a changed Kconfig default doesn't reach an existing `firmware/sdkconfig` or `firmware/test/uart_comm/sdkconfig`. Delete both after pulling F1b (the old ones keep 115200 baud and a 128-byte limit). After F2 an old one still lists the removed MSG_TYPE options, harmlessly.
  - **Memory:** every `RxFrame` is about 1 KB. The legacy managers' config queues and `uart_comm`'s `rx_queue` hold them by value. The diag reporter keeps its big buffers in `Impl` (they were on the task stack and would have overflowed it). The motor and IMU wake frames are `static const`. Keep any new `RxFrame` or `MAX_PAYLOAD` buffer off task stacks.
  - **Stack marks** are logged by the main firmware every 30 s (`CONFIG_DRIVELINK_STACK_LOG_PERIOD_S`; task names listed in `firmware/main/main.cpp`; per-task `uxTaskGetStackHighWaterMark`, no kernel lock). Measured on 2026-11-12 after three smoke runs, bytes free: main 1256 (stack 4608), uart_rx 3060, uart_tx 3168, motor_mgr_upd 2080, motor_mgr_cfg 1612 (stack 5120, the one task that needs more than 4096 with a 1 KB frame), imu_cfg 2036, imu_stream 2000, time_sync 2008, diag 2644, esp_timer 3344 (all other stacks 4096). The `main` task had only 232 bytes free at the old 3584: `app_main`'s locals (the managers) are large, so a new manager started from `app_main` needs another look at this mark.
  - **GET_INFO** (F3b) must report the build's `MAX_PAYLOAD` (the Kconfig range is 128 to 1024).
  - **Hardware notes.** The board has two USB ports: the CH343 adapter (COM3 on this PC) is wired to UART0 (GPIO43/44) and also flashes the board through its DTR/RTS lines; the native USB port (COM4) is the console. The first time the adapter's port is opened after a flash, it resets the board, so the first command is lost; `smoke_main_firmware.py` retries its first query for this reason. The IMU is not wired to a BNO055 on the bench board, so the IMU manager runs in mock mode there.

## Steps

States: `planned`, `in progress`, `waiting on hardware`, `in review`, `merged`. A step session may split its step further. Then it adds rows here, for example F3a-1 and F3a-2.

| Step | Content | Hardware check | Review effort | State | PR |
|---|---|---|---|---|---|
| S0 | Workflow: this file, the temporary `CLAUDE.md` section, CI on `firmware_overhaul`, the `/overhaul-step` skill, plan updates for the cleanup, the diag code table moved to `docs/protocol/` | no | high | merged | [#28](https://github.com/taruntom1/drivelink_v2/pull/28) |
| S1 | Repo restructure: firmware into `firmware/` (host plan, "S1: restructure in detail") | no | high | merged | [#29](https://github.com/taruntom1/drivelink_v2/pull/29) |
| F1a | Extract framing, CRC, scanner and `wire_codec` into `protocol/` with the wire format unchanged; `uart_comm` fixes for big frames (buffers in `Impl`, stack checks, non-blocking `sendData`) | no | high | merged | [#30](https://github.com/taruntom1/drivelink_v2/pull/30) |
| F1b | New framing: u16 LEN + HCRC8, `MAX_PAYLOAD` 1024, 921600 baud default, UART ISR in IRAM, protocol version bumps, HIL tests and `conftest.py` baud | yes: HIL pytest | xhigh | merged | [#31](https://github.com/taruntom1/drivelink_v2/pull/31) |
| F2 | `tools/gen/`, `protocol/defs/` (registry, diag codes), Kconfig MSG_TYPEs replaced, legacy streams moved to `0xBD–0xBF`, descriptor table format and hash | no | high | merged | [#33](https://github.com/taruntom1/drivelink_v2/pull/33) |
| F3a-1 | `core_os` (IDF and native), `firmware/test/native`, `system.yaml` (type 0x00), module registry and bind API, `control` / `sensor` loops, param service (atomic batches, staging slot, withdrawal) | bare board: boot check | high | merged | [#34](https://github.com/taruntom1/drivelink_v2/pull/34) |
| F3a-2 | Layout and pending copy, validation (GPIO registry, wiring, labels), NVS A/B, safe mode, boot with dependency order (`InitFailed`, `DependencyMissing`) | bare board: boot, save/reload | high | merged | [#35](https://github.com/taruntom1/drivelink_v2/pull/35) |
| F3b | System, layout, describe, param and action families; status stream; `GET_INFO`; diag redesign (wire v3); `mod_mock` | yes: mock instances on the board | high | merged | [#36](https://github.com/taruntom1/drivelink_v2/pull/36) |
| F3c | `drivelink-vdev`, first `libdrivelink` slice, `drivelink-cli`, `host/superbuild`, host-linux CI | bare board: CLI serial smoke (user requested) | high | in review | |
| F4a | Subscription and command groups (preallocated, epochs) | no | high | planned | |
| F4b | Arming, `DISARM` fast path, safety monitor, watchdog, `DIAG_RAISE`, `blocks_arm` / `disarms`, `kind: latched`; CLI arm and stream commands | yes: monitor and watchdog on the board with mock modules | high | planned | |
| F5a | Motor module types (actuator, feedback, controller) with `emergencyStop`, `onArmTransition`, bumpless mode switch | yes: motors | high | planned | |
| F5b | Delete `motor_manager` and the legacy motor streams; loop-stall test on hardware | yes: motors | high | planned | |
| F6 | `imu.bno055` module and calibration actions; delete `imu_bmo_055` | yes: IMU | high | planned | |
| F7a | Capture family, PSRAM (caps alloc only) | yes: capture | high | planned | |
| F7b | Test signal generator; evaluate `SPIRAM_XIP_FROM_PSRAM` | yes: motors | high | planned | |
| F8 | Time ping with TX stamping; delete `time_sync` | yes: time jitter | high | planned | |
| F9 | OTA: partition table, OTA family, rollback | yes: OTA and rollback | high | planned | |
| F10 | Cleanup: leftover Kconfig and legacy registry entries, rewrite `firmware/CLAUDE.md` for the new architecture, remove the temporary overhaul section from the root `CLAUDE.md`, merge `firmware_overhaul` into `main` | yes: full smoke test | high | planned | |
| H1 | Complete `libdrivelink`: time model, device model, services, plant identification math; host-windows CI. May start in a second session after F4b is merged | no (vdev) | high | planned | |
| H2 | GUI in local mode (Qt/QML): connect, device, layout, tune and monitor screens | yes: bench tuning | high | planned | |
| H3 | Agent: arbitration, lease, clock model distribution; GUI remote mode | yes: Pi + PC | high | planned | |
| H4 | `drivelink_hardware`, `drivelink_bringup`, diagnostics node, ROS CI | yes: robot | high | planned | |

## Models and effort

- **Plan and build** every step in one session on **Opus 5.5 at high effort**. The session that wrote the plan writes the code; there is no implementation subagent and no model switch after the plan is approved.
- **Review.** The session reviews its own diff. It also spawns an independent review agent, always on **Opus** at the step's review effort, when the step needs one (wire, safety, persistence or concurrency changes it can't fully cover itself, or a large diff). The PR says which was done and why.
- **Hardware checks** that run on the bare bench board (HIL pytest, mock modules, smoke tests, OTA) are flashed and run by the session after the review, with no help from you. Checks that need motors, a real IMU, the robot, a Pi or a PC are run by you from a script, and the step waits in `waiting on hardware`.
- **Haiku** is used only for search and read-only helper agents (finding code, reading CI logs). It never writes firmware or host code.
- **Max effort** isn't used by default. Use it only if a review on a step keeps missing problems that show up later, and log that in the decision log.
- **Changing a step's review effort.** If a step turns out harder (or easier) than its row says, change its review effort in the same PR and log the reason.

## Decision log

Choices made during implementation that the plans don't cover, or that change them. Anything that touches the wire protocol, a safety rule or a later phase is decided by the user first, and the plan is updated in the same PR.

| Date | Step | Decision | Why | Plan updated |
|---|---|---|---|---|
| 2026-11-10 | S0 | The diag code table lives in `docs/protocol/diag-codes.md` | `components/diag/README.md` was removed in `f4de709`, which broke the generator check | n/a (already the plan's direction) |
| 2026-11-10 | S1 | CI builds each app from inside its directory (`cd`) instead of `idf.py -C <app> -B build_variant ...` | `-B` resolves against the current directory but `SDKCONFIG` and the defaults resolve against the project dir, so with `-C` the variant paths would split between the root and `firmware/` | n/a |
| 2026-11-10 | S1 | `firmware/CLAUDE.md` writes all paths relative to the repo root, and the root `CLAUDE.md` holds the repo map and cross-cutting rules | An agent never has to guess the base directory | n/a |
| 2026-11-10 | S0 | The restructure no longer moves `review/` or `TODO.md`, and phases update `docs/protocol/` instead of component READMEs or `review/` statuses | Those files were removed in `f4de709` | yes |
| 2026-11-10 | F1a | `protocol/` uses namespace `drivelink::protocol` (`::wire` for the codec) and headers under `drivelink/protocol/` | Matches the host plan's tree and keeps the host from depending on `uart_comm` | n/a |
| 2026-11-10 | F1a | CRC-16 is a pure table in `protocol/` instead of `esp_rom_crc16_be` | One tested implementation for the firmware and the host; same results (check value 0x29B1, checked against `protocol.py`) | n/a |
| 2026-11-10 | F1a | `Scanner` is the pure half of the old `Parser`: it returns frames as views, has no clock and no logging, and loops past rejected candidates itself. `Parser` keeps the driver reads, the stale timer, the atomics and the log lines | `protocol/` has to build without IDF or an OS | n/a |
| 2026-11-10 | F1a | Task stack sizes are not changed in F1a; `stackHighWater()` is added and logged, and F1b sets the sizes from HIL measurements | With 255-byte frames and the buffers moved off the stacks, nothing needs a bigger stack yet; the real sizes depend on 1024-byte frames | n/a |
| 2026-11-10 | F1a | `sendData` needs `openStream(channel, max_len)` first, and a busy slot skips the write (counted locally in `ErrorCounters::stream_skipped`, no diag code until F3b) | The control tick must never wait or allocate; each channel gets a slot of its own size | n/a |
| 2026-11-10 | F1a | The CMake package export and `package.xml` for `protocol/` wait for F3c; the protocol tests are built by `firmware/test/host` until then | No host consumer exists yet | n/a |
| 2026-11-12 | F1b | New counter `header_crc_errors` and diag code `uart_comm.HeaderCrcError` (0x0007); `length_errors` counts only valid headers that announce too much | A bad header CRC and an over-limit length are different faults (noise vs. a peer's bug) and need to be told apart | n/a |
| 2026-11-12 | F1b | The F1b framing bumps diag to wire version 2, so F3b's diag redesign ships as version 3 | Every protocol's version byte is bumped with a frame change, and diag was already counted in the plan as the "diag v2" step | yes (`plans/firmware-architecture.md`) |
| 2026-11-12 | F1b | `CONFIG_UART_COMM_MAX_PAYLOAD` has a range of 128..1024 (default 1024); the driver RX and TX rings default to 4096 | The protocol limit is 1024 and a ring must hold more than one full frame; 128 keeps room for the legacy managers' largest frames | n/a |
| 2026-11-12 | F1b | The scanner keeps a read offset (`m_head`) and compacts only when the caller asks for write space, and consumption is deferred (`m_pending`) | A rejected candidate drops one byte and rescans without moving 2 KB each time; a returned frame stays valid until the next call | n/a |
| 2026-11-12 | F1b | Task stacks come from hardware marks: main 3584 → 4608 (`sdkconfig.defaults`), motor config task 4096 → 5120, all other tasks stay 4096 | Rule: peak use plus about 1 KB. Measured on the board with 1024-byte frames; only these two needed more | n/a |
| 2026-11-12 | F1b | The main firmware logs per-task stack marks (`DRIVELINK_STACK_LOG_PERIOD_S`, default 30 s, 0 = off) using `xTaskGetHandle` + `uxTaskGetStackHighWaterMark` | `uxTaskGetSystemState` scans every stack inside a critical section and stalled the motor loop on the other core every period | n/a |
| 2026-11-12 | F1b | Diag's query frame, response body and TX buffer moved into `Impl`; the motor and IMU wake frames and the HIL app's frame and reply buffers are static | With 1 KB frames they overflowed or crowded the task stacks (the diag task was a real overflow risk on a `GetEntries` query) | n/a |
| 2026-11-12 | F1b | `smoke_main_firmware.py` (new) checks the managers answer with the new versions; its first query is retried for 5 s | The first open of the adapter's port after a flash resets the board and loses the first command | n/a |
| 2026-11-12 | S0 | Workflow change: the session plans and reviews on Opus high, an implementation subagent builds at the step's model and effort, and the review agent is dropped. Bare-board hardware checks are run by the session itself; only checks that need attached equipment go to the user | Fewer manual model switches, and the planner that wrote the plan judges the result. The board needs no help for HIL or mock-module checks | n/a |
| 2026-11-12 | F2 | motor_manager and the IMU go to protocol v6 with the move of their streams to `0xBD`–`0xBF`; time_sync and diag stay at v2 | Every wire change bumps the version byte (user decision) | yes (`plans/firmware-architecture.md`, F2 row) |
| 2026-11-12 | F2 | The time ping (`0x02` / `0x42`) joins the registry in F8, not F2 | The legacy IMU configure response holds `0x42` until F6; deferring needs no extra wire change (user decision) | yes (MSG_TYPE section) |
| 2026-11-12 | F2 | Module type ids live in each type's YAML (`type_id`); `registry.yaml` holds only MSG_TYPEs, channel ranges and the reserved system id `0x00` | Adding a module stays one file; the generator checks the ids are unique | yes |
| 2026-11-12 | F2 | Generated namespaces are `drivelink::protocol::defs::<type with . → _>` | `imu.mock` and `feedback.mock` would collide as `defs::mock` | yes (Module API sketch) |
| 2026-11-12 | F2 | The descriptor hash is computed by the generator: CRC-32 (zlib) over the canonical JSON of the normalized registry, diag codes and types, descriptions included | Deterministic, changes exactly when a definition changes, and needs no CRC-32 or string walk in C++ | yes |
| 2026-11-12 | F2 | Legacy entries inside a channel range must fill its tail without gaps; the usable count stops at the first one (command groups: 29 channels until F5b and F6) | The plan's "keep the group channel limits below them", checked by the generator and by `static_assert`s | n/a |
| 2026-11-12 | F2 | `firmware/components/diag/host/diag_codes.py` is dropped | Nothing in the repo imports it; the device will serve code descriptors (F3b) | n/a |
| 2026-11-12 | F2 | Generator fixtures are generated into the host test build dir at build time, not checked in; only their `type_*.hpp` are compiled | Proves the generated headers compile with `-Werror` without a second set of checked-in files (the fixture `registry.hpp` would clash with the real one) | n/a |
| 2026-11-12 | F2 | Type YAML conventions: `persist` is on by default for params unless `read_only` or `persist: false`; `testgen: true` sets the `Testgen` flag; retired items keep only `id`, `name` and `retired`; enum params need a `default`; unknown keys are rejected | Keeps the YAML short and catches typos; written down in `docs/protocol/definitions.md` | n/a |
| 2026-11-12 | F3a-1 | F3a is split into F3a-1 (core_os, test/native, system.yaml, registry, bind API, loops, param service) and F3a-2 (layout, validation, GPIO registry, NVS A/B, safe mode, boot order) | About 4–5k lines in one PR is too much to review well (user decision) | n/a (status rows) |
| 2026-11-12 | F3a-1 | Loop defaults: `control.rate_hz` 1000 (100–2000) and `sensor.rate_hz` 100 (10–500), both u16 and `locked_while_armed`, applied live by restarting the loop timer | Same as today's motor loop and IMU sampler (user decision); resolves the plan's open question | yes |
| 2026-11-12 | F3a-1 | An instance's loop is stored in its layout entry; F3b's `ADD_INSTANCE(type, loop, label)` takes it, defaulting to the type's first loop class | The plan's op had no loop argument; a core-level param would break the one-id-space-per-type rule (user decision) | yes (layout ops, layout blob) |
| 2026-11-12 | F3a-1 | `system.yaml` is type 0x00 with no loops, params and signals only, first in `kTypeTable`; it holds only the items in use (the loop rates) | Describe and param ops treat it like any type; later items join with the step that consumes them | n/a |
| 2026-11-12 | F3a-1 | A param batch is all system params (applied on the config task) or all params of one loop | System params belong to no loop, and the cross-loop rule needs a single domain | n/a |
| 2026-11-12 | F3a-1 | `core::Result` / `core::Detail` are C++ enums in `core/status.hpp` with no wire numbers; F3b moves them into `registry.yaml`. Added beyond the plan: Results `NotFound`, `Exists`, `NoMemory`, `NotArmed`; Details for refs and binds, values, batches (`Withdrawn`, `BatchTooLarge`, `DuplicateRef`, `TimerFailed` ...), layout and the registry | The wire numbering belongs to the family step; each failure site still names its own check | n/a |
| 2026-11-12 | F3a-1 | `reboot_required` writes answer `PendingReboot` / `Unsupported` until F3a-2's pending copy | Definite answer, nothing applied | n/a |
| 2026-11-12 | F3a-1 | Batches: at most 64 entries (`kMaxBatchEntries`), no Ref twice (`DuplicateRef`), an empty batch is `Ok` | One staging slot of fixed size per loop; a duplicate has no defined order | n/a |
| 2026-11-12 | F3a-1 | Module types come from an explicit list that main passes to `Core::init`, checked against `kTypeTable`; no static self-registration | The linker drops unreferenced static registrars in component archives | n/a |
| 2026-11-12 | F3a-1 | `start()` must bind every param (structural included), signal and input; `bindParam` writes the boot value into the storage | Catches module bugs at boot; storage and shadow agree from the start | n/a |
| 2026-11-12 | F3a-1 | `core.InstanceStartFailed` arg1: an instance's own stage (< 0x100), or 0x100 create returned null, 0x101 start false without a stage, 0x102 bind rejected, 0x103 item unbound | Diag args are wire contract; core and module stages must not collide | n/a |
| 2026-11-12 | F3a-1 | `core_os` picks its backend by include dir and sources (`backend/idf`, `backend/native`), not `#ifdef`; it is the one exception to the `src/` layout rule | Each backend's state lives in its own `backend_types.hpp` | n/a |
| 2026-11-12 | F3a-1 | The core runs in the main firmware beside the legacy managers, with empty loops (control on core 1 at priority 20, sensor on core 0 at priority 10) | Boots the new code on the device from the first step; the empty ticks cost almost nothing | n/a |
| 2026-11-12 | F3a-1 | No loop-overrun diag counter: loop stats (ticks, overruns, last and max run time) go out in F3b's status stream | The plan reports overruns per loop in the status stream | n/a |
| 2026-11-12 | F3a-1 | `firmware/test/host` became `firmware/test/native` (target `native_tests`, CI job `native-tests`) | The plan's F3 rename; it now builds the core against the native backend | n/a |
| 2026-11-12 | F3a-2 | GPIO params are marked `gpio: true` in the YAML (`ItemFlag::Gpio` 0x20): `u8`, `reboot_required`, no min/max/default; the value `0xFF` (`kNoGpio`) means no pin and is the default | The core's GPIO registry needs to know which params are pins; a default pin would make two new instances conflict (user decision) | yes |
| 2026-11-12 | F3a-2 | Wiring refs may say `ref_order: after` (`ItemFlag::RefTicksAfter` 0x40, default `before`). Creation is always dependencies-first; the tick order follows the flag (feedback → controller → actuator) | The controller refs both a producer (feedback) and a consumer (actuator); the ref alone can't say which ticks first (user decision) | yes (controller YAML example) |
| 2026-11-12 | F3a-2 | A wiring value is `type << 8 \| instance` in the low 16 bits (upper 16 zero); 0 means unwired, which validation allows (the module's `start()` decides). A ref's category matches the type name before the dot | Same packing as diag's arg0; a new controller can be added before it is wired (user decision) | yes |
| 2026-11-12 | F3a-2 | A param batch is all pending (`reboot_required` params, or params of instances that exist only in the pending layout) or all live; an accepted pending batch answers `Result::PendingReboot`. `Status::accepted()` is `Ok` or `PendingReboot` | The answer tells the host the value is staged, not applied (user decision) | yes |
| 2026-11-12 | F3a-2 | Every configured instance has a record, running or not (`Running`, `Failed`, `Invalid`, `DependencyMissing`, `Offline`). A SET on an instance that isn't running writes its shadow on the caller (the offline domain) | A SAVE must never drop the config of an instance that failed to start, and safe mode must still read and edit params | n/a |
| 2026-11-12 | F3a-2 | NVS layout entries with an unregistered type, a duplicate (type, inst) or past `max_instances` get no record and are dropped (namespace erased) by the next SAVE | The firmware can't decode their params, so it can't keep them consistently | n/a |
| 2026-11-12 | F3a-2 | Layout ops are refused only if they make a previously valid entry invalid, or the entry they touch is invalid | An entry that was already invalid (say a pin the policy dropped) would otherwise block every op, including its own fix | n/a |
| 2026-11-12 | F3a-2 | Layout ops (add, remove, label, discard) answer `Ok` and are refused while armed; REMOVE_INSTANCE is refused while another entry refs the instance (`Referenced`); the device picks the lowest free instance id (0..254, 0xFF stays the wire's unparsed marker) | The plan's ADD_INSTANCE has no id; removal must not leave dangling wiring | yes |
| 2026-11-12 | F3a-2 | `reboot_required` system params are refused (`Unsupported`): the system is not a layout entry. None exist today | No pending copy for the system yet; added when one is needed | n/a |
| 2026-11-12 | F3a-2 | The pending copy keeps only `reboot_required` values for copied instances; their live params come from the record shadow at SAVE and GET_PENDING. It stays after SAVE; `pendingReboot` = copy active or the last SAVE wrote it | A live SET after a pending op must not be lost by SAVE; NVS holds the pending layout until the reboot | n/a |
| 2026-11-12 | F3a-2 | NVS: generation `gen` goes to slot `gen & 1`; instance blobs first, the layout blob last as the commit record; a layout blob in the wrong slot for its parity is invalid; the system blob (`t00i00`) is part of every generation. SAVE never writes the slot of the complete generation it loaded or last saved | A torn SAVE always leaves a complete generation. The last rule was found in review: after an incomplete newer generation, `newest + 1` landed on the only complete one | yes |
| 2026-11-12 | F3a-2 | SAVE erases the namespaces of dropped instances right after the layout commit, not at the next boot | The new layout no longer references them, and stale blobs of older generations are ignored anyway | yes |
| 2026-11-12 | F3a-2 | REVERT reloads every live, persisted, writable param (system, running and offline instances) from the complete saved generation through the normal paths, in batches of up to 64 per domain; it is not atomic across batches. No saved config → `NotFound` / `NoSavedConfig` | The staging slot holds 64 entries; one batch per loop could overflow it | n/a |
| 2026-11-12 | F3a-2 | The native `KvStore` is in memory with fault injection (power loss after n writes, a failing write, a torn write); a file-backed store waits for the vdev (F3c) | The tests need deterministic faults; only the vdev needs persistence across processes | n/a |
| 2026-11-12 | F3a-2 | Safe mode: the crash counter is cleared by a `OneShotTimer` 30 s after a normal boot, not in safe mode; any non-crash reset (a REBOOT) clears it too. In safe mode validation still runs and reports `LayoutInvalid` | The device stays safe while the crash loop goes on, and a host REBOOT is enough to leave | n/a |
| 2026-11-12 | F3a-2 | Per-type `<source>.InitFailed` waits for the diag v2 sources (F3b); `core.InstanceStartFailed` covers instance failures until then. New core codes: `SafeModeEntered`, `LayoutInvalid`, `DependencyMissing`, `ConfigLoadFailed`, `NvsWriteFailed`, `NvsInitFailed`; `core.InitFailed` stage 4 is boot | Sources become module types only with diag v2 | n/a |
| 2026-11-12 | F3a-2 | NVS is initialised by main (`core_os::nvsInit()`); a full partition or a newer NVS version is erased and initialised again and reported (`core.NvsInitFailed` arg0 1); the core boots either way | The standard IDF recovery; losing the config is better than not booting | n/a |
| 2026-11-12 | F3a-2 | `CONFIG_CORE_BOOT_SELFTEST` (main, default off) flips `control.rate_hz` 1000 ↔ 500 and SAVEs on every boot, for the bench save/reload check before SAVE is on the wire | F3a-2 has no wire ops; the check needs to run on the board | n/a |
| 2026-11-12 | F3a-2 | Workflow change: the session that plans a step also builds it (Opus 5.5, high; no implementation subagent, no model switch) and reviews its own diff; an independent review agent (Opus, at the step's review effort) is spawned only when the step needs one. The table's "Build model · effort" column is removed | The planner already holds the plan and its decisions, which a second agent has to re-derive; the review agent stays for the risky steps (user decision) | n/a (status file, root `CLAUDE.md`, `/overhaul-step`) |

| 2026-11-12 | F3b | One PR; separate current/pending layout views; whole descriptors with generator size limits; deterministic dynamic mocks and test actions; bench-only crash injection | User decisions in the pre-plan interview | yes |
| 2026-11-12 | F3b | This Codex session plans and implements; an available independent Codex reviewer replaces Opus for this step | Explicit user-approved workflow exception | n/a |
| 2026-11-12 | F3b | Core starts at wire v1; diag redesign ships at v3. Instance IDs are included consistently in records, clears, counters and queries; state uses 254 fixed sparse slots | Shared codecs and definite instance identity; no steady-state allocation | yes |
| 2026-11-12 | F3b | Status rate defaults to 10 Hz (1..100); CPU load uses idle runtime deltas; initial/disabled/unsupported samples are 0xFF | Measured utilization with constant work and no stack scans | yes |
| 2026-11-12 | F3b | Action completion events are wake hints; Ready withdrawal tests a deadline and state, while claimed actions wait for completion. Reboot effects wait for TX drain and are refused while armed | Independent review found stale-event early withdrawal and an EXIT_SAFE_MODE reboot bypass; both fixed and tested | yes |

### F3c decisions

- User approved the plan in `plans/f3c-implementation.md`, this Codex session
  implementing, and an available independent Codex reviewer at high effort.
  Multiple commits are allowed, with exactly one PR into firmware_overhaul.
- User chose readable CLI output plus --json and a CLI smoke on the bare ESP32,
  extending F3c's original vdev-only validation row.
- Internal: pinned standalone Asio and nlohmann JSON; serialized requests with
  timeout-seq retirement and read-only retry; bounded drop-oldest event queues.
- Internal: optional per-blob durable native KV, OS state-directory ownership
  lock and a persistent running marker/crash counter. Existing core A/B logic,
  wire formats and staging deadlines are unchanged. Test harness retries only
  definite Busy/Withdrawn rejections to tolerate desktop scheduling.
- Review fixes: exact JSON ABI pinning; reject incomplete diag terminal pages;
  retain idle PTY masters and bound command drains from enqueue, using generation
  guards on handlers and composed-write continuation. Hardware fixes preserve
  Windows DTR/RTS and retry only native open errors before sending a request.

### F3b decisions

- User approved this Codex session and an available independent Codex reviewer
  in place of the Opus-specific workflow. The planning session implements it.
- User chose one F3b PR unless a concrete blocker appears; separate current and
  pending layout views; deterministic dynamic mocks with actions; whole
  descriptors bounded by generator checks; test-only hardware crash injection.
- Internal: status defaults to 10 Hz (1..100); CPU utilization uses per-core idle runtime deltas (0xFF if unavailable); diagnostic identities use 254 fixed sparse slots.

## Hardware test results

| Date | Step | Board | What was run | Result |
|---|---|---|---|---|
| 2026-11-12 | F1b | ESP32-S3 (QFN56 rev 0.2, 8 MB PSRAM) on the bench; CH343 adapter on UART0 (GPIO43/44), native USB console | HIL test app: `pytest pytest_uart_comm.py --port COM3` at 921600 baud. Main firmware: `smoke_main_firmware.py`, three runs, with the console read for the stack marks | HIL: 22 of 22 passed (including TC-20 false header, TC-21 four 1024-byte echoes, TC-22 1024-byte stream). Smoke: diag GetInfo and GetEntries (v2), motor SET_TIMING (v5, Ok), IMU SetStreamEnabled (v5, mock IMU), time_sync request (v2) all passed. Stack marks in the handoff note. Not covered: a real BNO055, motors under load, other hosts than Windows. |
| 2026-11-12 | F3a-1 | ESP32-S3 bench board; native USB only (COM4), CH343 adapter not connected | Main firmware flashed over COM4; console read for 65 s after a reset | Pass: one boot, no panic or reset loop; `Core started: 0 instances; loops control 1000 us, sensor 10000 us`; legacy managers started as before (IMU in mock mode, time_sync times out with no host). Stack marks at 30 s and 60 s, bytes free: core_ctrl 3256, core_sens 3252 (stacks 4096), main 1248, uart_rx 3196, uart_tx 3096, motor_mgr_upd 2088, motor_mgr_cfg 3156, imu_cfg 2092, imu_stream 2008, time_sync 2000, diag 2796, esp_timer 3352. Not covered: anything over UART0 (no adapter). |
| 2026-11-12 | F3a-2 | ESP32-S3 bench board; native USB only (COM4), CH343 adapter not connected | Main firmware flashed over COM4 on a blank NVS; then a `CONFIG_CORE_BOOT_SELFTEST` build (save/reload) over four resets; then the normal firmware again. Console read after each reset | Pass. Blank NVS: `Core started: 0 instances (0 failed, 0 configured); safe mode off; config defaults; loops control 1000 us`, no NVS error. Self-test: boot 1 set 500 Hz and SAVEd (gen 1); boot 2 logged `config loaded from NVS`, control 2000 us, `reloaded 500 from NVS`, restored 1000 and SAVEd (gen 2); boots 3 and 4 repeated it (gens 3, 4). Normal firmware after it: `config loaded from NVS`, control 1000 us. Stack marks at 30 s, bytes free: main 1256, core_ctrl 3252, core_sens 3248, others as in F3a-1. Not covered: anything over UART0 (no adapter), safe mode on the board (no crash hook). |
| 2026-11-12 | F3b | ESP32-S3 bare bench board; CH343 UART0 COM3 and native USB console COM4 | UART test app: 22 HIL pytest cases. Main firmware: core discovery and descriptor paging, current/pending layout, three mock instances and wiring, parameter staging, SAVE/reboot/reload, loop/paused actions with blob echo, cleanup and status; legacy main smoke. Separate CORE_BENCH_CRASH build: three deliberate panic resets, safe-mode discovery and EXIT_SAFE_MODE recovery. Normal firmware restored and both main smoke scripts rerun. | Pass. All 22 HIL cases, core smoke, legacy smoke (diag v3, motor/IMU v6, time_sync v2), and crash-loop recovery passed. Final normal build rejects BenchCrash and has zero mock instances after cleanup. Stack bytes free: main 1272, core_cfg 4764 (6144 stack), diag 1572, core_ctrl 3260, core_sens 3248; all task margins exceed 1 KB. No motors, real IMU or host/vdev exercised. |
| 2026-11-13 | F3c | Bare ESP32-S3; CH343 UART0 COM3, native USB console COM4 | Normal firmware flashed; new CLI serial smoke (discovery/descriptors, current/pending mock layout, typed params, SAVE/reboot/reload, loop/paused actions and blob echo, status/diag); legacy main smoke; console stack marks | Pass. CLI and legacy smoke passed, normal firmware rejects BenchCrash, zero failed mock instances or dropped diag records/events. Original zero-instance layout and rates 1000/100/10 Hz restored. Every console stack margin exceeds 1 KB (full values below). No motors or real IMU tested. |

## Notes

### F3c hardware result (2026-11-13)

ESP32-S3 bare bench board, CH343 UART0 COM3 and native USB console COM4. Normal
main firmware flashed; `host/test/hardware_smoke.py` passed discovery, all current
descriptor pages, current/pending layout, three temporary mocks and wiring, typed
param writes, SAVE/reboot/reload, loop/paused actions and blob echo, status and
diag. Normal firmware rejected bench crash. Original zero-instance layout and
system rates (1000/100/10 Hz) restored. Diagnostic query during mocks reported
3 instances OK, 0 failed, 0 records/events dropped and safe mode off.

Console stack bytes free after cleanup: main 1304, uart_rx 3164, uart_tx 3096,
motor_mgr_upd 1880, motor_mgr_cfg 3156, imu_cfg 2084, imu_stream 1996,
time_sync_task 1792, diag 2048, esp_timer 3352, core_ctrl 3256, core_sens 3260,
core_cfg 4352. All exceed 1 KB. No motors or real IMU were exercised. The first
preflight attempts made no configuration changes and exposed the Windows
bootstrap-line and transient-open issues, fixed before this passing run.

- **Legacy wire specs.** The deleted READMEs (motor_manager v4, IMU v4, time_sync v1, diag v1) are in git history: `git show 7a4d693:components/<name>/README.md`. The IMU guide is still at `firmware/components/imu_bmo_055/docs/protocol-api-guide.md`.
- **diag's wire spec** is now [diag.md](../docs/protocol/diag.md), redesign wire version 3.
