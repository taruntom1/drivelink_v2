#include "motor_controller.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cmath>

static const char *TAG = "motor_controller";

namespace motor_controller {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MotorController::MotorController(gpio_num_t pwm_gpio, gpio_num_t dir_gpio,
                                 gpio_num_t enc_a,    gpio_num_t enc_b,
                                 const MotorControllerConfig &config)
    : m_motor(pwm_gpio, dir_gpio),
      m_encoder(enc_a, enc_b),
      m_vel_pid([&]() {
          esp_pid::PidConfig vc = config.velocity_pid;
          vc.output_min = config.output_min;
          vc.output_max = config.output_max;
          return vc;
      }()),
      m_pos_pid([&]() {
          esp_pid::PidConfig pc = config.position_pid;
          pc.output_min = config.output_min;
          pc.output_max = config.output_max;
          return pc;
      }()),
      m_mode(ControlMode::OpenLoop),
      m_open_loop_duty(0.0f),
      m_vel_setpoint(0.0f),
      m_pos_setpoint(0.0f),
      m_counts_per_rev(config.counts_per_rev > 0.0f ? config.counts_per_rev : 1.0f),
      m_gear_ratio(config.gear_ratio > 0.0f ? config.gear_ratio : 1.0f),
      m_output_min(std::clamp(config.output_min, -1.0f, 1.0f)),
      m_output_max(std::clamp(config.output_max, -1.0f, 1.0f))
{
    m_mux = portMUX_INITIALIZER_UNLOCKED;
    if (m_output_min >= m_output_max) {
        ESP_LOGW(TAG, "Invalid output limits (min=%.3f >= max=%.3f); clamping to [-1, 1]",
                 m_output_min, m_output_max);
        m_output_min = -1.0f;
        m_output_max =  1.0f;
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
                 "cpr=%.1f gear=%.3f out=[%.2f,%.2f]",
                 pwm_gpio, dir_gpio, enc_a, enc_b,
                 m_counts_per_rev, m_gear_ratio,
                 m_output_min, m_output_max);
    }
}

// ---------------------------------------------------------------------------
// Main update loop
// ---------------------------------------------------------------------------

void MotorController::update()
{
    if (!isValid()) {
        return;
    }

    // Atomically snapshot mode, setpoints, and per-instance scaling so that
    // concurrent setters from other tasks see a consistent view.
    ControlMode mode;
    float open_duty, vel_sp, pos_sp;
    float counts_per_rev, gear_ratio, out_min, out_max;

    taskENTER_CRITICAL(&m_mux);
    mode          = m_mode;
    open_duty     = m_open_loop_duty;
    vel_sp        = m_vel_setpoint;
    pos_sp        = m_pos_setpoint;
    counts_per_rev = m_counts_per_rev;
    gear_ratio    = m_gear_ratio;
    out_min       = m_output_min;
    out_max       = m_output_max;
    taskEXIT_CRITICAL(&m_mux);

    const float two_pi_over_cpr_gear =
        (2.0f * static_cast<float>(M_PI)) / (counts_per_rev * gear_ratio);

    float output = 0.0f;

    switch (mode) {
        case ControlMode::OpenLoop:
            output = std::clamp(open_duty, out_min, out_max);
            break;

        case ControlMode::VelocityControl: {
            float omega = m_encoder.getRate() * two_pi_over_cpr_gear;
            output = m_vel_pid.compute(omega, vel_sp);
            ESP_LOGD(TAG, "VelCtrl: sp=%.3f meas=%.3f out=%.3f", vel_sp, omega, output);
            break;
        }

        case ControlMode::PositionControl: {
            float angle = static_cast<float>(m_encoder.getCount()) * two_pi_over_cpr_gear;
            output = m_pos_pid.compute(angle, pos_sp);
            ESP_LOGD(TAG, "PosCtrl: sp=%.3f meas=%.3f out=%.3f", pos_sp, angle, output);
            break;
        }
    }

    m_motor.setSpeed(output);
}

