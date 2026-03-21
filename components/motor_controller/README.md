# motor_controller

ESP-IDF C++ component for single-motor closed-loop and open-loop control on **ESP32-S3**.

Integrates the `motor_hal`, `rotary_encoder_hal`, and `pid` components into a
single, easy-to-use controller class supporting:

- **Open-loop** – direct duty-cycle / voltage control
- **Closed-loop velocity** – angular velocity PID
- **Closed-loop position** – angular position PID

---

## Dependencies

| Component            | Role                                |
|----------------------|-------------------------------------|
| `motor_hal`          | PWM + DIR motor actuation           |
| `rotary_encoder_hal` | Quadrature encoder feedback         |
| `pid`                | PID algorithm for closed-loop modes |

---

## Adding to Your Project

```cmake
idf_component_register(
    ...
    REQUIRES motor_controller
)
```

Include the header:

```cpp
#include "motor_controller.hpp"
```

---

## Quick Start

```cpp
#include "motor_controller.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    motor_controller::MotorControllerConfig cfg;
    cfg.counts_per_rev         = 1440.0f;  // 360 CPR encoder, 4x decoded
    cfg.gear_ratio             = 1.0f;     // direct drive
    cfg.velocity_pid.kp        = 0.5f;
    cfg.velocity_pid.ki        = 0.1f;
    cfg.velocity_pid.kd        = 0.01f;
    cfg.velocity_pid.sample_time_ms = 10;

    motor_controller::MotorController mc(
        GPIO_NUM_1, GPIO_NUM_2,   // motor: pwm_gpio, dir_gpio
        GPIO_NUM_4, GPIO_NUM_5,   // encoder: channel A, channel B
        cfg);

    if (!mc.isValid()) {
        ESP_LOGE("main", "MotorController init failed");
        return;
    }

    mc.setMode(motor_controller::ControlMode::VelocityControl);
    mc.setVelocitySetpoint(10.0f);  // 10 rad/s

    while (true) {
        mc.update();                // call at sample_time_ms interval
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

---

## API Reference

### `motor_controller::ControlMode`

```cpp
enum class ControlMode {
    OpenLoop,        // Direct duty-cycle, no feedback
    VelocityControl, // Closed-loop angular velocity (rad/s)
    PositionControl, // Closed-loop angular position (rad)
};
```

---

### `motor_controller::MotorControllerConfig`

| Field           | Type                  | Default  | Description                                                     |
|-----------------|-----------------------|----------|-----------------------------------------------------------------|
| `counts_per_rev`| `float`               | 1440     | Total encoder counts per output shaft revolution                |
| `gear_ratio`    | `float`               | 1.0      | Motor shaft revs per output shaft rev (1.0 = direct drive)      |
| `output_min`    | `float`               | -1.0     | Motor output lower clamp                                        |
| `output_max`    | `float`               | 1.0      | Motor output upper clamp                                        |
| `velocity_pid`  | `esp_pid::PidConfig`  | defaults | Velocity PID parameters (kp, ki, kd, sample_time_ms, …)        |
| `position_pid`  | `esp_pid::PidConfig`  | defaults | Position PID parameters (kp, ki, kd, sample_time_ms, …)        |
| `velocity_feedforward` | `float`         | 0.0      | Constant feedforward term added to velocity PID output          |
| `position_feedforward` | `float`         | 0.0      | Constant feedforward term added to position PID output          |

The `output_min` / `output_max` from the config are automatically propagated to
both PID controller output limits on construction.

---

### `motor_controller::MotorController`

#### Constructor

```cpp
MotorController(gpio_num_t pwm_gpio, gpio_num_t dir_gpio,
                gpio_num_t enc_a,    gpio_num_t enc_b,
                const MotorControllerConfig &config = MotorControllerConfig{})
```

Constructs the motor and encoder HAL instances and initialises the PID
controllers. Always check `isValid()` before use.

---

#### `update()`

```cpp
void update()
```

Executes one control cycle: reads encoder, runs PID (if applicable), and drives
the motor. Must be called at the configured `sample_time_ms` interval from a
single FreeRTOS task. Not re-entrant.

---

#### Mode switching

```cpp
void setMode(ControlMode mode)
ControlMode getMode() const
```

Switches the active control mode. The motor is stopped momentarily and the
relevant PID state is reset on every transition to prevent output bumps.
When switching into `PositionControl`, the position setpoint is initialized to
the current angle for smoother transitions.

---

#### Setpoints (thread-safe with `update()`)

```cpp
void setOpenLoopDuty(float duty)        // [-1.0, 1.0]
void setVelocitySetpoint(float rad_s)   // rad/s
void setPositionSetpoint(float radians) // rad
void setVelocityFeedforward(float ff)
void setPositionFeedforward(float ff)
```

---

#### Feedback

```cpp
float getAngle() const          // current output shaft angle, radians
float getAngularVelocity() const // current output shaft velocity, rad/s
```

Angle is derived from the accumulated encoder count; velocity from the
encoder HAL's built-in rate sampler. Values are cached during `update()`.
If the active mode does not update one of the two measurements in `update()`,
the getter falls back to a fresh encoder read for that specific value.

---

#### Runtime configuration

```cpp
void setVelocityPidTunings(float kp, float ki, float kd)
void setPositionPidTunings(float kp, float ki, float kd)
void setSampleTime(unsigned int ms)   // updates both PID controllers
void setCountsPerRev(float counts)
void setGearRatio(float ratio)
void setOutputLimits(float min, float max)
void stop()
void reset()
```

PID tuning setters should be called from the same task as `update()`, or while
the update loop is suspended.

---

#### `isValid()`

```cpp
bool isValid() const
```

Returns `true` if both motor and encoder hardware initialised successfully.

---

## Gear Ratio Convention

`gear_ratio` is defined as **motor shaft revolutions per output shaft revolution**.

| Setup                         | `gear_ratio` |
|-------------------------------|-------------|
| Direct drive (no gearbox)     | `1.0`       |
| 50:1 reduction gearbox        | `50.0`      |
| 2:1 reduction gearbox         | `2.0`       |

Internal angle/velocity conversion:

```
angle_rad  = count          / (counts_per_rev × gear_ratio) × 2π
omega_rad_s = counts_per_sec / (counts_per_rev × gear_ratio) × 2π
```

---

## Thread Safety

| Operation                                          | Thread-safe with `update()`? |
|----------------------------------------------------|------------------------------|
| `setOpenLoopDuty` / `setVelocitySetpoint` / `setPositionSetpoint` | Yes (spinlock) |
| `setVelocityFeedforward` / `setPositionFeedforward` | Yes (spinlock) |
| `setMode`                                          | Yes (spinlock)               |
| `setCountsPerRev` / `setGearRatio` / `setOutputLimits` | Yes (spinlock)           |
| `getAngle` / `getAngularVelocity`                  | Yes (spinlock + encoder HAL) |
| `stop`                                             | Yes (idempotent; coordinate for deterministic sequencing) |
| `reset` / `setVelocityPidTunings` / `setPositionPidTunings` / `setSampleTime` | No — suspend update loop |

---

## Configuration (menuconfig)

```
idf.py menuconfig  →  Motor Controller Configuration
```

| Option                              | Default | Description                                    |
|-------------------------------------|---------|------------------------------------------------|
| `MOTOR_CTRL_DEFAULT_COUNTS_PER_REV` | 1440    | Default encoder counts per output revolution   |
| `MOTOR_CTRL_DEFAULT_SAMPLE_TIME_MS` | 10      | Default PID sample time in milliseconds        |

See also **PID Controller Configuration** for default PID gains and limits.
