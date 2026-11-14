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
/// Each Motor instance takes an MCPWM operator of its own (6 at most on the
/// ESP32-S3), with one comparator and one generator. The two generators of an
/// operator share one force register, updated read-modify-write, so sharing
/// it would let one motor's releaseForce() undo another's forceLow(). The
/// timer of each MCPWM group is created on demand and shared.
///
/// Usage:
///   motor_hal::Motor left(GPIO_NUM_1, GPIO_NUM_2, {.start_forced = true});
///   left.write(0.0f);
///   left.releaseForce();
///   left.write(0.7f);   // forward at 70%
///   left.forceLow();    // stop, from any task
///
class Motor {
public:
    struct Options {
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

    /// Set the speed in [-1, 1] (clamped; positive is forward). Writes the DIR
    /// level, then the compare value, which takes effect at the next PWM
    /// period. DIR is written only with a
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

    /// Returns true if hardware was allocated successfully.
    bool isValid() const;

    /// Why construction failed: ESP_OK when valid, ESP_ERR_INVALID_ARG for
    /// bad pins, ESP_ERR_NOT_FOUND when the MCPWM pool is full, otherwise
    /// the driver error. ESP_ERR_INVALID_STATE for a moved-from object.
    esp_err_t initError() const;

private:
    struct Impl;
    Impl *m_impl;
};

} // namespace motor_hal
