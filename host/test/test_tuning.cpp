#include "doctest.h"
#include "drivelink/tuning.hpp"
#include <cmath>
#include <numbers>
#include <vector>
using namespace drivelink;

namespace {
struct Trace { std::vector<double> t, y; };
// Sampled at 1 kHz from -0.1 s; f(t) is the response after the step at 0.
template<class F> Trace sample(double seconds, F f, double before = 0)
{
    Trace r;
    for (int i = -100; i <= int(seconds * 1000); ++i) {
        const double t = i / 1000.0;
        r.t.push_back(t);
        r.y.push_back(t < 0 ? before : f(t));
    }
    return r;
}
} // namespace

TEST_CASE("first-order step: rise 2.2 tau, no overshoot, 2 % settling 3.9 tau")
{
    const double tau = 0.1;
    const auto r = sample(1.5, [&](double t) { return 2.0 * (1 - std::exp(-t / tau)); });
    const auto m = stepMetrics(r.t, r.y, 0, 0, 2.0);
    CHECK(m.problem.empty());
    REQUIRE(m.rise_s); CHECK(*m.rise_s == doctest::Approx(tau * std::log(9.0)).epsilon(0.01));
    REQUIRE(m.overshoot_pct); CHECK(*m.overshoot_pct == doctest::Approx(0));
    REQUIRE(m.settling_s); CHECK(*m.settling_s == doctest::Approx(tau * std::log(50.0)).epsilon(0.01));
    REQUIRE(m.steady_error); CHECK(std::fabs(*m.steady_error) < 1e-4);
}

TEST_CASE("underdamped second order: overshoot from the damping ratio")
{
    const double zeta = 0.3, wn = 40, wd = wn * std::sqrt(1 - zeta * zeta), phi = std::acos(zeta);
    const auto r = sample(2.0, [&](double t) {
        return 1.0 - std::exp(-zeta * wn * t) / std::sqrt(1 - zeta * zeta) * std::sin(wd * t + phi);
    });
    const auto m = stepMetrics(r.t, r.y, 0, 0, 1.0);
    const double expected = 100 * std::exp(-zeta * std::numbers::pi / std::sqrt(1 - zeta * zeta));
    REQUIRE(m.overshoot_pct); CHECK(*m.overshoot_pct == doctest::Approx(expected).epsilon(0.02));
    REQUIRE(m.settling_s); CHECK(*m.settling_s > 4.0 / (zeta * wn) * 0.8); CHECK(*m.settling_s < 4.0 / (zeta * wn) * 1.3);
    REQUIRE(m.rise_s); CHECK(*m.rise_s > 0); CHECK(*m.rise_s < 0.05);
}

TEST_CASE("a negative step from an offset, with steady-state error and noise")
{
    // 3 -> 1, settles at 1.1 (error -0.1), with deterministic noise of 0.5 % of the step.
    const auto r = sample(1.0, [](double t) {
        return 1.1 + 1.9 * std::exp(-t / 0.05) + 0.01 * std::sin(t * 997.0);
    }, 3.0);
    const auto m = stepMetrics(r.t, r.y, 0, 3.0, 1.0);
    REQUIRE(m.steady_error); CHECK(*m.steady_error == doctest::Approx(-0.1).epsilon(0.05));
    REQUIRE(m.overshoot_pct); CHECK(*m.overshoot_pct == doctest::Approx(0));
    REQUIRE(m.rise_s); CHECK(*m.rise_s > 0.05); CHECK(*m.rise_s < 0.15);
    // 1.1 is 5 % of the step away from the target: outside a 2 % band, it never settles.
    CHECK_FALSE(m.settling_s);
    CHECK(m.problem == "the response does not settle within the window");
    StepOptions wide; wide.band = 0.06;
    CHECK(stepMetrics(r.t, r.y, 0, 3.0, 1.0, wide).settling_s.has_value());
}

TEST_CASE("end_time cuts the window: the return to 0 after the run is not part of the step")
{
    // A 0.5 s step, then the target returns to 0 (the GUI's fresh frame).
    const auto r = sample(1.0, [](double t) { return t < 0.5 ? 1 - std::exp(-t / 0.05) : std::exp(-(t - 0.5) / 0.05); });
    CHECK_FALSE(stepMetrics(r.t, r.y, 0, 0, 1).settling_s);
    StepOptions o; o.end_time = 0.5;
    const auto m = stepMetrics(r.t, r.y, 0, 0, 1, o);
    REQUIRE(m.settling_s); CHECK(*m.settling_s == doctest::Approx(0.05 * std::log(50.0)).epsilon(0.03));
}

TEST_CASE("missing metrics say why")
{
    const auto flat = sample(1.0, [](double) { return 0.5; });
    CHECK(stepMetrics(flat.t, flat.y, 0, 1, 1).problem == "no step");
    const auto slow = sample(1.0, [](double t) { return 0.5 * t; });
    const auto m = stepMetrics(slow.t, slow.y, 0, 0, 1);
    CHECK_FALSE(m.rise_s); CHECK(m.problem == "the response does not reach 90 % of the step");
    CHECK(m.steady_error.has_value());
    const std::vector<double> t{0.0, 0.001}, y{0.0, 1.0};
    CHECK(stepMetrics(t, y, 0, 0, 1).problem == "too few samples after the step");
    const std::vector<double> shortT{0.0};
    CHECK(stepMetrics(shortT, y, 0, 0, 1).problem == "time and response lengths differ");
    // NaN samples are skipped, not propagated.
    auto gappy = sample(1.0, [](double t) { return 1 - std::exp(-t / 0.1); });
    gappy.y[500] = NAN;
    const auto g = stepMetrics(gappy.t, gappy.y, 0, 0, 1);
    REQUIRE(g.steady_error); CHECK(std::isfinite(*g.steady_error));
}
