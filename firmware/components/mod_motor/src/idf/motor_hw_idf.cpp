// Device backend of mod_motor/hardware.hpp over motor_hal and
// rotary_encoder_hal. The HAL constructors log their own failures; the
// modules report them through core.InstanceStartFailed.
#include "mod_motor/hardware.hpp"
#include "esp_motor.hpp"
#include "encoder.hpp"

namespace mod_motor::hw {
namespace {

class IdfPwmDir final : public PwmDirOutput {
public:
    IdfPwmDir(uint8_t pwm_pin, uint8_t dir_pin)
        : m_motor(static_cast<gpio_num_t>(pwm_pin), static_cast<gpio_num_t>(dir_pin),
                  motor_hal::Motor::Options{.exclusive_operator = true, .start_forced = true}) {}
    core_os::Err initError() const { return m_motor.initError(); }
    core_os::Err write(float duty) override { return m_motor.write(duty); }
    core_os::Err forceLow() override { return m_motor.forceLow(); }
    core_os::Err releaseForce() override { return m_motor.releaseForce(); }

private:
    motor_hal::Motor m_motor;  // its destructor forces low and frees the channel
};

class IdfQuadrature final : public QuadratureCounter {
public:
    IdfQuadrature(uint8_t pin_a, uint8_t pin_b, bool pullup)
        : m_encoder(static_cast<gpio_num_t>(pin_a), static_cast<gpio_num_t>(pin_b),
                    rotary_encoder_hal::Encoder::Options{.pullup = pullup}) {}
    core_os::Err initError() const { return m_encoder.initError(); }
    core_os::Err read(int32_t &count) override { return m_encoder.readCount(count); }

private:
    rotary_encoder_hal::Encoder m_encoder;
};

} // namespace

std::unique_ptr<PwmDirOutput> openPwmDir(uint8_t pwm_pin, uint8_t dir_pin, core_os::Err &err)
{
    auto out = std::make_unique<IdfPwmDir>(pwm_pin, dir_pin);
    err = out->initError();
    if (err != ESP_OK) { return nullptr; }
    return out;
}

std::unique_ptr<QuadratureCounter> openQuadrature(uint8_t pin_a, uint8_t pin_b, bool pullup, core_os::Err &err)
{
    auto counter = std::make_unique<IdfQuadrature>(pin_a, pin_b, pullup);
    err = counter->initError();
    if (err != ESP_OK) { return nullptr; }
    return counter;
}

} // namespace mod_motor::hw
