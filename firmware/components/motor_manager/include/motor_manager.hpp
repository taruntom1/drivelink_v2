#pragma once

#include "motor_manager_types.hpp"
#include "uart_comm.hpp"
#include "stoppable_task.hpp"
#include "task_timer.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace motor_manager {

class MotorManager {
public:
    explicit MotorManager(uart_comm::UartComm &uart, const ManagerConfig &config = ManagerConfig{});
    ~MotorManager();

    MotorManager(const MotorManager &) = delete;
    MotorManager &operator=(const MotorManager &) = delete;
    MotorManager(MotorManager &&) = delete;
    MotorManager &operator=(MotorManager &&) = delete;

    bool isValid() const;

    // Each command returns its result code and why it failed (ResultDetail),
    // as sent in the configure response.
    Result createMotor(uint8_t motor_id, const MotorConfigPayload &cfg);
    Result deleteMotor(uint8_t motor_id);
    Result configureMotor(uint8_t motor_id, const MotorConfigPayload &cfg);
    Result setMode(uint8_t motor_id, motor_controller::ControlMode mode);
    Result setPidParams(uint8_t motor_id, PidKind kind, float kp, float ki, float kd, float d_cutoff_hz);
    Result setSetpoint(uint8_t motor_id, SetpointKind kind, float value);
    Result stopMotor(uint8_t motor_id);
    Result resetMotor(uint8_t motor_id);
    Result setTelemetryGlobal(bool enabled, uint8_t field_mask);
    Result setTelemetryForMotor(uint8_t motor_id, bool enabled, uint8_t field_mask, bool update_field_mask);

    /// Change the control loop and telemetry periods at runtime.
    /// A value of 0 keeps the current setting, so setTiming(0, 0, ...) just
    /// reads them. The control period is clamped to
    /// [kControlPeriodMinUs, kControlPeriodMaxUs]. The telemetry period is
    /// clamped to [control period, kTelemetryPeriodMaxUs] and rounded up to a
    /// whole number of control periods; the requested value is remembered so
    /// the telemetry rate is re-derived when only the control period changes.
    /// @param applied_control_period_us    Control period now in effect.
    /// @param applied_telemetry_period_us  Telemetry period now in effect.
    Result setTiming(uint32_t control_period_us, uint32_t telemetry_period_us,
                     uint32_t &applied_control_period_us,
                     uint32_t &applied_telemetry_period_us);

    /// Run one control cycle for every motor with the given dt. Only needed
    /// when the internal update task is disabled.
    void updateAll(float dt_s);
    /// Encode and send one telemetry packet now. Only needed when the
    /// internal update task is disabled.
    void publishTelemetry();
    void handleSetpointStream(const uart_comm::RxFrame &frame);
    void handleConfigureCommand(const uart_comm::RxFrame &frame);

private:
    struct MotorSlot {
        uint8_t id = 0;
        uint64_t pin_mask = 0; ///< GPIOs this motor owns (one bit per pin), 0 in mock mode
        bool telemetry_enabled = true;
        uint8_t telemetry_field_mask = kTelemetryFieldAll;
        float mock_angle_rad = 0.0f;
        float mock_velocity_rad_s = 0.0f;
        std::unique_ptr<motor_controller::MotorController> motor;
    };

    static constexpr size_t kSetpointKindCount = 3;

    // Latest setpoints received from the stream for one motor, waiting to be
    // applied by the update task. One value per SetpointKind, latest wins.
    struct PendingSetpoints {
        uint8_t motor_id = 0;
        bool in_use = false;
        std::array<bool, kSetpointKindCount> pending{};
        std::array<float, kSetpointKindCount> value{};
    };

    // Config-task state for detecting a stalled control timer.
    struct ControlTimerWatch {
        uint32_t last_wakes = 0;
        int64_t last_change_us = 0;
        bool stalled = false; ///< motor_manager.ControlTimerStalled is active
    };

    void updateTaskLoop();
    void configTaskLoop();
    /// Report or clear ControlTimerStalled. Config task only.
    void watchControlTimer(ControlTimerWatch &watch);
    void updateAllLocked(float dt_s);
    ResultCode encodeTelemetryLocked(uint8_t *out, size_t cap, size_t &out_len, uint64_t timestamp_us) const;
    Result parseAndDispatchConfigure(const uint8_t *payload, size_t len);
    ResultCode findSlotIndexById(uint8_t motor_id, size_t &index) const;
    /// Encode and queue a configure response. Non-blocking, so the RX task
    /// can use it too. Returns `result` (the command's own outcome).
    Result sendConfigureResponse(uint8_t seq, uint8_t subcommand, uint8_t motor_id, Result result,
                                 const uint8_t *body = nullptr, size_t body_len = 0);
    void stopTasks();
    ResultCode queueSetpoint(uint8_t motor_id, SetpointKind kind, float value);
    Result setSetpointLocked(uint8_t motor_id, SetpointKind kind, float value);
    void applyPendingSetpointsLocked();
    Result checkNewMotorLocked(uint8_t motor_id, uint64_t pin_mask) const;
    void setMotorKnown(uint8_t motor_id, bool known);
    bool isMotorKnown(uint8_t motor_id) const;

    uart_comm::UartComm &m_uart;
    ManagerConfig m_config;
    mutable SemaphoreHandle_t m_mutex;
    std::vector<MotorSlot> m_motors;
    std::atomic<bool> m_valid;
    bool m_use_mock_feedback;
    bool m_telemetry_enabled;
    uint8_t m_telemetry_field_mask;
    QueueHandle_t m_config_queue;
    rtos_util::StoppableTask m_update_task;
    rtos_util::StoppableTask m_config_task;
    uart_comm::CallbackRegistration m_configure_cb;
    uart_comm::CallbackRegistration m_setpoint_cb;

    // Control timing. The periods are written by setTiming() under m_mutex
    // and read lock-free by the update task.
    rtos_util::TaskTimer m_control_timer;
    std::atomic<uint32_t> m_control_period_us{0};
    std::atomic<uint32_t> m_telemetry_divider{1};
    uint32_t m_requested_telemetry_period_us = 0; // guarded by m_mutex
    // Bumped by the update task on every timer wake; the config task
    // watches it to report a stalled control timer.
    std::atomic<uint32_t> m_timer_wakes{0};

    // Motor ids that exist, one bit per id. Written under m_mutex when a
    // motor is created or deleted; read lock-free by queueSetpoint() on the
    // RX task, so setpoints for unknown ids never reach the mailbox.
    std::array<std::atomic<uint32_t>, 8> m_known_motor_ids{};

    // Setpoint mailbox (C4): written by the UART RX task under a spinlock so
    // it never waits on m_mutex; drained by the update task each tick.
    // Sized to max_motors in the constructor and never resized afterwards.
    std::vector<PendingSetpoints> m_pending_setpoints;
    std::vector<PendingSetpoints> m_pending_snapshot; // update task only
    portMUX_TYPE m_pending_lock = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace motor_manager
