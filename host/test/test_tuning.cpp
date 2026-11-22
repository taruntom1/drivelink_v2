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

namespace {
// An open-loop step capture at 1 kHz: the input holds `pre` for `preS`
// (after 60 samples at 0, like the pre-trigger part), then steps to `to`.
struct Capture { std::vector<double> t, u, y; };
template<class Plant> Capture capture(double pre, double preS, double to, double after, Plant plant, double noise = 0)
{
    Capture c;
    const int start = -60, stepAt = int(preS * 1000), end = stepAt + int(after * 1000);
    for (int i = start; i <= end; ++i) {
        const double t = i / 1000.0;
        const double u = i < 0 ? 0 : i < stepAt ? pre : to;
        c.t.push_back(t); c.u.push_back(u);
        c.y.push_back(plant(t, stepAt / 1000.0) + noise * std::sin(i * 1.7) * std::cos(i * 0.31));
    }
    return c;
}
double fopdt(double k, double tc, double l, double du, double tau) { return tau <= l ? 0 : k * du * (1 - std::exp(-(tau - l) / tc)); }
} // namespace

TEST_CASE("FOPDT fit recovers K, T and L from a noisy step from 0")
{
    const double K = 6.7, Tc = 0.085, L = 0.006;
    const auto c = capture(0, 0, 0.3, 1.0, [&](double t, double ts) { return fopdt(K, Tc, L, 0.3, t - ts); }, 0.01);
    const auto f = fitFopdt(c.t, c.u, c.y);
    CHECK(f.problem.empty());
    CHECK(f.settled);
    CHECK(f.step_time == doctest::Approx(0));
    CHECK(f.input_from == 0); CHECK(f.input_to == doctest::Approx(0.3));
    REQUIRE(f.gain); CHECK(*f.gain == doctest::Approx(K).epsilon(0.02));
    REQUIRE(f.time_constant_s); CHECK(*f.time_constant_s == doctest::Approx(Tc).epsilon(0.05));
    REQUIRE(f.dead_time_s); CHECK(std::fabs(*f.dead_time_s - L) < 0.0015);
    REQUIRE(f.error_pct); CHECK(*f.error_pct < 2);
    CHECK(fitQuality(f) == FitQuality::Good);
    CHECK(fopdtValue(f, -0.01) == doctest::Approx(f.response_from));
    CHECK(fopdtValue(f, 0.8) == doctest::Approx(K * 0.3).epsilon(0.02));
}

TEST_CASE("FOPDT fit of a step from a held pre-level, and of a negative step")
{
    const double K = 10, Tc = 0.1, L = 0.01;
    // 0.1 for 1 s (the first half of the square), then 0.3.
    const auto up = capture(0.1, 1.0, 0.3, 1.0, [&](double t, double ts) {
        const double first = t < 0 ? 0 : fopdt(K, Tc, L, 0.1, t);
        return first + (t < ts ? 0 : fopdt(K, Tc, L, 0.2, t - ts));
    });
    const auto f = fitFopdt(up.t, up.u, up.y);
    CHECK(f.problem.empty());
    CHECK(f.step_time == doctest::Approx(1.0));
    CHECK(f.input_from == doctest::Approx(0.1)); CHECK(f.input_to == doctest::Approx(0.3));
    CHECK(f.response_from == doctest::Approx(1.0).epsilon(0.01));
    REQUIRE(f.gain); CHECK(*f.gain == doctest::Approx(K).epsilon(0.02));
    REQUIRE(f.time_constant_s); CHECK(*f.time_constant_s == doctest::Approx(Tc).epsilon(0.05));
    REQUIRE(f.dead_time_s); CHECK(std::fabs(*f.dead_time_s - L) < 0.0015);
    // 0.3 then 0.1: the same plant, K stays positive.
    const auto down = capture(0.3, 1.0, 0.1, 1.0, [&](double t, double ts) {
        const double first = t < 0 ? 0 : fopdt(K, Tc, L, 0.3, t);
        return first + (t < ts ? 0 : fopdt(K, Tc, L, -0.2, t - ts));
    });
    const auto g = fitFopdt(down.t, down.u, down.y);
    CHECK(g.problem.empty());
    REQUIRE(g.gain); CHECK(*g.gain == doctest::Approx(K).epsilon(0.02));
}

