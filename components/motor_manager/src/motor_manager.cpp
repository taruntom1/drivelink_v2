#include "motor_manager.hpp"

#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sys/time.h>

static const char *TAG = "motor_manager";

namespace motor_manager {

namespace {

constexpr TickType_t kMutexTimeoutTicks = pdMS_TO_TICKS(50);
constexpr UBaseType_t kConfigQueueDepth = 8;
constexpr TickType_t kConfigQueueRxTimeoutTicks = pdMS_TO_TICKS(100);

} // namespace

MotorManager::MotorManager(uart_comm::UartComm &uart, const ManagerConfig &config)
    : m_uart(uart),
      m_config(config),
      m_mutex(xSemaphoreCreateMutex()),
      m_motors(),
      m_valid(false),
      m_use_mock_feedback(false),
      m_telemetry_enabled(true),
      m_telemetry_field_mask(kTelemetryFieldAll),
      m_config_queue(nullptr)
{
    if (m_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create manager mutex");
        return;
    }

    m_motors.reserve(m_config.max_motors);
    m_pending_setpoints.resize(m_config.max_motors);
    m_pending_snapshot.resize(m_config.max_motors);
    m_config_queue = xQueueCreate(kConfigQueueDepth, sizeof(uart_comm::RxFrame));
    if (m_config_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create configure queue");
        return;
    }

    if (!m_uart.isValid()) {
        ESP_LOGE(TAG, "UartComm is invalid");
        return;
    }

    if (m_config.max_motors == 0U) {
        ESP_LOGE(TAG, "max_motors must be > 0");
        return;
    }

    m_use_mock_feedback = m_config.use_mock_feedback;

    const esp_err_t cfg_cb_ret = m_uart.registerCallback(
        m_config.configure_msg_type,
        [this](const uart_comm::RxFrame &frame) {
            this->handleConfigureCommand(frame);
        },
        m_configure_cb);
    if (cfg_cb_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register configure callback (msg=0x%02X): %s",
                 m_config.configure_msg_type, esp_err_to_name(cfg_cb_ret));
        return;
    }

    const esp_err_t sp_cb_ret = m_uart.registerCallback(
        m_config.setpoint_stream_channel,
        [this](const uart_comm::RxFrame &frame) {
            this->handleSetpointStream(frame);
        },
        m_setpoint_cb);
    if (sp_cb_ret != ESP_OK) {
        m_configure_cb.reset();
        ESP_LOGE(TAG, "Failed to register setpoint stream callback (ch=0x%02X): %s",
                 m_config.setpoint_stream_channel, esp_err_to_name(sp_cb_ret));
        return;
    }

    if (m_config.update_task.enable) {
        if (m_config.update_task.period_ms == 0U) {
            ESP_LOGE(TAG, "update task period must be > 0");
            return;
        }

        const esp_err_t upd_ret = m_update_task.start(
            "motor_mgr_upd",
            m_config.update_task.stack_size,
            m_config.update_task.priority,
            [this] { updateTaskLoop(); });
        if (upd_ret != ESP_OK) {
            m_setpoint_cb.reset();
            m_configure_cb.reset();
            ESP_LOGE(TAG, "Failed to create update task: %s", esp_err_to_name(upd_ret));
            return;
        }
    }

    const esp_err_t cfg_ret = m_config_task.start(
        "motor_mgr_cfg",
        m_config.config_task_stack_size,
        m_config.config_task_priority,
        [this] { configTaskLoop(); });
    if (cfg_ret != ESP_OK) {
        m_setpoint_cb.reset();
        m_configure_cb.reset();
        m_update_task.stop();
        ESP_LOGE(TAG, "Failed to create config task: %s", esp_err_to_name(cfg_ret));
        return;
    }

    m_valid.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "MotorManager ready (max_motors=%u cfg_msg=0x%02X sp_ch=0x%02X tm_ch=0x%02X source=%s)",
             m_config.max_motors,
             m_config.configure_msg_type,
             m_config.setpoint_stream_channel,
             m_config.telemetry_stream_channel,
             m_use_mock_feedback ? "mock_setpoint" : "motor_controller");
}

