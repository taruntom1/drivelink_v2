# DriveLink v2

DriveLink is a robot motion stack. The ESP32-S3 firmware runs motor control, encoder and IMU feedback in configurable loops. It describes its modules over the binary UART protocol, so the generic host can work with new module types. The firmware uses ESP-IDF 5.5.2, the shared protocol library uses C++20, and the host uses C++23.

> **Status.** The firmware has not yet run on motors or a real IMU: the bench board has neither, and those checks are scripted and waiting (see "Deferred hardware checks" in [plans/host-status.md](plans/host-status.md)). The previous firmware is tagged `legacy-final`.

The host includes a typed C++ library, CLI, virtual device, desktop GUI, Linux agent and ROS 2 integration. For released packages and setup, see the [user guide](https://taruntom1.github.io/drivelink_v2/). For source builds and design decisions, see the [host README](host/README.md) and [host architecture](plans/host-architecture.md).

## Layout

| Path | What |
|---|---|
| [firmware/](firmware/README.md) | The ESP-IDF project: `main/`, `components/`, native unit tests, hardware-in-the-loop tests |
| [protocol/](protocol/CLAUDE.md) | The shared C++20 wire library and the definitions (`protocol/defs/`) it is generated from |
| [host/](host/README.md) | Library, CLI, virtual device, desktop GUI, Linux agent, ROS 2 packages and host tests |
| [docs/](docs/protocol/) | The wire spec (`docs/protocol/`) and [adding a module type](docs/adding-a-module.md) |
| `tools/` | The definitions generator, `validate.py` (every check) and `bench_fw.py` (bench flashing) |
| `plans/` | The architecture plans, the step status and the GUI design records |

## Build and test

```bash
python tools/validate.py full
```

runs the full validation gate. See [development](docs/development.md) for the check matrix and prerequisites, [firmware/README.md](firmware/README.md) for flashing, and [CLAUDE.md](CLAUDE.md) for the repository rules.
