#include "motor_manager.hpp"

#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

static const char *TAG = "motor_manager";

namespace motor_manager {

namespace {

constexpr TickType_t kMutexTimeoutTicks = pdMS_TO_TICKS(50);
constexpr UBaseType_t kConfigQueueDepth = 8;
constexpr TickType_t kConfigQueueRxTimeoutTicks = pdMS_TO_TICKS(100);
constexpr TickType_t kTaskStopGraceTicks = pdMS_TO_TICKS(20);

} // namespace

MotorManager::MotorManager(uart_comm::UartComm &uart, const ManagerConfig &config)
    : m_uart(uart),
      m_config(config),
      m_mutex(xSemaphoreCreateMutex()),
      m_motors(),
      m_valid(false),
      m_stop_update_task(false),
      m_stop_config_task(false),
    m_telemetry_enabled(true),
    m_telemetry_field_mask(kTelemetryFieldAll),
      m_update_task(nullptr),
      m_config_task(nullptr),
      m_config_queue(nullptr)
{
    if (m_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create manager mutex");
        return;
    }

    m_motors.reserve(m_config.max_motors);
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

    const esp_err_t cfg_cb_ret = m_uart.registerCallback(
        m_config.configure_msg_type,
        [this](const uart_comm::RxFrame &frame) {
            this->handleConfigureCommand(frame);
        });
    if (cfg_cb_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register configure callback (msg=0x%02X): %s",
                 m_config.configure_msg_type, esp_err_to_name(cfg_cb_ret));
        return;
    }

    const esp_err_t sp_cb_ret = m_uart.registerCallback(
        m_config.setpoint_stream_channel,
        [this](const uart_comm::RxFrame &frame) {
            this->handleSetpointStream(frame);
        });
    if (sp_cb_ret != ESP_OK) {
        m_uart.registerCallback(m_config.configure_msg_type, uart_comm::RxCallback{});
        ESP_LOGE(TAG, "Failed to register setpoint stream callback (ch=0x%02X): %s",
                 m_config.setpoint_stream_channel, esp_err_to_name(sp_cb_ret));
        return;
    }

    if (m_config.update_task.enable) {
        if (m_config.update_task.period_ms == 0U) {
            ESP_LOGE(TAG, "update task period must be > 0");
            return;
        }

        BaseType_t ok = xTaskCreate(
            &MotorManager::updateTaskEntry,
            "motor_mgr_upd",
            m_config.update_task.stack_size,
            this,
            m_config.update_task.priority,
            &m_update_task);
        if (ok != pdPASS) {
            m_update_task = nullptr;
            m_uart.registerCallback(m_config.configure_msg_type, uart_comm::RxCallback{});
            m_uart.registerCallback(m_config.setpoint_stream_channel, uart_comm::RxCallback{});
            ESP_LOGE(TAG, "Failed to create update task");
            return;
        }
    }

    BaseType_t cfg_ok = xTaskCreate(
        &MotorManager::configTaskEntry,
        "motor_mgr_cfg",
        m_config.config_task_stack_size,
        this,
        m_config.config_task_priority,
        &m_config_task);
    if (cfg_ok != pdPASS) {
        m_config_task = nullptr;
        m_uart.registerCallback(m_config.configure_msg_type, uart_comm::RxCallback{});
        m_uart.registerCallback(m_config.setpoint_stream_channel, uart_comm::RxCallback{});
        ESP_LOGE(TAG, "Failed to create config task");
        return;
    }

    m_valid.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "MotorManager ready (max_motors=%u cfg_msg=0x%02X sp_ch=0x%02X tm_ch=0x%02X)",
             m_config.max_motors,
             m_config.configure_msg_type,
             m_config.setpoint_stream_channel,
             m_config.telemetry_stream_channel);
}

