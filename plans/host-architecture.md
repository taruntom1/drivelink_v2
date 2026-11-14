# Plan: host software architecture and repo restructure

Status: architecture target, 2026-11-10 (revised after review). F3c implements
the first host slice (library, CLI, virtual device and superbuild); subsequent
services and the F8 ping/clock estimator are built; the full device model,
automatic timestamp mapping, GUI, agent and ROS remain planned.

**Baseline.** The host is written against the firmware described in [firmware-architecture.md](firmware-architecture.md), after all its phases are done. A minimal slice of `libdrivelink` and `drivelink-cli` is built earlier, alongside firmware phases F3–F9, to drive and test them. That firmware is:
- self-describing: module instances with params, signals, inputs, actions and diag codes
- streamed through subscription groups and command groups
- protected by arming, a watchdog and a safety monitor
- stamped with monotonic timestamps, which the host maps to its own clock through pings
- updated over the link (OTA)

The `diag` plan (fault codes over UART) is also assumed done. Read the firmware plan first; this plan covers only the host and the repo layout.

## Goals

1. A GUI on the PC to tune the controllers while the ESP is plugged straight into the PC.
2. A ROS 2 interface on the robot host.
3. Tuning from the PC while the ESP is plugged into the robot host, over the network.
4. Adding a new sensor, actuator or control system needs no host code. The firmware module and its YAML are enough.

## Decisions