MotorManager::~MotorManager()
{
    m_valid.store(false, std::memory_order_release);

    // Unregister first. reset() waits for a callback that is already running
    // on the UART RX task, so none can touch this object afterwards.
    m_setpoint_cb.reset();
    m_configure_cb.reset();

    stopTasks();

    if (m_mutex != nullptr) {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        for (auto &slot : m_motors) {
            if (slot.motor != nullptr) {
                slot.motor->stop();
            }
        }
        m_motors.clear();
        xSemaphoreGive(m_mutex);
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }
    if (m_config_queue != nullptr) {
        vQueueDelete(m_config_queue);
        m_config_queue = nullptr;
    }
}

bool MotorManager::isValid() const
{
    return m_valid.load(std::memory_order_acquire);
}

ResultCode MotorManager::createMotor(uint8_t motor_id, const MotorConfigPayload &cfg)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t existing_index = 0;
    const ResultCode find_code = findSlotIndexById(motor_id, existing_index);
    if (find_code == ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::AlreadyExists;
    }

    if (m_motors.size() >= m_config.max_motors) {
        xSemaphoreGive(m_mutex);
        return ResultCode::CapacityReached;
    }

    MotorSlot slot{};
    slot.id = motor_id;
    slot.telemetry_enabled = true;
    slot.telemetry_field_mask = kTelemetryFieldAll;
    slot.mock_angle_rad = 0.0f;
    slot.mock_velocity_rad_s = 0.0f;

    if (!m_use_mock_feedback) {
        motor_controller::MotorControllerConfig motor_cfg{};
        motor_cfg.counts_per_rev = cfg.counts_per_rev;
        motor_cfg.gear_ratio = cfg.gear_ratio;
        motor_cfg.output_min = cfg.output_min;
        motor_cfg.output_max = cfg.output_max;
        motor_cfg.velocity_pid.kp = cfg.vel_kp;
        motor_cfg.velocity_pid.ki = cfg.vel_ki;
        motor_cfg.velocity_pid.kd = cfg.vel_kd;
        motor_cfg.position_pid.kp = cfg.pos_kp;
        motor_cfg.position_pid.ki = cfg.pos_ki;
        motor_cfg.position_pid.kd = cfg.pos_kd;
        motor_cfg.velocity_feedforward = cfg.velocity_feedforward;
        motor_cfg.position_feedforward = cfg.position_feedforward;

        auto motor = std::make_unique<motor_controller::MotorController>(
            cfg.pwm_gpio, cfg.dir_gpio, cfg.enc_a, cfg.enc_b, motor_cfg);
        if (!motor->isValid()) {
            xSemaphoreGive(m_mutex);
            ESP_LOGE(TAG, "Motor create failed (id=%u pwm=%d dir=%d encA=%d encB=%d)",
                     motor_id, cfg.pwm_gpio, cfg.dir_gpio, cfg.enc_a, cfg.enc_b);
            return ResultCode::InternalError;
        }
        slot.motor = std::move(motor);
    }

    m_motors.push_back(std::move(slot));
    xSemaphoreGive(m_mutex);

    ESP_LOGI(TAG, "Created motor id=%u pwm=%d dir=%d encA=%d encB=%d",
             motor_id, cfg.pwm_gpio, cfg.dir_gpio, cfg.enc_a, cfg.enc_b);
    return ResultCode::Ok;
}

ResultCode MotorManager::deleteMotor(uint8_t motor_id)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::NotFound;
    }

    if (m_motors[index].motor != nullptr) {
        m_motors[index].motor->stop();
    }
    m_motors.erase(m_motors.begin() + static_cast<std::ptrdiff_t>(index));
    xSemaphoreGive(m_mutex);

    ESP_LOGI(TAG, "Deleted motor id=%u", motor_id);
    return ResultCode::Ok;
}

