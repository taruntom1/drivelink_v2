# DriveLink v2

DriveLink is a robot motion stack: ESP32-S3 firmware that runs motor control, encoder and IMU feedback in configurable loops, and generic host software that talks to it over a binary UART protocol. The firmware is self-describing, so the host needs no code for a new module type. The firmware is written in C++ on ESP-IDF 5.5.2; the host is C++20.

> **Status.** The firmware has not yet run on motors or a real IMU: the bench board has neither, and those checks are scripted and waiting (see "Deferred hardware checks" in [plans/host-status.md](plans/host-status.md)). The previous firmware is tagged `legacy-final`.

The host library, CLI and virtual device are available; see [host/README.md](host/README.md). The typed host API, GUI, robot-host agent and ROS 2 interface are planned in [plans/host-architecture.md](plans/host-architecture.md) with the steps in [plans/host-steps.md](plans/host-steps.md) and tracked in [plans/host-status.md](plans/host-status.md).

## Layout

| Path | What |
|---|---|
| [firmware/](firmware/README.md) | The ESP-IDF project: `main/`, `components/`, native unit tests, hardware-in-the-loop tests |
| [protocol/](protocol/CLAUDE.md) | The shared C++20 wire library and the definitions (`protocol/defs/`) it is generated from |
| [host/](host/README.md) | `libdrivelink`, `drivelink-cli`, `drivelink-vdev`, the superbuild and host tests |
| [docs/](docs/protocol/) | The wire spec (`docs/protocol/`) and [adding a module type](docs/adding-a-module.md) |
| `tools/` | The definitions generator, `validate.py` (every check) and `bench_fw.py` (bench flashing) |
| `plans/` | The architecture plans, the step status and the GUI design records |

## Build and test

```bash
python tools/validate.py full
```

runs every firmware build, the generator checks, the native tests and the host tests on Windows and Linux. See [firmware/README.md](firmware/README.md) for flashing and [CLAUDE.md](CLAUDE.md) for the repo rules.