TEST_CASE("FOPDT fit says why a capture gives no usable model")
{
    // An integrating response (position under a duty step) never settles.
    const auto ramp = capture(0, 0, 0.3, 1.0, [](double t, double ts) { return t < ts ? 0 : 2.0 * (t - ts); });
    const auto r = fitFopdt(ramp.t, ramp.u, ramp.y);
    CHECK_FALSE(r.settled);
    CHECK(r.problem.find("did not settle") != std::string::npos);
    CHECK(simcPi(r, TuneSpeed::Balanced, 0.001).problem == r.problem);
    // A constant input has no step.
    auto flat = capture(0, 0, 0.3, 1.0, [](double, double) { return 0.0; });
    std::fill(flat.u.begin(), flat.u.end(), 0.2);
    CHECK(fitFopdt(flat.t, flat.u, flat.y).problem == "no step in the input");
    // The response stays within its noise.
    const auto quiet = capture(0, 0, 0.3, 1.0, [](double, double) { return 0.0; }, 0.05);
    const auto q = fitFopdt(quiet.t, quiet.u, quiet.y);
    CHECK(q.problem == "the response barely changed (not above its noise)");
    CHECK_FALSE(q.gain);
    CHECK(simcPi(q, TuneSpeed::Balanced, 0.001).problem == q.problem);
    // Reversed polarity: a model, but no gains.
    const auto rev = capture(0, 0, 0.3, 1.0, [](double t, double ts) { return -fopdt(5, 0.05, 0.004, 0.3, t - ts); });
    const auto v = fitFopdt(rev.t, rev.u, rev.y);
    REQUIRE(v.gain); CHECK(*v.gain == doctest::Approx(-5).epsilon(0.02));
    CHECK(simcPi(v, TuneSpeed::Balanced, 0.001).problem.find("polarity") != std::string::npos);
    // A lightly damped response is not first order.
    const auto ring = capture(0, 0, 0.3, 2.0, [](double t, double ts) {
        const double tau = t - ts;
        return tau <= 0 ? 0 : 3.0 * (1 - std::exp(-2.0 * tau) * std::cos(20.0 * tau));
    });
    const auto p = fitFopdt(ring.t, ring.u, ring.y);
    REQUIRE(p.error_pct);
    CHECK(fitQuality(p) == FitQuality::Poor);
    CHECK_FALSE(simcPi(p, TuneSpeed::Balanced, 0.001).problem.empty());
    // Too short or mismatched captures.
    const std::vector<double> a{0, 1, 2}, b{0, 1};
    CHECK(fitFopdt(a, a, b).problem == "time, input and response lengths differ");
    CHECK(fitFopdt(a, a, a).problem == "too few samples");
}

TEST_CASE("SIMC PI gains for each speed, with the loop-period floor")
{
    FopdtFit f;
    f.gain = 2; f.time_constant_s = 0.1; f.dead_time_s = 0.01; f.error_pct = 1; f.settled = true; f.steady_start = true;
    auto g = simcPi(f, TuneSpeed::Balanced, 0.001);
    CHECK(g.problem.empty());
    CHECK(g.closed_loop_s == doctest::Approx(0.01));
    CHECK(g.kp == doctest::Approx(2.5)); CHECK(g.ki == doctest::Approx(2.5 / 0.08)); CHECK(g.kd == 0);
    g = simcPi(f, TuneSpeed::Robust, 0.001);
    CHECK(g.kp == doctest::Approx(1.25)); CHECK(g.ki == doctest::Approx(12.5));
    g = simcPi(f, TuneSpeed::Fast, 0.001);
    CHECK(g.closed_loop_s == doctest::Approx(0.005));
    CHECK(g.kp == doctest::Approx(0.1 / (2 * 0.015))); CHECK(g.ki == doctest::Approx(g.kp / 0.06));
    // No dead time: τc is four loop periods.
    f.dead_time_s = 0;
    g = simcPi(f, TuneSpeed::Balanced, 0.001);
    CHECK(g.closed_loop_s == doctest::Approx(0.004));
    CHECK(g.kp == doctest::Approx(12.5)); CHECK(g.ki == doctest::Approx(12.5 / 0.016));
    CHECK_FALSE(simcPi(f, TuneSpeed::Balanced, 0).problem.empty());
    f.error_pct = 20;
    CHECK(simcPi(f, TuneSpeed::Balanced, 0.001).problem == "the model does not fit the response well");
}