| Topic | Decision |
|---|---|
| Robot host | Raspberry Pi 5, Ubuntu 26.04 arm64, ROS 2 Lyrical Luth (LTS, supported to May 2031). |
| PC GUI | C++ with Qt 6 and QML. Built with MinGW on Windows (Qt's own MinGW kit). |
| Host core | `libdrivelink`, plain C++20 with no Qt and no ROS. Used by the GUI, the agent, the ROS plugin and the CLI. |
| Generic host | The host has no module-specific code. It reads the descriptors (items and diag codes) from the device at every connect and builds its models from them. The descriptor hash in `GET_INFO` tells it when the definitions changed; a disk cache keyed by the hash is deferred until connecting gets slow. It uses the generated headers from `protocol/` only for core and system ids. |
| Port ownership on the robot | A headless agent (`drivelink-agent`) always owns the port. The ROS plugin and the remote GUI are its clients. |
| Port ownership on the bench | The GUI opens the serial port itself through `libdrivelink`. No agent is needed on Windows. |
| Agent API | The device protocol over TCP, plus a few agent-only messages. Clients use the same frames as over serial. gRPC is out because of MinGW, and Zenoh would add a Rust toolchain. |
| Clock | `libdrivelink` runs the time pings and fits offset and drift. The agent owns the clock model and sends it to its clients. |
| Configuration | Lives on the ESP (NVS). The host reads it and writes it, and never stores it. |
| Transport | UART at 921600 baud, behind a transport interface. USB can be added later without changes above the transport. |
| Repo | One repo. The firmware lives in `firmware/`. Definitions and codecs shared with the host live in `protocol/`. |
| Versioning | One version for the whole repo. The host compares the core protocol version and the descriptor hash, and refuses to connect on a protocol mismatch with a clear message. |

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

- **Transport.**
  - `SerialTransport` uses standalone Asio and works on Linux and MinGW.
  - `TcpTransport` connects to the agent or to `drivelink-vdev`.
  - `UsbTransport` comes later, if needed.
- **Link.** Frame encoding and the frame scanner come from `protocol/`: a `u16` LEN with a header CRC8, and the protocol constant `MAX_PAYLOAD` (1024) as the scanner's hard bound before `GET_INFO` is known.
- **Session.**
  - Allocates `seq`, tracks pending requests with timeouts and retries for idempotent reads, and matches responses to them.
  - Sends streams to subscribers.
  - Pages bulk transfers (descriptors, capture reads, OTA writes) with a bounded number of chunks in flight.
- **Time.**
  - Sends a ping on `0x02` every 1 s, and faster during the first few seconds.
  - Keeps the pings with the smallest round trip and fits ESP time to host time (offset and drift) over a sliding window.
  - Gives every timestamped item both the ESP µs and the mapped host time.
- **Device model.**
  - At connect, or after a Boot event, it:
    1. reads `GET_INFO`
    2. checks the protocol version, and the `safe_mode` flag
    3. reads the descriptors: types, items and diag codes
    4. lists the instances
    5. reads every param
  - This builds a tree of type → instance (label) → params, signals, inputs and actions, with typed values, units, ranges and enum names.
  - It follows the arm state, the pending-reboot and NVS-dirty flags, and the active faults through the status stream and diag events.
- **Services.** These wrap the firmware families:
  - param batches, including atomic multi-param writes
  - group setup
  - capture (configure, arm, read, decode into per-signal arrays)
  - test signals
  - actions
  - OTA (stream and prepare the image; explicit reboot commits it after reply drain; reconnect, verify full running build identity and confirm. Reconcile uncertain outcomes with STATUS without automatically repeating REBOOT.)
  - diag (decode events, query entries, name codes from the code descriptors the device serves)
  - arming (arm with conditions reported, `DISARM` through the fast-path MSG_TYPE, `ACK_FAULT`)
  - command groups with prefix-only keepalives; automatic keepalives and NaN-to-hold mapping follow the arming/hold behavior (F4b or later); F4a exposes explicit sends and rejects non-finite values
- **Growth.** The first slice (transport, session, the core families) and `drivelink-cli` are built in firmware phase F3 and grow with each firmware phase. F8 adds the reusable clock estimator, ping API and CLI measurement; H1 adds periodic integration, automatic timestamp mapping, the device model and the remaining services.
- **Threading.**
  - One I/O thread (Asio) does all the reading and writing.
  - Clients get events through thread-safe queues.
  - The latest value of each subscription group sits in a lock-free single-writer buffer (seqlock), so the ROS `read()` never blocks.
  - Command group frames are queued without blocking.

### `drivelink-agent` (robot host)

- Owns the serial port, runs as a systemd service, and keeps working while ROS is down.
- Clients speak the device protocol over TCP. The agent arbitrates between them:
  - **`seq`.** It rewrites each client's `seq` before forwarding a request. It maps the response back and routes it to the client that sent the request.
  - **Groups.** Group ids are device resources. The agent gives each client its own groups out of the device's pool, rewrites the group ids and stream channels both ways, and sends each client only its own subscription streams. It tracks each group's epoch, so frames left over from a group's previous owner are dropped. The status stream and diag events go to every client.
  - **Time.** The agent alone answers to the ESP's clock. Clients never send pings; the agent sends them its clock model (offset, drift, reference) as an agent message whenever the model changes.
  - **Control lease.** Only one client at a time may send `ARM`, configure command groups or send command frames. While ROS's controllers are active, ROS holds the lease. The GUI takes it explicitly to tune.
    - **Any client may disarm**, with or without the lease: the agent forwards `DISARM` (fast path or system op) from every client at once. Decided 2026-11-12 ([gui-design.md](gui-design.md)).
    - A lease change, or the lease holder disconnecting, makes the agent send `DISARM` on the fast-path MSG_TYPE. That leaves the device plain Disarmed, not in Fault.
    - The previous holder gets a lease-revoked message. ROS recovers as described under `drivelink_hardware`.
  - **Exclusive operations.** Capture, test signals, `SAVE`, `REVERT`, `FACTORY_RESET`, layout ops and OTA need the lease, or need the device to be disarmed with nobody holding the lease.
  - **Param writes.** Any client may write live params at any time. Batches are atomic on the device. This stays allowed while ROS holds the lease; the GUI warns before such an edit.
  - **History.** The agent keeps recent diag events and arm-state changes, and sends them to a client when it connects. It keeps no captures.
- **Agent-only messages** use their own MSG_TYPEs from the generated registry: hello and version, lease request/grant/revoke, clock model, client list.

### `drivelink-gui` (Qt/QML)

- Uses the current Qt 6 LTS with Qt's MinGW kit on Windows. On Linux it uses the Qt from Ubuntu 26.04.
- **C++ backend:**
  - A device connection, either local (serial) or remote (agent), behind one interface.
  - Its own command group, with every input it can command at hold values and keepalives, so `ARM` is never refused for a missing command group ([gui-design.md](gui-design.md)). A direct serial connection sets it up at connect. A remote connection starts as an observer and sets it up only after receiving the control lease, following the agent's command-group ownership rule.
  - Models built from the device model:
    - an instance tree (type, label, health)
    - a param table per instance (typed editors, units, ranges, enums, a pending-reboot marker, a dirty marker)
    - a signal list
    - action forms
- **QML:** entirely generic, with no screen for any particular module. A module can still add an optional custom panel under `gui/modules/<type>/`, matched by type name.
- **Plots:** a custom `QQuickItem` that draws line strips through the scene graph from a C++ ring buffer. Live plots use subscription groups the GUI sets up itself; captures are plotted from uploaded buffers.
- **Screens:**
  - **Connect:** a serial port, or an agent address.
  - **Device:** info, arm/disarm with the reasons `ARM` was refused, fault acknowledgement, faults (diag entries with the names and descriptions the device serves), save/revert, reboot, OTA, and a safe-mode banner with `EXIT_SAFE_MODE`.
  - **Layout:** add or remove instances, wiring (`Ref` params with pickers), labels, pending-reboot banner.
  - **Tune:**
    1. Pick a controller instance and edit its gains live.
    2. Choose a test signal and the capture signals.
    3. Run, then overlay the results against earlier runs.
    4. Save or revert.
  - **Monitor:** free-form live plots of any signals, and a manual command panel that sets inputs by hand while armed.
- **Plant identification:** fit a first-order-plus-deadtime model to an open-loop duty capture and suggest starting gains. The math lives in `libdrivelink` and has unit tests.

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
  2. Configure one subscription group per loop and one command group.
  3. Stay disarmed. The command group sends keepalive frames.
- **`prepare_command_mode_switch` / `perform_command_mode_switch`:**
  1. Set each controller's `mode` param to match the newly claimed command interfaces. The firmware switches modes bumplessly, so this works while armed.
  2. Set command values to hold (current position, zero velocity, zero duty) and send a fresh value frame after the mode write completes. The F5a controller ignores old queued/inactive setpoints; keepalives do not satisfy the new mode's fresh-command requirement.
  3. When the first command interface is claimed, `ARM`. When the last one is released, `DISARM`.
- **`on_deactivate`:** `DISARM`, then release the lease.
- **`read()`:** copies the latest group values out of the seqlock buffers, with timestamps mapped to ROS time.
- **`write()`:** queues one command group frame and never blocks.
  - A NaN command value (the ros2_control default before a controller writes) is sent as hold: the last measured position, or zero velocity or duty. The firmware rejects non-finite frames, and a rejected frame doesn't feed the watchdog.
- **Status and faults:** diag events and the status stream are published on `/diagnostics` through a small node in `drivelink_bringup`.
- **Losing the device while active.** A disarm (watchdog, fault, or the GUI taking the lease) makes `read()` return `ERROR`. ros2_control then deactivates the component. Recovery is documented and done by an operator or a supervisor node:
  1. acknowledge any fault (from the GUI, or a service on the diagnostics node)
  2. hand the lease back
  3. reactivate the component with `ros2 control set_hardware_component_state`

  ros2_control does not reactivate a component on its own.
- **IMU:** exposed as sensor state interfaces, published with `imu_sensor_broadcaster`.
- **`drivelink_bringup`:** launch files, controller YAML, the diagnostics node and URDF snippets.

## Link budget

At 921600 baud 8N1 the link carries 92 160 B/s in each direction. Frame overhead is 9 bytes (including the header CRC8). A subscription group frame is `9 + 12 + 4·k` bytes, and a command group frame is `9 + 4 + 4·k`.

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
- Normal running at 100–200 Hz uses a small share of 921600 baud.
- Tuning doesn't depend on the link, because capture records on the device at the full loop rate.
- The control rate itself never uses the link.
- At high rates with many motors, consider 2–3 Mbaud, or a USB transport later.

## Target repository layout

```
drivelink_v2/
├─ CLAUDE.md                  repo map and cross-cutting rules
├─ README.md                  project overview
├─ firmware/                  ESP-IDF project
│  ├─ CLAUDE.md  README.md  CMakeLists.txt  sdkconfig.defaults  sdkconfig.ci.nolog
│  ├─ main/
│  ├─ components/             uart_comm, diag, core, core_os, mod_motor, mod_imu_bno055,
│  │                          mod_mock, motor_hal, rotary_encoder_hal, pid, rtos_util
│  └─ test/
│     ├─ native/              core and module tests, built natively
│     └─ uart_comm/           HIL test app + pytest
├─ protocol/                  pure C++20: IDF component + CMake package + package.xml
│  ├─ defs/                   registry.yaml, diag_codes.yaml, system.yaml, <module type>.yaml
│  ├─ include/drivelink/protocol/   framing, wire_codec, codecs; gen/ (generated)
│  ├─ src/
│  └─ test/
├─ host/
│  ├─ CLAUDE.md
│  ├─ superbuild/             CMakeLists.txt + CMakePresets.json (COLCON_IGNORE)
│  ├─ libdrivelink/           (+ package.xml)
│  ├─ vdev/                   drivelink-vdev: links firmware/components/{core,core_os,mod_*} natively
│  ├─ cli/                    drivelink-cli
│  ├─ agent/
│  ├─ gui/                    (COLCON_IGNORE)
│  │  └─ modules/<type>/      optional custom panels
│  └─ ros/
│     ├─ drivelink_hardware/
│     └─ drivelink_bringup/
├─ docs/
│  ├─ architecture.md
│  ├─ adding-a-module.md
│  └─ protocol/               generated tables + hand-written family specs
├─ tools/
│  └─ gen/                    YAML → C++ / docs generator
├─ plans/
└─ .github/workflows/
```

Each directory is created by the phase that first puts something in it. No empty placeholder directories.

## Build systems and toolchains

- **Firmware:** `idf.py -C firmware build`. `protocol/` joins through `EXTRA_COMPONENT_DIRS`.
- **`protocol/` and `libdrivelink/`:** one `CMakeLists.txt` each.
  - Under IDF (`ESP_PLATFORM`), `protocol/` calls `idf_component_register`.
  - Otherwise each one is an ordinary library that installs and exports a CMake package (`find_package(drivelink_protocol)`, `find_package(drivelink)`).
  - Each one also has a `package.xml` with `<build_type>cmake</build_type>`, so colcon builds it as a dependency in the same workspace.
- **Host superbuild:** `host/superbuild/CMakeLists.txt` brings in `protocol/` and builds libdrivelink, vdev, cli, agent and gui together (`add_subdirectory` with explicit binary dirs).
  - It lives in its own directory, not at `host/CMakeLists.txt`. colcon treats any directory with a `CMakeLists.txt` that calls `project()` as a package and does not look for packages inside it, so a superbuild at `host/` would hide `host/libdrivelink` and `host/ros/*`.
  - `vdev` compiles the firmware core and modules against `core_os`'s native backend.
  - `CMakePresets.json` has `windows-mingw`, `linux` and `linux-no-gui` presets.
  - The Windows preset names Qt's MinGW compiler explicitly.
- **ROS:** `colcon build` from the repo root. `firmware/`, `host/superbuild/`, `host/gui/`, `host/vdev/`, `host/cli/` and `host/agent/` carry `COLCON_IGNORE`. colcon finds `protocol/`, `host/libdrivelink/` and `host/ros/*` through their `package.xml`.
- **MinGW notes:**
  - Never mix compilers. Qt, libdrivelink, vdev and the GUI all build with Qt's MinGW kit.
  - Strawberry Perl puts a different `g++` on `PATH` (the firmware native tests use it today). Name the compiler in the preset, and ship the right runtime DLLs with `windeployqt`.
  - Asio on MinGW needs `ws2_32` and `mswsock`, and `_WIN32_WINNT` defined.
- **Dependencies:** keep them few and mostly header-only. Use standalone Asio and the vendored doctest, pulled in with CPM/FetchContent or vendored. No vcpkg, because its MinGW support is community-only. The generator needs Python 3 and PyYAML.

## Adding a module

1. `protocol/defs/<type>.yaml`: the type id, params, signals, inputs, actions and loop classes. Then run the generator.
2. `firmware/components/mod_<...>/`: the factory and the `Instance`, which binds the generated ids to its storage, plus native tests for its pure logic.
3. diag codes for its faults in `diag_codes.yaml`, with `blocks_arm` and `disarms` set. The device serves their names and descriptions, so the host needs no rebuild to show them.
4. Host: nothing. Optionally, a custom GUI panel.
5. ROS: nothing in code. Only the joint or sensor mapping in the robot's config.

`docs/adding-a-module.md` turns this list into a checklist.

## CI (target)

| Job | Runs on | What |
|---|---|---|
| firmware | `espressif/idf:v5.5.2` | `idf.py build` for `firmware` and `firmware/test/uart_comm`, plus the nolog variant, zero warnings |
| generated | Ubuntu | Rerun `tools/gen`, and fail on any diff |
| protocol | Ubuntu | build + tests, zero warnings |
| firmware-native | Ubuntu | core and module tests |
| host-linux | Ubuntu 26.04 | superbuild (no GUI at first), libdrivelink tests, agent ↔ vdev integration tests |
| host-windows | Windows, Qt `win64_mingw` + its MinGW | superbuild with GUI |
| ros | `ros:lyrical` | `colcon build` + `colcon test`, including a launch test against vdev |

## Phases

| Phase | Content |
|---|---|
| S0 | Workflow setup on `firmware_overhaul`: the status file, the temporary `CLAUDE.md` section, CI, the `/overhaul-step` skill. |
| S1 | Repo restructure (detailed below). No behavior change. |
| F1–F10 | The firmware plan ([firmware-architecture.md](firmware-architecture.md)). From F3 on, each firmware phase also extends the first slice of `libdrivelink` and `drivelink-cli`, which drive and test it. The host-linux CI job starts in F3. |
| H1 | Complete `libdrivelink`: periodic time integration/automatic timestamp mapping over F8’s estimator, the device model, the remaining services, and plant identification math. The host-windows CI job. Tested against vdev. |
| H2 | GUI in local mode: connect, device, layout, tune and monitor screens. |
| H3 | Agent: arbitration, lease, clock model distribution. The GUI's remote mode. |
| H4 | `drivelink_hardware` and `drivelink_bringup`, the diagnostics node, the ROS CI job. |

H1 can start once F4 is done, because vdev then covers the descriptors, params, groups and arming. Later firmware phases add services to `libdrivelink` as they land.

### S1: restructure in detail

This is based on `firmware_overhaul` after S0:
- diag has landed, so `components/diag`, `tools/gen_diag_codes.py` and `sdkconfig.ci.nolog` exist.
- The cleanup commit `f4de709` removed the component READMEs, `review/` and `TODO.md`.
- `docs/protocol/diag-codes.md` exists.

It is done in two commits, so git's rename detection is exact and `git log --follow` keeps working.

**Commit 1: moves only (`git mv`, no content changes)**

| From | To |
|---|---|
| `main/` | `firmware/main/` |
| `components/` (with `components/CLAUDE.md`) | `firmware/components/` |
| `CMakeLists.txt` | `firmware/CMakeLists.txt` |
| `sdkconfig.defaults`, `sdkconfig.ci.nolog` | `firmware/` |
| `test/host/` | `firmware/test/host/` |
| `test/uart_comm/` | `firmware/test/uart_comm/` |
| `CLAUDE.md` | `firmware/CLAUDE.md` |
| `README.md` | `firmware/README.md` |

`tools/`, `plans/`, `docs/`, `.claude/` and `.github/` stay at the root.

These relative paths stay valid because the depth does not change:
- `firmware/test/uart_comm/CMakeLists.txt` → `../../components`
- `firmware/test/host/CMakeLists.txt` → `../../components`

**Commit 2: path updates and new root files**

- `.github/workflows/ci.yml`:
  - firmware matrix: `firmware` and `firmware/test/uart_comm`
  - the nolog build: `firmware/sdkconfig.ci.nolog`
  - host tests: `firmware/test/host`
  - the generated-files check: the new output paths
- `tools/gen_diag_codes.py`: its input and output paths under `firmware/components/diag/`. The code table stays at `docs/protocol/diag-codes.md`. In F2 the script is replaced by `tools/gen/`.
- `firmware/CMakeLists.txt`: the rule that sets `LOG_LOCAL_LEVEL` for the nolog build matches components by path. Check that it still finds them.
- `firmware/CLAUDE.md`:
  - commands become `idf.py -C firmware ...`, or tell the reader to work from `firmware/`
  - paths to `test/host`, `test/uart_comm` and `tools/`
  - the temporary overhaul section moves to the new root `CLAUDE.md`, not to `firmware/CLAUDE.md`
- New root `CLAUDE.md` (short):
  - repo map
  - the "host and firmware change together" rule
  - where each area's rules live (`firmware/CLAUDE.md`, later `protocol/` and `host/CLAUDE.md`)
  - the definition of done for each area
  - the temporary overhaul section, moved here from today's `CLAUDE.md`
- New root `README.md`: what DriveLink is (firmware plus host), the layout, and links to `firmware/README.md` and the two plans.
- `firmware/README.md`: fix the build commands and paths.
- `.gitignore`:
  - `/components/.idf/` becomes `/firmware/components/.idf/`
  - Fix the lines with trailing `# ...` comments (`flasher_args.json`, `*.exe`, `*.workspace`, `*.suo`, `*.sln.docstates`). Git treats the `#` as part of the pattern, so these lines match nothing today. This was checked with `git check-ignore`.
- `plans/firmware-architecture.md` already uses the `firmware/` paths. `plans/overhaul-status.md`: mark S1 merged and write the handoff for F1a.
- `firmware/test/uart_comm/conftest.py` and `pytest_uart_comm.py`: their docstrings give `test/uart_comm/...` paths and `idf.py -C test/uart_comm`. Prefix them with `firmware/`. The code itself has no repo paths.

**Local follow-ups (not in the PR):**
- delete the old root `build/` and `sdkconfig`
- point the VS Code ESP-IDF extension at `firmware/`
- the `.claude/worktrees` checkouts pick up the move on their next rebase

**Verification:**
- `idf.py -C firmware build`, `idf.py -C firmware/test/uart_comm build` and the nolog build, all with zero warnings
- the firmware host tests pass
- the generated-files check passes
- CI is green
- `git log --follow firmware/components/uart_comm/src/parser.cpp` shows the full history

## Open questions

- ~~While ROS holds the lease, may the GUI change live params, or only read them?~~ Decided 2026-11-11 ([gui-design.md](gui-design.md)): it may, and the GUI warns that the robot is being driven and changes apply live.
- ~~Should the agent keep a short history of captures and diag records, so a GUI that connects later can see them?~~ Decided 2026-11-11: diag events and arm-state changes only. Captures stay with the client that ran them.
- ~~Should the GUI save "tuning sessions" (captures plus the params used) as files on the PC?~~ Decided 2026-11-11: yes, as session files that can be reopened and exported to CSV.
- Python bindings for `libdrivelink` (for scripts and to replace `firmware/test/uart_comm/protocol.py`): when, if ever?

## F6 calibration contract

IMU actions and typed calibration params remain descriptor-driven. Read/write
stages configuration, explicit SAVE commits it, and boot restores it. A generic
action form can pass the versioned blob; a custom GUI panel is optional. Vdev
uses `imu.mock` and the shared pure state/codec, without a physical bus backend.
Calibration quality comes from levels, while `calibration.valid` means a staged
blob exists. See `docs/protocol/imu.md`.
