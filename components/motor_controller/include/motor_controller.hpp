/**
 * @file   motor_controller.hpp
 * @brief  Public API declarations for motor_controller.
 *
 * Component header documentation.
 *
 * @author Project Contributors
 * @date   2026-04-07
 */
#pragma once

#include "esp_motor.hpp"
#include "encoder.hpp"
#include "pid_controller.hpp"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include <atomic>

namespace motor_controller {

/// Motor control mode.
enum class ControlMode {
    OpenLoop,        ///< Direct duty-cycle control; no encoder feedback.
    VelocityControl, ///< Closed-loop angular-velocity control via PID.
    PositionControl, ///< Closed-loop angular-position control via PID.
};

/// Full configuration for a MotorController instance.
///
/// Both @p velocity_pid and @p position_pid embed an esp_pid::PidConfig which
/// carries its own sample_time_ms. The convenience setter
/// MotorController::setSampleTime() updates both simultaneously.
struct MotorControllerConfig {
    /// Total encoder counts per one full revolution of the measured shaft.
    /// For a 4x-decoded quadrature encoder with N pulses/rev use N * 4.
    float counts_per_rev = CONFIG_MOTOR_CTRL_DEFAULT_COUNTS_PER_REV;

    /// Gear ratio: motor (encoder) shaft revolutions per output shaft revolution.
    /// Set to 1.0 for direct drive. Set to 50.0 for a 50:1 reduction gearbox
    /// where the encoder is mounted on the motor shaft.
    float gear_ratio = 1.0f;

    /// Motor output lower clamp applied to the PID output (and open-loop duty).
    /// Must be in [-1.0, 1.0] and less than output_max.
    float output_min = -1.0f;

    /// Motor output upper clamp applied to the PID output (and open-loop duty).
    /// Must be in [-1.0, 1.0] and greater than output_min.
    float output_max = 1.0f;

    /// Velocity PID parameters. output_min/output_max are overridden by the
    /// MotorController constructor to match the above limits.
    esp_pid::PidConfig velocity_pid;

    /// Position PID parameters. output_min/output_max are overridden by the
    /// MotorController constructor to match the above limits.
    esp_pid::PidConfig position_pid;

    /// Constant feedforward term added to velocity PID output.
    float velocity_feedforward = 0.0f;

    /// Constant feedforward term added to position PID output.
    float position_feedforward = 0.0f;
};

/// Single-motor closed-loop and open-loop controller.
///
/// Combines a Motor HAL instance, a Rotary Encoder HAL instance, and two
/// PID controllers (velocity and position) in one object.
///
/// Usage:
/// @code
///   motor_controller::MotorControllerConfig cfg;
///   cfg.counts_per_rev = 1440.0f;   // 360 CPR encoder, 4x decoded
///   cfg.gear_ratio     = 1.0f;      // direct drive
///   cfg.velocity_pid.kp = 0.5f;
///   cfg.velocity_pid.ki = 0.1f;
///   cfg.velocity_pid.kd = 0.01f;
///   cfg.velocity_pid.sample_time_ms = 10;
///
///   motor_controller::MotorController mc(
///       GPIO_NUM_1, GPIO_NUM_2,   // motor: pwm_gpio, dir_gpio
///       GPIO_NUM_4, GPIO_NUM_5,   // encoder: channel A, channel B
///       cfg);
///
///   if (!mc.isValid()) { /* handle init failure */ }
///
///   mc.setMode(motor_controller::ControlMode::VelocityControl);
///   mc.setVelocitySetpoint(10.0f);  // 10 rad/s
///
///   // Called periodically from a FreeRTOS task at cfg.velocity_pid.sample_time_ms:
///   mc.update();
/// @endcode
///
/// Thread safety:
///   - setMode() and setOutputLimits() synchronize mode/limits with update()
///     through an internal spinlock.
///   - setOpenLoopDuty(), setVelocitySetpoint(), setPositionSetpoint(),
///     setVelocityFeedforward(), setPositionFeedforward(), setCountsPerRev(),
///     setGearRatio(), getAngle(), and getAngularVelocity() use lock-free atomic
///     loads/stores for single-value state shared with update().
///   - PID tuning/reset setters (setVelocityPidTunings, setPositionPidTunings,
///     setSampleTime, reset) mutate PID controller internals which are not
///     individually thread-safe; call them from the same task as update(), or
///     while the update loop is suspended.
///   - stop() is idempotent but should still be coordinated with the update task
///     for deterministic command sequencing.
///   - update() is not re-entrant; call it from a single task only.
class MotorController {
public:
    /// Construct a MotorController.
    /// @param pwm_gpio  PWM output GPIO for the motor driver.
    /// @param dir_gpio  Direction output GPIO for the motor driver.
    /// @param enc_a     Encoder channel A GPIO.
    /// @param enc_b     Encoder channel B GPIO.
    /// @param config    Configuration parameters (gains, counts/rev, etc.).
    MotorController(gpio_num_t pwm_gpio, gpio_num_t dir_gpio,
                    gpio_num_t enc_a,    gpio_num_t enc_b,
                    const MotorControllerConfig &config = MotorControllerConfig{});

    ~MotorController() = default;

    MotorController(const MotorController &) = delete;
    MotorController &operator=(const MotorController &) = delete;

    MotorController(MotorController &&) noexcept = default;
    MotorController &operator=(MotorController &&) noexcept = default;

    // -------------------------------------------------------------------------
    // Main control loop
    // -------------------------------------------------------------------------

