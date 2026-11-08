#pragma once

#include "motor_manager_types.hpp"
#include "uart_comm.hpp"
#include "stoppable_task.hpp"
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

    ResultCode createMotor(uint8_t motor_id, const MotorConfigPayload &cfg);
    ResultCode deleteMotor(uint8_t motor_id);
    ResultCode configureMotor(uint8_t motor_id, const MotorConfigPayload &cfg);
    ResultCode setMode(uint8_t motor_id, motor_controller::ControlMode mode);
    ResultCode setPidParams(uint8_t motor_id, PidKind kind, float kp, float ki, float kd, unsigned int sample_time_ms);
    ResultCode setSetpoint(uint8_t motor_id, SetpointKind kind, float value);
    ResultCode stopMotor(uint8_t motor_id);
    ResultCode resetMotor(uint8_t motor_id);
    ResultCode setTelemetryGlobal(bool enabled, uint8_t field_mask);
    ResultCode setTelemetryForMotor(uint8_t motor_id, bool enabled, uint8_t field_mask, bool update_field_mask);

    void updateAll();
    void publishTelemetry();
    void handleSetpointStream(const uart_comm::RxFrame &frame);
    void handleConfigureCommand(const uart_comm::RxFrame &frame);

private:
    struct MotorSlot {
        uint8_t id = 0;
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

    void updateTaskLoop();
    void configTaskLoop();
    ResultCode encodeTelemetryLocked(uint8_t *out, size_t cap, uint8_t &out_len) const;
    ResultCode parseAndDispatchConfigure(const uint8_t *payload, uint8_t len);
    ResultCode findSlotIndexById(uint8_t motor_id, size_t &index) const;
    ResultCode sendConfigureResponse(uint8_t seq, uint8_t subcommand, uint8_t motor_id, ResultCode code);
    void stopTasks();
    ResultCode queueSetpoint(uint8_t motor_id, SetpointKind kind, float value);
    void applyPendingSetpoints();

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

    // Setpoint mailbox (C4): written by the UART RX task under a spinlock so
    // it never waits on m_mutex; drained by the update task each tick.
    // Sized to max_motors in the constructor and never resized afterwards.
    std::vector<PendingSetpoints> m_pending_setpoints;
    std::vector<PendingSetpoints> m_pending_snapshot; // update task only
    portMUX_TYPE m_pending_lock = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace motor_manager
