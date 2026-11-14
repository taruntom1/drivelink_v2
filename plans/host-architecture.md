# Plan: host software architecture

Status: architecture target, revised 2026-11-14 (F10) against the firmware as
built. The first host slice exists: `libdrivelink` (transport, session, the
core families, the F8 clock estimator and the F9 OTA service, all returning
JSON), `drivelink-cli`, `drivelink-vdev` and the superbuild. Still to build:
the typed API and device model, automatic timestamp mapping, the GUI, the
agent and ROS. The steps are H1a to H4 in [overhaul-status.md](overhaul-status.md),
run on the `host_overhaul` branch.

**Baseline.** The host is written against the firmware on `main` after F10
([firmware-architecture.md](firmware-architecture.md) is the design record;
`docs/protocol/` is the wire spec and wins where they differ). That firmware is:
- self-describing: module types, instances, params, signals, inputs, actions,
  sources and diag codes, all served by the device
- configured through a pending layout that SAVE writes to NVS (A/B) and a
  reboot applies; loops are instances of the built-in `loop` type (0x05) with
  a rate and a `realtime` flag, and each module type requires, forbids or
  accepts a real-time loop
- limited by read-only `limits.*` system params (loops, instances, groups,
  link values), so a host can show "4 of 4 loops" before it tries an op
- streamed through subscription groups (31 channels, 0x81–0x9F) and command
  groups (32 channels, 0xA0–0xBF), preallocated, with epochs; the pool sizes
  are Kconfig (default 8 each) and reported in `limits.*`
- protected by arming (`ARM` needs a configured command group), a command
  watchdog, a safety monitor, the `DISARM` fast path (MSG_TYPE 0x03) and diag
  codes that block arming or disarm
- able to capture signals into PSRAM at the loop rate (capture family, core
  events on 0xC1) and to run one test signal generator per running loop
- stamped with monotonic device timestamps that the host maps to its own
  clock through time protocol v1 pings (0x02 / 0x42)
- updated over the link (disarmed OTA: FINISH prepares, explicit REBOOT
  commits after reply drain, the host CONFIRMs after verifying identity)

## Goals

1. A GUI on the PC to tune the controllers while the ESP is plugged straight into the PC.
2. A ROS 2 interface on the robot host.
3. Tuning from the PC while the ESP is plugged into the robot host, over the network.
4. Adding a new sensor, actuator or control system needs no host code. The firmware module and its YAML are enough ([docs/adding-a-module.md](../docs/adding-a-module.md)).

## Decisions