MotorManager::~MotorManager()
{
    m_uart.registerCallback(m_config.configure_msg_type, uart_comm::RxCallback{});
    m_uart.registerCallback(m_config.setpoint_stream_channel, uart_comm::RxCallback{});

    stopUpdateTask();
    m_stop_config_task.store(true, std::memory_order_relaxed);
    if (m_config_queue != nullptr) {
        uart_comm::RxFrame wake{};
        xQueueSend(m_config_queue, &wake, 0);
    }
    vTaskDelay(kTaskStopGraceTicks);
    if (m_config_task != nullptr) {
        vTaskDelete(m_config_task);
        m_config_task = nullptr;
    }

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

    motor_controller::MotorControllerConfig motor_cfg{};
    motor_cfg.counts_per_rev = cfg.counts_per_rev;
    motor_cfg.gear_ratio = cfg.gear_ratio;
    motor_cfg.output_min = cfg.output_min;
    motor_cfg.output_max = cfg.output_max;
    motor_cfg.velocity_pid.kp = cfg.vel_kp;
    motor_cfg.velocity_pid.ki = cfg.vel_ki;
    motor_cfg.velocity_pid.kd = cfg.vel_kd;
    motor_cfg.velocity_pid.sample_time_ms = cfg.sample_time_ms;
    motor_cfg.position_pid.kp = cfg.pos_kp;
    motor_cfg.position_pid.ki = cfg.pos_ki;
    motor_cfg.position_pid.kd = cfg.pos_kd;
    motor_cfg.position_pid.sample_time_ms = cfg.sample_time_ms;
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

    MotorSlot slot{};
    slot.id = motor_id;
    slot.telemetry_enabled = true;
    slot.telemetry_field_mask = kTelemetryFieldAll;
    slot.motor = std::move(motor);
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
    motor->setCountsPerRev(cfg.counts_per_rev);
    motor->setGearRatio(cfg.gear_ratio);
    motor->setOutputLimits(cfg.output_min, cfg.output_max);
    motor->setVelocityPidTunings(cfg.vel_kp, cfg.vel_ki, cfg.vel_kd);
    motor->setPositionPidTunings(cfg.pos_kp, cfg.pos_ki, cfg.pos_kd);
    motor->setSampleTime(cfg.sample_time_ms);
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
    m_motors[index].motor->setMode(mode);
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
    if (kind == PidKind::Velocity) {
        motor->setVelocityPidTunings(kp, ki, kd);
    } else if (kind == PidKind::Position) {
        motor->setPositionPidTunings(kp, ki, kd);
    } else {
        xSemaphoreGive(m_mutex);
        return ResultCode::InvalidArgument;
    }
    motor->setSampleTime(sample_time_ms);

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

    auto &motor = m_motors[index].motor;
    switch (kind) {
        case SetpointKind::OpenLoopDuty:
            motor->setOpenLoopDuty(value);
            break;
        case SetpointKind::VelocityRadPerSec:
            motor->setVelocitySetpoint(value);
            break;
        case SetpointKind::PositionRad:
            motor->setPositionSetpoint(value);
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
    m_motors[index].motor->stop();
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
    m_motors[index].motor->reset();
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

void MotorManager::updateAll()
{
    if (!isValid()) {
        return;
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return;
    }

    for (auto &slot : m_motors) {
        if (slot.motor != nullptr) {
            slot.motor->update();
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

    if (rc == ResultCode::Ok && payload_len > 0U) {
        m_uart.sendData(m_config.telemetry_stream_channel, payload, payload_len);
    }
}

void MotorManager::updateTaskEntry(void *arg)
{
    auto *self = static_cast<MotorManager *>(arg);
    const TickType_t delay_ticks = pdMS_TO_TICKS(self->m_config.update_task.period_ms);
    while (!self->m_stop_update_task.load(std::memory_order_relaxed)) {
        self->updateAll();
        self->publishTelemetry();
        vTaskDelay(delay_ticks);
    }
    vTaskDelete(nullptr);
}

void MotorManager::configTaskEntry(void *arg)
{
    auto *self = static_cast<MotorManager *>(arg);
    uart_comm::RxFrame frame{};
    while (!self->m_stop_config_task.load(std::memory_order_relaxed)) {
        if (xQueueReceive(self->m_config_queue, &frame, kConfigQueueRxTimeoutTicks) != pdTRUE) {
            continue;
        }
        if (self->m_stop_config_task.load(std::memory_order_relaxed)) {
            break;
        }
        const ResultCode rc = self->parseAndDispatchConfigure(frame.payload, frame.len);
        if (rc != ResultCode::Ok) {
            ESP_LOGW(TAG, "Configure command failed: %s", resultCodeToString(rc));
        }
    }
    vTaskDelete(nullptr);
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

    const uint32_t timestamp_ms = esp_log_timestamp();

    out[0] = kProtocolVersion;
    out[1] = 0;
    std::memcpy(&out[2], &timestamp_ms, sizeof(uint32_t));
    size_t write_idx = kTelemetryPacketHeaderSize;
    uint8_t entry_count = 0;

    for (const auto &slot : m_motors) {
        if (slot.motor == nullptr || !slot.telemetry_enabled) {
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
            const float angle = slot.motor->getAngle();
            std::memcpy(&out[write_idx], &angle, sizeof(float));
            write_idx += sizeof(float);
        }
        if ((field_mask & kTelemetryFieldVelocity) != 0U) {
            const float velocity = slot.motor->getAngularVelocity();
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

void MotorManager::stopUpdateTask()
{
    if (m_update_task == nullptr) {
        return;
    }
    m_stop_update_task.store(true, std::memory_order_relaxed);
    vTaskDelay(kTaskStopGraceTicks);
    if (m_update_task != nullptr) {
        vTaskDelete(m_update_task);
        m_update_task = nullptr;
    }
}

} // namespace motor_manager