ResultCode MotorManager::configureMotor(uint8_t motor_id, const MotorConfigPayload &cfg)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::NotFound;
    }

    auto &motor = m_motors[index].motor;
    if (m_use_mock_feedback || motor == nullptr) {
        xSemaphoreGive(m_mutex);
        ESP_LOGI(TAG, "Configured motor id=%u (mock)", motor_id);
        return ResultCode::Ok;
    }

    motor->setCountsPerRev(cfg.counts_per_rev);
    motor->setGearRatio(cfg.gear_ratio);
    motor->setOutputLimits(cfg.output_min, cfg.output_max);
    motor->setVelocityPidTunings(cfg.vel_kp, cfg.vel_ki, cfg.vel_kd);
    motor->setPositionPidTunings(cfg.pos_kp, cfg.pos_ki, cfg.pos_kd);
    motor->setVelocityFeedforward(cfg.velocity_feedforward);
    motor->setPositionFeedforward(cfg.position_feedforward);
    xSemaphoreGive(m_mutex);

    ESP_LOGI(TAG, "Configured motor id=%u", motor_id);
    return ResultCode::Ok;
}

ResultCode MotorManager::setMode(uint8_t motor_id, motor_controller::ControlMode mode)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::NotFound;
    }
    if (!m_use_mock_feedback && m_motors[index].motor != nullptr) {
        m_motors[index].motor->setMode(mode);
    }
    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode MotorManager::setPidParams(uint8_t motor_id, PidKind kind, float kp, float ki, float kd, unsigned int sample_time_ms)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::NotFound;
    }

    auto &motor = m_motors[index].motor;
    if (m_use_mock_feedback || motor == nullptr) {
        xSemaphoreGive(m_mutex);
        return ResultCode::Ok;
    }

    if (kind == PidKind::Velocity) {
        motor->setVelocityPidTunings(kp, ki, kd);
    } else if (kind == PidKind::Position) {
        motor->setPositionPidTunings(kp, ki, kd);
    } else {
        xSemaphoreGive(m_mutex);
        return ResultCode::InvalidArgument;
    }
    // The PID dt now comes from the update loop; sample_time_ms is ignored.
    (void)sample_time_ms;

    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode MotorManager::setSetpoint(uint8_t motor_id, SetpointKind kind, float value)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::NotFound;
    }

    auto &slot = m_motors[index];
    auto &motor = slot.motor;
    switch (kind) {
        case SetpointKind::OpenLoopDuty:
            if (m_use_mock_feedback || motor == nullptr) {
                slot.mock_velocity_rad_s = value;
            } else {
                motor->setOpenLoopDuty(value);
            }
            break;
        case SetpointKind::VelocityRadPerSec:
            if (m_use_mock_feedback || motor == nullptr) {
                slot.mock_velocity_rad_s = value;
            } else {
                motor->setVelocitySetpoint(value);
            }
            break;
        case SetpointKind::PositionRad:
            if (m_use_mock_feedback || motor == nullptr) {
                slot.mock_angle_rad = value;
            } else {
                motor->setPositionSetpoint(value);
            }
            break;
        default:
            xSemaphoreGive(m_mutex);
            return ResultCode::InvalidArgument;
    }

    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode MotorManager::stopMotor(uint8_t motor_id)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::NotFound;
    }
    auto &slot = m_motors[index];
    if (m_use_mock_feedback || slot.motor == nullptr) {
        slot.mock_velocity_rad_s = 0.0f;
    } else {
        slot.motor->stop();
    }
    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode MotorManager::resetMotor(uint8_t motor_id)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::NotFound;
    }
    auto &slot = m_motors[index];
    if (m_use_mock_feedback || slot.motor == nullptr) {
        slot.mock_angle_rad = 0.0f;
        slot.mock_velocity_rad_s = 0.0f;
    } else {
        slot.motor->reset();
    }
    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode MotorManager::setTelemetryGlobal(bool enabled, uint8_t field_mask)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if ((field_mask & ~kTelemetryFieldAll) != 0U) {
        return ResultCode::InvalidArgument;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    m_telemetry_enabled = enabled;
    m_telemetry_field_mask = field_mask;

    xSemaphoreGive(m_mutex);

    ESP_LOGI(TAG, "Telemetry global updated (enabled=%u field_mask=0x%02X)",
             static_cast<unsigned>(enabled ? 1U : 0U),
             static_cast<unsigned>(field_mask));
    return ResultCode::Ok;
}

