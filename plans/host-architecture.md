# Plan: host software architecture and repo restructure

Status: draft, 2026-11-09 (revised). Nothing here is built yet.

**Baseline.** The host is written against the firmware described in [firmware-architecture.md](firmware-architecture.md), after all its phases are done. That firmware is:
- self-describing: module instances with params, signals, inputs and actions
- streamed through subscription groups and command groups
- protected by arming and a watchdog
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
| Generic host | The host has no module-specific code. It reads the descriptors from the device, caches them by descriptor hash, and builds its models from them. It uses the generated headers from `protocol/` only for core and system ids. |
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
- **Link.** Frame encoding and the frame scanner come from `protocol/`, with LEN as a `u16`.
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
    2. checks the protocol version
    3. loads the descriptors from its cache by hash, or fetches them
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
  - OTA (stream the image, confirm after reconnect)
  - diag (decode events, query entries, map codes to names through the generated table)
- **Threading.**
  - One I/O thread (Asio) does all the reading and writing.
  - Clients get events through thread-safe queues.
  - The latest value of each subscription group sits in a lock-free single-writer buffer (seqlock), so the ROS `read()` never blocks.
  - Command group frames are queued without blocking.

### `drivelink-agent` (robot host)

- Owns the serial port, runs as a systemd service, and keeps working while ROS is down.
- Clients speak the device protocol over TCP. The agent arbitrates between them:
  - **`seq`.** It rewrites each client's `seq` before forwarding a request. It maps the response back and routes it to the client that sent the request.
  - **Groups.** Group ids are device resources. The agent gives each client its own groups out of the device's pool, rewrites the group ids and stream channels both ways, and sends each client only its own subscription streams. The status stream and diag events go to every client.
  - **Time.** The agent alone answers to the ESP's clock. Clients never send pings; the agent sends them its clock model (offset, drift, reference) as an agent message whenever the model changes.
  - **Control lease.** Only one client at a time may send `ARM`, configure command groups or send command frames. While ROS's controllers are active, ROS holds the lease. The GUI takes it explicitly to tune, and ROS gets it back with a clear notice. Losing the lease disarms the device.
  - **Exclusive operations.** Capture, test signals, `SAVE`, `REVERT`, `FACTORY_RESET`, layout ops and OTA need the lease, or need the device to be disarmed with nobody holding the lease.
  - **Param writes.** Any client may write live params at any time. Batches are atomic on the device. Whether to restrict this while ROS holds the lease is an open question.
- **Agent-only messages** use their own MSG_TYPEs from the generated registry: hello and version, lease request/grant/revoke, clock model, client list.

### `drivelink-gui` (Qt/QML)

- Uses the current Qt 6 LTS with Qt's MinGW kit on Windows. On Linux it uses the Qt from Ubuntu 26.04.
- **C++ backend:**
  - A device connection, either local (serial) or remote (agent), behind one interface.
  - Models built from the device model:
    - an instance tree (type, label, health)
    - a param table per instance (typed editors, units, ranges, enums, a pending-reboot marker, a dirty marker)
    - a signal list
    - action forms
- **QML:** entirely generic, with no screen for any particular module. A module can still add an optional custom panel under `gui/modules/<type>/`, matched by type name.
- **Plots:** a custom `QQuickItem` that draws line strips through the scene graph from a C++ ring buffer. Live plots use subscription groups the GUI sets up itself; captures are plotted from uploaded buffers.
- **Screens:**
  - **Connect:** a serial port, or an agent address.
  - **Device:** info, arm/disarm, faults (diag entries with names and descriptions), save/revert, reboot, OTA.
  - **Layout:** add or remove instances, wiring (`Ref` params with pickers), labels, pending-reboot banner.
  - **Tune:**
    1. Pick a controller instance and edit its gains live.
    2. Choose a test signal and the capture signals.
    3. Run, then overlay the results against earlier runs.
    4. Save or revert.
  - **Monitor:** free-form live plots of any signals.
- **Plant identification:** fit a first-order-plus-deadtime model to an open-loop duty capture and suggest starting gains. The math lives in `libdrivelink` and has unit tests.

### `drivelink_hardware` (ROS 2)

- A ros2_control `SystemInterface`.
- **Parameters:**
  - how to connect: the agent address, or a serial port for running without the agent
  - for each joint: the label of its controller instance, and which interfaces map to which signals and inputs (by item name, for example `position` → `position`, `velocity` → `velocity`, command `velocity` → `velocity_sp`)
  - for each sensor: the label of its instance and its signal names (for the IMU, the quaternion, gyro and linear acceleration signals)
