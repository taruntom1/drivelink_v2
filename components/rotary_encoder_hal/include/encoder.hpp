#pragma once

#include <cstdint>
#include "driver/gpio.h"

namespace esp_rotary_encoder {

/// Direction of encoder rotation, determined from the most recent rate sample.
enum class Direction {
    Forward,   ///< Count is increasing (positive delta)
    Reverse,   ///< Count is decreasing (negative delta)
    Stopped    ///< No count change detected in the last sample period
};

namespace detail {
struct PcntUnit;
}

/// Quadrature rotary encoder reader using the ESP32-S3 PCNT peripheral.
///
/// Each Encoder instance allocates one PCNT unit (both channels) from the
/// hardware pool, configured for 4x quadrature decoding. The ESP32-S3
/// has 4 PCNT units, so a maximum of 4 encoders can be active simultaneously.
///
/// An internal periodic timer samples the count at a fixed interval to
/// compute rotation rate and direction. The sampling period is configurable
/// via menuconfig (ENCODER_HAL_RATE_SAMPLE_PERIOD_MS).
///
/// Usage:
///   esp_rotary_encoder::Encoder wheel(GPIO_NUM_4, GPIO_NUM_5);
///   if (wheel.isValid()) {
///       int32_t pos = wheel.getCount();
///       float speed = wheel.getRate();
///       auto dir = wheel.getDirection();
///       wheel.reset();
///   }
///
class Encoder {
public:
    /// Construct an encoder on the given quadrature signal pins.
    /// Allocates a PCNT unit, configures channels for quadrature decoding,
    /// applies glitch filter, and starts the rate sampling timer.
    /// @param pin_a  GPIO number for encoder channel A (CLK)
    /// @param pin_b  GPIO number for encoder channel B (DT)
    Encoder(gpio_num_t pin_a, gpio_num_t pin_b);

    /// Destructor. Stops the rate timer and releases the PCNT unit back to the pool.
    ~Encoder();

    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;

    Encoder(Encoder &&other) noexcept;
    Encoder &operator=(Encoder &&other) noexcept;

    /// Returns the accumulated pulse count (position).
    /// Uses the PCNT accumulator, so the value is not limited to 16 bits.
    /// Thread-safe: pcnt_unit_get_count is ISR-safe.
    /// @return Accumulated count, or 0 if the encoder is invalid.
    int32_t getCount() const;

    /// Returns the last computed rotation direction.
    /// Based on the count delta from the most recent rate sample period.
    /// @return Direction::Forward, Direction::Reverse, or Direction::Stopped.
    Direction getDirection() const;

    /// Returns the rotation rate in pulses per second (signed).
    /// Positive when counting forward, negative when reversing.
    /// Updated internally at the configured sample rate.
    /// @return Rate in pulses/second, or 0.0f if the encoder is invalid.
    float getRate() const;

    /// Reset the accumulated count to zero.
    /// Also resets the internal rate measurement baseline so that
    /// the next rate sample starts fresh.
    void reset();

    /// Returns true if the PCNT hardware was allocated and configured successfully.
    bool isValid() const;

private:
    struct Impl;
    Impl *m_impl;
};

} // namespace esp_rotary_encoder