ResultCode MotorManager::setTelemetryForMotor(uint8_t motor_id, bool enabled, uint8_t field_mask, bool update_field_mask)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if ((field_mask & ~kTelemetryFieldAll) != 0U) {
        return ResultCode::InvalidArgument;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
        xSemaphoreGive(m_mutex);
        return ResultCode::NotFound;
    }

    auto &slot = m_motors[index];
    slot.telemetry_enabled = enabled;
    if (update_field_mask) {
        slot.telemetry_field_mask = field_mask;
    }

    xSemaphoreGive(m_mutex);

    ESP_LOGI(TAG, "Telemetry motor updated (id=%u enabled=%u field_mask=0x%02X apply=%u)",
             motor_id,
             static_cast<unsigned>(enabled ? 1U : 0U),
             static_cast<unsigned>(field_mask),
             static_cast<unsigned>(update_field_mask ? 1U : 0U));
    return ResultCode::Ok;
}

void MotorManager::updateAll(float dt_s)
{
    if (!isValid()) {
        return;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return;
    }

    for (auto &slot : m_motors) {
        if (!m_use_mock_feedback && slot.motor != nullptr) {
            slot.motor->update(dt_s);
        }
    }

    xSemaphoreGive(m_mutex);
}

void MotorManager::publishTelemetry()
{
    if (!isValid()) {
        return;
    }

    uint8_t payload[CONFIG_UART_COMM_MAX_PAYLOAD] = {};
    uint8_t payload_len = 0;

    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return;
    }

    const ResultCode rc = encodeTelemetryLocked(payload, sizeof(payload), payload_len);
    xSemaphoreGive(m_mutex);

    if (rc == ResultCode::Ok && payload_len > kTelemetryPacketHeaderSize) {
        m_uart.sendData(m_config.telemetry_stream_channel, payload, payload_len);
    }
}

void MotorManager::updateTaskLoop()
{
    const TickType_t delay_ticks = pdMS_TO_TICKS(m_config.update_task.period_ms);
    const float dt_s = static_cast<float>(m_config.update_task.period_ms) / 1000.0f;
    while (!m_update_task.stopRequested()) {
        applyPendingSetpoints();
        updateAll(dt_s);
        publishTelemetry();
        m_update_task.sleepFor(delay_ticks);
    }
}

void MotorManager::configTaskLoop()
{
    uart_comm::RxFrame frame{};
    while (!m_config_task.stopRequested()) {
        if (xQueueReceive(m_config_queue, &frame, kConfigQueueRxTimeoutTicks) != pdTRUE) {
            continue;
        }
        if (m_config_task.stopRequested()) {
            break;
        }
        const ResultCode rc = parseAndDispatchConfigure(frame.payload, frame.len);
        if (rc != ResultCode::Ok) {
            ESP_LOGW(TAG, "Configure command failed: %s", resultCodeToString(rc));
        }
    }
}

ResultCode MotorManager::encodeTelemetryLocked(uint8_t *out, size_t cap, uint8_t &out_len) const
{
    if (cap < kTelemetryPacketHeaderSize) {
        return ResultCode::InvalidArgument;
    }

    if (!m_telemetry_enabled) {
        out_len = 0;
        return ResultCode::Ok;
    }

    timeval now{};
    gettimeofday(&now, nullptr);
    const uint64_t timestamp_us = (static_cast<uint64_t>(now.tv_sec) * 1000000ULL) +
                                  static_cast<uint64_t>(now.tv_usec);

    out[0] = kProtocolVersion;
    out[1] = 0;
    std::memcpy(&out[2], &timestamp_us, sizeof(uint64_t));
    size_t write_idx = kTelemetryPacketHeaderSize;
    uint8_t entry_count = 0;

    for (const auto &slot : m_motors) {
        if (!slot.telemetry_enabled) {
            continue;
        }
        if (!m_use_mock_feedback && slot.motor == nullptr) {
            continue;
        }

        const uint8_t field_mask = static_cast<uint8_t>(slot.telemetry_field_mask & m_telemetry_field_mask);

        size_t required = kTelemetryEntryBaseSize;
        if ((field_mask & kTelemetryFieldAngle) != 0U) {
            required += sizeof(float);
        }
        if ((field_mask & kTelemetryFieldVelocity) != 0U) {
            required += sizeof(float);
        }

        if ((write_idx + required) > cap) {
            break;
        }

        out[write_idx++] = slot.id;
        out[write_idx++] = field_mask;

        if ((field_mask & kTelemetryFieldAngle) != 0U) {
            const float angle = (m_use_mock_feedback || slot.motor == nullptr)
                                    ? slot.mock_angle_rad
                                    : slot.motor->getAngle();
            std::memcpy(&out[write_idx], &angle, sizeof(float));
            write_idx += sizeof(float);
        }
        if ((field_mask & kTelemetryFieldVelocity) != 0U) {
            const float velocity = (m_use_mock_feedback || slot.motor == nullptr)
                                       ? slot.mock_velocity_rad_s
                                       : slot.motor->getAngularVelocity();
            std::memcpy(&out[write_idx], &velocity, sizeof(float));
            write_idx += sizeof(float);
        }

        ++entry_count;
    }

    out[1] = entry_count;
    out_len = static_cast<uint8_t>(write_idx);
    return ResultCode::Ok;
}