- **`on_configure`:**
  1. Connect and load the device model.
  2. Resolve every label and item name to a `Ref`. Anything missing, wrongly typed or failed is a clear error.
  3. Set the controller `mode` params to match the claimed command interfaces.
- **`on_activate`:**
  1. Take the lease.
  2. Configure one subscription group per loop and one command group.
  3. `ARM`.
- **`on_deactivate`:** `DISARM`, then release the lease.
- **`read()`:** copies the latest group values out of the seqlock buffers, with timestamps mapped to ROS time.
- **`write()`:** queues one command group frame and never blocks.
- **Status and faults:** diag events and the status stream are published on `/diagnostics` through a small node in `drivelink_bringup`. A disarm while active returns `ERROR` from `read()`, so the controller manager reacts.
- **IMU:** exposed as sensor state interfaces, published with `imu_sensor_broadcaster`.
- **`drivelink_bringup`:** launch files, controller YAML, the diagnostics node and URDF snippets.

## Link budget

At 921600 baud 8N1 the link carries 92 160 B/s in each direction. Frame overhead is 8 bytes. A subscription group frame is `8 + 11 + 4·k` bytes, and a command group frame is `8 + 3 + 4·k`.

The table below is for ESP → host: motor signals (position, velocity, duty), plus an IMU group of 10 values at 100 Hz, plus the status stream at 10 Hz.

| Motors × 3 signals | 100 Hz | 200 Hz | 500 Hz | 1 kHz |
|---|---|---|---|---|
| 2 (frame 43 B) | 11 % | 16 % | 30 % | 53 % |
| 4 (frame 67 B) | 14 % | 21 % | 43 % | 79 % |
| 8 (frame 115 B) | 19 % | 32 % | 69 % | 132 % |

Host → ESP, one velocity per motor:

| Motors | 100 Hz | 200 Hz | 500 Hz | 1 kHz |
|---|---|---|---|---|
| 4 (frame 27 B) | 3 % | 6 % | 15 % | 29 % |
| 8 (frame 43 B) | 5 % | 9 % | 23 % | 47 % |

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
│  ├─ CLAUDE.md  README.md  TODO.md  CMakeLists.txt  sdkconfig.defaults  sdkconfig.ci.nolog
│  ├─ main/
│  ├─ components/             uart_comm, diag, core, core_os, mod_motor, mod_imu_bno055,
│  │                          mod_mock, motor_hal, rotary_encoder_hal, pid, rtos_util
│  ├─ review/
│  └─ test/
│     ├─ native/              core and module tests, built natively
│     └─ uart_comm/           HIL test app + pytest
├─ protocol/                  pure C++20: IDF component + CMake package + package.xml
│  ├─ defs/                   registry.yaml, diag_codes.yaml, system.yaml, <module type>.yaml
│  ├─ include/drivelink/protocol/   framing, wire_codec, codecs; gen/ (generated)
│  ├─ src/
│  └─ test/
├─ host/
│  ├─ CMakeLists.txt  CMakePresets.json  CLAUDE.md
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
- **Host superbuild:** `host/CMakeLists.txt` brings in `protocol/` and builds libdrivelink, vdev, cli, agent and gui together.
  - `vdev` compiles the firmware core and modules against `core_os`'s native backend.
  - `CMakePresets.json` has `windows-mingw`, `linux` and `linux-no-gui` presets.
  - The Windows preset names Qt's MinGW compiler explicitly.
- **ROS:** `colcon build` from the repo root. `firmware/` and `host/gui/` carry `COLCON_IGNORE`.
- **MinGW notes:**
  - Never mix compilers. Qt, libdrivelink, vdev and the GUI all build with Qt's MinGW kit.
  - Strawberry Perl puts a different `g++` on `PATH` (the firmware host tests use it today). Name the compiler in the preset, and ship the right runtime DLLs with `windeployqt`.
  - Asio on MinGW needs `ws2_32` and `mswsock`, and `_WIN32_WINNT` defined.
- **Dependencies:** keep them few and mostly header-only. Use standalone Asio and the vendored doctest, pulled in with CPM/FetchContent or vendored. No vcpkg, because its MinGW support is community-only. The generator needs Python 3 and PyYAML.

## Adding a module

