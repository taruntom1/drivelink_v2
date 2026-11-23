# DriveLink firmware (ESP32-S3)

[main/main.cpp](main/main.cpp) assembles the firmware. See the [startup rules](main/CLAUDE.md) for initialization order and partial-failure handling.

## Components

| Component | Owns |
|---|---|
| [core](components/core/README.md) | Module lifecycle, loop execution, device configuration, safety and device services. |
| [core_os](components/core_os/README.md) | Platform interfaces with ESP-IDF and native backends. |
| [uart_comm](components/uart_comm/README.md) | UART driver, RX dispatch, command buffers and TX stream slots. Shared codecs and frame scanning live in [protocol](../protocol/README.md). |
| [diag](components/diag/README.md) | Diagnostic records, counters and reporting to the host. |
| [rtos_util](components/rtos_util/README.md) | FreeRTOS task, timer and delay helpers. |
| [mod_motor](components/mod_motor/README.md) | Motor actuator, encoder feedback and cascade controller module types. |
| [motor_hal](components/motor_hal/README.md) | MCPWM peripheral allocation and motor output. |
| [rotary_encoder_hal](components/rotary_encoder_hal/README.md) | PCNT peripheral allocation and quadrature counting. |
| [pid](components/pid/README.md) | Header-only PID kernel used by the controller. |
| [mod_imu_bno055](components/mod_imu_bno055/README.md) | BNO055 sensor integration and calibration. |
| [mod_mock](components/mod_mock/README.md) | Mock module types shared with the virtual device. |

## Configuration

| Change | Source of truth |
|---|---|
| Build-time pins, queue depths, task stacks or capacity limits | The owning component's Kconfig, for example [uart_comm/Kconfig](components/uart_comm/Kconfig). |
| Wire message or stream channel ID | [Protocol registry](../protocol/defs/registry.yaml). |
| Device loop rates, persistent module parameters or layout | Device configuration, edited over UART and saved to NVS. See the [core configuration contract](../docs/protocol/core.md). |

## Build, test and release

Use the [development commands](../docs/development.md#commands) for source builds, the [validation matrix](../docs/development.md#validation-toolsvalidatepy) for automated checks, and the [firmware test rules](test/CLAUDE.md) and [bench procedure](../docs/bench.md) for physical checks.

For a release image, use the flashing instructions in the package’s `README.txt`. The [firmware packager](../tools/package_fw.py) generates the commands for the image layout and identifies which flashing methods preserve NVS. See the [version convention](../docs/development.md#version-version) for the embedded app version.

Regenerate firmware notices from the repository root:

```bash
python host/gui/notices/gen_notices.py --write --manifest firmware/notices/notices.json --output firmware/notices/THIRD_PARTY_NOTICES.txt
```