ResultCode MotorManager::findSlotIndexById(uint8_t motor_id, size_t &index) const
{
    for (size_t i = 0; i < m_motors.size(); ++i) {
        if (m_motors[i].id == motor_id) {
            index = i;
            return ResultCode::Ok;
        }
    }
    return ResultCode::NotFound;
}

ResultCode MotorManager::queueSetpoint(uint8_t motor_id, SetpointKind kind, float value)
{
    const auto kind_index = static_cast<size_t>(kind);
    if (kind_index >= kSetpointKindCount) {
        return ResultCode::InvalidArgument;
    }

    ResultCode rc = ResultCode::CapacityReached;
    taskENTER_CRITICAL(&m_pending_lock);
    PendingSetpoints *entry = nullptr;
    PendingSetpoints *free_entry = nullptr;
    for (auto &e : m_pending_setpoints) {
        if (e.in_use && e.motor_id == motor_id) {
            entry = &e;
            break;
        }
        if (!e.in_use && free_entry == nullptr) {
            free_entry = &e;
        }
    }
    if (entry == nullptr && free_entry != nullptr) {
        entry = free_entry;
        entry->in_use = true;
        entry->motor_id = motor_id;
        entry->pending.fill(false);
    }
    if (entry != nullptr) {
        entry->value[kind_index] = value;
        entry->pending[kind_index] = true;
        rc = ResultCode::Ok;
    }
    taskEXIT_CRITICAL(&m_pending_lock);
    return rc;
}

void MotorManager::applyPendingSetpoints()
{
    // Move everything pending out of the mailbox under the spinlock, then
    // apply it without the spinlock held (setSetpoint takes m_mutex).
    size_t count = 0;
    taskENTER_CRITICAL(&m_pending_lock);
    for (auto &e : m_pending_setpoints) {
        if (e.in_use) {
            m_pending_snapshot[count++] = e;
            e.in_use = false;
        }
    }
    taskEXIT_CRITICAL(&m_pending_lock);

    for (size_t i = 0; i < count; ++i) {
        const PendingSetpoints &e = m_pending_snapshot[i];
        for (size_t k = 0; k < kSetpointKindCount; ++k) {
            if (!e.pending[k]) {
                continue;
            }
            const ResultCode rc = setSetpoint(e.motor_id, static_cast<SetpointKind>(k), e.value[k]);
            if (rc != ResultCode::Ok) {
                ESP_LOGW(TAG, "Setpoint rejected motor=%u kind=%u rc=%s",
                         e.motor_id, static_cast<unsigned>(k), resultCodeToString(rc));
            }
        }
    }
}

void MotorManager::stopTasks()
{
    m_update_task.requestStop();
    m_config_task.requestStop();
    if (m_config_queue != nullptr && m_config_task.isRunning()) {
        // Wake the config task out of xQueueReceive. It re-checks the stop
        // flag before handling a frame, so this frame is never dispatched.
        // If the queue is full, the 100 ms receive timeout wakes it instead.
        uart_comm::RxFrame wake{};
        (void)xQueueSend(m_config_queue, &wake, 0);
    }
    m_update_task.stop();
    m_config_task.stop();
}

} // namespace motor_manager
