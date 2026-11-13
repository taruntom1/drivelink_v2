#include "doctest.h"

#include "mod_motor/velocity_estimator.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

using mod_motor::VelocityEstimator;

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kCpr = 1000.0f; // counts per output revolution

} // namespace

TEST_CASE("countsToRadians and countsPerSecToRadPerSec include the gear ratio")
{
    CHECK(mod_motor::countsToRadians(1000, kCpr, 1.0f) == doctest::Approx(kTwoPi));
    CHECK(mod_motor::countsToRadians(-500, kCpr, 1.0f) == doctest::Approx(-kTwoPi / 2.0f));
    CHECK(mod_motor::countsToRadians(1000, kCpr, 2.0f) == doctest::Approx(kTwoPi / 2.0f));
    CHECK(mod_motor::countsPerSecToRadPerSec(2000.0f, kCpr, 1.0f) == doctest::Approx(2.0f * kTwoPi));
}

TEST_CASE("lowPassAlpha")
{
    CHECK(mod_motor::lowPassAlpha(0.0f, 0.01f) == 1.0f);
    CHECK(mod_motor::lowPassAlpha(-1.0f, 0.01f) == 1.0f);
    CHECK(mod_motor::lowPassAlpha(10.0f, 0.01f) ==
          doctest::Approx(1.0f - std::exp(-kTwoPi * 10.0f * 0.01f)));
    // A higher cutoff passes more of each new sample.
    CHECK(mod_motor::lowPassAlpha(100.0f, 0.01f) > mod_motor::lowPassAlpha(10.0f, 0.01f));
}

TEST_CASE("VelocityEstimator: the first sample only primes")
{
    VelocityEstimator est;
    CHECK_FALSE(est.isPrimed());
    CHECK(est.update(12345, 0.01f, kCpr, 1.0f, 0.0f) == 0.0f);
    CHECK(est.isPrimed());
    CHECK(est.estimate() == 0.0f);
}

TEST_CASE("VelocityEstimator: unfiltered estimate is delta / dt in rad/s")
{
    VelocityEstimator est;
    est.update(0, 0.01f, kCpr, 1.0f, 0.0f);
    // 100 counts in 10 ms = 10 rev/s.
    CHECK(est.update(100, 0.01f, kCpr, 1.0f, 0.0f) == doctest::Approx(10.0f * kTwoPi));
    // Same delta over twice the time: half the speed.
    CHECK(est.update(200, 0.02f, kCpr, 1.0f, 0.0f) == doctest::Approx(5.0f * kTwoPi));
    // Reverse.
    CHECK(est.update(100, 0.01f, kCpr, 1.0f, 0.0f) == doctest::Approx(-10.0f * kTwoPi));
    // A 4:1 gearbox divides the output-shaft speed.
    CHECK(est.update(200, 0.01f, kCpr, 4.0f, 0.0f) == doctest::Approx(2.5f * kTwoPi));
}

TEST_CASE("VelocityEstimator: the filter converges to a constant rate")
{
    constexpr float kDt = 0.001f;
    constexpr float kCutoffHz = 20.0f;
    const float alpha = mod_motor::lowPassAlpha(kCutoffHz, kDt);
    const float target = 10.0f * kTwoPi; // 10 counts per ms

    VelocityEstimator est;
    int32_t count = 0;
    est.update(count, kDt, kCpr, 1.0f, kCutoffHz);

    count += 10;
    CHECK(est.update(count, kDt, kCpr, 1.0f, kCutoffHz) == doctest::Approx(alpha * target));

    for (int i = 0; i < 1000; ++i) { // 50 time constants
        count += 10;
        est.update(count, kDt, kCpr, 1.0f, kCutoffHz);
    }
    CHECK(est.estimate() == doctest::Approx(target).epsilon(1e-4));
}

TEST_CASE("VelocityEstimator: invalid dt changes nothing")
{
    VelocityEstimator est;
    est.update(0, 0.01f, kCpr, 1.0f, 0.0f);
    const float v = est.update(100, 0.01f, kCpr, 1.0f, 0.0f);

    CHECK(est.update(5000, 0.0f, kCpr, 1.0f, 0.0f) == v);
    CHECK(est.update(5000, -0.01f, kCpr, 1.0f, 0.0f) == v);
    CHECK(est.update(5000, std::nanf(""), kCpr, 1.0f, 0.0f) == v);
    CHECK(est.update(5000, INFINITY, kCpr, 1.0f, 0.0f) == v);
    // The previous count was not replaced by the ignored samples.
    CHECK(est.update(200, 0.01f, kCpr, 1.0f, 0.0f) == doctest::Approx(v));
}

TEST_CASE("VelocityEstimator: an int32 wrap between samples gives the right delta")
{
    VelocityEstimator est;
    est.update(std::numeric_limits<int32_t>::max() - 4, 0.01f, kCpr, 1.0f, 0.0f);
    // max-4 -> min+5 is +10 counts.
    CHECK(est.update(std::numeric_limits<int32_t>::min() + 5, 0.01f, kCpr, 1.0f, 0.0f) ==
          doctest::Approx(1.0f * kTwoPi));
}

TEST_CASE("VelocityEstimator: reset re-primes so a count jump is not a speed")
{
    VelocityEstimator est;
    est.update(0, 0.01f, kCpr, 1.0f, 0.0f);
    est.update(100, 0.01f, kCpr, 1.0f, 0.0f);
    REQUIRE(est.estimate() != 0.0f);

    est.reset(); // e.g. the encoder count was cleared
    CHECK_FALSE(est.isPrimed());
    CHECK(est.estimate() == 0.0f);
    CHECK(est.update(-999999, 0.01f, kCpr, 1.0f, 0.0f) == 0.0f);
    CHECK(est.update(-999899, 0.01f, kCpr, 1.0f, 0.0f) == doctest::Approx(10.0f * kTwoPi));
}
