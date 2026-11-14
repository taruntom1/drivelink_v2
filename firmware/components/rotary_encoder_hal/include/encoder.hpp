#pragma once

#include <cstdint>
#include "driver/gpio.h"
#include "esp_err.h"

namespace rotary_encoder_hal {

namespace detail {
struct PcntUnit;
struct EncoderImpl;
}

/// Quadrature rotary encoder reader using the ESP32-S3 PCNT peripheral.
///
/// Each Encoder instance allocates one PCNT unit (both channels) from the
/// hardware pool, configured for 4x quadrature decoding. The ESP32-S3
/// has 4 PCNT units, so a maximum of 4 encoders can be active simultaneously.
///
/// It only counts: the velocity estimate is the caller's (mod_motor's
/// VelocityEstimator, from counts sampled at the loop rate).
///
/// Usage:
///   rotary_encoder_hal::Encoder wheel(GPIO_NUM_4, GPIO_NUM_5);
///   int32_t pos = 0;
///   if (wheel.isValid() && wheel.readCount(pos) == ESP_OK) { ... }
///
class Encoder {
public:
    /// Construct an encoder on the given quadrature signal pins.
    /// Allocates a PCNT unit, configures channels for quadrature decoding,
    /// and applies the glitch filter.
    /// @param pin_a  GPIO number for encoder channel A (CLK)
    /// @param pin_b  GPIO number for encoder channel B (DT)
    Encoder(gpio_num_t pin_a, gpio_num_t pin_b);

    struct Options {
        /// Internal pull-ups on A and B. The PCNT driver enables them; false
        /// disables them again (push-pull encoders, external pull-ups).
        bool pullup = true;
    };
    Encoder(gpio_num_t pin_a, gpio_num_t pin_b, const Options &options);

    /// Destructor. Releases the PCNT unit back to the pool.
    ~Encoder();

    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;

    Encoder(Encoder &&other) noexcept;
    Encoder &operator=(Encoder &&other) noexcept;

    /// Read the accumulated count (the PCNT accumulator, so not limited to
    /// 16 bits), reporting the driver result. ISR-safe driver call under the
    /// unit's spinlock; no allocation, no log.
    /// @return ESP_OK, ESP_ERR_INVALID_STATE when not valid, or the driver error.
    esp_err_t readCount(int32_t &count) const;

    /// Returns true if the PCNT hardware was allocated and configured successfully.
    bool isValid() const;

    /// Why construction failed: ESP_OK when valid, ESP_ERR_INVALID_ARG for
    /// bad pins, ESP_ERR_NOT_FOUND when the PCNT pool is full, otherwise the
    /// driver error. ESP_ERR_INVALID_STATE for a moved-from object.
    esp_err_t initError() const;

private:
    detail::EncoderImpl *m_impl;
};

} // namespace rotary_encoder_hal
