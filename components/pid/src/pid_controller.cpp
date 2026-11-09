#include "pid_controller.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cmath>
#include <utility>

static const char *const TAG = "pid_controller";

namespace esp_pid {
namespace detail {

struct PidImpl {
    // User-domain gains (unscaled)
    float kp;
    float ki;
    float kd;

    // Configuration
    Direction direction;
    float out_min;
    float out_max;
    DerivativeMode derivative_mode;
    float derivative_cutoff_hz;

    // Runtime state
    Mode mode;
    float integral_sum;
    float last_input;
    float last_error;
    float last_output;
    float d_filtered; // filtered derivative rate (units of input per second)
    bool first_compute;
};

} // namespace detail

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

bool isValidCutoff(float hz)
{
    return std::isfinite(hz) && hz >= 0.0f;
}

// Smoothing factor of a first-order low-pass with cutoff `hz` sampled after
// `dt_s` seconds. 1 means "no filtering".
float lowPassAlpha(float hz, float dt_s)
{
    if (hz <= 0.0f) {
        return 1.0f;
    }
    return 1.0f - std::exp(-kTwoPi * hz * dt_s);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PidController::PidController(const PidConfig &config)
    : m_impl(new (std::nothrow) detail::PidImpl{})
{
    if (!m_impl) {
        ESP_LOGE(TAG, "Failed to allocate PidImpl");
        return;
    }

    m_impl->kp = config.kp;
    m_impl->ki = config.ki;
    m_impl->kd = config.kd;
    m_impl->direction = config.direction;
    m_impl->out_min = config.output_min;
    m_impl->out_max = config.output_max;
    m_impl->derivative_mode = config.derivative_mode;
    m_impl->derivative_cutoff_hz = config.derivative_cutoff_hz;

    m_impl->mode = Mode::Manual;
    m_impl->integral_sum = 0.0f;
    m_impl->last_input = 0.0f;
    m_impl->last_error = 0.0f;
    m_impl->last_output = 0.0f;
    m_impl->d_filtered = 0.0f;
    m_impl->first_compute = true;

    // Validate output limits
    if (m_impl->out_min >= m_impl->out_max) {
        ESP_LOGW(TAG, "Invalid output limits (min=%.4f >= max=%.4f), using defaults [-1, 1]",
                 m_impl->out_min, m_impl->out_max);
        m_impl->out_min = -1.0f;
        m_impl->out_max = 1.0f;
    }

    // Validate derivative filter cutoff
    if (!isValidCutoff(m_impl->derivative_cutoff_hz)) {
        ESP_LOGW(TAG, "Invalid derivative cutoff (%.3f Hz), disabling the filter",
                 m_impl->derivative_cutoff_hz);
        m_impl->derivative_cutoff_hz = 0.0f;
    }

    // Warn on negative gains (but still store them clamped to 0)
    if (config.kp < 0.0f || config.ki < 0.0f || config.kd < 0.0f) {
        ESP_LOGW(TAG, "Negative gains clamped to 0 (kp=%.4f ki=%.4f kd=%.4f)",
                 config.kp, config.ki, config.kd);
        m_impl->kp = std::max(0.0f, m_impl->kp);
        m_impl->ki = std::max(0.0f, m_impl->ki);
        m_impl->kd = std::max(0.0f, m_impl->kd);
    }

    ESP_LOGI(TAG, "PID created: kp=%.4f ki=%.4f kd=%.4f d_cutoff=%.1f Hz dir=%s",
             m_impl->kp, m_impl->ki, m_impl->kd, m_impl->derivative_cutoff_hz,
             m_impl->direction == Direction::Direct ? "direct" : "reverse");
}

PidController::~PidController()
{
    delete m_impl;
    m_impl = nullptr;
}

// ---------------------------------------------------------------------------
// Copy semantics (deep copy)
// ---------------------------------------------------------------------------

PidController::PidController(const PidController &other)
    : m_impl(nullptr)
{
    if (other.m_impl) {
        m_impl = new (std::nothrow) detail::PidImpl(*other.m_impl);
        if (!m_impl) {
            ESP_LOGE(TAG, "Failed to allocate PidImpl during copy");
        }
    }
}

PidController &PidController::operator=(const PidController &other)
{
    if (this != &other) {
        delete m_impl;
        m_impl = nullptr;
        if (other.m_impl) {
            m_impl = new (std::nothrow) detail::PidImpl(*other.m_impl);
            if (!m_impl) {
                ESP_LOGE(TAG, "Failed to allocate PidImpl during copy assignment");
            }
        }
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

PidController::PidController(PidController &&other) noexcept
    : m_impl(std::exchange(other.m_impl, nullptr))
{
}

PidController &PidController::operator=(PidController &&other) noexcept
{
    if (this != &other) {
        delete m_impl;
        m_impl = std::exchange(other.m_impl, nullptr);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Core computation
// ---------------------------------------------------------------------------

float PidController::compute(float input, float setpoint, float dt_s, float feedforward)
{
    if (!m_impl || m_impl->mode == Mode::Manual) {
        return 0.0f;
    }
    if (!(dt_s > 0.0f) || !std::isfinite(dt_s)) {
        return m_impl->last_output;
    }

    const float dt = dt_s;
    float error = setpoint - input;
    float sign = (m_impl->direction == Direction::Reverse) ? -1.0f : 1.0f;

    // Proportional term
    float p_term = sign * m_impl->kp * error;

    // Integral term (accumulate)
    m_impl->integral_sum += sign * m_impl->ki * error * dt;
    m_impl->integral_sum = std::clamp(m_impl->integral_sum, m_impl->out_min, m_impl->out_max);

    // Derivative term
    float d_term = 0.0f;
    if (!m_impl->first_compute) {
        float d_raw;
        if (m_impl->derivative_mode == DerivativeMode::OnError) {
            d_raw = error - m_impl->last_error;
        } else {
            // Derivative on measurement: negate to get correct sign
            d_raw = -(input - m_impl->last_input);
        }

        // Filter the derivative rate (not the per-sample delta), so the
        // filter state stays meaningful when dt changes between calls.
        const float d_rate = d_raw / dt;
        const float alpha = lowPassAlpha(m_impl->derivative_cutoff_hz, dt);
        m_impl->d_filtered = alpha * d_rate + (1.0f - alpha) * m_impl->d_filtered;

        d_term = sign * m_impl->kd * m_impl->d_filtered;
    } else {
        m_impl->first_compute = false;
    }

    // Compute and clamp output
    float output = p_term + m_impl->integral_sum + d_term + feedforward;
    output = std::clamp(output, m_impl->out_min, m_impl->out_max);

    // Store state for next iteration
    m_impl->last_input = input;
    m_impl->last_error = error;
    m_impl->last_output = output;

    return output;
}

// ---------------------------------------------------------------------------
// Configuration setters
// ---------------------------------------------------------------------------

void PidController::setMode(Mode mode)
{
    if (!m_impl) {
        return;
    }

    if (mode == Mode::Automatic && m_impl->mode == Mode::Manual) {
        // Bumpless transfer: reset state when entering automatic mode
        reset();
    }

    m_impl->mode = mode;
    ESP_LOGI(TAG, "PID mode changed to %s",
             mode == Mode::Automatic ? "automatic" : "manual");
}

void PidController::setTunings(float kp, float ki, float kd)
{
    if (!m_impl) {
        return;
    }

    if (kp < 0.0f || ki < 0.0f || kd < 0.0f) {
        ESP_LOGW(TAG, "setTunings rejected: negative gains not allowed (kp=%.4f ki=%.4f kd=%.4f)",
                 kp, ki, kd);
        return;
    }

    m_impl->kp = kp;
    m_impl->ki = ki;
    m_impl->kd = kd;

    ESP_LOGI(TAG, "PID tunings updated: kp=%.4f ki=%.4f kd=%.4f", kp, ki, kd);
}

void PidController::setOutputLimits(float min, float max)
{
    if (!m_impl) {
        return;
    }

    if (min >= max) {
        ESP_LOGW(TAG, "setOutputLimits rejected: min (%.4f) must be less than max (%.4f)",
                 min, max);
        return;
    }

    m_impl->out_min = min;
    m_impl->out_max = max;

    // Clamp current state to new limits
    m_impl->integral_sum = std::clamp(m_impl->integral_sum, min, max);
    m_impl->last_output = std::clamp(m_impl->last_output, min, max);

    ESP_LOGI(TAG, "PID output limits updated: [%.4f, %.4f]", min, max);
}

void PidController::setDirection(Direction dir)
{
    if (!m_impl) {
        return;
    }

    m_impl->direction = dir;
    ESP_LOGI(TAG, "PID direction changed to %s",
             dir == Direction::Direct ? "direct" : "reverse");
}

void PidController::setDerivativeCutoff(float hz)
{
    if (!m_impl) {
        return;
    }

    if (!isValidCutoff(hz)) {
        ESP_LOGW(TAG, "setDerivativeCutoff rejected: cutoff must be finite and >= 0 (got %.3f)", hz);
        return;
    }

    m_impl->derivative_cutoff_hz = hz;
    ESP_LOGI(TAG, "PID derivative cutoff updated: %.1f Hz", hz);
}

float PidController::getDerivativeCutoff() const
{
    return m_impl ? m_impl->derivative_cutoff_hz : 0.0f;
}

void PidController::reset()
{
    if (!m_impl) {
        return;
    }

    m_impl->integral_sum = 0.0f;
    m_impl->last_input = 0.0f;
    m_impl->last_error = 0.0f;
    m_impl->last_output = 0.0f;
    m_impl->d_filtered = 0.0f;
    m_impl->first_compute = true;

    ESP_LOGI(TAG, "PID state reset");
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

float PidController::getKp() const
{
    return m_impl ? m_impl->kp : 0.0f;
}

float PidController::getKi() const
{
    return m_impl ? m_impl->ki : 0.0f;
}

float PidController::getKd() const
{
    return m_impl ? m_impl->kd : 0.0f;
}

Mode PidController::getMode() const
{
    return m_impl ? m_impl->mode : Mode::Manual;
}

Direction PidController::getDirection() const
{
    return m_impl ? m_impl->direction : Direction::Direct;
}

float PidController::getIntegralTerm() const
{
    return m_impl ? m_impl->integral_sum : 0.0f;
}

float PidController::getLastError() const
{
    return m_impl ? m_impl->last_error : 0.0f;
}

float PidController::getLastOutput() const
{
    return m_impl ? m_impl->last_output : 0.0f;
}

bool PidController::isValid() const
{
    return m_impl != nullptr;
}

} // namespace esp_pid
