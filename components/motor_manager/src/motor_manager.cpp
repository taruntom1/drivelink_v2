#include "motor_manager.hpp"

#include "diag.hpp"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tick_util.hpp"
#include "gpio_policy.hpp"
#include "motor_protocol.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <limits>
#include <sys/time.h>

static const char *const TAG = "motor_manager";

namespace motor_manager {

namespace {

constexpr TickType_t kMutexTimeoutTicks = rtos_util::msToTicksCeil(CONFIG_MOTOR_MANAGER_MUTEX_TIMEOUT_MS);
constexpr UBaseType_t kConfigQueueDepth = CONFIG_MOTOR_MANAGER_CONFIG_QUEUE_DEPTH;
constexpr TickType_t kConfigQueueRxTimeoutTicks = pdMS_TO_TICKS(100);

// The update task waits for the timer at most this long (longer than the
// largest control period) before reporting that the timer is not firing.
constexpr TickType_t kControlWakeTimeoutTicks = pdMS_TO_TICKS(250);
// Cap on the ticks folded into one dt after a stall, so a long hiccup cannot
// dump a huge step into the PID integrators.
constexpr uint32_t kMaxCatchUpTicks = 10;

namespace codes = diag::codes::motor_manager;

// motor_manager.InitFailed arg0. Reported to the host: never renumber.
namespace init_stage {
constexpr uint32_t kMutex = 1;
constexpr uint32_t kConfigQueue = 2;
constexpr uint32_t kInvalidConfig = 3;
constexpr uint32_t kConfigureCallback = 4;
constexpr uint32_t kSetpointCallback = 5;
constexpr uint32_t kUpdateTask = 6;
constexpr uint32_t kControlTimer = 7;
constexpr uint32_t kConfigTask = 8;
} // namespace init_stage

static_assert(gpio_policy::kGpioCount == GPIO_NUM_MAX, "gpio_policy is written for the ESP32-S3");

// The uart_comm link pins: a motor must never take over the host link.
constexpr uint64_t kHostLinkGpioMask =
    gpio_policy::gpioBit(CONFIG_UART_COMM_TX_PIN) | gpio_policy::gpioBit(CONFIG_UART_COMM_RX_PIN);

uint64_t wallClockUs()
{
    timeval now{};
    gettimeofday(&now, nullptr);
    return (static_cast<uint64_t>(now.tv_sec) * 1000000ULL) + static_cast<uint64_t>(now.tv_usec);
}

uint32_t clampControlPeriod(uint32_t period_us)
{
    return std::clamp(period_us, kControlPeriodMinUs, kControlPeriodMaxUs);
}

// Control ticks per telemetry packet for a requested telemetry period.
uint32_t telemetryDivider(uint32_t telemetry_period_us, uint32_t control_period_us)
{
    const uint32_t tp = std::clamp(telemetry_period_us, control_period_us, kTelemetryPeriodMaxUs);
    return std::max<uint32_t>(1U, (tp + control_period_us - 1U) / control_period_us);
}

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
        DIAG_ERROR(codes::kInitFailed, ESP_ERR_NO_MEM, init_stage::kMutex, 0, "Failed to create manager mutex");
        return;
    }

    m_motors.reserve(m_config.max_motors);
    m_pending_setpoints.resize(m_config.max_motors);
    m_pending_snapshot.resize(m_config.max_motors);
    m_config_queue = xQueueCreate(kConfigQueueDepth, sizeof(uart_comm::RxFrame));
    if (m_config_queue == nullptr) {
        DIAG_ERROR(codes::kInitFailed, ESP_ERR_NO_MEM, init_stage::kConfigQueue, 0,
                   "Failed to create configure queue");
        return;
    }

    if (!m_uart.isValid()) {
        DIAG_ERROR(codes::kInitFailed, ESP_ERR_INVALID_STATE, init_stage::kInvalidConfig, 0, "UartComm is invalid");
        return;
    }

    if (m_config.max_motors == 0U) {
        DIAG_ERROR(codes::kInitFailed, ESP_ERR_INVALID_ARG, init_stage::kInvalidConfig, 0, "max_motors must be > 0");
        return;
    }

