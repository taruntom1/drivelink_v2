# motor_controller

## Overview
`motor_controller` composes `motor_hal`, `rotary_encoder_hal`, and `pid` to provide a single-motor control layer with open-loop, velocity PID, and position PID modes.

## Features
- Open-loop duty control
- Velocity and position closed-loop modes
- Runtime mode switching with transition-safe PID handling
- Runtime tuning of PID gains, limits, sample time, and conversion factors
- Cached feedback with fallback to live reads

## Dependencies
Required:
- `motor_hal`
- `rotary_encoder_hal`
- `pid`
- `freertos`
- `log`

Optional:
- None

## Configuration
| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_MOTOR_CTRL_DEFAULT_COUNTS_PER_REV` | int | 1440 | Default encoder counts per output shaft revolution. |
| `CONFIG_MOTOR_CTRL_DEFAULT_SAMPLE_TIME_MS` | int | 10 | Default PID sample time. |

## Public API
Public header: `include/motor_controller.hpp`

### Types
- `ControlMode`: `OpenLoop`, `VelocityControl`, `PositionControl`
- `MotorControllerConfig`: controller and PID configuration bundle

### Core methods
- `MotorController(...)`: constructs HAL + PID stack.
- `void update()`: executes one control cycle.
- `void setMode(ControlMode mode)` / `ControlMode getMode() const`
- `void setOpenLoopDuty(float duty)`
- `void setVelocitySetpoint(float rad_per_sec)`
- `void setPositionSetpoint(float radians)`
- `void setVelocityFeedforward(float)` / `setPositionFeedforward(float)`
- `float getAngle() const` / `float getAngularVelocity() const`
- Runtime tuning: `setVelocityPidTunings`, `setPositionPidTunings`, `setSampleTime`, `setCountsPerRev`, `setGearRatio`, `setOutputLimits`
- Lifecycle helpers: `bool isValid() const`, `void stop()`, `void reset()`

## Usage Example
```cpp
#include "motor_controller.hpp"

extern "C" void app_main(void)
{
    motor_controller::MotorController mc(
        GPIO_NUM_1, GPIO_NUM_2,
        GPIO_NUM_4, GPIO_NUM_5,
        motor_controller::MotorControllerConfig{});

    if (!mc.isValid()) {
        return;
    }

    mc.setMode(motor_controller::ControlMode::VelocityControl);
    mc.setVelocitySetpoint(8.0f);

    while (true) {
        mc.update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

## Error Handling
- Construction failures are exposed via `isValid()==false`.
- Invalid runtime configuration values are rejected/clamped and logged.
- PID/manual mode constraints are handled internally.

## Thread Safety
- Atomic fields are used for simple shared values (setpoints/feedforward/conversion factors).
- Spinlock protects mode transitions and output limit updates.
- `update()` is not re-entrant and should run from one task.
- PID tuning/reset APIs should be synchronized externally with update loop.

## Limitations & Known Issues
- One `MotorController` controls one motor only.
- No internal scheduler; caller must run `update()` periodically.
