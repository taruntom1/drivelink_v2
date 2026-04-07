# motor_hal

## Overview
`motor_hal` is the low-level PWM+DIR motor actuation HAL for ESP32-S3. It owns MCPWM resource allocation, configures generator/comparator/timer state, and exposes a small `esp_motor::Motor` API for signed speed control.

## Features
- PWM + DIR motor control with signed speed API (`-1.0..1.0`)
- Shared MCPWM pool with lazy timer/operator creation
- Deterministic resource release in destructors
- Move-only hardware-owning motor objects
- Kconfig-driven PWM frequency and timer resolution

## Dependencies
Required:
- `esp_driver_mcpwm`
- `esp_driver_gpio`

Optional:
- None

## Configuration
| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_MOTOR_HAL_PWM_FREQ_HZ` | int | 25000 | PWM frequency used for all allocated channels. |
| `CONFIG_MOTOR_HAL_TIMER_RESOLUTION_HZ` | int | 1000000 | MCPWM timer resolution that determines duty granularity. |

## Public API
Public header: `include/esp_motor.hpp`

### `esp_motor::Motor::Motor(gpio_num_t pwm_gpio, gpio_num_t dir_gpio)`
- **Signature**: constructor
- **Purpose**: Allocates one MCPWM channel from the pool and configures DIR GPIO.
- **Parameters**:
  | Name | Direction | Type | Description |
  |------|-----------|------|-------------|
  | `pwm_gpio` | IN | `gpio_num_t` | PWM output GPIO. |
  | `dir_gpio` | IN | `gpio_num_t` | Direction output GPIO. |
- **Returns**: N/A (constructor)
- **Notes**: Check `isValid()` after construction.

### `esp_motor::Motor::~Motor()`
- **Purpose**: Stops output and releases MCPWM resources.

### `void setSpeed(float speed)`
- **Purpose**: Sets signed motor command.
- **Parameters**:
  | Name | Direction | Type | Description |
  |------|-----------|------|-------------|
  | `speed` | IN | `float` | Command in `[-1.0, 1.0]` (clamped). |
- **Returns**: void

### `void setSpeedPercent(float percent)`
- **Purpose**: Convenience wrapper for percentage input.

### `void stop()`
- **Purpose**: Commands zero output.

### `bool isValid() const`
- **Purpose**: Indicates successful hardware initialization.

### `float getSpeed() const`
- **Purpose**: Returns last applied signed speed.

### Move operations
- `Motor(Motor&&) noexcept`
- `Motor& operator=(Motor&&) noexcept`
- **Purpose**: Transfer ownership of hardware-backed instance safely.

## Usage Example
```cpp
#include "esp_motor.hpp"

extern "C" void app_main(void)
{
    esp_motor::Motor m(GPIO_NUM_1, GPIO_NUM_2);
    if (!m.isValid()) {
        return;
    }

    m.setSpeed(0.5f);
    m.setSpeed(-0.3f);
    m.stop();
}
```

## Error Handling
Public API is mostly void/bool. Internal failures are logged with ESP-IDF logging.
- Invalid init/resource exhaustion -> `isValid()==false`
- MCPWM/GPIO driver errors -> logged in constructor/pool methods

## Thread Safety
- Pool allocation/deallocation is mutex-protected.
- Concurrent writes to **same** `Motor` instance are externally synchronized by design.

## Limitations & Known Issues
- Max active motors is bounded by MCPWM resource topology (12 slots).
- No explicit per-instance locking in `Motor` methods.