    m_use_mock_feedback = m_config.use_mock_feedback;

    // Timing: clamp at the API boundary and remember the requested telemetry
    // period so it can be re-derived when the control period changes.
    const uint32_t control_period_us = clampControlPeriod(m_config.update_task.control_period_us);
    if (control_period_us != m_config.update_task.control_period_us) {
        ESP_LOGW(TAG, "control_period_us %" PRIu32 " clamped to %" PRIu32,
                 m_config.update_task.control_period_us, control_period_us);
    }
    m_requested_telemetry_period_us = (m_config.update_task.telemetry_period_us != 0U)
                                          ? m_config.update_task.telemetry_period_us
                                          : control_period_us;
    m_control_period_us.store(control_period_us, std::memory_order_relaxed);
    m_telemetry_divider.store(telemetryDivider(m_requested_telemetry_period_us, control_period_us),
                              std::memory_order_relaxed);

    const esp_err_t cfg_cb_ret = m_uart.registerCallback(
        m_config.configure_msg_type,
        [this](const uart_comm::RxFrame &frame) {
            this->handleConfigureCommand(frame);
        },
        m_configure_cb);
    if (cfg_cb_ret != ESP_OK) {
        DIAG_ERROR(codes::kInitFailed, cfg_cb_ret, init_stage::kConfigureCallback, 0,
                   "Failed to register configure callback (msg=0x%02X): %s",
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
        DIAG_ERROR(codes::kInitFailed, sp_cb_ret, init_stage::kSetpointCallback, 0,
                   "Failed to register setpoint stream callback (ch=0x%02X): %s",
                   m_config.setpoint_stream_channel, esp_err_to_name(sp_cb_ret));
        return;
    }

    if (m_config.update_task.enable) {
        const esp_err_t upd_ret = m_update_task.start(
            "motor_mgr_upd",
            m_config.update_task.stack_size,
            m_config.update_task.priority,
            [this] { updateTaskLoop(); },
            m_config.update_task.core_id);
        if (upd_ret != ESP_OK) {
            m_setpoint_cb.reset();
            m_configure_cb.reset();
            DIAG_ERROR(codes::kInitFailed, upd_ret, init_stage::kUpdateTask, 0,
                       "Failed to create update task (core=%d): %s",
                       static_cast<int>(m_config.update_task.core_id), esp_err_to_name(upd_ret));
            return;
        }

        // Start the timer only once the task it wakes exists.
        esp_err_t tmr_ret = m_control_timer.create(m_update_task, "motor_ctrl");
        if (tmr_ret == ESP_OK) {
            tmr_ret = m_control_timer.start(m_control_period_us.load(std::memory_order_relaxed));
        }
        if (tmr_ret != ESP_OK) {
            m_control_timer.destroy();
            m_setpoint_cb.reset();
            m_configure_cb.reset();
            m_update_task.stop();
            DIAG_ERROR(codes::kInitFailed, tmr_ret, init_stage::kControlTimer, 0,
                       "Failed to start control timer: %s", esp_err_to_name(tmr_ret));
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
        m_control_timer.destroy();
        m_update_task.stop();
        DIAG_ERROR(codes::kInitFailed, cfg_ret, init_stage::kConfigTask, 0,
                   "Failed to create config task: %s", esp_err_to_name(cfg_ret));
        return;
    }

    m_valid.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "MotorManager ready (max_motors=%u cfg_msg=0x%02X sp_ch=0x%02X tm_ch=0x%02X source=%s "
                  "control=%s period=%" PRIu32 " us telemetry_every=%" PRIu32 " ticks core=%d)",
             m_config.max_motors,
             m_config.configure_msg_type,
             m_config.setpoint_stream_channel,
             m_config.telemetry_stream_channel,
             m_use_mock_feedback ? "mock_setpoint" : "motor_controller",
             m_config.update_task.enable ? "timer" : "manual",
             control_period_us,
             m_telemetry_divider.load(std::memory_order_relaxed),
             static_cast<int>(m_config.update_task.core_id));
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
    if (!protocol::isMotorConfigFinite(cfg)) {
        ESP_LOGW(TAG, "Create motor id=%u rejected: non-finite config value", motor_id);
        return ResultCode::InvalidArgument;
    }
    // Mock mode allocates no hardware, so its pins are not checked or owned.
    const uint64_t pin_mask = m_use_mock_feedback ? 0U : gpio_policy::motorPinMask(cfg);
    if (!m_use_mock_feedback && !gpio_policy::areMotorPinsAllowed(cfg, kHostLinkGpioMask)) {
        ESP_LOGW(TAG, "Create motor id=%u rejected: pins pwm=%u dir=%u encA=%u encB=%u must be "
                      "distinct, existing and not reserved (flash, PSRAM, USB, UART link)",
                 motor_id, cfg.pwm_gpio, cfg.dir_gpio, cfg.enc_a, cfg.enc_b);
        return ResultCode::InvalidArgument;
    }

    // Check id, capacity and pins first, then build the controller (hardware
    // allocation, logging) without holding m_mutex, so the control loop does
    // not skip ticks while a motor is being created.
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }
    const ResultCode pre_rc = checkNewMotorLocked(motor_id, pin_mask);
    xSemaphoreGive(m_mutex);
    if (pre_rc != ResultCode::Ok) {
        return pre_rc;
    }