// ---------------------------------------------------------------------------
// Mode control
// ---------------------------------------------------------------------------

void MotorController::setMode(ControlMode mode)
{
    // Stop motor first for safety, then switch mode.
    m_motor.stop();

    // Reset PID state when entering a closed-loop mode to prevent
    // integral windup accumulated while in a different mode.
    if (mode == ControlMode::VelocityControl) {
        m_vel_pid.reset();
        m_vel_pid.setMode(esp_pid::Mode::Automatic);
        m_pos_pid.setMode(esp_pid::Mode::Manual);
    } else if (mode == ControlMode::PositionControl) {
        m_pos_pid.reset();
        m_pos_pid.setMode(esp_pid::Mode::Automatic);
        m_vel_pid.setMode(esp_pid::Mode::Manual);
    } else {
        // OpenLoop
        m_vel_pid.setMode(esp_pid::Mode::Manual);
        m_pos_pid.setMode(esp_pid::Mode::Manual);
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

void MotorController::setOpenLoopDuty(float duty)
{
    duty = std::clamp(duty, -1.0f, 1.0f);
    taskENTER_CRITICAL(&m_mux);
    m_open_loop_duty = duty;
    taskEXIT_CRITICAL(&m_mux);
}

void MotorController::setVelocitySetpoint(float rad_per_sec)
{
    taskENTER_CRITICAL(&m_mux);
    m_vel_setpoint = rad_per_sec;
    taskEXIT_CRITICAL(&m_mux);
}

void MotorController::setPositionSetpoint(float radians)
{
    taskENTER_CRITICAL(&m_mux);
    m_pos_setpoint = radians;
    taskEXIT_CRITICAL(&m_mux);
}

// ---------------------------------------------------------------------------
// Feedback
// ---------------------------------------------------------------------------

float MotorController::getAngle() const
{
    if (!m_encoder.isValid()) {
        return 0.0f;
    }
    taskENTER_CRITICAL(&m_mux);
    const float cpr = m_counts_per_rev;
    const float gr  = m_gear_ratio;
    taskEXIT_CRITICAL(&m_mux);
    return static_cast<float>(m_encoder.getCount())
           * (2.0f * static_cast<float>(M_PI)) / (cpr * gr);
}

float MotorController::getAngularVelocity() const
{
    if (!m_encoder.isValid()) {
        return 0.0f;
    }
    taskENTER_CRITICAL(&m_mux);
    const float cpr = m_counts_per_rev;
    const float gr  = m_gear_ratio;
    taskEXIT_CRITICAL(&m_mux);
    return m_encoder.getRate()
           * (2.0f * static_cast<float>(M_PI)) / (cpr * gr);
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

void MotorController::setSampleTime(unsigned int ms)
{
    m_vel_pid.setSampleTime(ms);
    m_pos_pid.setSampleTime(ms);
}

void MotorController::setCountsPerRev(float counts)
{
    if (counts <= 0.0f) {
        ESP_LOGW(TAG, "setCountsPerRev: value must be > 0 (got %.2f); ignoring", counts);
        return;
    }
    taskENTER_CRITICAL(&m_mux);
    m_counts_per_rev = counts;
    taskEXIT_CRITICAL(&m_mux);
    ESP_LOGI(TAG, "counts_per_rev updated to %.1f", counts);
}

void MotorController::setGearRatio(float ratio)
{
    if (ratio <= 0.0f) {
        ESP_LOGW(TAG, "setGearRatio: value must be > 0 (got %.4f); ignoring", ratio);
        return;
    }
    taskENTER_CRITICAL(&m_mux);
    m_gear_ratio = ratio;
    taskEXIT_CRITICAL(&m_mux);
    ESP_LOGI(TAG, "gear_ratio updated to %.4f", ratio);
}

void MotorController::setOutputLimits(float min, float max)
{
    if (min >= max) {
        ESP_LOGW(TAG, "setOutputLimits: min (%.3f) must be < max (%.3f); ignoring", min, max);
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

} // namespace motor_controller
