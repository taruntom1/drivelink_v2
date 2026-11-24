#include "fake_motor_hw.hpp"
#include "mod_motor/hardware.hpp"
#include <utility>

namespace fake_motor_hw {

std::array<Output, kMax> outputs;
std::array<Counter, kMax> counters;
size_t output_count = 0, counter_count = 0;
core_os::Err fail_next_pwm_open = 0, fail_next_quadrature_open = 0;

void reset()
{
    for (auto &o : outputs) {
        o.pwm_pin = o.dir_pin = 0; o.pwm_freq_hz = 0; o.open = false; o.forced = false; o.duty = 0;
        o.writes = 0; o.forces = 0; o.releases = 0;
        o.fail_write = false; o.fail_force = false; o.fail_release = false;
        o.during_release = nullptr; o.during_release_ctx = nullptr;
    }
    for (auto &c : counters) {
        c.pin_a = c.pin_b = 0; c.pullup = false; c.glitch_ns = 0; c.open = false; c.count = 0; c.fail_read = false;
    }
    output_count = counter_count = 0;
    fail_next_pwm_open = fail_next_quadrature_open = 0;
}

namespace {
constexpr core_os::Err kDriverError = 0x103;

class FakePwmDir final : public mod_motor::hw::PwmDirOutput {
public:
    explicit FakePwmDir(Output &o) : m_o(o) {}
    ~FakePwmDir() override { m_o.forced = true; m_o.open = false; }
    core_os::Err write(float duty) override {
        if (m_o.fail_write.load()) { return kDriverError; }
        m_o.duty = duty; ++m_o.writes; return 0;
    }
    core_os::Err forceLow() override {
        if (m_o.fail_force.load()) { return kDriverError; }
        m_o.forced = true; ++m_o.forces; return 0;
    }
    core_os::Err releaseForce() override {
        if (m_o.fail_release.load()) { return kDriverError; }
        if (auto *hook = std::exchange(m_o.during_release, nullptr)) { hook(m_o.during_release_ctx); }
        m_o.forced = false; ++m_o.releases; return 0;
    }
private:
    Output &m_o;
};

class FakeQuadrature final : public mod_motor::hw::QuadratureCounter {
public:
    explicit FakeQuadrature(Counter &c) : m_c(c) {}
    ~FakeQuadrature() override { m_c.open = false; }
    core_os::Err read(int32_t &count) override {
        if (m_c.fail_read.load()) { return kDriverError; }
        count = m_c.count.load(); return 0;
    }
private:
    Counter &m_c;
};
} // namespace
} // namespace fake_motor_hw

namespace mod_motor::hw {

std::unique_ptr<PwmDirOutput> openPwmDir(uint8_t pwm_pin, uint8_t dir_pin, uint32_t pwm_freq_hz,
                                         core_os::Err &err)
{
    using namespace fake_motor_hw;
    err = fail_next_pwm_open; fail_next_pwm_open = 0;
    if (err != 0 || output_count == kMax) { if (err == 0) { err = -1; } return nullptr; }
    Output &o = outputs[output_count++];
    o.pwm_pin = pwm_pin; o.dir_pin = dir_pin; o.pwm_freq_hz = pwm_freq_hz; o.open = true; o.forced = true; // created forced
    return std::make_unique<FakePwmDir>(o);
}

std::unique_ptr<QuadratureCounter> openQuadrature(uint8_t pin_a, uint8_t pin_b, bool pullup, uint16_t glitch_ns,
                                                  core_os::Err &err)
{
    using namespace fake_motor_hw;
    err = fail_next_quadrature_open; fail_next_quadrature_open = 0;
    if (err != 0 || counter_count == kMax) { if (err == 0) { err = -1; } return nullptr; }
    Counter &c = counters[counter_count++];
    c.pin_a = pin_a; c.pin_b = pin_b; c.pullup = pullup; c.glitch_ns = glitch_ns; c.open = true;
    return std::make_unique<FakeQuadrature>(c);
}

} // namespace mod_motor::hw