    MotorSlot slot{};
    slot.id = motor_id;
    slot.pin_mask = pin_mask;
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
        motor_cfg.velocity_pid.derivative_cutoff_hz = cfg.vel_d_cutoff_hz;
        motor_cfg.position_pid.kp = cfg.pos_kp;
        motor_cfg.position_pid.ki = cfg.pos_ki;
        motor_cfg.position_pid.kd = cfg.pos_kd;
        motor_cfg.position_pid.derivative_cutoff_hz = cfg.pos_d_cutoff_hz;
        motor_cfg.velocity_feedforward = cfg.velocity_feedforward;
        motor_cfg.position_feedforward = cfg.position_feedforward;
        motor_cfg.velocity_filter_cutoff_hz = cfg.vel_filter_cutoff_hz;

        auto motor = std::make_unique<motor_controller::MotorController>(
            static_cast<gpio_num_t>(cfg.pwm_gpio), static_cast<gpio_num_t>(cfg.dir_gpio),
            static_cast<gpio_num_t>(cfg.enc_a), static_cast<gpio_num_t>(cfg.enc_b), motor_cfg);
        if (!motor->isValid()) {
            const motor_controller::HardwareFailure failure = motor->hardwareFailure();
            DIAG_ERROR(codes::kMotorCreateFailed, failure.error, motor_id, static_cast<uint32_t>(failure.stage),
                       "Motor create failed (id=%u pwm=%d dir=%d encA=%d encB=%d stage=%u): %s",
                       motor_id, cfg.pwm_gpio, cfg.dir_gpio, cfg.enc_a, cfg.enc_b,
                       static_cast<unsigned>(failure.stage), esp_err_to_name(failure.error));
            // An empty MCPWM or PCNT pool is a capacity limit, not a fault.
            return (failure.error == ESP_ERR_NOT_FOUND) ? ResultCode::CapacityReached : ResultCode::InternalError;
        }
        slot.motor = std::move(motor);
    }

    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError; // slot (and its hardware) is released on return
    }
    // Re-check: another caller may have created the same id meanwhile.
    const ResultCode rc = checkNewMotorLocked(motor_id, pin_mask);
    if (rc == ResultCode::Ok) {
        m_motors.push_back(std::move(slot)); // capacity reserved: no reallocation
        setMotorKnown(motor_id, true);
    }
    xSemaphoreGive(m_mutex);

    if (rc == ResultCode::Ok) {
        ESP_LOGI(TAG, "Created motor id=%u pwm=%d dir=%d encA=%d encB=%d",
                 motor_id, cfg.pwm_gpio, cfg.dir_gpio, cfg.enc_a, cfg.enc_b);
    }
    return rc;
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

    // Take ownership out of the slot and destroy it after releasing the
    // mutex: freeing the hardware and logging should not stall the loop.
    std::unique_ptr<motor_controller::MotorController> removed = std::move(m_motors[index].motor);
    if (removed != nullptr) {
        removed->stop();
    }
    m_motors.erase(m_motors.begin() + static_cast<std::ptrdiff_t>(index));
    setMotorKnown(motor_id, false);
    xSemaphoreGive(m_mutex);

    removed.reset();
    ESP_LOGI(TAG, "Deleted motor id=%u", motor_id);
    return ResultCode::Ok;
}

