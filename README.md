# DriveLink v2

DriveLink is a robot motion stack for the ESP32-S3. The firmware runs motor control and reads encoder and inertial sensor feedback in configurable loops. Over a serial link (UART), the device describes its modules, the values they expose, and the commands they accept. Host tools use those descriptions to discover and work with new module types.

To install a release and operate a device, start with the [user guide](https://taruntom1.github.io/drivelink_v2/). To build or test from source, start with the [development guide](docs/development.md).

**Hardware status.** Bare-board checks have run. Verification with physical motors and an inertial sensor is pending. See the [hardware log](plans/hardware-log.md) for completed runs and the [deferred hardware checks](plans/host-status.md#deferred-hardware-checks) for remaining coverage.

## Repository map

| Area | What lives there |
|---|---|
| [Firmware](firmware/README.md) | ESP-IDF 5.5.2 application, control loops, hardware drivers, and firmware tests. |
| [Shared protocol](protocol/README.md) | C++20 frame scanner and wire codecs, plus headers generated from the [protocol definitions](protocol/defs/). |
| [Host](host/README.md) | C++23 library, CLI, virtual device, desktop GUI, Linux agent, and ROS 2 integration. |
| [Wire specifications](docs/protocol/) | Message formats and device behavior. See [adding a module type](docs/adding-a-module.md) for the extension workflow. |
| [Tools](tools/) | Definition generation, validation, packaging, and bench builds. |
| [Design records](plans/) | Architecture, design decisions, and hardware verification plans. |

## Validation

From the repository root, run the full gate before opening a pull request:

```bash
python tools/validate.py full
```

For a focused run, select a check by name, for example `python tools/validate.py --only native`. See the [validation matrix and prerequisites](docs/development.md#validation-toolsvalidatepy) and [repository rules](CLAUDE.md).
