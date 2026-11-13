#pragma once

#include <algorithm>
#include <cmath>

namespace pid {
// Pure, nonlogging PID math for controller.cascade_pid: conditional
// integration (no saturation windup) and derivative on measurement.
inline float lowPassAlpha(float hz, float dt) {
    return hz <= 0 ? 1.F : 1.F - std::exp(-6.283185307179586F * hz * dt);
}
inline float derivativeRate(float input, float error, float last_input, float last_error,
                            float dt, float cutoff, bool on_error, float previous) {
    const float raw = (on_error ? error - last_error : last_input - input) / dt;
    return previous + lowPassAlpha(cutoff, dt) * (raw - previous);
}
inline float pidOutput(float p, float i, float d, float ff, float low, float high) {
    return std::clamp(p + i + d + ff, low, high);
}
struct Kernel {
    float kp = 0, ki = 0, kd = 0, cutoff = 0;
    float error = 0, p = 0, i = 0, d = 0, output = 0;
    float last_input = 0, rate = 0;
    bool primed = false;
    void reset() { error = p = i = d = output = last_input = rate = 0; primed = false; }
    void track(float input, float setpoint, float target, float limit) {
        reset();
        error = setpoint - input; p = kp * error;
        // With a held measurement error is zero. This output-domain I state
        // also supports bumpless transfer with ki == 0.
        output = std::clamp(target, -limit, limit);
        i = output - p; last_input = input;
    }
    float compute(float input, float setpoint, float dt, float limit) {
        if (!(dt > 0) || !std::isfinite(dt) || !std::isfinite(input) || !std::isfinite(setpoint)) {
            return 0;
        }
        const float next_error = setpoint - input;
        rate = primed ? derivativeRate(input, next_error, last_input, error, dt, cutoff, false, rate) : 0;
        error = next_error; p = kp * error; d = kd * rate;
        const float delta = ki * error * dt;
        const float candidate = std::clamp(i + delta, -limit, limit);
        const float proposed = p + candidate + d;
        if ((proposed <= limit && proposed >= -limit) || (proposed > limit && delta < 0) ||
            (proposed < -limit && delta > 0)) { i = candidate; }
        i = std::clamp(i, -limit, limit);
        output = pidOutput(p, i, d, 0, -limit, limit);
        last_input = input; primed = true;
        if (!std::isfinite(output)) { reset(); return 0; }
        return output;
    }
};
} // namespace pid