1. `protocol/defs/<type>.yaml`: the type id, params, signals, inputs, actions and loop classes. Then run the generator.
2. `firmware/components/mod_<...>/`: the factory and the `Instance`, which binds the generated ids to its storage, plus native tests for its pure logic.
3. diag codes for its faults in `diag_codes.yaml`, with `blocks_arm` and `disarms` set.
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
| 1 | Repo restructure (detailed below). No behavior change. |
| F1–F10 | The firmware plan ([firmware-architecture.md](firmware-architecture.md)). |
| H1 | `libdrivelink` (transport, session, time, device model, services), `drivelink-cli`, and the host-linux and host-windows CI. Tested against vdev. |
| H2 | GUI in local mode: connect, device, layout, tune and monitor screens, plant identification. |
| H3 | Agent: arbitration, lease, clock model distribution. The GUI's remote mode. |
| H4 | `drivelink_hardware` and `drivelink_bringup`, the diagnostics node, the ROS CI job. |

H1 can start once F4 is done, because vdev then covers the descriptors, params, groups and arming. Later firmware phases add services to `libdrivelink` as they land.

### Phase 1: restructure in detail

This assumes the diag plan has landed, so `components/diag`, `tools/gen_diag_codes.py` and `sdkconfig.ci.nolog` exist. It is done in two commits, so git's rename detection is exact and `git log --follow` keeps working.

**Commit 1: moves only (`git mv`, no content changes)**

| From | To |
|---|---|
| `main/` | `firmware/main/` |
| `components/` (with `components/CLAUDE.md`) | `firmware/components/` |
| `CMakeLists.txt` | `firmware/CMakeLists.txt` |
| `sdkconfig.defaults`, `sdkconfig.ci.nolog` | `firmware/` |
| `test/host/` | `firmware/test/host/` |
| `test/uart_comm/` | `firmware/test/uart_comm/` |
| `review/` | `firmware/review/` |
| `TODO.md` | `firmware/TODO.md` |
| `CLAUDE.md` | `firmware/CLAUDE.md` |
| `README.md` | `firmware/README.md` |

`tools/`, `plans/` and `.github/` stay at the root.

These relative paths stay valid because the depth does not change:
- `firmware/test/uart_comm/CMakeLists.txt` → `../../components`
- `firmware/test/host/CMakeLists.txt` → `../../components`
- the links between component READMEs

The paths inside `review/` and `TODO.md` are written relative to the firmware root, so they stay correct after the move.

**Commit 2: path updates and new root files**

- `.github/workflows/ci.yml`:
  - firmware matrix: `firmware` and `firmware/test/uart_comm`
  - the nolog build: `firmware/sdkconfig.ci.nolog`
  - host tests: `firmware/test/host`
  - the generated-files check: the new output paths
- `tools/gen_diag_codes.py`: its input and output paths under `firmware/components/diag/`. In F2 it is replaced by `tools/gen/`.
- `firmware/CMakeLists.txt`: the rule that sets `LOG_LOCAL_LEVEL` for the nolog build matches components by path. Check that it still finds them.
- `firmware/CLAUDE.md`:
  - commands become `idf.py -C firmware ...`, or tell the reader to work from `firmware/`
  - paths to `test/host`, `test/uart_comm` and `tools/`
  - the line on `review/` and `TODO.md`
- New root `CLAUDE.md` (short):
  - repo map
  - the "host and firmware change together" rule
  - where each area's rules live (`firmware/CLAUDE.md`, later `protocol/` and `host/CLAUDE.md`)
  - the definition of done for each area
- New root `README.md`: what DriveLink is (firmware plus host), the layout, and links to `firmware/README.md` and the two plans.
- `firmware/README.md`: fix the build commands and paths.
- `.gitignore`:
  - `/components/.idf/` becomes `/firmware/components/.idf/`
  - Fix the lines with trailing `# ...` comments (`*.exe`, `*.workspace`, `*.suo`, `*.sln.docstates`). Git treats the `#` as part of the pattern, so these lines match nothing today. This was checked with `git check-ignore`.
- `plans/esp-timer-control-loop.md`, `plans/imu-timer-sampling.md` and the diag plan are records of done work. Leave them as they are, and add one line at the top of each saying that paths are relative to `firmware/`.
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

- While ROS holds the lease, may the GUI change live params, or only read them?
- Should the agent keep a short history of captures and diag records, so a GUI that connects later can see them?
- Should the GUI save "tuning sessions" (captures plus the params used) as files on the PC?
- Python bindings for `libdrivelink` (for scripts and to replace `firmware/test/uart_comm/protocol.py`): when, if ever?