TEST_CASE("FOPDT fit through aliasing noise: settled, the error beyond the noise, K and T kept")
{
    // A three-sample ±15 % pattern on the moving response (as irregular ticks
    // alias a measured velocity), quiet at rest.
    const double K = 10, Tc = 0.1, L = 0.02;
    const int pattern[3] = {-1, 2, -1};
    auto c = capture(0, 1.0, 0.3, 1.0, [&](double t, double ts) { return fopdt(K, Tc, L, 0.3, t - ts); });
    for (size_t i = 0; i < c.y.size(); ++i) { c.y[i] *= 1 + 0.075 * pattern[i % 3]; }
    const auto f = fitFopdt(c.t, c.u, c.y);
    CHECK(f.problem.empty());
    CHECK(f.settled);
    CHECK(f.noise > 0.1);
    REQUIRE(f.gain); CHECK(*f.gain == doctest::Approx(K).epsilon(0.03));
    REQUIRE(f.time_constant_s); CHECK(*f.time_constant_s == doctest::Approx(Tc).epsilon(0.1));
    REQUIRE(f.error_pct); CHECK(fitQuality(f) == FitQuality::Good);
    CHECK(simcPi(f, TuneSpeed::Balanced, 0.001).problem.empty());
}

TEST_CASE("FOPDT fit: a pre-level still moving, NaN samples, and too few samples on either side")
{
    const double K = 10, Tc = 0.1, L = 0.01;
    // A large pre-level (0.25) held only 0.15 s (1.5 T) before a small step to
    // 0.3: still rising at the step by more than 5 % of the step.
    const auto early = capture(0.25, 0.15, 0.3, 1.0, [&](double t, double ts) {
        return (t < 0 ? 0 : fopdt(K, Tc, L, 0.25, t)) + (t < ts ? 0 : fopdt(K, Tc, L, 0.05, t - ts));
    });
    const auto e = fitFopdt(early.t, early.u, early.y);
    CHECK_FALSE(e.steady_start);
    CHECK(e.problem == "the response was still moving before the step: hold the start level longer");
    CHECK(simcPi(e, TuneSpeed::Balanced, 0.001).problem == e.problem);
    // NaN samples are skipped.
    auto gappy = capture(0, 0.5, 0.3, 1.0, [&](double t, double ts) { return fopdt(K, Tc, L, 0.3, t - ts); });
    gappy.y[700] = NAN; gappy.u[300] = NAN; gappy.t[900] = NAN;
    const auto g = fitFopdt(gappy.t, gappy.u, gappy.y);
    CHECK(g.problem.empty());
    REQUIRE(g.gain); CHECK(*g.gain == doctest::Approx(K).epsilon(0.02));
    // The step at the second sample: one sample before it.
    std::vector<double> t, u, y;
    for (int i = 0; i < 50; ++i) { t.push_back(i * 0.01); u.push_back(i == 0 ? 0 : 0.3); y.push_back(i < 3 ? 0 : 3); }
    CHECK(fitFopdt(t, u, y).problem == "too few samples before the step");
    // The step near the end: too few after it.
    for (int i = 0; i < 50; ++i) { u[size_t(i)] = i < 45 ? 0 : 0.3; }
    CHECK(fitFopdt(t, u, y).problem == "too few samples after the step");
}
