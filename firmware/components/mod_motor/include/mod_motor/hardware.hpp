#pragma once

#include "core_os/types.hpp"
#include <cstdint>
#include <memory>

/// The driver seam of the real motor modules. The module logic
/// (`actuator.pwm_dir`, `feedback.quadrature`) is pure; the device backend
/// (`src/idf/motor_hw_idf.cpp`) implements these over motor_hal and
/// rotary_encoder_hal, and the native tests link a fake. Nothing here logs.
namespace mod_motor::hw {

/// One PWM+DIR motor output. It is created forced low on an MCPWM operator and
/// timer of its own, so a force or release never touches another output's
/// register and outputs may run at different PWM frequencies.
/// Its destructor forces the PWM output low and frees the channel, which
/// disconnects the pin (it then floats; DIR stays driven).
class PwmDirOutput {
public:
    virtual ~PwmDirOutput() = default;
    /// Signed duty in [-1, 1]: the DIR level (high for negative; left as it
    /// is for zero), then the compare value, which takes effect at the next
    /// PWM period. Loop only.
    virtual core_os::Err write(float duty) = 0;
    /// Force the PWM output low until releaseForce(). Lock-free and
    /// allocation-free; may run on the monitor concurrently with write().
    virtual core_os::Err forceLow() = 0;
    virtual core_os::Err releaseForce() = 0;
};

/// One quadrature encoder counter (4x decoding, accumulated past 16 bits).
class QuadratureCounter {
public:
    virtual ~QuadratureCounter() = default;
    /// The accumulated count. Loop only; bounded, no lock wait, no log.
    virtual core_os::Err read(int32_t &count) = 0;
};

/// Setup time only (they allocate). On failure: nullptr, and `err` is the
/// driver error (on the device an esp_err_t; ESP_ERR_NOT_FOUND when the
/// MCPWM operators or PCNT units are used up). `glitch_ns` 0 turns the filter
/// off.
std::unique_ptr<PwmDirOutput> openPwmDir(uint8_t pwm_pin, uint8_t dir_pin, uint32_t pwm_freq_hz,
                                         core_os::Err &err);
std::unique_ptr<QuadratureCounter> openQuadrature(uint8_t pin_a, uint8_t pin_b, bool pullup, uint16_t glitch_ns,
                                                  core_os::Err &err);

} // namespace mod_motor::hw