ResultCode MotorManager::configureMotor(uint8_t motor_id, const MotorConfigPayload &cfg)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (!protocol::isMotorConfigFinite(cfg)) {
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
    motor->setVelocityDerivativeCutoff(cfg.vel_d_cutoff_hz);
    motor->setPositionPidTunings(cfg.pos_kp, cfg.pos_ki, cfg.pos_kd);
    motor->setPositionDerivativeCutoff(cfg.pos_d_cutoff_hz);
    motor->setVelocityFeedforward(cfg.velocity_feedforward);
    motor->setPositionFeedforward(cfg.position_feedforward);
    motor->setVelocityFilterCutoff(cfg.vel_filter_cutoff_hz);
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

ResultCode MotorManager::setPidParams(uint8_t motor_id, PidKind kind, float kp, float ki, float kd, float d_cutoff_hz)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (!isValidPidKind(kind) || !std::isfinite(kp) || !std::isfinite(ki) || !std::isfinite(kd) ||
        !std::isfinite(d_cutoff_hz)) {
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

    auto &motor = m_motors[index].motor;
    if (m_use_mock_feedback || motor == nullptr) {
        xSemaphoreGive(m_mutex);
        return ResultCode::Ok;
    }

    if (kind == PidKind::Velocity) {
        motor->setVelocityPidTunings(kp, ki, kd);
        motor->setVelocityDerivativeCutoff(d_cutoff_hz);
    } else if (kind == PidKind::Position) {
        motor->setPositionPidTunings(kp, ki, kd);
        motor->setPositionDerivativeCutoff(d_cutoff_hz);
    } else {
        xSemaphoreGive(m_mutex);
        return ResultCode::InvalidArgument;
    }

    xSemaphoreGive(m_mutex);
    return ResultCode::Ok;
}

ResultCode MotorManager::setSetpoint(uint8_t motor_id, SetpointKind kind, float value)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (!isValidSetpointKind(kind) || !std::isfinite(value)) {
        return ResultCode::InvalidArgument;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }
    const ResultCode rc = setSetpointLocked(motor_id, kind, value);
    xSemaphoreGive(m_mutex);
    return rc;
}