    /// Execute one control cycle.
    ///
    /// Reads encoder feedback, runs the active PID controller (if applicable),
    /// and drives the motor. Must be called at the configured sample_time_ms
    /// interval. Not re-entrant; call from a single FreeRTOS task.
    void update();

    // -------------------------------------------------------------------------
    // Mode control
    // -------------------------------------------------------------------------

    /// Switch the active control mode.
    ///
    /// PID state is reset when entering any closed-loop mode to prevent
    /// integral windup and output bumps. The motor is stopped momentarily
    /// on every mode transition for safety.
    /// Thread-safe with respect to update().
    /// @param mode New control mode.
    void setMode(ControlMode mode);

    /// @return Currently active control mode.
    ControlMode getMode() const;

    // -------------------------------------------------------------------------
    // Setpoint setters (thread-safe with update())
    // -------------------------------------------------------------------------

    /// Set open-loop motor duty cycle (used only in ControlMode::OpenLoop).
    /// @param duty  Duty cycle in [-1.0, 1.0]; clamped if out of range.
    void setOpenLoopDuty(float duty);

    /// Set the target angular velocity for ControlMode::VelocityControl.
    /// @param rad_per_sec  Target angular velocity in radians/second.
    void setVelocitySetpoint(float rad_per_sec);

    /// Set the target angular position for ControlMode::PositionControl.
    /// @param radians  Target angular position in radians.
    void setPositionSetpoint(float radians);

    /// Set a constant feedforward value for velocity control.
    /// @param feedforward Added to velocity PID output every update.
    void setVelocityFeedforward(float feedforward);

    /// Set a constant feedforward value for position control.
    /// @param feedforward Added to position PID output every update.
    void setPositionFeedforward(float feedforward);

    // -------------------------------------------------------------------------
    // Feedback (thread-safe; reads are atomic or spinlock-protected)
    // -------------------------------------------------------------------------

    /// @return Current angular position of the output shaft in radians.
    ///         Derived from the accumulated encoder count.
    float getAngle() const;

    /// @return Current angular velocity of the output shaft in radians/second.
    ///         Derived from the encoder rate (counts/s) sampled by the encoder HAL.
    float getAngularVelocity() const;

    // -------------------------------------------------------------------------
    // Runtime configuration setters
    // -------------------------------------------------------------------------

    /// Update velocity PID gains.
    /// @param kp  Proportional gain (>= 0).
    /// @param ki  Integral gain (>= 0).
    /// @param kd  Derivative gain (>= 0).
    void setVelocityPidTunings(float kp, float ki, float kd);

    /// Update position PID gains.
    /// @param kp  Proportional gain (>= 0).
    /// @param ki  Integral gain (>= 0).
    /// @param kd  Derivative gain (>= 0).
    void setPositionPidTunings(float kp, float ki, float kd);

    /// Set the PID sample time for both velocity and position controllers.
    /// @param ms  Sample period in milliseconds (> 0).
    void setSampleTime(unsigned int ms);

    /// Update total encoder counts per output shaft revolution.
    /// @param counts  Counts per revolution (> 0).
    void setCountsPerRev(float counts);

    /// Update gear ratio (motor shaft revolutions per output shaft revolution).
    /// @param ratio  Gear ratio (> 0); 1.0 = direct drive.
    void setGearRatio(float ratio);

    /// Update motor output clamp limits for all modes.
    /// @param min  Lower bound (must be < max and in [-1.0, 1.0]).
    /// @param max  Upper bound (must be > min and in [-1.0, 1.0]).
    void setOutputLimits(float min, float max);

    // -------------------------------------------------------------------------
    // Status
    // -------------------------------------------------------------------------

    /// @return true if both motor and encoder hardware initialised successfully.
    bool isValid() const;

    /// Stop motor output immediately (idempotent).
    /// Leaves controller mode unchanged.
    void stop();

    /// Reset controller and feedback state (idempotent).
    /// Resets both PIDs, clears encoder count/rate baseline, zeros cached feedback,
    /// and zeroes setpoints/open-loop duty.
    void reset();

private:
    float countsToRadians(int32_t counts, float counts_per_rev, float gear_ratio) const;
    float rateToAngularVelocity(float counts_per_sec, float counts_per_rev, float gear_ratio) const;

    // Hardware handles
    esp_motor::Motor            m_motor;
    esp_rotary_encoder::Encoder m_encoder;

    // Controllers (velocity and position)
    esp_pid::PidController m_vel_pid;
    esp_pid::PidController m_pos_pid;

    // Active control mode (protected by m_mux)
    ControlMode m_mode;

    // Setpoints/feedforward (atomic; lock-free shared with update())
    std::atomic<float> m_open_loop_duty;
    std::atomic<float> m_vel_setpoint;
    std::atomic<float> m_pos_setpoint;
    std::atomic<float> m_vel_feedforward;
    std::atomic<float> m_pos_feedforward;

    // Per-instance conversion config (atomic; updated by runtime setters)
    std::atomic<float> m_counts_per_rev;
    std::atomic<float> m_gear_ratio;
    float        m_output_min;
    float        m_output_max;

    // Cached feedback state
    std::atomic<float> m_cached_angle_rad;
    std::atomic<float> m_cached_velocity_rad_s;
    std::atomic<bool>  m_cached_angle_valid;
    std::atomic<bool>  m_cached_velocity_valid;

    // Spinlock protecting mode and output limits during concurrent access.
    // Declared mutable so that const getters can acquire it without const_cast.
    mutable portMUX_TYPE m_mux;
};

} // namespace motor_controller
