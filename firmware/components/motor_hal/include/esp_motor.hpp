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
    struct Options {
        /// Take an MCPWM operator of its own (6 on the ESP32-S3) instead of
        /// sharing one with another motor. Required when forceLow() and
        /// releaseForce() run on different tasks: both generators of an
        /// operator share one force register, updated read-modify-write.
        bool exclusive_operator = false;
        /// Construct with the PWM output forced low; releaseForce() starts it.
        bool start_forced = false;
    };

    /// Construct a motor on the given pins.
    /// @param pwm_gpio  GPIO number for the PWM output (speed)
    /// @param dir_gpio  GPIO number for the direction output
    Motor(gpio_num_t pwm_gpio, gpio_num_t dir_gpio);
    Motor(gpio_num_t pwm_gpio, gpio_num_t dir_gpio, const Options &options);

    /// Destructor. Releases the MCPWM channel back to the pool.
    ~Motor();

    Motor(const Motor &) = delete;
    Motor &operator=(const Motor &) = delete;

    Motor(Motor &&other) noexcept;
    Motor &operator=(Motor &&other) noexcept;

    /// Set motor speed. Positive = forward, negative = reverse.
    /// @param speed  Value in [-1.0, 1.0], clamped if out of range.
    void setSpeed(float speed);

    /// The canonical speed write: DIR level, then the compare value (which
    /// takes effect at the next PWM period). DIR is written only with a
    /// nonzero compare value: a zero write leaves it, so a caller that passes
    /// through zero for at least one PWM period on a reversal never flips DIR
    /// under the old compare value. A non-finite speed writes 0.
    /// No lock, no allocation; logs nothing on success.
    /// @return ESP_OK, ESP_ERR_INVALID_STATE when not valid, or the driver error.
    esp_err_t write(float speed);

    /// Hold the PWM output low (continuous force), whatever write() does.
    /// Register writes only: no lock, no allocation, no log. May run
    /// concurrently with write(), which uses other registers.
    esp_err_t forceLow();

    /// Remove the force set by forceLow() or Options::start_forced.
    esp_err_t releaseForce();

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
