#pragma once

#include <cstdint>
#include "driver/gpio.h"
#include "esp_err.h"

namespace motor_hal {

namespace detail {
struct McpwmChannel;
}

/// Motor control class for PWM+DIR type DC motor drivers.
///
/// Each Motor instance allocates one MCPWM generator + comparator from the
/// hardware resource pool. Timers and operators are created on demand and
/// shared across motors.
///
/// Usage:
///   motor_hal::Motor left(GPIO_NUM_1, GPIO_NUM_2);
///   left.setSpeed(0.7f);   // forward at 70%
///   left.setSpeed(-0.5f);  // reverse at 50%
///   left.stop();
///
class Motor {
public:
    /// Construct a motor on the given pins.
    /// @param pwm_gpio  GPIO number for the PWM output (speed)
    /// @param dir_gpio  GPIO number for the direction output
    Motor(gpio_num_t pwm_gpio, gpio_num_t dir_gpio);

    /// Destructor. Releases the MCPWM channel back to the pool.
    ~Motor();

    Motor(const Motor &) = delete;
    Motor &operator=(const Motor &) = delete;

    Motor(Motor &&other) noexcept;
    Motor &operator=(Motor &&other) noexcept;

    /// Set motor speed. Positive = forward, negative = reverse.
    /// @param speed  Value in [-1.0, 1.0], clamped if out of range.
    void setSpeed(float speed);

    /// Set motor speed as a percentage.
    /// @param percent  Value in [-100.0, 100.0], clamped if out of range.
    void setSpeedPercent(float percent);

    /// Stop the motor (sets PWM duty to 0).
    void stop();

    /// Returns true if hardware was allocated successfully.
    bool isValid() const;

    /// Why construction failed: ESP_OK when valid, ESP_ERR_INVALID_ARG for
    /// bad pins, ESP_ERR_NOT_FOUND when the MCPWM pool is full, otherwise
    /// the driver error. ESP_ERR_INVALID_STATE for a moved-from object.
    esp_err_t initError() const;

    /// Returns the current speed setting in [-1.0, 1.0].
    float getSpeed() const;

private:
    struct Impl;
    Impl *m_impl;
};

} // namespace motor_hal