ResultCode MotorManager::setSetpointLocked(uint8_t motor_id, SetpointKind kind, float value)
{
    size_t index = 0;
    if (findSlotIndexById(motor_id, index) != ResultCode::Ok) {
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
            return ResultCode::InvalidArgument;
    }
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

ResultCode MotorManager::setTiming(uint32_t control_period_us, uint32_t telemetry_period_us,
                                   uint32_t &applied_control_period_us,
                                   uint32_t &applied_telemetry_period_us)
{
    if (!isValid()) {
        return ResultCode::InvalidState;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return ResultCode::InternalError;
    }

    const uint32_t old_period = m_control_period_us.load(std::memory_order_relaxed);
    const uint32_t new_period = (control_period_us != 0U) ? clampControlPeriod(control_period_us) : old_period;
    if (telemetry_period_us != 0U) {
        m_requested_telemetry_period_us = telemetry_period_us;
    }
    const uint32_t divider = telemetryDivider(m_requested_telemetry_period_us, new_period);

    ResultCode rc = ResultCode::Ok;
    esp_err_t timer_err = ESP_OK;
    if (new_period != old_period && m_control_timer.isCreated()) {
        // The update task reads the period on every wake, so PID dt and the
        // filter coefficients follow from the next tick on.
        timer_err = m_control_timer.setPeriod(new_period);
        if (timer_err != ESP_OK) {
            rc = ResultCode::InternalError; // reported below, after m_mutex is released
        }
    }
    if (rc == ResultCode::Ok) {
        m_control_period_us.store(new_period, std::memory_order_relaxed);
    }
    const uint32_t period_now = m_control_period_us.load(std::memory_order_relaxed);
    const uint32_t divider_now = (rc == ResultCode::Ok)
                                     ? divider
                                     : telemetryDivider(m_requested_telemetry_period_us, period_now);
    m_telemetry_divider.store(divider_now, std::memory_order_relaxed);
    xSemaphoreGive(m_mutex);

    if (timer_err != ESP_OK) {
        DIAG_ERROR(codes::kTimerRestartFailed, timer_err, new_period, 0,
                   "Failed to restart control timer at %" PRIu32 " us: %s", new_period, esp_err_to_name(timer_err));
    }

    applied_control_period_us = period_now;
    applied_telemetry_period_us = divider_now * period_now;

    ESP_LOGI(TAG, "Timing: control=%" PRIu32 " us telemetry=%" PRIu32 " us (every %" PRIu32 " ticks)",
             applied_control_period_us, applied_telemetry_period_us, divider_now);
    return rc;
}

uint32_t MotorManager::getControlOverrunCount() const
{
    return m_overrun_count.load(std::memory_order_relaxed);
}

SetpointStreamCounters MotorManager::getSetpointStreamCounters() const
{
    SetpointStreamCounters c{};
    c.bad_frames = m_bad_setpoint_frames.load(std::memory_order_relaxed);
    c.rejected_entries = m_rejected_setpoints.load(std::memory_order_relaxed);
    return c;
}

void MotorManager::updateAll(float dt_s)
{
    if (!isValid()) {
        return;
    }
    if (xSemaphoreTake(m_mutex, kMutexTimeoutTicks) != pdTRUE) {
        return;
    }
    updateAllLocked(dt_s);
    xSemaphoreGive(m_mutex);
}

void MotorManager::updateAllLocked(float dt_s)
{
    if (m_use_mock_feedback) {
        return;
    }
    for (auto &slot : m_motors) {
        if (slot.motor != nullptr) {
            slot.motor->update(dt_s);
        }
    }
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

    const ResultCode rc = encodeTelemetryLocked(payload, sizeof(payload), payload_len, wallClockUs());
    xSemaphoreGive(m_mutex);

    if (rc == ResultCode::Ok && payload_len > kTelemetryPacketHeaderSize) {
        m_uart.sendData(m_config.telemetry_stream_channel, payload, payload_len);
    }
}

void MotorManager::updateTaskLoop()
{
    uint8_t payload[CONFIG_UART_COMM_MAX_PAYLOAD] = {};
    uint32_t pending_ticks = 0;
    int32_t telemetry_countdown = 0; // publish on the first run
    bool timer_stalled = false;      // motor_manager.ControlTimerStalled is active

#if CONFIG_MOTOR_MANAGER_LOG_LOOP_STATS
    constexpr int64_t kStatsIntervalUs = 5000000;
    int64_t stats_window_start_us = esp_timer_get_time();
    int64_t stats_last_run_us = 0;
    int64_t int_min = std::numeric_limits<int64_t>::max(), int_max = 0, int_sum = 0;
    int64_t dur_min = std::numeric_limits<int64_t>::max(), dur_max = 0, dur_sum = 0;
    uint32_t int_n = 0, dur_n = 0;
#endif

    while (!m_update_task.stopRequested()) {
        // Woken by the control timer (and by stop()); the count says how many
        // timer periods elapsed since we last took the notification.
        const uint32_t fired = ulTaskNotifyTake(pdTRUE, kControlWakeTimeoutTicks);
        if (m_update_task.stopRequested()) {
            break;
        }
        if (fired == 0U) {
            // Reported once per stall (m_mutex is not held here), cleared below.
            if (!timer_stalled) {
                timer_stalled = true;
                DIAG_WARN(codes::kControlTimerStalled, ESP_ERR_TIMEOUT, pdTICKS_TO_MS(kControlWakeTimeoutTicks), 0,
                          "Control timer has not fired for %u ms",
                          static_cast<unsigned>(pdTICKS_TO_MS(kControlWakeTimeoutTicks)));
            }
            continue;
        }
        if (timer_stalled) {
            timer_stalled = false;
            DIAG_CLEAR(codes::kControlTimerStalled);
        }
        pending_ticks += fired;

        // Never wait for the mutex: a busy mutex (a config command in
        // progress) skips this tick, and the next run gets the longer dt.
        if (xSemaphoreTake(m_mutex, 0) != pdTRUE) {
            continue;
        }

#if CONFIG_MOTOR_MANAGER_LOG_LOOP_STATS
        const int64_t run_start_us = esp_timer_get_time();
        if (stats_last_run_us != 0) {
            const int64_t interval = run_start_us - stats_last_run_us;
            int_min = std::min(int_min, interval);
            int_max = std::max(int_max, interval);
            int_sum += interval;
            ++int_n;
        }
        stats_last_run_us = run_start_us;
#endif

        if (pending_ticks > 1U) {
            m_overrun_count.fetch_add(pending_ticks - 1U, std::memory_order_relaxed);
            DIAG_COUNT_N(codes::kControlOverrun, pending_ticks - 1U);
        }
        const uint32_t ticks = std::min(pending_ticks, kMaxCatchUpTicks);
        pending_ticks = 0;
        const uint32_t period_us = m_control_period_us.load(std::memory_order_relaxed);
        const float dt_s = static_cast<float>(ticks) * static_cast<float>(period_us) * 1e-6f;

        // Telemetry every N elapsed ticks (counting skipped ones too).
        telemetry_countdown -= static_cast<int32_t>(ticks);
        const bool publish = (telemetry_countdown <= 0);
        if (publish) {
            const auto divider = static_cast<int32_t>(m_telemetry_divider.load(std::memory_order_relaxed));
            telemetry_countdown = std::max<int32_t>(telemetry_countdown + divider, 1);
        }

        applyPendingSetpointsLocked();
        // Latch the timestamp at the sampling instant, not at encode time.
        const uint64_t sample_time_us = publish ? wallClockUs() : 0U;
        updateAllLocked(dt_s);

        uint8_t payload_len = 0;
        ResultCode enc_rc = ResultCode::InvalidState;
        if (publish) {
            enc_rc = encodeTelemetryLocked(payload, sizeof(payload), payload_len, sample_time_us);
        }
        xSemaphoreGive(m_mutex);

        if (enc_rc == ResultCode::Ok && payload_len > kTelemetryPacketHeaderSize) {
            m_uart.sendData(m_config.telemetry_stream_channel, payload, payload_len);
        }

#if CONFIG_MOTOR_MANAGER_LOG_LOOP_STATS
        const int64_t now_us = esp_timer_get_time();
        const uint32_t overruns = m_overrun_count.load(std::memory_order_relaxed);
        const int64_t dur = now_us - run_start_us;
        dur_min = std::min(dur_min, dur);
        dur_max = std::max(dur_max, dur);
        dur_sum += dur;
        ++dur_n;
        if ((now_us - stats_window_start_us) >= kStatsIntervalUs && int_n > 0U) {
            ESP_LOGI(TAG, "Loop stats (period %" PRIu32 " us): interval min/mean/max = %" PRId64 "/%" PRId64 "/%" PRId64
                          " us, run min/mean/max = %" PRId64 "/%" PRId64 "/%" PRId64 " us, overruns total %" PRIu32,
                     period_us, int_min, int_sum / int_n, int_max,
                     dur_min, dur_sum / dur_n, dur_max, overruns);
            stats_window_start_us = now_us;
            int_min = dur_min = std::numeric_limits<int64_t>::max();
            int_max = dur_max = int_sum = dur_sum = 0;
            int_n = dur_n = 0;
        }
#endif
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

ResultCode MotorManager::encodeTelemetryLocked(uint8_t *out, size_t cap, uint8_t &out_len,
                                               uint64_t timestamp_us) const
{
    if (cap < kTelemetryPacketHeaderSize) {
        return ResultCode::InvalidArgument;
    }

    if (!m_telemetry_enabled) {
        out_len = 0;
        return ResultCode::Ok;
    }

    protocol::TelemetryEncoder encoder(out, cap, timestamp_us);

    for (const auto &slot : m_motors) {
        if (!slot.telemetry_enabled) {
            continue;
        }
        if (!m_use_mock_feedback && slot.motor == nullptr) {
            continue;
        }

        const bool mock = m_use_mock_feedback || slot.motor == nullptr;
        protocol::TelemetryEntry entry{};
        entry.motor_id = slot.id;
        entry.field_mask = static_cast<uint8_t>(slot.telemetry_field_mask & m_telemetry_field_mask);
        if ((entry.field_mask & kTelemetryFieldAngle) != 0U) {
            entry.angle_rad = mock ? slot.mock_angle_rad : slot.motor->getAngle();
        }
        if ((entry.field_mask & kTelemetryFieldVelocity) != 0U) {
            entry.velocity_rad_s = mock ? slot.mock_velocity_rad_s : slot.motor->getAngularVelocity();
        }
        if ((entry.field_mask & kTelemetryFieldOutput) != 0U) {
            // Mock mode has no motor output to report.
            entry.output = mock ? 0.0f : slot.motor->getOutput();
        }

        if (!encoder.append(entry)) {
            break; // packet full
        }
    }

    const size_t len = encoder.finish();
    if (len == 0U) {
        out_len = 0;
        return ResultCode::InternalError;
    }
    out_len = static_cast<uint8_t>(len);
    return ResultCode::Ok;
}

ResultCode MotorManager::checkNewMotorLocked(uint8_t motor_id, uint64_t pin_mask) const
{
    size_t existing_index = 0;
    if (findSlotIndexById(motor_id, existing_index) == ResultCode::Ok) {
        return ResultCode::AlreadyExists;
    }
    if (m_motors.size() >= m_config.max_motors) {
        return ResultCode::CapacityReached;
    }
    for (const auto &slot : m_motors) {
        if ((slot.pin_mask & pin_mask) != 0U) {
            return ResultCode::InvalidArgument; // a pin another motor already owns
        }
    }
    return ResultCode::Ok;
}

void MotorManager::setMotorKnown(uint8_t motor_id, bool known)
{
    const uint32_t bit = 1U << (motor_id % 32U);
    auto &word = m_known_motor_ids[motor_id / 32U];
    if (known) {
        word.fetch_or(bit, std::memory_order_release);
    } else {
        word.fetch_and(~bit, std::memory_order_release);
    }
}

bool MotorManager::isMotorKnown(uint8_t motor_id) const
{
    const uint32_t bit = 1U << (motor_id % 32U);
    return (m_known_motor_ids[motor_id / 32U].load(std::memory_order_acquire) & bit) != 0U;
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
    // Runs on the RX task: reject everything the control tick would reject,
    // so only applicable setpoints reach the mailbox.
    const auto kind_index = static_cast<size_t>(kind);
    if (kind_index >= kSetpointKindCount || !std::isfinite(value)) {
        return ResultCode::InvalidArgument;
    }
    if (!isMotorKnown(motor_id)) {
        return ResultCode::NotFound;
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

void MotorManager::applyPendingSetpointsLocked()
{
    // Caller holds m_mutex. Move everything pending out of the mailbox under
    // the spinlock, then apply it without the spinlock held.
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
            // No logging here: this runs on the control tick under m_mutex.
            const ResultCode rc = setSetpointLocked(e.motor_id, static_cast<SetpointKind>(k), e.value[k]);
            if (rc != ResultCode::Ok) {
                m_rejected_setpoints.fetch_add(1U, std::memory_order_relaxed);
                DIAG_COUNT(codes::kSetpointRejected);
            }
        }
    }
}

void MotorManager::stopTasks()
{
    // Stop the timer first (and wait out an in-flight callback) so nothing
    // notifies the update task while it is being stopped and deleted.
    m_control_timer.destroy();

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
