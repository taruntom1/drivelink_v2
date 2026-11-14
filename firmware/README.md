# DriveLink Firmware (ESP32-S3)

DriveLink is an ESP-IDF firmware stack for robot motion and telemetry on ESP32-S3. It combines motor actuation, encoder feedback, closed-loop control, IMU streaming, and time synchronization over a structured UART protocol.

The project is written in C++ and organized into reusable ESP-IDF components, so each hardware or control concern can evolve independently.

## What This Repository Is

This directory is the embedded firmware side of DriveLink. It is responsible for:

- Driving DC motors using PWM + direction control.
- Reading wheel/shaft rotary encoders.
- Running PID-based velocity/position control loops.
- Exposing module configuration, command groups and subscription streams over UART (the core).
- Streaming IMU telemetry data.
- Synchronizing time with an external host to align logs/telemetry.

In short, it is the real-time robot control runtime that talks to an upstream controller (PC/SBC/MCU) through a binary UART link.

## Key Features

- Structured UART transport (`uart_comm`) with:
	- CRC-16 integrity checks.
	- Callback-based message handling.
	- Priority command frames and latest-wins streaming channels.
- Motor hardware abstraction (`motor_hal`) with pooled MCPWM resource management (one operator per motor).
- Rotary encoder abstraction (`rotary_encoder_hal`) using ESP32-S3 PCNT (quadrature decoding).
- Header-only PID kernel (`pid`).
- The core (`core`): module instances in configurable loops, params, NVS, command and subscription groups, arming and the safety monitor.
- Motor module types (`mod_motor`): `actuator.pwm_dir`, `feedback.quadrature` and the cascade controller `controller.cascade_pid` (open loop, velocity, position).
- BNO055 module (`mod_imu_bno055`) with signals, modes and persistent calibration through core params/actions.
- Host-initiated time protocol v1 with monotonic RX/TX stamps; the host fits offset/drift. See [time.md](../docs/protocol/time.md).

## Runtime Architecture

The firmware entry point (`firmware/main/main.cpp`) brings up the stack in this order:

1. diagnostic state
2. `uart_comm::UartComm`
3. the core and its `CoreLink` (all module types and time pings)
4. the diag reporter

A part that fails to start is reported through diag and left out; the rest keep running. `firmware/main/CLAUDE.md` has the details.

## Components

- `firmware/components/uart_comm`:
	- Binary framing and transport over UART.
	- RX/TX tasks, callback registration, queued receive path, and diagnostics counters.
- `firmware/components/motor_hal`:
	- Low-level motor control API (`motor_hal::Motor`) over MCPWM + DIR GPIO.
- `firmware/components/rotary_encoder_hal`:
	- Encoder count API (`rotary_encoder_hal::Encoder`) using PCNT.
- `firmware/components/pid`:
	- Hardware-independent PID kernel (`pid_kernel.hpp`).
- `firmware/components/core`, `core_os`:
	- The core and its OS adapters (see their READMEs).
- `firmware/components/mod_motor`, `mod_mock`:
	- Motor module types and the mocks (see their READMEs).
- `firmware/components/mod_imu_bno055`:
	- Instance-owned BNO055 driver, module signals and persistent calibration.
- Time ping codecs live in `protocol/`; CoreLink and the existing UART TX task supply device timestamps. The legacy `time_sync` component is removed.

## Protocol-Oriented Control Model

DriveLink expects a host side that exchanges frames over UART:

- Command/response messages for configuration and control operations.
- Stream channels for fast-changing values (setpoints, telemetry).
- Message IDs and stream channels from the generated registry (`protocol/defs/registry.yaml`, listed in `docs/protocol/registry.md`).

This design allows low-latency control updates while keeping configuration and diagnostics explicit.

## Build And Flash

Run every command from the repository root; paths below are relative to it.

Prerequisites:

- ESP-IDF installed and exported in your shell.
- ESP32-S3 target toolchain available.

Typical development flow:

```bash
idf.py -C firmware build
idf.py -C firmware flash
idf.py -C firmware monitor
```

One-shot flow:

```bash
idf.py -C firmware build flash monitor
```

Configuration menu:

```bash
idf.py -C firmware menuconfig
```

If target/config artifacts become inconsistent:

```bash
idf.py -C firmware fullclean
idf.py -C firmware build
```

## Configuration Notes

- Most component defaults are configured via Kconfig and emitted into `sdkconfig`.
- UART pins/baud, IMU stream behavior, and control loop timings are tunable. Message types and stream channels are not: they come from the registry.
- `firmware/main/main.cpp` sets example startup values (for example periodic time sync every 5 seconds).

## Current Validation Strategy

There is no comprehensive unit test suite in this repository yet. Validation is currently done through:

- Successful ESP-IDF build.
- Flash and runtime observation through serial logs.
- Functional verification of command/stream behavior from the host side.