| Topic | Decision |
|---|---|
| Robot host | Raspberry Pi 5, Ubuntu 26.04 arm64, ROS 2 Lyrical Luth (LTS, supported to May 2031). |
| PC GUI | C++ with Qt 6 and QML. Built with MinGW on Windows (Qt's own MinGW kit). |
| Host core | `libdrivelink`, plain C++20 with no Qt and no ROS. Used by the GUI, the agent, the ROS plugin and the CLI. |
| Host API | **Typed** (decided 2026-11-14). The public `libdrivelink` API returns typed C++ results and exposes a device model; nlohmann JSON leaves the public API. JSON formatting moves into `drivelink-cli`, whose readable and `--json` output stays as it is, so the smoke scripts keep working. |
| Generic host | The host has no module-specific code. It reads the descriptors (items and diag codes) from the device at every connect and builds its models from them. The descriptor hash in `GET_INFO` tells it when the definitions changed; a disk cache keyed by the hash is deferred until connecting gets slow. It uses the generated headers from `protocol/` only for core and system ids. |
| Device identity | A read-only device id (the factory MAC) and a user-set name in `GET_INFO`, set with a new system op (decided 2026-11-14, built in H1b as core v9; the proposal is [todo-device-name.md](todo-device-name.md)). The host keys recent devices and session files on the id. |
| Port ownership on the robot | A headless agent (`drivelink-agent`) always owns the port. The ROS plugin and the remote GUI are its clients. |
| Port ownership on the bench | The GUI opens the serial port itself through `libdrivelink`. No agent is needed on Windows. |
| Agent API | The device protocol over TCP, plus a few agent-only messages. Clients use the same frames as over serial. gRPC is out because of MinGW, and Zenoh would add a Rust toolchain. |
| Clock | `libdrivelink` runs the time pings and fits offset and drift (the F8 `ClockEstimator`). The agent owns the clock model and sends it to its clients. |
| Configuration | Lives on the ESP (NVS). The host reads it and writes it, and never stores it. |
| Transport | UART at 921600 baud, behind a transport interface. USB can be added later without changes above the transport. |
| Repo and branches | One repo. The firmware is in `firmware/`, definitions and codecs shared with the host in `protocol/`. The host steps and the HW campaign run on `host_overhaul`, cut from `main` after F10; `main` stays frozen until `host_overhaul` merges. HW fixes to the firmware also go to `host_overhaul`. |
| Versioning | One version for the whole repo. The host compares the core protocol version and the descriptor hash, and refuses to connect on a protocol mismatch with a clear message. Every wire change bumps the version byte of the protocol it changes. |

## Architecture

```
 PC (Windows)                          Robot host (Raspberry Pi 5, Ubuntu 26.04, ROS 2 Lyrical)
 ┌───────────────────────┐   TCP       ┌──────────────────────────────────────────────┐
 │ drivelink-gui         │◄───────────►│ drivelink-agent                              │
 │  (Qt/QML)             │             │  libdrivelink: serial link, session,         │
 │  libdrivelink         │             │  time model, device model, arbitration       │
 │   ├ LocalDevice ──┐   │             │         ▲ TCP on localhost                   │
 │   └ RemoteDevice  │   │             │ drivelink_hardware (ros2_control plugin)     │
 └───────────────────│───┘             └──────────────────────┬───────────────────────┘
                     │ serial                                 │ UART 921600
               (bench mode)                                   ▼
                     ▼                                     ESP32-S3
                 ESP32-S3

 CI and development: drivelink-vdev (the firmware core built natively, with mock modules)
 stands in for the ESP on TCP or a pty.
```

### `libdrivelink` (host core)

**What exists (F3c–F9).** `Session` is a thread-safe synchronous facade over
one Asio I/O thread: it allocates `seq`, correlates responses, retries only
idempotent reads, retires timed-out ids until reconnect, queues command-group
frames without blocking, and hands every other frame to a bounded event queue
that drops oldest. `Client` wraps the families (info, describe, layout,
system, param, action, diag, group, capture, testgen, OTA) and returns JSON.
`ClockEstimator` fits offset and drift from pings with I/O-thread timestamps.
The rules for this code are in [host/CLAUDE.md](../host/CLAUDE.md).

**Target (H1a/H1b):**

- **Transport.**
  - `SerialTransport` uses standalone Asio and works on Linux and MinGW (kept: bootstrap lines stay disabled so opening the port doesn't reset the board).
  - `TcpTransport` connects to the agent or to `drivelink-vdev`.
  - `UsbTransport` comes later, if needed.
- **Link.** Frame encoding and the frame scanner come from `protocol/`: a `u16` LEN with a header CRC8, and the protocol constant `MAX_PAYLOAD` (1024) as the scanner's hard bound before `GET_INFO` gives the device's `max_payload`.
- **Session.** As today, plus an asynchronous request path for the GUI and the agent (completion callbacks or futures delivered off the I/O thread). The synchronous facade stays for the CLI and tests. Pages bulk transfers (descriptors, capture reads, OTA writes) with a bounded number of chunks in flight; OTA WRITE keeps its own retry exception.
- **Time.**
  - Sends a ping on `0x02` every 1 s, and faster during the first few seconds (periodic ownership is H1a; F8 has the single-shot ping and the estimator).
  - Keeps the pings with the smallest round trip and fits ESP time to host time (offset and drift) over a sliding window, restarting on a Boot event.
  - Gives every timestamped item (group frames, status, capture, events) both the ESP µs and the mapped host time, with the estimator's uncertainty.
- **Device model** (typed, H1a).
  - At connect, or after a Boot event, it:
    1. reads `GET_INFO`: versions, build id, descriptor hash, `max_payload`, capture capacity, flags (pending reboot, layout invalid, config from NVS, safe mode) and the running loops (from H1b also the device id and name)
    2. checks the core protocol version, and the `safe_mode` flag
    3. reads the descriptors: types (with their realtime requirement), items and diag codes
    4. lists the configured and the pending instances, including the loop instances
    5. reads every param, including the `limits.*` system params
  - This builds a tree of type → instance (label, loop, state) → params, signals, inputs, sources and actions, with typed values, units, ranges, enum names and flags (`reboot_required`, `locked_while_armed`, `testgen`, GPIO, `ref` wiring).
  - It follows the arm state, the last disarm reason, the pending-reboot and NVS-dirty flags, the per-loop overruns, the capture and testgen state and the active faults through the status stream, core events and diag events.
  - The pending layout is a second view of the tree: added, removed and relabelled instances, applied by SAVE and a reboot.
- **Services** (typed wrappers of the firmware families):
  - param batches, including atomic multi-param writes within one loop (cross-loop batches are refused by the device)
  - layout: add (with a loop, or the lowest compatible one), remove (refused with `Referenced` while dependents exist), label, discard; save, revert, factory reset, reboot, exit safe mode
  - group setup: subscription and command groups with epochs, bound to any loop
  - capture (configure, trigger kinds, arm, status, read, decode into per-signal arrays) and test signals (per loop: step, square, ramp, sine, linear and log chirp; stop and status)
  - actions, including paused actions with blob payloads
  - OTA (stream and prepare the image; explicit reboot commits it after reply drain; reconnect, verify full running build identity and confirm; reconcile uncertain outcomes with STATUS without automatically repeating REBOOT)
  - diag (decode events, query entries, name codes from the code descriptors the device serves)
  - arming (arm with the refusal detail reported, `DISARM` through the fast-path MSG_TYPE, `ACK_FAULT`)
  - command groups with prefix-only keepalives; NaN-to-hold mapping and automatic keepalives in the client, since the device rejects non-finite values and only a valid frame feeds the watchdog
  - device identity (H1b): read id and name, set name
- **Plant identification** (H1b): fit a first-order-plus-deadtime model to an open-loop duty capture and suggest starting gains. Pure math with unit tests.
- **Threading.**
  - One I/O thread (Asio) does all the reading and writing.
  - Clients get events through bounded thread-safe queues.
  - The latest value of each subscription group sits in a lock-free single-writer buffer (seqlock), so the ROS `read()` never blocks.
  - Command group frames are queued without blocking.

### `drivelink-cli`

Stays the scripting and test front end. It moves onto the typed API in H1a and
owns all JSON formatting; its output formats and exit codes do not change, so
the smoke scripts under `firmware/test/uart_comm/` and `host/test/` keep
working. Python bindings stay an open question (below).

### `drivelink-agent` (robot host)

- Owns the serial port, runs as a systemd service, and keeps working while ROS is down.
- Clients speak the device protocol over TCP. The agent arbitrates between them:
  - **`seq`.** It rewrites each client's `seq` before forwarding a request. It maps the response back and routes it to the client that sent the request.
  - **Groups.** Group ids are device resources (the pool sizes are in `limits.*`). The agent gives each client its own groups out of the device's pool, rewrites the group ids and stream channels both ways, and sends each client only its own subscription streams. It tracks each group's epoch, so frames left over from a group's previous owner are dropped. The status stream, core events and diag events go to every client.
  - **Time.** The agent alone answers to the ESP's clock. Clients never send pings; the agent sends them its clock model (offset, drift, reference) as an agent message whenever the model changes.
  - **Control lease.** Only one client at a time may send `ARM`, configure command groups or send command frames. While ROS's controllers are active, ROS holds the lease. The GUI takes it explicitly to tune.
    - **Any client may disarm**, with or without the lease: the agent forwards `DISARM` (fast path or system op) from every client at once. Decided 2026-11-12 ([gui-design.md](gui-design.md)).
    - A lease change, or the lease holder disconnecting, makes the agent send `DISARM` on the fast-path MSG_TYPE. That leaves the device plain Disarmed, not in Fault.
    - The previous holder gets a lease-revoked message. ROS recovers as described under `drivelink_hardware`.
  - **Exclusive operations.** Capture, test signals, `SAVE`, `REVERT`, `FACTORY_RESET`, layout ops and OTA need the lease, or need the device to be disarmed with nobody holding the lease.
  - **Param writes.** Any client may write live params at any time. Batches are atomic on the device. This stays allowed while ROS holds the lease; the GUI warns before such an edit.
  - **History.** The agent keeps recent diag events and arm-state changes, and sends them to a client when it connects. It keeps no captures.
- **Agent-only messages** get their own MSG_TYPEs in `protocol/defs/registry.yaml`, allocated in H3 from the free command, response and event values: hello and version, lease request/grant/revoke, clock model, client list. They never reach the device.

### `drivelink-gui` (Qt/QML)

- Uses the current Qt 6 LTS with Qt's MinGW kit on Windows. On Linux it uses the Qt from Ubuntu 26.04.
- The screens follow the Figma design ([gui-design.md](gui-design.md), [gui-design-status.md](gui-design-status.md)), including G7b's loops-and-sources rework.
- **C++ backend:**
  - A device connection, either local (serial) or remote (agent), behind one interface.
  - Its own command group, with every input it can command at hold values and keepalives, so `ARM` is never refused with `NoCommandGroup`. A direct serial connection sets it up at connect. A remote connection starts as an observer and sets it up only after receiving the control lease, following the agent's command-group ownership rule.
  - Models built from the device model:
    - an instance tree (type, label, loop, health), with loops as instances
    - a param table per instance (typed editors, units, ranges, enums, a pending-reboot marker, a dirty marker)
    - a signal list, including sources' value and age signals
    - action forms
- **QML:** entirely generic, with no screen for any particular module. A module can still add an optional custom panel under `gui/modules/<type>/`, matched by type name (the IMU calibration panel is the first).
- **Plots:** a custom `QQuickItem` that draws line strips through the scene graph from a C++ ring buffer. Live plots use subscription groups the GUI sets up itself; captures are plotted from uploaded buffers.
- **Screens:**
  - **Connect:** a serial port, or an agent address; recent devices keyed by device id.
  - **Device:** info (name, id, versions), loops and their rates and overruns, limits, arm/disarm with the reasons `ARM` was refused, fault acknowledgement, faults (diag entries with the names and descriptions the device serves), save/revert, reboot, OTA, rename, and a safe-mode banner with `EXIT_SAFE_MODE`.
  - **Layout:** add or remove instances (including loops), the loop each module runs in, wiring (`Ref` params with pickers), sources, labels, pending-reboot banner.
  - **Tune:**
    1. Pick a controller instance and edit its gains live.
    2. Choose a test signal and the capture signals.
    3. Run, then overlay the results against earlier runs.
    4. Save or revert.
  - **Monitor:** free-form live plots of any signals, and a manual command panel that sets inputs by hand while armed.
- **Tuning sessions** (captures plus the params used) are saved as files on the PC, keyed by device id, and can be reopened and exported to CSV.

### `drivelink_hardware` (ROS 2)

- A ros2_control `SystemInterface`.
- **Parameters:**
  - how to connect: the agent address, or a serial port for running without the agent
  - for each joint: the label of its controller instance, and which interfaces map to which signals and inputs (by item name, for example `position` → `position`, `velocity` → `velocity`, command `velocity` → `velocity_sp`)
  - for each sensor: the label of its instance and its signal names (for the IMU, the quaternion, gyro and linear acceleration signals)
- **`on_configure`:**
  1. Connect and load the device model. Safe mode or a pending reboot is a clear error.
  2. Resolve every label and item name to a `Ref`. Anything missing, wrongly typed or failed is a clear error.
- **`on_activate`:**
  1. Take the lease.
  2. Configure one subscription group per loop that holds a mapped signal, and one command group.
  3. Stay disarmed. The command group sends keepalive frames.
- **`prepare_command_mode_switch` / `perform_command_mode_switch`:**
  1. Set each controller's `mode` param to match the newly claimed command interfaces. The firmware switches modes bumplessly, so this works while armed.
  2. Set command values to hold (current position, zero velocity, zero duty) and send a fresh value frame after the mode write completes. The controller ignores old queued/inactive setpoints; keepalives do not satisfy the new mode's fresh-command requirement.
  3. When the first command interface is claimed, `ARM`. When the last one is released, `DISARM`.
- **`on_deactivate`:** `DISARM`, then release the lease.
- **`read()`:** copies the latest group values out of the seqlock buffers, with timestamps mapped to ROS time.
- **`write()`:** queues one command group frame and never blocks.
  - A NaN command value (the ros2_control default before a controller writes) is sent as hold: the last measured position, or zero velocity or duty. The firmware rejects non-finite frames, and a rejected frame doesn't feed the watchdog.
- **Status and faults:** diag events and the status stream are published on `/diagnostics` through a small node in `drivelink_bringup`.
- **Losing the device while active.** A disarm (watchdog, fault, a loop stall, or the GUI taking the lease) makes `read()` return `ERROR`. ros2_control then deactivates the component. Recovery is documented and done by an operator or a supervisor node:
  1. acknowledge any fault (from the GUI, or a service on the diagnostics node)
  2. hand the lease back
  3. reactivate the component with `ros2 control set_hardware_component_state`

  ros2_control does not reactivate a component on its own.
- **IMU:** exposed as sensor state interfaces, published with `imu_sensor_broadcaster`. `imu.bno055` runs in a non-real-time loop, so its group is separate from the motor loop's.
- **`drivelink_bringup`:** launch files, controller YAML, the diagnostics node and URDF snippets.

## Link budget

At 921600 baud 8N1 the link carries 92 160 B/s in each direction. Frame overhead is 9 bytes (including the header CRC8). A subscription group frame is `9 + 12 + 4·k` bytes (version, epoch, counter, u64 timestamp, then the values), and a command group frame is `9 + 4 + 4·k`.

The table below is for ESP → host: motor signals (position, velocity, duty), plus an IMU group of 10 values at 100 Hz, plus the status stream at 10 Hz.

| Motors × 3 signals | 100 Hz | 200 Hz | 500 Hz | 1 kHz |
|---|---|---|---|---|
| 2 (frame 45 B) | 12 % | 17 % | 31 % | 56 % |
| 4 (frame 69 B) | 14 % | 22 % | 44 % | 82 % |
| 8 (frame 117 B) | 20 % | 32 % | 70 % | 134 % |

Host → ESP, one velocity per motor:

| Motors | 100 Hz | 200 Hz | 500 Hz | 1 kHz |
|---|---|---|---|---|
| 4 (frame 29 B) | 3 % | 6 % | 16 % | 31 % |
| 8 (frame 45 B) | 5 % | 10 % | 24 % | 49 % |

What follows from the tables:
- Normal running at 100–200 Hz uses a small share of 921600 baud. A subscription group's divider sets its rate below the loop rate.
- Tuning doesn't depend on the link, because capture records on the device at the full loop rate.
- The control rate itself never uses the link.
- At high rates with many motors, consider 2–3 Mbaud, or a USB transport later.

## Repository layout

What exists after F10, and what the host steps add (marked *planned*). Each directory is created by the step that first puts something in it. No empty placeholder directories.

```
drivelink_v2/
├─ CLAUDE.md  README.md
├─ firmware/                  ESP-IDF project (CLAUDE.md, README.md)
│  ├─ main/
│  ├─ components/             uart_comm, diag, rtos_util, core_os, core, mod_motor,
│  │                          mod_imu_bno055, mod_mock, motor_hal, rotary_encoder_hal, pid
│  └─ test/
│     ├─ native/              core, module and protocol tests, built natively
│     └─ uart_comm/           HIL test app + pytest, smoke scripts
├─ protocol/                  pure C++20: IDF component + CMake package + package.xml
│  ├─ defs/                   registry.yaml, diag_codes.yaml, system.yaml, loop.yaml, <module type>.yaml
│  ├─ include/drivelink/protocol/   framing, wire_codec, codecs; gen/ (generated)
│  ├─ src/
│  └─ test/
├─ host/
│  ├─ CLAUDE.md  README.md
│  ├─ cmake/                  pinned dependencies
│  ├─ superbuild/             CMakeLists.txt + CMakePresets.json (COLCON_IGNORE)
│  ├─ libdrivelink/           (+ package.xml)
│  ├─ vdev/                   drivelink-vdev: links the firmware core, core_os, mod_mock and the pure motor controller natively
│  ├─ cli/                    drivelink-cli
│  ├─ test/                   host unit and integration tests, hardware smoke scripts
│  ├─ agent/                  planned (H3)
│  ├─ gui/                    planned (H2, COLCON_IGNORE)
│  │  └─ modules/<type>/      optional custom panels
│  └─ ros/                    planned (H4)
│     ├─ drivelink_hardware/
│     └─ drivelink_bringup/
├─ docs/
│  ├─ adding-a-module.md
│  └─ protocol/               generated tables + hand-written family specs
├─ tools/
│  ├─ gen/                    YAML → C++ / docs generator
│  ├─ validate.py             every check, locally and in CI
│  └─ bench_fw.py             guarded bench/normal/XiP/OTA-image builds and flashing
├─ plans/                     plans, status, GUI design records; archive/ holds finished step plans
└─ .github/workflows/
```

## Build systems and toolchains

- **Firmware:** `idf.py -C firmware build`. `protocol/` joins through `EXTRA_COMPONENT_DIRS`.
- **`protocol/` and `libdrivelink/`:** one `CMakeLists.txt` each.
  - Under IDF (`ESP_PLATFORM`), `protocol/` calls `idf_component_register`.
  - Otherwise each one is an ordinary library that installs and exports a CMake package (`find_package(drivelink_protocol)`, `find_package(drivelink)`).
  - Each one also has a `package.xml` with `<build_type>cmake</build_type>`, so colcon builds it as a dependency in the same workspace.
- **Host superbuild:** `host/superbuild/CMakeLists.txt` brings in `protocol/` and builds libdrivelink, vdev, cli, and later agent and gui, together (`add_subdirectory` with explicit binary dirs).
  - It lives in its own directory, not at `host/CMakeLists.txt`. colcon treats any directory with a `CMakeLists.txt` that calls `project()` as a package and does not look for packages inside it, so a superbuild at `host/` would hide `host/libdrivelink` and `host/ros/*`.
  - `vdev` compiles the firmware core and the pure modules against `core_os`'s native backend.
  - `CMakePresets.json` has `windows-mingw`, `linux` and `linux-no-gui` presets (the last is the same as `linux` until the GUI lands).
  - The Windows preset names Qt's MinGW compiler explicitly.
- **ROS:** `colcon build` from the repo root. `firmware/`, `host/superbuild/`, `host/gui/`, `host/vdev/`, `host/cli/` and `host/agent/` carry `COLCON_IGNORE`. colcon finds `protocol/`, `host/libdrivelink/` and `host/ros/*` through their `package.xml`.
- **MinGW notes:**
  - Never mix compilers. Qt, libdrivelink, vdev and the GUI all build with Qt's MinGW kit.
  - Strawberry Perl puts a different `g++` on `PATH` (the firmware native tests use it). Name the compiler in the preset, and ship the right runtime DLLs with `windeployqt`.
  - Asio on MinGW needs `ws2_32` and `mswsock`, and `_WIN32_WINNT` defined.
- **Dependencies:** keep them few and mostly header-only, version- and SHA256-pinned in `host/cmake/Dependencies.cmake` (standalone Asio, doctest, nlohmann JSON for the CLI). No vcpkg, because its MinGW support is community-only. The generator needs Python 3 and PyYAML.

## Adding a module

The checklist is [docs/adding-a-module.md](../docs/adding-a-module.md). On the host side: nothing in code; optionally a custom GUI panel; for ROS only the joint or sensor mapping in the robot's config.

## CI

Every check is defined once in `tools/validate.py`; CI jobs only provide the toolchain and call it with `--only`, and `python tools/validate.py full` runs the same checks locally.

| Job | Runs on | What | State |
|---|---|---|---|
| firmware | `espressif/idf:v5.5.2` | the main, nolog and HIL test-app builds, zero warnings | exists |
| native-tests | Ubuntu | generator checks and tests, firmware native and protocol tests | exists |
| host-linux | Ubuntu | superbuild, libdrivelink/CLI tests, vdev integration, installed-package smoke | exists |
| host-windows | Windows, Qt `win64_mingw` + its MinGW | superbuild, tests; with the GUI from H2 | H1a (local `host-win` check exists) |
| agent | Ubuntu | agent ↔ vdev integration tests | H3 |
| ros | `ros:lyrical` | `colcon build` + `colcon test`, including a launch test against vdev | H4 |

## Phases

| Phase | Content |
|---|---|
| S0, S1, F1–F10 | Done: the workflow, the repo restructure and the firmware plan ([firmware-architecture.md](firmware-architecture.md)), with the first host slice growing alongside from F3. |
| HW | Run every deferred hardware check (motors, IMU, Pi). Can run beside the host steps as hardware allows; fixes go to `host_overhaul`. |
| H1a | Typed `libdrivelink`: the device model, typed services, the asynchronous request path and the seqlock group buffers; periodic time and automatic timestamp mapping over F8's estimator; the CLI on the typed API with its JSON formatting; the host-windows CI job. Tested against vdev. |
| H1b | Device identity (core v9: id and name in `GET_INFO`, a set-name system op, NVS storage; firmware, host, vdev and docs together, bare-board check) and the plant identification math. |
| H2 | GUI in local mode: connect, device, layout, tune and monitor screens, per the Figma design. |
| H3 | Agent: arbitration, lease, clock model distribution, agent MSG_TYPEs. The GUI's remote mode. |
| H4 | `drivelink_hardware` and `drivelink_bringup`, the diagnostics node, the ROS CI job. |

The S1 restructure details are in git history (`git show 272575d:plans/host-architecture.md`).

## Open questions

- ~~While ROS holds the lease, may the GUI change live params, or only read them?~~ Decided 2026-11-11 ([gui-design.md](gui-design.md)): it may, and the GUI warns that the robot is being driven and changes apply live.
- ~~Should the agent keep a short history of captures and diag records, so a GUI that connects later can see them?~~ Decided 2026-11-11: diag events and arm-state changes only. Captures stay with the client that ran them.
- ~~Should the GUI save "tuning sessions" (captures plus the params used) as files on the PC?~~ Decided 2026-11-11: yes, as session files that can be reopened and exported to CSV.
- ~~Typed host API or JSON results?~~ Decided 2026-11-14: typed; JSON only in the CLI.
- Python bindings for `libdrivelink` (for scripts and to replace `firmware/test/uart_comm/protocol.py`): when, if ever? Until then, scripts drive the CLI's `--json` output.

## IMU calibration contract

IMU actions and typed calibration params remain descriptor-driven. Read/write
stages configuration, explicit SAVE commits it, and boot restores it. A generic
action form can pass the versioned blob; a custom GUI panel is optional. Vdev
uses `imu.mock` and the shared pure state/codec, without a physical bus backend.
Calibration quality comes from levels, while `calibration.valid` means a staged
blob exists. See `docs/protocol/imu.md`.
