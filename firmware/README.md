# DriveLink firmware (ESP32-S3)

The embedded side of DriveLink: an ESP-IDF 5.5.2 firmware in C++ for robot
motion on the ESP32-S3. It is a self-describing core. The host discovers the
module types, instances, params, signals, inputs, actions and diag codes from
the device, configures everything over the UART link, and keeps nothing of its
own: the configuration lives in the device's NVS.

> **Not yet run on motors or a real IMU.** The bench board has neither. The
> motor, IMU and Pi checks are scripted and listed under "Deferred hardware
> checks" in [plans/host-status.md](../plans/host-status.md). Until
> they pass, treat real-motor and real-IMU behaviour as unverified. The last
> commit of the previous firmware is tagged `legacy-final`.

## What it does

- **Modules in configurable loops.** Loops are instances of the built-in
  `loop` type (rate, priority, real-time flag). Module instances run in them:
  `actuator.pwm_dir` (MCPWM + DIR), `feedback.quadrature` (PCNT),
  `controller.cascade_pid` (open loop, velocity, position), `imu.bno055` with
  persistent calibration, and mocks for testing.
- **Layout and persistence.** Add, remove and label instances in a pending
  layout, save it to NVS (A/B), revert, factory-reset; a boot that keeps
  crashing falls back to safe mode.
- **Streams.** Subscription groups stream signals from a loop; command groups
  set inputs. Sources let one loop read another loop's signal.
- **Safety.** Arming, a watchdog on command frames, a safety monitor for loop
  stalls, a fast `DISARM` path, and diag codes that block arming or disarm.
- **Tuning.** Capture of signals into PSRAM at the loop rate, and per-loop
  test signals (step, chirp and others).
- **Time and updates.** Host-initiated time pings with monotonic RX/TX stamps,
  and disarmed OTA into a dual-slot table with host confirmation and rollback.

The wire protocol is specified in [docs/protocol/](../docs/protocol/). The
host library, CLI and virtual device that speak it are in [host/](../host/README.md).

## Startup

`firmware/main/main.cpp` brings up diag, `uart_comm::UartComm`, NVS, the core
(boot from NVS, then start) and its `CoreLink`, and finally the diag reporter.
A core that fails to start is reported through diag and left out; the link
keeps running so the host can ask what went wrong. Details are in
[main/CLAUDE.md](main/CLAUDE.md).

## Components

Each component under `components/` has a README with its API, Kconfig options
and rules.

| Component | What |
|---|---|
| `uart_comm` | UART framing and transport: RX/TX tasks, callbacks, command pool, stream slots |
| `diag` | Error codes to the host (events and queries) |
| `rtos_util` | `StoppableTask`, `TaskTimer`, `msToTicksCeil` |
| `core_os` | OS adapters with an ESP-IDF and a native backend |
| `core` | The pure core: registry, loops, params, layout, NVS, groups, arming, sources, capture, testgen, OTA |
| `mod_motor` | Motor actuator, quadrature feedback and cascade controller module types |
| `mod_imu_bno055` | The BNO055 IMU module type |
| `mod_mock` | Mock module types (also run by the virtual device) |
| `motor_hal`, `rotary_encoder_hal` | MCPWM and PCNT wrappers with resource pools |
| `pid` | The header-only PID kernel |

The shared wire library and the generated definitions are in
[protocol/](../protocol/CLAUDE.md), at the repository root.

## Build and flash

Run every command from the repository root. `python tools/validate.py full`
runs every build and test; the guide is in [CLAUDE.md](CLAUDE.md).

```bash
idf.py -C firmware build
idf.py -C firmware build flash monitor
idf.py -C firmware menuconfig
```

Flash a board on the dual-slot partition table with
`python tools/bench_fw.py normal`, which checks the configuration and never
erases NVS. Later updates go over the link with the CLI's OTA workflow
(`docs/protocol/ota.md`).

The app version (`esp_app_desc_t.version`, GET_INFO's `firmware_version`) is
the root `VERSION`, with `+g<sha7>[.dirty]` away from its tag
([development guide](../docs/development.md), "Version").

## Configuration

Component tunables (UART pins and baud, queue depths, task stacks, pool sizes,
capture limits) are Kconfig options under each component's menu. Message types
and stream channels are not configurable: they come from
`protocol/defs/registry.yaml`. Loop rates, module params and the layout are not
Kconfig either: the host sets them over the link and the device saves them.

## Tests

- Native unit tests for the core, the module logic and the protocol
  (`firmware/test/native/`, `protocol/test/`).
- A hardware-in-the-loop test app for `uart_comm` (`firmware/test/uart_comm/`).
- Smoke scripts against the main firmware on a bare board
  (`firmware/test/uart_comm/smoke_*.py`, `host/test/*_smoke.py`).

See [test/CLAUDE.md](test/CLAUDE.md).
