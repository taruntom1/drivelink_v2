#include "drivelink/tuning.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace drivelink {
namespace {
struct Point { double t, z, y; };
// The time at which the segment a→b crosses `level` (a.z < level <= b.z).
double crossing(const Point &a, const Point &b, double level)
{
    if (b.z == a.z) { return b.t; }
    return a.t + (b.t - a.t) * (level - a.z) / (b.z - a.z);
}
} // namespace

StepMetrics stepMetrics(std::span<const double> t, std::span<const double> response, double step_time, double from,
                        double to, const StepOptions &options)
{
    StepMetrics m;
    const double size = to - from;
    if (t.size() != response.size()) { m.problem = "time and response lengths differ"; return m; }
    if (!std::isfinite(size) || std::fabs(size) <= 1e-12 * std::max(1.0, std::fabs(to))) {
        m.problem = "no step";
        return m;
    }
    // Normalized response after the step: 0 at `from`, 1 at `to`.
    std::vector<Point> p;
    p.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        if (!std::isfinite(t[i]) || !std::isfinite(response[i]) || t[i] < step_time || t[i] > options.end_time) {
            continue;
        }
        p.push_back({t[i], (response[i] - from) / size, response[i]});
    }
    if (p.size() < 3) { m.problem = "too few samples after the step"; return m; }

    // Rise: the first crossing of 10 %, then the first crossing of 90 %.
    std::optional<double> t10, t90;
    for (size_t i = 0; i < p.size(); ++i) {
        const Point prev = i == 0 ? Point{step_time, p[0].z, p[0].y} : p[i - 1];
        if (!t10 && p[i].z >= 0.1) { t10 = i == 0 ? p[0].t : crossing(prev, p[i], 0.1); }
        if (t10 && p[i].z >= 0.9) { t90 = i == 0 ? p[0].t : crossing(prev, p[i], 0.9); break; }
    }
    if (t10 && t90) { m.rise_s = *t90 - *t10; }

    double peak = p.front().z;
    for (const auto &q : p) { peak = std::max(peak, q.z); }
    m.overshoot_pct = std::max(0.0, peak - 1.0) * 100.0;

    // Settling: the sample after the last one outside the band.
    std::optional<size_t> outside;
    for (size_t i = 0; i < p.size(); ++i) {
        if (std::fabs(p[i].z - 1.0) > options.band) { outside = i; }
    }
    if (!outside) { m.settling_s = p.front().t - step_time; }
    else if (*outside + 1 < p.size()) { m.settling_s = p[*outside + 1].t - step_time; }

    const size_t tail = std::max<size_t>(1, static_cast<size_t>(std::ceil(options.tail * static_cast<double>(p.size()))));
    double sum = 0;
    for (size_t i = p.size() - tail; i < p.size(); ++i) { sum += p[i].y; }
    m.steady_error = to - sum / static_cast<double>(tail);

    if (!m.rise_s) { m.problem = "the response does not reach 90 % of the step"; }
    else if (!m.settling_s) { m.problem = "the response does not settle within the window"; }
    return m;
}

namespace {
bool same(double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max({1.0, std::fabs(a), std::fabs(b)}); }
struct Sse { double sse = std::numeric_limits<double>::infinity(), k = 0; };
// Least squares for K at a given L and T over (τ from the step, y − y0).
// SSE = Σy² − (Σgy)²/Σg² at the optimal K, so one pass is enough.
Sse fitK(std::span<const double> tau, std::span<const double> dy, double du, double l, double tc)
{
    double gg = 0, gy = 0, yy = 0;
    for (size_t i = 0; i < tau.size(); ++i) {
        const double g = tau[i] > l ? du * (1 - std::exp(-(tau[i] - l) / tc)) : 0.0;
        gg += g * g; gy += g * dy[i]; yy += dy[i] * dy[i];
    }
    Sse r;
    if (!(gg > 0)) { return r; }
    r.k = gy / gg;
    r.sse = std::max(0.0, yy - gy * gy / gg);
    return r;
}
struct Best { double l = 0, t = 0; Sse s; };
// Golden-section search on log T for one L.
Best bestT(std::span<const double> tau, std::span<const double> dy, double du, double l, double lo, double hi)
{
    const double phi = 0.6180339887498949;
    double a = std::log(lo), b = std::log(hi);
    double c = b - phi * (b - a), d = a + phi * (b - a);
    Sse fc = fitK(tau, dy, du, l, std::exp(c)), fd = fitK(tau, dy, du, l, std::exp(d));
    for (int i = 0; i < 36; ++i) {
        if (fc.sse <= fd.sse) { b = d; d = c; fd = fc; c = b - phi * (b - a); fc = fitK(tau, dy, du, l, std::exp(c)); }
        else { a = c; c = d; fc = fd; d = a + phi * (b - a); fd = fitK(tau, dy, du, l, std::exp(d)); }
    }
    return fc.sse <= fd.sse ? Best{l, std::exp(c), fc} : Best{l, std::exp(d), fd};
}
} // namespace

