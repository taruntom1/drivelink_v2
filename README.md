# DriveLink Firmware (ESP32-S3)

DriveLink is an ESP-IDF firmware stack for robot motion and telemetry on ESP32-S3. It combines motor actuation, encoder feedback, closed-loop control, IMU streaming, and time synchronization over a structured UART protocol.

The project is written in C++ and organized into reusable ESP-IDF components, so each hardware or control concern can evolve independently.

## What This Repository Is

This repository is the embedded firmware side of DriveLink. It is responsible for:

- Driving DC motors using PWM + direction control.
- Reading wheel/shaft rotary encoders.
- Running PID-based velocity/position control loops.
- Exposing remote motor configuration and setpoint control over UART.
- Streaming motor and IMU telemetry data.
- Synchronizing time with an external host to align logs/telemetry.

In short, it is the real-time robot control runtime that talks to an upstream controller (PC/SBC/MCU) through a binary UART link.

## Key Features

- Structured UART transport (`uart_comm`) with:
	- CRC-16 integrity checks.
	- Callback-based message handling.
	- Priority command frames and latest-wins streaming channels.
- Motor hardware abstraction (`motor_hal`) with pooled MCPWM resource management.
- Rotary encoder abstraction (`rotary_encoder_hal`) using ESP32-S3 PCNT (quadrature decoding).
- Generic PID controller component (`pid`) with runtime tuning.
- Integrated per-motor controller (`motor_controller`) supporting:
	- Open-loop duty control.
	- Closed-loop velocity control.
	- Closed-loop position control.
	- Runtime setpoint, gain, feedforward, and limit updates.
- Multi-motor orchestration (`motor_manager`) with:
	- Dynamic create/delete/configure motor commands.
	- Per-motor and global telemetry configuration.
	- Setpoint stream ingestion over UART.
	- Periodic update task and telemetry publication.
- BNO055 IMU manager (`imu_bmo_055`) with:
	- Stream enable/disable and period control.
	- Selectable stream field masks (Euler, quaternion, accel, gyro, mag, temperature, calibration state).
	- Calibration read/write/apply support.
- Time synchronization service (`time_sync`) with optional round-trip compensation and periodic sync.

## Runtime Architecture

The firmware entry point (`main/main.cpp`) brings up the stack in this order:

1. `uart_comm::UartComm`
2. `motor_manager::MotorManager`
3. `time_sync::TimeSync`
4. `imu_bmo_055::ImuBno055Manager`

Each module validates initialization through `isValid()`. If any required subsystem fails to initialize, startup exits early.

## Components

- `components/uart_comm`:
	- Binary framing and transport over UART.
	- RX/TX tasks, callback registration, queued receive path, and diagnostics counters.
- `components/motor_hal`:
	- Low-level motor control API (`motor_hal::Motor`) over MCPWM + DIR GPIO.
- `components/rotary_encoder_hal`:
	- Encoder position/rate/direction API (`rotary_encoder_hal::Encoder`) using PCNT.
- `components/pid`:
	- Hardware-independent PID algorithm implementation.
- `components/motor_controller`:
	- Binds motor HAL + encoder HAL + PID for one motor control instance.
- `components/motor_manager`:
	- Protocol-facing manager for multiple motors, config commands, setpoint stream, telemetry stream.
- `components/imu_bmo_055`:
	- BNO055 device management and telemetry/control protocol integration.
- `components/time_sync`:
	- Request/response time sync protocol and periodic synchronization task.

## Protocol-Oriented Control Model

DriveLink expects a host side that exchanges frames over UART:

- Command/response messages for configuration and control operations.
- Stream channels for fast-changing values (setpoints, telemetry).
- Module-specific message IDs and channel IDs sourced from `sdkconfig` defaults/Kconfig.

This design allows low-latency control updates while keeping configuration and diagnostics explicit.

## Build And Flash

Prerequisites:

- ESP-IDF installed and exported in your shell.
- ESP32-S3 target toolchain available.

Typical development flow:

```bash
idf.py build
idf.py flash
idf.py monitor
```

One-shot flow:

```bash
idf.py build flash monitor
```

Configuration menu:

```bash
idf.py menuconfig
```

If target/config artifacts become inconsistent:

```bash
idf.py fullclean
idf.py build
```

## Configuration Notes

- Most component defaults are configured via Kconfig and emitted into `sdkconfig`.
- UART pins/baud, manager message types/channels, IMU stream behavior, and control loop timings are tunable.
- `main/main.cpp` currently sets example startup values (for example `max_motors = 4` and periodic time sync every 5 seconds).

## Current Validation Strategy

There is no comprehensive unit test suite in this repository yet. Validation is currently done through:

- Successful ESP-IDF build.
- Flash and runtime observation through serial logs.
- Functional verification of command/stream behavior from the host side.