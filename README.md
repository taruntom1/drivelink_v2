# DriveLink v2

DriveLink is a robot motion stack: ESP32-S3 firmware for motor control, encoder and IMU feedback and time synchronization, and host software that talks to it over a binary UART protocol. The firmware is written in C++ on ESP-IDF 5.5.2.

The host side (a core library, a GUI, a robot-host agent and a ROS 2 interface) is planned and not built yet. The firmware is being rebuilt to match, in steps tracked in [plans/overhaul-status.md](plans/overhaul-status.md).

## Layout

| Path | What |
|---|---|
| [firmware/](firmware/README.md) | The ESP-IDF project: `main/`, `components/`, host unit tests, hardware-in-the-loop tests |
| `docs/protocol/` | The wire spec and the diag code table |
| `tools/` | Generators and scripts |
| `plans/` | The architecture plans and the overhaul status |

More top-level directories (`protocol/`, `host/`) arrive with later overhaul steps.

## Plans

- [plans/firmware-architecture.md](plans/firmware-architecture.md): the target firmware architecture and its phases.
- [plans/host-architecture.md](plans/host-architecture.md): the host software and the repo layout.

## Build

```bash
idf.py -C firmware build
```

See [firmware/README.md](firmware/README.md) for flashing and configuration, and [CLAUDE.md](CLAUDE.md) for the repo rules.
