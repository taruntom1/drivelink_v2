#include "doctest.h"

#include "pid_controller.hpp"

#include <cmath>

using pid::PidConfig;
using pid::PidController;

namespace {

PidController makePid(float kp, float ki, float kd, float cutoff_hz = 0.0f)
{
    PidConfig cfg{};
    cfg.kp = kp;
    cfg.ki = ki;
    cfg.kd = kd;
    cfg.output_min = -1.0f;
    cfg.output_max = 1.0f;
    cfg.derivative_cutoff_hz = cutoff_hz;
    PidController pid(cfg);
    pid.setMode(pid::Mode::Automatic);
    return pid;
}

} // namespace

TEST_CASE("PID defaults come from Kconfig: output [-1, 1], derivative filter off")
{
    const PidConfig cfg{};
    CHECK(cfg.output_min == doctest::Approx(-1.0f));
    CHECK(cfg.output_max == doctest::Approx(1.0f));
    CHECK(cfg.derivative_cutoff_hz == 0.0f);
}

TEST_CASE("PID in Manual mode outputs 0")
{
    PidConfig cfg{};
    cfg.kp = 1.0f;
    PidController pid(cfg);
    REQUIRE(pid.isValid());
    CHECK(pid.getMode() == pid::Mode::Manual);
    CHECK(pid.compute(0.0f, 0.5f, 0.01f) == 0.0f);
}

TEST_CASE("PID proportional term, direction and output clamp")
{
    auto pid = makePid(0.5f, 0.0f, 0.0f);
    CHECK(pid.compute(0.0f, 1.0f, 0.01f) == doctest::Approx(0.5f));

    pid.setDirection(pid::Direction::Reverse);
    CHECK(pid.compute(0.0f, 1.0f, 0.01f) == doctest::Approx(-0.5f));

    auto strong = makePid(10.0f, 0.0f, 0.0f);
    CHECK(strong.compute(0.0f, 1.0f, 0.01f) == doctest::Approx(1.0f));
    CHECK(strong.compute(0.0f, -1.0f, 0.01f) == doctest::Approx(-1.0f));
}

TEST_CASE("PID feedforward is added before the clamp")
{
    auto pid = makePid(0.0f, 0.0f, 0.0f);
    CHECK(pid.compute(0.0f, 0.0f, 0.01f, 0.3f) == doctest::Approx(0.3f));
    CHECK(pid.compute(0.0f, 0.0f, 0.01f, 5.0f) == doctest::Approx(1.0f));
}

TEST_CASE("PID integral scales with dt")
{
    auto a = makePid(0.0f, 1.0f, 0.0f);
    CHECK(a.compute(0.0f, 0.5f, 0.01f) == doctest::Approx(0.005f));

    auto b = makePid(0.0f, 1.0f, 0.0f);
    CHECK(b.compute(0.0f, 0.5f, 0.02f) == doctest::Approx(0.010f));

    // Ten 10 ms steps integrate the same as five 20 ms steps.
    auto fast = makePid(0.0f, 2.0f, 0.0f);
    auto slow = makePid(0.0f, 2.0f, 0.0f);
    for (int i = 0; i < 10; ++i) {
        fast.compute(0.0f, 0.25f, 0.01f);
    }
    for (int i = 0; i < 5; ++i) {
        slow.compute(0.0f, 0.25f, 0.02f);
    }
    CHECK(fast.getIntegralTerm() == doctest::Approx(slow.getIntegralTerm()));
    CHECK(fast.getIntegralTerm() == doctest::Approx(0.05f));
}

TEST_CASE("PID anti-windup clamps the integral to the output limits")
{
    auto pid = makePid(0.0f, 10.0f, 0.0f);
    for (int i = 0; i < 50; ++i) {
        pid.compute(0.0f, 1.0f, 0.1f); // +1.0 per step without a clamp
    }
    CHECK(pid.getIntegralTerm() == doctest::Approx(1.0f));

    // One step of opposite error unwinds it at once, instead of after ~49
    // steps as an unclamped integral would.
    pid.compute(0.0f, -1.0f, 0.1f);
    CHECK(pid.getIntegralTerm() == doctest::Approx(0.0f));
}

TEST_CASE("PID derivative on measurement: no kick on the first sample or on setpoint changes")
{
    auto pid = makePid(0.0f, 0.0f, 1.0f);
    CHECK(pid.compute(0.0f, 0.0f, 0.1f) == doctest::Approx(0.0f)); // first sample: no D
    CHECK(pid.compute(0.0f, 1.0f, 0.1f) == doctest::Approx(0.0f)); // setpoint step, input still
}

