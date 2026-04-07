/**
 * @file   pid_controller.hpp
 * @brief  Public API declarations for pid controller component.
 *
 * Component header documentation.
 *
 * @author Project Contributors
 * @date   2026-04-07
 */
#pragma once

#include "sdkconfig.h"
#include <cstdint>

namespace esp_pid {

/// PID operating mode.
enum class Mode {
    Manual,    ///< Output is not computed; compute() returns 0.
    Automatic  ///< Normal closed-loop operation.
};

/// Controller action direction.
enum class Direction {
    Direct,  ///< Positive error produces positive output.
    Reverse  ///< Positive error produces negative output.
};

/// Derivative term computation mode.
enum class DerivativeMode {
    OnMeasurement, ///< Derivative of measurement (avoids setpoint kick).
    OnError        ///< Derivative of error (responds to setpoint changes).
};

/// Configuration for PidController construction.
/// All fields have sensible defaults sourced from Kconfig where appropriate.
struct PidConfig {
    float kp = 0.0f;
    float ki = 0.0f;
    float kd = 0.0f;
    Direction direction = Direction::Direct;
    unsigned int sample_time_ms = CONFIG_PID_DEFAULT_SAMPLE_TIME_MS;
    float output_min = CONFIG_PID_DEFAULT_OUTPUT_MIN / 1000.0f;
    float output_max = CONFIG_PID_DEFAULT_OUTPUT_MAX / 1000.0f;
    DerivativeMode derivative_mode = DerivativeMode::OnMeasurement;
    float derivative_filter_alpha = CONFIG_PID_DEFAULT_DERIVATIVE_FILTER_ALPHA / 1000.0f;
};

namespace detail {
struct PidImpl;
} // namespace detail

/// PID controller for real-time control loops.
///
/// Pure algorithm component with no hardware dependencies. Designed to be
/// called at a fixed interval matching sample_time_ms.
///
/// Usage:
/// @code
///   esp_pid::PidConfig cfg;
///   cfg.kp = 1.0f;
///   cfg.ki = 0.5f;
///   cfg.kd = 0.1f;
///   esp_pid::PidController pid(cfg);
///   pid.setMode(esp_pid::Mode::Automatic);
///
///   // In control loop (called every sample_time_ms):
///   float output = pid.compute(measured_value, target_value);
/// @endcode
class PidController {
public:
    /// Construct a PID controller with the given configuration.
    /// @param config Controller parameters. Gains must be non-negative.
    explicit PidController(const PidConfig &config);

    ~PidController();

    /// Copy constructor (deep copy of controller state).
    PidController(const PidController &other);
    /// Copy assignment (deep copy of controller state).
    PidController &operator=(const PidController &other);

    /// Move constructor.
    PidController(PidController &&other) noexcept;
    /// Move assignment.
    PidController &operator=(PidController &&other) noexcept;

    /// Compute one PID iteration.
    /// @param input      Current process measurement.
    /// @param setpoint   Desired target value.
    /// @param feedforward Optional feedforward term added to output (default 0).
    /// @return Clamped control output, or 0.0f if in Manual mode.
    float compute(float input, float setpoint, float feedforward = 0.0f);

    /// Set operating mode.
    /// Transitioning from Manual to Automatic calls reset() internally
    /// to prevent integral bump.
    /// @param mode Mode::Manual or Mode::Automatic.
    void setMode(Mode mode);

    /// Set PID gains. Negative values are rejected (no change applied).
    /// @param kp Proportional gain (>= 0).
    /// @param ki Integral gain (>= 0).
    /// @param kd Derivative gain (>= 0).
    void setTunings(float kp, float ki, float kd);

    /// Set output limits. min must be less than max; otherwise no change.
    /// @param min Lower output clamp.
    /// @param max Upper output clamp.
    void setOutputLimits(float min, float max);

    /// Set controller direction.
    /// @param dir Direction::Direct or Direction::Reverse.
    void setDirection(Direction dir);

    /// Set computation sample time.
    /// @param ms Sample time in milliseconds (must be > 0).
    void setSampleTime(unsigned int ms);

    /// Reset internal state (integral accumulator, derivative history).
    /// Idempotent. Safe to call in any mode.
    void reset();

    /// @return Current proportional gain.
    float getKp() const;
    /// @return Current integral gain.
    float getKi() const;
    /// @return Current derivative gain.
    float getKd() const;
    /// @return Current operating mode.
    Mode getMode() const;
    /// @return Current controller direction.
    Direction getDirection() const;
    /// @return Current integral accumulator value (for diagnostics).
    float getIntegralTerm() const;
    /// @return Most recent error value (setpoint - input).
    float getLastError() const;
    /// @return Most recent computed output value.
    float getLastOutput() const;
    /// @return true if the controller was constructed successfully.
    bool isValid() const;

private:
    detail::PidImpl *m_impl;
};

} // namespace esp_pid
