# motor_hal

ESP-IDF C++ component for controlling DC motors via the **MCPWM peripheral** on **ESP32-S3**.

Supports **PWM + DIR** type motor drivers — one PWM signal for speed, one GPIO for direction.

---

## Hardware Requirements

- **Target:** ESP32-S3
- **Driver type:** PWM + DIR (e.g. DRV8833, TB6612FNG, L298N-compatible)
- **Max motors:** 12 (hardware limit: 2 MCPWM groups × 3 operators × 2 generators)

---

## Adding to Your Project

In your component's or `main`'s `CMakeLists.txt`:

```cmake
idf_component_register(
    ...
    REQUIRES motor_hal
)
```

Include the header:

```cpp
#include "esp_motor.hpp"
```

---

## Quick Start

```cpp
#include "esp_motor.hpp"

extern "C" void app_main(void)
{
    motor_hal::Motor left(GPIO_NUM_1, GPIO_NUM_2);   // pwm_gpio, dir_gpio
    motor_hal::Motor right(GPIO_NUM_3, GPIO_NUM_4);

    left.setSpeed(0.7f);    // 70% forward
    right.setSpeed(-0.5f);  // 50% reverse

    left.stop();
    right.stop();
}
```

---

## API Reference

### `motor_hal::Motor`

#### Constructor

```cpp
Motor(gpio_num_t pwm_gpio, gpio_num_t dir_gpio)
```

Allocates one MCPWM generator from the hardware pool and configures the direction GPIO.

| Parameter | Description |
|-----------|-------------|
| `pwm_gpio` | GPIO for the PWM speed signal |
| `dir_gpio` | GPIO for the direction signal |

If no MCPWM hardware is available (all 12 slots in use), or the pins are not two distinct output-capable GPIOs (`GPIO_IS_VALID_OUTPUT_GPIO`), construction succeeds but the motor is marked invalid. Always check `isValid()` before use. `setSpeed()` treats a non-finite speed as 0.

---

#### `setSpeed(float speed)`

```cpp
void setSpeed(float speed)
```

Sets motor speed and direction.

| Value | Behaviour |
|-------|-----------|
| `0.0` | Stopped (0% duty) |
| `> 0.0` | Forward |
| `< 0.0` | Reverse |

- Range: **-1.0 to 1.0** (values outside this range are clamped)
- Maps linearly to PWM duty cycle: `duty = |speed| × 100%`

---

#### `setSpeedPercent(float percent)`

```cpp
void setSpeedPercent(float percent)
```

Convenience wrapper for `setSpeed`. Accepts **-100.0 to 100.0**.

```cpp
motor.setSpeedPercent(75.0f);   // equivalent to setSpeed(0.75f)
motor.setSpeedPercent(-50.0f);  // equivalent to setSpeed(-0.5f)
```

---

#### `stop()`

```cpp
void stop()
```

Sets PWM duty to 0. Equivalent to `setSpeed(0.0f)`.

---

#### `isValid()`

```cpp
bool isValid() const
```

Returns `true` if MCPWM hardware was allocated successfully. Returns `false` if:
- All 12 motor slots are already in use
- GPIO configuration failed
- MCPWM driver initialization failed

Always check before using a motor object:

```cpp
motor_hal::Motor m(GPIO_NUM_5, GPIO_NUM_6);
if (!m.isValid()) {
    ESP_LOGE(TAG, "Motor init failed");
    return;
}
```

---

#### `getSpeed()`

```cpp
float getSpeed() const
```

Returns the last speed value passed to `setSpeed()`, in the range **-1.0 to 1.0**. Returns `0.0` if the motor is invalid.

---

#### Copy / Move semantics

```cpp
Motor(const Motor &) = delete;             // not copyable
Motor &operator=(const Motor &) = delete;

Motor(Motor &&) noexcept;                  // movable
Motor &operator=(Motor &&) noexcept;
```

Motors own hardware resources and cannot be copied. They can be moved into containers:

```cpp
std::vector<motor_hal::Motor> motors;
motors.emplace_back(GPIO_NUM_1, GPIO_NUM_2);
```

---

## Configuration (menuconfig)

```
idf.py menuconfig  →  Motor HAL Configuration
```

| Option | Default | Range | Description |
|--------|---------|-------|-------------|
| `MOTOR_HAL_PWM_FREQ_HZ` | `25000` | 1000–100000 | PWM switching frequency in Hz |
| `MOTOR_HAL_TIMER_RESOLUTION_HZ` | `1000000` | 100000–80000000 | MCPWM timer resolution in Hz |

**Duty cycle granularity** = `1 / (TIMER_RESOLUTION_HZ / PWM_FREQ_HZ)`:

| Resolution | Frequency | Period ticks | Granularity |
|-----------|-----------|-------------|-------------|
| 1 MHz | 25 kHz | 40 | 2.5% |
| 10 MHz | 25 kHz | 400 | 0.25% |
| 10 MHz | 50 kHz | 200 | 0.5% |

---

## Hardware Allocation Model

MCPWM resources are allocated lazily and shared where possible:

```
Group 0:  Timer (shared)
  ├── Operator 0  →  Motor 0 (slot A),  Motor 1  (slot B)
  ├── Operator 1  →  Motor 2 (slot A),  Motor 3  (slot B)
  └── Operator 2  →  Motor 4 (slot A),  Motor 5  (slot B)

Group 1:  Timer (shared)
  ├── Operator 0  →  Motor 6 (slot A),  Motor 7  (slot B)
  ├── Operator 1  →  Motor 8 (slot A),  Motor 9  (slot B)
  └── Operator 2  →  Motor 10 (slot A), Motor 11 (slot B)
```

- **Timers** are created when the first motor in a group is constructed, destroyed when the last one is destructed.
- **Operators** are created when the first of their two motors is constructed, destroyed when both are destructed.
- **Comparators and generators** are created and destroyed per motor instance.

---

## Thread Safety

| Operation | Thread safe |
|-----------|-------------|
| `Motor` construction / destruction | Yes (pool uses a FreeRTOS mutex) |
| `setSpeed()` / `stop()` | Yes (comparator register write and GPIO are atomic) |
| Concurrent `setSpeed` on the **same** Motor from two tasks | No — add external synchronization |