FopdtFit fitFopdt(std::span<const double> t, std::span<const double> input, std::span<const double> response,
                  const FopdtOptions &options)
{
    FopdtFit f;
    if (t.size() != input.size() || t.size() != response.size()) { f.problem = "time, input and response lengths differ"; return f; }
    std::vector<double> T, U, Y;
    for (size_t i = 0; i < t.size(); ++i) {
        if (std::isfinite(t[i]) && std::isfinite(input[i]) && std::isfinite(response[i])) {
            T.push_back(t[i]); U.push_back(input[i]); Y.push_back(response[i]);
        }
    }
    const size_t n = T.size();
    if (n < 8) { f.problem = "too few samples"; return f; }
    // The step: the last change of the input to its final value.
    size_t k = n;
    for (size_t i = n; i-- > 0;) {
        if (!same(U[i], U.back())) { k = i; break; }
    }
    if (k == n) { f.problem = "no step in the input"; return f; }
    const size_t s = k + 1;
    size_t j = k;
    while (j > 0 && same(U[j - 1], U[k])) { --j; }
    f.step_time = T[s]; f.input_from = U[k]; f.input_to = U.back();
    const double du = f.input_to - f.input_from;
    const size_t before = k - j + 1;
    if (before < 2) { f.problem = "too few samples before the step"; return f; }
    const size_t m = n - s;
    if (m < 10) { f.problem = "too few samples after the step"; return f; }

    // Levels: the end of the level before the step, and the tail.
    const size_t nb = std::max<size_t>(2, size_t(std::ceil(options.baseline * double(before))));
    double y0 = 0, var = 0;
    for (size_t i = k + 1 - nb; i <= k; ++i) { y0 += Y[i]; }
    y0 /= double(nb);
    for (size_t i = k + 1 - nb; i <= k; ++i) { var += (Y[i] - y0) * (Y[i] - y0); }
    const double sd = std::sqrt(var / double(nb));
    const size_t nt = std::max<size_t>(2, size_t(std::ceil(options.tail * double(m))));
    double y1 = 0, tm = 0;
    for (size_t i = n - nt; i < n; ++i) { y1 += Y[i]; tm += T[i]; }
    y1 /= double(nt); tm /= double(nt);
    // The tail's trend and its noise around it.
    double sty = 0, stt = 0;
    for (size_t i = n - nt; i < n; ++i) { sty += (T[i] - tm) * (Y[i] - y1); stt += (T[i] - tm) * (T[i] - tm); }
    const double slope = stt > 0 ? sty / stt : 0.0, span = T[n - 1] - T[n - nt];
    double rr = 0;
    for (size_t i = n - nt; i < n; ++i) {
        const double e = Y[i] - (y1 + slope * (T[i] - tm));
        rr += e * e;
    }
    const double noise = nt > 2 ? std::sqrt(rr / double(nt - 2)) : 0.0;
    f.noise = noise;
    f.response_from = y0; f.response_to = y1;
    const double dy = y1 - y0;
    // The change must stand out from the levels' uncertainty (the standard
    // error of the two means) and exceed one sample's noise.
    const double se = std::sqrt(sd * sd / double(nb) + noise * noise / double(nt));
    if (!(std::fabs(dy) > options.min_snr * se) || !(std::fabs(dy) > std::max(sd, noise)) ||
        std::fabs(dy) <= 1e-12 * std::max(1.0, std::fabs(y0))) {
        f.problem = "the response barely changed (not above its noise)";
        return f;
    }
    // Settled: the tail drifts less than a fraction of the step, or less than
    // its noise can explain (three standard errors of the trend).
    const double drift = std::fabs(slope) * span;
    const double driftNoise = stt > 0 ? 3 * noise / std::sqrt(stt) * span : 0.0;
    f.settled = drift <= std::max(options.settled * std::fabs(dy), driftNoise);
    // The same test on the start level: a pre-level still moving biases y0.
    {
        double bt = 0;
        for (size_t i = k + 1 - nb; i <= k; ++i) { bt += T[i]; }
        bt /= double(nb);
        double bty = 0, btt = 0;
        for (size_t i = k + 1 - nb; i <= k; ++i) { bty += (T[i] - bt) * (Y[i] - y0); btt += (T[i] - bt) * (T[i] - bt); }
        const double bslope = btt > 0 ? bty / btt : 0.0, bspan = T[k] - T[k + 1 - nb];
        double br = 0;
        for (size_t i = k + 1 - nb; i <= k; ++i) {
            const double e = Y[i] - (y0 + bslope * (T[i] - bt));
            br += e * e;
        }
        const double bnoise = nb > 2 ? std::sqrt(br / double(nb - 2)) : 0.0;
        const double ballow = std::max(options.settled * std::fabs(dy), btt > 0 ? 3 * bnoise / std::sqrt(btt) * bspan : 0.0);
        f.steady_start = std::fabs(bslope) * bspan <= ballow;
    }

    // Start values from the 28.3 % and 63.2 % crossings (Smith's two points).
    std::vector<double> tau, ry;
    tau.reserve(m); ry.reserve(m);
    for (size_t i = s; i < n; ++i) { tau.push_back(T[i] - f.step_time); ry.push_back(Y[i] - y0); }
    const double window = tau.back();
    std::optional<double> t28, t63;
    for (size_t i = 0; i < m; ++i) {
        const double z = ry[i] / dy;
        if (!t28 && z >= 0.283) { t28 = tau[i]; }
        if (!t63 && z >= 0.632) { t63 = tau[i]; break; }
    }
    double t0 = window / 3, l0 = 0;
    if (t28 && t63 && *t63 > *t28) { t0 = 1.5 * (*t63 - *t28); l0 = std::max(0.0, *t63 - t0); }
    else if (t63) { t0 = std::max(*t63, window / double(m)); }
    // The search runs on at most 500 samples; the error uses all of them.
    const size_t stride = std::max<size_t>(1, (m + 499) / 500);
    std::vector<double> st, sy;
    for (size_t i = 0; i < m; i += stride) { st.push_back(tau[i]); sy.push_back(ry[i]); }
    const double dt = window / double(m - 1);
    const double lmax = std::max(dt, std::min(window * 0.5, std::max(3 * l0, l0 + 3 * t0)));
    const double tlo = std::max(dt * 0.25, t0 / 20), thi = std::max(tlo * 2, std::min(window * 5, t0 * 20));
    Best best;
    double lo = 0, hi = lmax;
    for (int pass = 0; pass < 4; ++pass) {
        const int points = pass == 0 ? 61 : 21;
        const double step = (hi - lo) / (points - 1);
        for (int i = 0; i < points; ++i) {
            const auto b = bestT(st, sy, du, lo + step * i, tlo, thi);
            if (b.s.sse < best.s.sse) { best = b; }
        }
        lo = std::max(0.0, best.l - step); hi = std::min(lmax, best.l + step);
    }
    if (!std::isfinite(best.s.sse)) { f.problem = "no model fits the response"; return f; }
    const auto full = fitK(tau, ry, du, best.l, best.t);
    f.gain = full.k; f.time_constant_s = best.t; f.dead_time_s = best.l;
    double sse = 0;
    for (size_t i = 0; i < m; ++i) {
        const double e = ry[i] - (fopdtValue(f, tau[i] + f.step_time) - y0);
        sse += e * e;
    }
    // The error beyond the measurement noise: a noisy encoder alone does not
    // make a model poor.
    f.error_pct = std::sqrt(std::max(0.0, sse / double(m) - noise * noise)) / std::fabs(dy) * 100.0;
    if (!f.steady_start) { f.problem = "the response was still moving before the step: hold the start level longer"; }
    else if (!f.settled) { f.problem = "the response did not settle: capture longer, or the response integrates"; }
    else if (fitQuality(f) == FitQuality::Poor) { f.problem = "the model does not fit the response well"; }
    return f;
}

