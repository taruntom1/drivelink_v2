# pid

## Overview
`pid` provides a hardware-independent PID controller implementation for ESP-IDF C++ applications. It supports runtime tuning, output clamping, derivative mode selection, and optional feedforward.

## Features
- Direct and reverse control direction
- Derivative on measurement or error
- Low-pass derivative filtering
- Manual/automatic mode switching
- Output saturation and integral clamping

## Dependencies
Required:
- `log`

Optional:
- None

## Configuration
| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_PID_DEFAULT_SAMPLE_TIME_MS` | int | 10 | Default controller sample period. |
| `CONFIG_PID_DEFAULT_OUTPUT_MIN` | int | -1000 | Default min output scaled by 1000 (`-1.0`). |
| `CONFIG_PID_DEFAULT_OUTPUT_MAX` | int | 1000 | Default max output scaled by 1000 (`1.0`). |
| `CONFIG_PID_DEFAULT_DERIVATIVE_FILTER_ALPHA` | int | 1000 | Default derivative filter alpha scaled by 1000 (`1.0`). |

## Public API
Public header: `include/pid_controller.hpp`

### Types
- `Mode`, `Direction`, `DerivativeMode`
- `PidConfig`

### `class PidController`
- Construction/copy/move/destruction
- `float compute(float input, float setpoint, float feedforward = 0.0f)`
- `setMode`, `setTunings`, `setOutputLimits`, `setDirection`, `setSampleTime`, `reset`
- Diagnostic getters (`getKp`, `getKi`, `getKd`, `getIntegralTerm`, etc.)
- `bool isValid() const`

## Usage Example
```cpp
#include "pid_controller.hpp"

esp_pid::PidConfig cfg{};
cfg.kp = 1.0f;
cfg.ki = 0.2f;
cfg.kd = 0.01f;

esp_pid::PidController pid(cfg);
pid.setMode(esp_pid::Mode::Automatic);

float out = pid.compute(0.5f, 1.0f);
```

## Error Handling
- Invalid constructor values are clamped/defaulted with logs.
- Invalid tuning/limit/sample-time updates are rejected and logged.
- `isValid()` reports allocation status.

## Thread Safety
- Not explicitly thread-safe; use from one control task or externally synchronize.

## Limitations & Known Issues
- Timing is caller-driven; `compute()` does not enforce wall-clock cadence.