TEST_CASE("PID derivative is a rate: it scales with 1/dt")
{
    auto a = makePid(0.0f, 0.0f, 1.0f);
    a.compute(0.0f, 0.0f, 0.1f);
    // Input rises 0.05 in 0.1 s: rate 0.5/s, D on measurement is -0.5.
    CHECK(a.compute(0.05f, 0.0f, 0.1f) == doctest::Approx(-0.5f));

    auto b = makePid(0.0f, 0.0f, 1.0f);
    b.compute(0.0f, 0.0f, 0.2f);
    CHECK(b.compute(0.05f, 0.0f, 0.2f) == doctest::Approx(-0.25f));
}

TEST_CASE("PID derivative cutoff applies a first-order low-pass")
{
    constexpr float kCutoffHz = 10.0f;
    constexpr float kDt = 0.01f;
    const float alpha = 1.0f - std::exp(-2.0f * 3.14159265f * kCutoffHz * kDt);

    auto pid = makePid(0.0f, 0.0f, 1.0f, kCutoffHz);
    CHECK(pid.getDerivativeCutoff() == doctest::Approx(kCutoffHz));
    pid.compute(0.0f, 0.0f, kDt);
    // Raw rate is 1/s (D = -1), the filter passes alpha of it on the first step.
    const float d1 = pid.compute(0.01f, 0.0f, kDt);
    CHECK(d1 == doctest::Approx(-alpha));
    // Input holds still: the raw rate is 0, and the filter decays by (1 - alpha).
    const float d2 = pid.compute(0.01f, 0.0f, kDt);
    CHECK(d2 == doctest::Approx(-alpha * (1.0f - alpha)));
}

TEST_CASE("PID ignores a non-positive or non-finite dt")
{
    auto pid = makePid(0.0f, 1.0f, 0.0f);
    const float out = pid.compute(0.0f, 0.5f, 0.1f);
    const float integral = pid.getIntegralTerm();

    CHECK(pid.compute(0.0f, 0.5f, 0.0f) == doctest::Approx(out));
    CHECK(pid.compute(0.0f, 0.5f, -0.1f) == doctest::Approx(out));
    CHECK(pid.compute(0.0f, 0.5f, std::nanf("")) == doctest::Approx(out));
    CHECK(pid.compute(0.0f, 0.5f, INFINITY) == doctest::Approx(out));
    CHECK(pid.getIntegralTerm() == doctest::Approx(integral));
}

TEST_CASE("PID setters reject invalid values and keep the previous ones")
{
    auto pid = makePid(1.0f, 2.0f, 3.0f);

    pid.setTunings(-1.0f, 0.0f, 0.0f);
    CHECK(pid.getKp() == 1.0f);
    CHECK(pid.getKi() == 2.0f);
    CHECK(pid.getKd() == 3.0f);

    pid.setDerivativeCutoff(-5.0f);
    CHECK(pid.getDerivativeCutoff() == 0.0f);
    pid.setDerivativeCutoff(std::nanf(""));
    CHECK(pid.getDerivativeCutoff() == 0.0f);

    pid.setOutputLimits(0.5f, 0.5f); // min must be < max
    CHECK(pid.compute(0.0f, 10.0f, 0.01f) == doctest::Approx(1.0f));
}

TEST_CASE("PID rejects non-finite gains and limits")
{
    auto pid = makePid(1.0f, 2.0f, 3.0f);
    pid.setTunings(std::nanf(""), 0.0f, 0.0f);
    pid.setTunings(0.0f, INFINITY, 0.0f);
    CHECK(pid.getKp() == 1.0f);
    CHECK(pid.getKi() == 2.0f);

    pid.setOutputLimits(std::nanf(""), 1.0f);
    pid.setOutputLimits(-1.0f, INFINITY);
    CHECK(pid.compute(0.0f, 10.0f, 0.01f) == doctest::Approx(1.0f)); // still [-1, 1]

    PidConfig cfg{};
    cfg.kp = std::nanf("");
    cfg.ki = 0.5f;
    cfg.output_min = -INFINITY;
    PidController from_bad_config(cfg);
    CHECK(from_bad_config.getKp() == 0.0f); // replaced by 0
    CHECK(from_bad_config.getKi() == 0.5f); // valid gains are kept
    from_bad_config.setMode(pid::Mode::Automatic);
    const float out = from_bad_config.compute(0.0f, 100.0f, 1.0f);
    CHECK(std::isfinite(out));
    CHECK(out == doctest::Approx(1.0f)); // limits fell back to [-1, 1]
}

TEST_CASE("PID reset clears the integral and derivative history")
{
    auto pid = makePid(0.0f, 1.0f, 1.0f);
    pid.compute(0.0f, 1.0f, 0.1f);
    pid.compute(0.5f, 1.0f, 0.1f);
    REQUIRE(pid.getIntegralTerm() != 0.0f);

    pid.reset();
    CHECK(pid.getIntegralTerm() == 0.0f);
    CHECK(pid.getLastOutput() == 0.0f);
    // After a reset the first sample again has no D term.
    CHECK(pid.compute(5.0f, 5.0f, 0.1f) == doctest::Approx(0.0f));
}
