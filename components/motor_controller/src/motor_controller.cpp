#include "motor_controller.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cmath>

static const char *const TAG = "motor_controller";

namespace motor_controller {

namespace {

bool isValidCutoff(float hz)
{
    return std::isfinite(hz) && hz >= 0.0f;
}

bool isPositiveFinite(float v)
{
    return std::isfinite(v) && v > 0.0f;
}

float finiteOr(float v, float fallback)
{
    return std::isfinite(v) ? v : fallback;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MotorController::MotorController(gpio_num_t pwm_gpio, gpio_num_t dir_gpio,
                                 gpio_num_t enc_a,    gpio_num_t enc_b,
                                 const MotorControllerConfig &config)
    : m_motor(pwm_gpio, dir_gpio),
      m_encoder(enc_a, enc_b),
      m_vel_pid([&]() {
          pid::PidConfig vc = config.velocity_pid;
          vc.output_min = config.output_min;
          vc.output_max = config.output_max;
          return vc;
      }()),
      m_pos_pid([&]() {
          pid::PidConfig pc = config.position_pid;
          pc.output_min = config.output_min;
          pc.output_max = config.output_max;
          return pc;
      }()),
      m_mode(ControlMode::OpenLoop),
      m_open_loop_duty(0.0f),
      m_vel_setpoint(0.0f),
      m_pos_setpoint(0.0f),
      m_vel_feedforward(finiteOr(config.velocity_feedforward, 0.0f)),
      m_pos_feedforward(finiteOr(config.position_feedforward, 0.0f)),
      m_counts_per_rev(isPositiveFinite(config.counts_per_rev) ? config.counts_per_rev : 1.0f),
      m_gear_ratio(isPositiveFinite(config.gear_ratio) ? config.gear_ratio : 1.0f),
      m_output_min(std::clamp(finiteOr(config.output_min, -1.0f), -1.0f, 1.0f)),
      m_output_max(std::clamp(finiteOr(config.output_max, 1.0f), -1.0f, 1.0f)),
      m_vel_filter_cutoff_hz(isValidCutoff(config.velocity_filter_cutoff_hz)
                                 ? config.velocity_filter_cutoff_hz
                                 : 0.0f),
      m_estimator_reset_requested(false),
      m_vel_estimator(),
      m_cached_angle_rad(0.0f),
      m_cached_velocity_rad_s(0.0f),
      m_cached_output(0.0f),
      m_cached_valid(false)
{
    m_mux = portMUX_INITIALIZER_UNLOCKED;
    if (m_output_min >= m_output_max) {
        ESP_LOGW(TAG, "Invalid output limits (min=%.3f >= max=%.3f); clamping to [-1, 1]",
                 m_output_min, m_output_max);
        m_output_min = -1.0f;
        m_output_max =  1.0f;
    }
    if (!isValidCutoff(config.velocity_filter_cutoff_hz)) {
        ESP_LOGW(TAG, "Invalid velocity filter cutoff (%.3f Hz); disabling the filter",
                 config.velocity_filter_cutoff_hz);
    }

    if (!m_motor.isValid()) {
        ESP_LOGE(TAG, "Motor hardware init failed (pwm_gpio=%d dir_gpio=%d)",
                 pwm_gpio, dir_gpio);
    }
    if (!m_encoder.isValid()) {
        ESP_LOGE(TAG, "Encoder hardware init failed (enc_a=%d enc_b=%d)",
                 enc_a, enc_b);
    }

    if (isValid()) {
        ESP_LOGI(TAG,
                 "MotorController ready: pwm=%d dir=%d encA=%d encB=%d "
                 "cpr=%.1f gear=%.3f out=[%.2f,%.2f] vel_cutoff=%.1f Hz",
                 pwm_gpio, dir_gpio, enc_a, enc_b,
                 m_counts_per_rev.load(), m_gear_ratio.load(),
                 m_output_min, m_output_max, m_vel_filter_cutoff_hz.load());
    }
}

// ---------------------------------------------------------------------------
// Main update loop
// ---------------------------------------------------------------------------

float MotorController::estimateVelocity(int32_t count, float dt_s,
                                        float counts_per_rev, float gear_ratio)
{
    if (m_estimator_reset_requested.exchange(false, std::memory_order_acq_rel)) {
        m_vel_estimator.reset();
    }
    return m_vel_estimator.update(count, dt_s, counts_per_rev, gear_ratio,
                                  m_vel_filter_cutoff_hz.load(std::memory_order_relaxed));
}

void MotorController::update(float dt_s)
{
    if (!isValid()) {
        return;
    }

    // Snapshot mode/limits and single-value state so concurrent setters from
    // other tasks see valid values without lengthy critical sections.
    ControlMode mode;
    float out_min, out_max;

    taskENTER_CRITICAL(&m_mux);
    mode    = m_mode;
    out_min = m_output_min;
    out_max = m_output_max;
    taskEXIT_CRITICAL(&m_mux);
    const float open_duty      = m_open_loop_duty.load();
    const float vel_sp         = m_vel_setpoint.load();
    const float pos_sp         = m_pos_setpoint.load();
    const float vel_ff         = m_vel_feedforward.load();
    const float pos_ff         = m_pos_feedforward.load();
    const float counts_per_rev = m_counts_per_rev.load();
    const float gear_ratio     = m_gear_ratio.load();

    // Read the encoder once per cycle; angle and velocity come from the same sample.
    const int32_t count = m_encoder.getCount();
    const float angle = countsToRadians(count, counts_per_rev, gear_ratio);
    const float omega = estimateVelocity(count, dt_s, counts_per_rev, gear_ratio);

    float output = 0.0f;
    switch (mode) {
        case ControlMode::OpenLoop:
            output = std::clamp(open_duty, out_min, out_max);
            break;

        case ControlMode::VelocityControl:
            output = m_vel_pid.compute(omega, vel_sp, dt_s, vel_ff);
            break;

        case ControlMode::PositionControl:
            output = m_pos_pid.compute(angle, pos_sp, dt_s, pos_ff);
            break;
    }

    m_motor.setSpeed(output);

    m_cached_angle_rad.store(angle, std::memory_order_relaxed);
    m_cached_velocity_rad_s.store(omega, std::memory_order_relaxed);
    m_cached_output.store(output, std::memory_order_relaxed);
    m_cached_valid.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Mode control
// ---------------------------------------------------------------------------

void MotorController::setMode(ControlMode mode)
{
    // Stop motor first for safety, then switch mode.
    m_motor.stop();
    m_cached_output.store(0.0f, std::memory_order_relaxed);

    const float cpr = m_counts_per_rev.load();
    const float gr = m_gear_ratio.load();
    const float current_angle = countsToRadians(m_encoder.getCount(), cpr, gr);

    // Reset PID state when entering a closed-loop mode to prevent
    // integral windup accumulated while in a different mode.
    if (mode == ControlMode::VelocityControl) {
        m_vel_pid.reset();
        m_vel_pid.setMode(pid::Mode::Automatic);
        m_pos_pid.setMode(pid::Mode::Manual);
    } else if (mode == ControlMode::PositionControl) {
        m_pos_pid.reset();
        m_pos_pid.setMode(pid::Mode::Automatic);
        m_vel_pid.setMode(pid::Mode::Manual);
        m_pos_setpoint.store(current_angle);
    } else {
        // OpenLoop
        m_vel_pid.setMode(pid::Mode::Manual);
        m_pos_pid.setMode(pid::Mode::Manual);
    }

    taskENTER_CRITICAL(&m_mux);
    m_mode = mode;
    taskEXIT_CRITICAL(&m_mux);

    ESP_LOGI(TAG, "Control mode -> %s",
             mode == ControlMode::OpenLoop        ? "OpenLoop"        :
             mode == ControlMode::VelocityControl ? "VelocityControl" :
                                                    "PositionControl");
}

ControlMode MotorController::getMode() const
{
    ControlMode mode;
    taskENTER_CRITICAL(&m_mux);
    mode = m_mode;
    taskEXIT_CRITICAL(&m_mux);
    return mode;
}

// ---------------------------------------------------------------------------
// Setpoint setters
// ---------------------------------------------------------------------------

// Non-finite values are ignored: NaN would pass every clamp and reach the PWM.
void MotorController::setOpenLoopDuty(float duty)
{
    if (!std::isfinite(duty)) {
        return;
    }
    duty = std::clamp(duty, -1.0f, 1.0f);
    m_open_loop_duty.store(duty);
}

void MotorController::setVelocitySetpoint(float rad_per_sec)
{
    if (std::isfinite(rad_per_sec)) {
        m_vel_setpoint.store(rad_per_sec);
    }
}

void MotorController::setPositionSetpoint(float radians)
{
    if (std::isfinite(radians)) {
        m_pos_setpoint.store(radians);
    }
}

void MotorController::setVelocityFeedforward(float feedforward)
{
    if (std::isfinite(feedforward)) {
        m_vel_feedforward.store(feedforward);
    }
}

void MotorController::setPositionFeedforward(float feedforward)
{
    if (std::isfinite(feedforward)) {
        m_pos_feedforward.store(feedforward);
    }
}

// ---------------------------------------------------------------------------
// Feedback
// ---------------------------------------------------------------------------

float MotorController::getAngle() const
{
    if (!m_encoder.isValid()) {
        return 0.0f;
    }
    if (m_cached_valid.load(std::memory_order_acquire)) {
        return m_cached_angle_rad.load(std::memory_order_relaxed);
    }
    return countsToRadians(m_encoder.getCount(), m_counts_per_rev.load(), m_gear_ratio.load());
}

float MotorController::getAngularVelocity() const
{
    if (!m_encoder.isValid() || !m_cached_valid.load(std::memory_order_acquire)) {
        return 0.0f;
    }
    return m_cached_velocity_rad_s.load(std::memory_order_relaxed);
}

float MotorController::getOutput() const
{
    return m_cached_output.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Runtime configuration setters
// ---------------------------------------------------------------------------

void MotorController::setVelocityPidTunings(float kp, float ki, float kd)
{
    m_vel_pid.setTunings(kp, ki, kd);
}

void MotorController::setPositionPidTunings(float kp, float ki, float kd)
{
    m_pos_pid.setTunings(kp, ki, kd);
}

void MotorController::setVelocityDerivativeCutoff(float hz)
{
    m_vel_pid.setDerivativeCutoff(hz);
}

void MotorController::setPositionDerivativeCutoff(float hz)
{
    m_pos_pid.setDerivativeCutoff(hz);
}

void MotorController::setVelocityFilterCutoff(float hz)
{
    if (!isValidCutoff(hz)) {
        ESP_LOGW(TAG, "setVelocityFilterCutoff: cutoff must be finite and >= 0 (got %.3f); ignoring", hz);
        return;
    }
    m_vel_filter_cutoff_hz.store(hz);
    ESP_LOGI(TAG, "velocity filter cutoff updated to %.1f Hz", hz);
}

void MotorController::setCountsPerRev(float counts)
{
    if (!isPositiveFinite(counts)) {
        ESP_LOGW(TAG, "setCountsPerRev: value must be finite and > 0 (got %.2f); ignoring", counts);
        return;
    }
    m_counts_per_rev.store(counts);
    ESP_LOGI(TAG, "counts_per_rev updated to %.1f", counts);
}

void MotorController::setGearRatio(float ratio)
{
    if (!isPositiveFinite(ratio)) {
        ESP_LOGW(TAG, "setGearRatio: value must be finite and > 0 (got %.4f); ignoring", ratio);
        return;
    }
    m_gear_ratio.store(ratio);
    ESP_LOGI(TAG, "gear_ratio updated to %.4f", ratio);
}

void MotorController::setOutputLimits(float min, float max)
{
    if (!std::isfinite(min) || !std::isfinite(max) || min >= max) {
        ESP_LOGW(TAG, "setOutputLimits: limits must be finite and min (%.3f) < max (%.3f); ignoring", min, max);
        return;
    }
    min = std::clamp(min, -1.0f, 1.0f);
    max = std::clamp(max, -1.0f, 1.0f);
    if (min >= max) {
        ESP_LOGW(TAG, "setOutputLimits: after clamping to [-1,1] limits are equal (%.3f); ignoring", min);
        return;
    }
    taskENTER_CRITICAL(&m_mux);
    m_output_min = min;
    m_output_max = max;
    taskEXIT_CRITICAL(&m_mux);
    m_vel_pid.setOutputLimits(min, max);
    m_pos_pid.setOutputLimits(min, max);
    ESP_LOGI(TAG, "Output limits updated to [%.3f, %.3f]", min, max);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

bool MotorController::isValid() const
{
    return m_motor.isValid() && m_encoder.isValid();
}

void MotorController::stop()
{
    m_motor.stop();
    m_open_loop_duty.store(0.0f);
    m_cached_output.store(0.0f, std::memory_order_relaxed);
}

void MotorController::reset()
{
    m_vel_pid.reset();
    m_pos_pid.reset();
    m_encoder.reset();
    m_motor.stop();

    m_open_loop_duty.store(0.0f);
    m_vel_setpoint.store(0.0f);
    m_pos_setpoint.store(0.0f);

    // The encoder count jumped to 0: the next update() must not difference
    // against the old count.
    m_estimator_reset_requested.store(true, std::memory_order_release);
    m_cached_valid.store(false, std::memory_order_release);
    m_cached_angle_rad.store(0.0f, std::memory_order_relaxed);
    m_cached_velocity_rad_s.store(0.0f, std::memory_order_relaxed);
    m_cached_output.store(0.0f, std::memory_order_relaxed);
}

} // namespace motor_controller
