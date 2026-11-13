#include "doctest.h"

#include "pid_kernel.hpp"

#include <cmath>
#include <limits>

// pid::Kernel, the PID of controller.cascade_pid. The cascade behaviour that
// uses it is tested in test_motor_modules.cpp.

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
} // namespace

TEST_CASE("lowPassAlpha is 1 with no cutoff and the first-order factor otherwise")
{
    CHECK(pid::lowPassAlpha(0.F, .01F) == 1.F);
    CHECK(pid::lowPassAlpha(-5.F, .01F) == 1.F);
    CHECK(pid::lowPassAlpha(10.F, .01F) == doctest::Approx(1.F - std::exp(-6.2831853F * 10.F * .01F)));
}

TEST_CASE("Kernel proportional term and symmetric output limit")
{
    pid::Kernel k; k.kp = 2;
    CHECK(k.compute(0, .25F, .01F, 1) == doctest::Approx(.5F));
    CHECK(k.p == doctest::Approx(.5F));
    CHECK(k.compute(0, 10, .01F, 1) == 1);
    CHECK(k.compute(0, -10, .01F, 1) == -1);
}

TEST_CASE("Kernel integral scales with dt")
{
    pid::Kernel k; k.ki = 1;
    CHECK(k.compute(0, 1, .01F, 10) == doctest::Approx(.01F));
    CHECK(k.compute(0, 1, .02F, 10) == doctest::Approx(.03F));
}

TEST_CASE("Kernel integrates conditionally: never further into saturation, always back out")
{
    pid::Kernel k; k.kp = 1; k.ki = 10; k.kd = 1;
    k.track(0, 0, .5F, 1);
    CHECK(k.compute(0, 0, .01F, 1) == doctest::Approx(.5F)); // primes the derivative
    SUBCASE("a push further into saturation is not integrated") {
        k.kd = 0;
        CHECK(k.compute(0, 1, .01F, 1) == 1);
        CHECK(k.i == doctest::Approx(.5F));
    }
    SUBCASE("an error that reduces the integral is integrated while still saturated") {
        // The falling input makes a large positive derivative term, so the
        // output stays saturated high while the error is negative.
        CHECK(k.compute(-.1F, -.15F, .01F, 1) == 1);
        CHECK(k.d == doctest::Approx(10.F));
        CHECK(k.i == doctest::Approx(.5F - 10.F * .05F * .01F));
    }
}

TEST_CASE("Kernel derivative acts on the measurement: no kick on the first sample or a setpoint step")
{
    pid::Kernel k; k.kd = 1;
    CHECK(k.compute(5, 0, .01F, 100) == 0);  // unprimed
    CHECK(k.compute(5, 50, .01F, 100) == 0); // setpoint step, input unchanged
    CHECK(k.compute(4.9F, 50, .01F, 100) == doctest::Approx(10.F));
    pid::Kernel slow; slow.kd = 1;
    slow.compute(5, 0, .02F, 100);
    CHECK(slow.compute(4.9F, 0, .02F, 100) == doctest::Approx(5.F)); // a rate: scales with 1/dt
}

TEST_CASE("Kernel derivative cutoff applies a first-order low-pass")
{
    pid::Kernel k; k.kd = 1; k.cutoff = 10;
    k.compute(5, 0, .01F, 100);
    CHECK(k.compute(4.9F, 0, .01F, 100) == doctest::Approx(pid::lowPassAlpha(10, .01F) * 10.F));
}

TEST_CASE("Kernel ignores a bad dt, input or setpoint and keeps its state")
{
    pid::Kernel k; k.kp = 1; k.ki = 1;
    k.compute(0, 1, .01F, 10);
    const float i = k.i;
    for (float dt : {0.F, -1.F, kNaN, kInf}) { CHECK(k.compute(0, 1, dt, 10) == 0); }
    CHECK(k.compute(kNaN, 1, .01F, 10) == 0);
    CHECK(k.compute(0, kInf, .01F, 10) == 0);
    CHECK(k.i == i);
    CHECK(k.primed);
    CHECK(k.last_input == 0);
}

TEST_CASE("Kernel resets and outputs 0 when its output is not finite")
{
    pid::Kernel k; k.kp = kNaN;
    CHECK(k.compute(0, 1, .01F, 1) == 0);
    CHECK_FALSE(k.primed);
    CHECK(k.i == 0);
}

TEST_CASE("Kernel track presets the integral for a bumpless switch, within the limit")
{
    pid::Kernel k; k.kp = .1F;
    k.track(1, 3, .4F, 1);
    CHECK(k.output == doctest::Approx(.4F));
    CHECK(k.i == doctest::Approx(.2F));
    CHECK(k.compute(1, 3, .01F, 1) == doctest::Approx(.4F));
    k.track(0, 0, 2, 1);
    CHECK(k.output == 1); CHECK(k.i == 1);
    k.reset();
    CHECK(k.i == 0); CHECK(k.output == 0); CHECK_FALSE(k.primed);
}