double fopdtValue(const FopdtFit &f, double t)
{
    if (!f.gain || !f.time_constant_s || !f.dead_time_s) { return std::numeric_limits<double>::quiet_NaN(); }
    const double tau = t - f.step_time - *f.dead_time_s;
    const double du = f.input_to - f.input_from;
    return tau <= 0 ? f.response_from : f.response_from + *f.gain * du * (1 - std::exp(-tau / *f.time_constant_s));
}

FitQuality fitQuality(const FopdtFit &f)
{
    if (!f.error_pct || !std::isfinite(*f.error_pct)) { return FitQuality::Poor; }
    return *f.error_pct <= 5 ? FitQuality::Good : *f.error_pct <= 15 ? FitQuality::Fair : FitQuality::Poor;
}

PiGains simcPi(const FopdtFit &f, TuneSpeed speed, double loop_period_s)
{
    PiGains g;
    if (!f.gain || !f.time_constant_s || !f.dead_time_s) { g.problem = f.problem.empty() ? "no model" : f.problem; return g; }
    if (!f.settled || !f.steady_start) { g.problem = f.problem; return g; }
    if (fitQuality(f) == FitQuality::Poor) { g.problem = "the model does not fit the response well"; return g; }
    const double k = *f.gain, tc_plant = *f.time_constant_s, l = *f.dead_time_s;
    if (!(k > 0)) { g.problem = "K is not positive: the response moves against the input (check the polarity)"; return g; }
    const double factor = speed == TuneSpeed::Robust ? 3.0 : speed == TuneSpeed::Balanced ? 1.0 : 0.5;
    g.closed_loop_s = std::max(factor * l, loop_period_s > 0 ? 4 * loop_period_s : 0.0);
    if (!(g.closed_loop_s + l > 0)) { g.problem = "no dead time and no loop period: the closed-loop time is 0"; return g; }
    g.kp = tc_plant / (k * (g.closed_loop_s + l));
    g.ki = g.kp / std::min(tc_plant, 4 * (g.closed_loop_s + l));
    if (!std::isfinite(g.kp) || !std::isfinite(g.ki)) { g = {}; g.problem = "the gains are not finite"; }
    return g;
}
} // namespace drivelink
