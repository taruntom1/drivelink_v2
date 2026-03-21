#include "motor_manager.hpp"

#include "esp_log.h"

#include <cstring>

static const char *TAG = "motor_mgr_uart";

namespace motor_manager {

namespace {

bool readFloatLe(const uint8_t *src, size_t src_len, size_t &offset, float &out)
{
    if ((offset + sizeof(float)) > src_len) {
        return false;
    }
    std::memcpy(&out, &src[offset], sizeof(float));
    offset += sizeof(float);
    return true;
}

bool readU32Le(const uint8_t *src, size_t src_len, size_t &offset, uint32_t &out)
{
    if ((offset + sizeof(uint32_t)) > src_len) {
        return false;
    }
    std::memcpy(&out, &src[offset], sizeof(uint32_t));
    offset += sizeof(uint32_t);
    return true;
}

bool parseControlMode(uint8_t raw, motor_controller::ControlMode &out)
{
    switch (raw) {
        case 0:
            out = motor_controller::ControlMode::OpenLoop;
            return true;
        case 1:
            out = motor_controller::ControlMode::VelocityControl;
            return true;
        case 2:
            out = motor_controller::ControlMode::PositionControl;
            return true;
        default:
            return false;
    }
}

} // namespace

void MotorManager::handleSetpointStream(const uart_comm::RxFrame &frame)
{
    if (!isValid()) {
        return;
    }
    if (frame.len < 2U) {
        ESP_LOGW(TAG, "Setpoint stream too short: len=%u", frame.len);
        return;
    }

    const uint8_t version = frame.payload[0];
    const uint8_t count = frame.payload[1];
    if (version != kProtocolVersion) {
        ESP_LOGW(TAG, "Setpoint stream version mismatch: got=%u expected=%u", version, kProtocolVersion);
        return;
    }

    const size_t expected_len = 2U + static_cast<size_t>(count) * kSetpointEntrySize;
    if (frame.len < expected_len) {
        ESP_LOGW(TAG, "Setpoint stream length mismatch: len=%u expected>=%u", frame.len, static_cast<unsigned>(expected_len));
        return;
    }

    size_t offset = 2;
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t motor_id = frame.payload[offset++];
        const uint8_t kind_raw = frame.payload[offset++];
        float value = 0.0f;
        if (!readFloatLe(frame.payload, frame.len, offset, value)) {
            ESP_LOGW(TAG, "Failed parsing setpoint entry index=%u", i);
            return;
        }
        const auto kind = static_cast<SetpointKind>(kind_raw);
        const ResultCode rc = setSetpoint(motor_id, kind, value);
        if (rc != ResultCode::Ok) {
            ESP_LOGW(TAG, "Setpoint rejected motor=%u kind=%u rc=%s",
                     motor_id, kind_raw, resultCodeToString(rc));
        }
    }
}

void MotorManager::handleConfigureCommand(const uart_comm::RxFrame &frame)
{
    if (!isValid()) {
        return;
    }
    if (frame.len < 3U) {
        ESP_LOGW(TAG, "Configure payload too short: len=%u", frame.len);
        return;
    }

    if (m_config_queue == nullptr) {
        return;
    }
    if (xQueueSend(m_config_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Configure queue full; dropping command");
    }
}

ResultCode MotorManager::parseAndDispatchConfigure(const uint8_t *payload, uint8_t len)
{
    const uint8_t version = payload[0];
    const auto subcommand = static_cast<ConfigureSubcommand>(payload[1]);
    const uint8_t motor_id = payload[2];

    if (version != kProtocolVersion) {
        return sendConfigureResponse(subcommand, motor_id, ResultCode::InvalidArgument);
    }

    ResultCode rc = ResultCode::InvalidArgument;

    switch (subcommand) {
        case ConfigureSubcommand::CreateMotor:
        case ConfigureSubcommand::ConfigureMotor: {
            constexpr size_t kRequiredLen = 3U + 4U + 14U * sizeof(float) + sizeof(uint32_t);
            if (len < kRequiredLen) {
                rc = ResultCode::InvalidArgument;
                break;
            }
            size_t offset = 3;
            MotorConfigPayload cfg{};
            cfg.pwm_gpio = static_cast<gpio_num_t>(payload[offset++]);
            cfg.dir_gpio = static_cast<gpio_num_t>(payload[offset++]);
            cfg.enc_a = static_cast<gpio_num_t>(payload[offset++]);
            cfg.enc_b = static_cast<gpio_num_t>(payload[offset++]);

            if (!readFloatLe(payload, len, offset, cfg.counts_per_rev) ||
                !readFloatLe(payload, len, offset, cfg.gear_ratio) ||
                !readFloatLe(payload, len, offset, cfg.output_min) ||
                !readFloatLe(payload, len, offset, cfg.output_max) ||
                !readFloatLe(payload, len, offset, cfg.vel_kp) ||
                !readFloatLe(payload, len, offset, cfg.vel_ki) ||
                !readFloatLe(payload, len, offset, cfg.vel_kd) ||
                !readFloatLe(payload, len, offset, cfg.pos_kp) ||
                !readFloatLe(payload, len, offset, cfg.pos_ki) ||
                !readFloatLe(payload, len, offset, cfg.pos_kd) ||
                !readFloatLe(payload, len, offset, cfg.velocity_feedforward) ||
                !readFloatLe(payload, len, offset, cfg.position_feedforward)) {
                rc = ResultCode::InvalidArgument;
                break;
            }

            float reserved0 = 0.0f;
            float reserved1 = 0.0f;
            if (!readFloatLe(payload, len, offset, reserved0) ||
                !readFloatLe(payload, len, offset, reserved1)) {
                rc = ResultCode::InvalidArgument;
                break;
            }

            uint32_t sample_time = 0;
            if (!readU32Le(payload, len, offset, sample_time)) {
                rc = ResultCode::InvalidArgument;
                break;
            }
            cfg.sample_time_ms = sample_time;

            if (subcommand == ConfigureSubcommand::CreateMotor) {
                rc = createMotor(motor_id, cfg);
            } else {
                rc = configureMotor(motor_id, cfg);
            }
            break;
        }

        case ConfigureSubcommand::DeleteMotor:
            rc = deleteMotor(motor_id);
            break;

        case ConfigureSubcommand::SetMode: {
            if (len < 4U) {
                rc = ResultCode::InvalidArgument;
                break;
            }
            motor_controller::ControlMode mode{};
            if (!parseControlMode(payload[3], mode)) {
                rc = ResultCode::InvalidArgument;
                break;
            }
            rc = setMode(motor_id, mode);
            break;
        }

        case ConfigureSubcommand::SetPidParams: {
            if (len < 3U + 1U + 3U * sizeof(float) + sizeof(uint32_t)) {
                rc = ResultCode::InvalidArgument;
                break;
            }
            size_t offset = 3;
            const auto pid_kind = static_cast<PidKind>(payload[offset++]);
            float kp = 0.0f;
            float ki = 0.0f;
            float kd = 0.0f;
            uint32_t sample_time = 0;
            if (!readFloatLe(payload, len, offset, kp) ||
                !readFloatLe(payload, len, offset, ki) ||
                !readFloatLe(payload, len, offset, kd) ||
                !readU32Le(payload, len, offset, sample_time)) {
                rc = ResultCode::InvalidArgument;
                break;
            }
            rc = setPidParams(motor_id, pid_kind, kp, ki, kd, sample_time);
            break;
        }

        case ConfigureSubcommand::SetSetpoint: {
            if (len < 3U + 1U + sizeof(float)) {
                rc = ResultCode::InvalidArgument;
                break;
            }
            size_t offset = 3;
            const auto kind = static_cast<SetpointKind>(payload[offset++]);
            float value = 0.0f;
            if (!readFloatLe(payload, len, offset, value)) {
                rc = ResultCode::InvalidArgument;
                break;
            }
            rc = setSetpoint(motor_id, kind, value);
            break;
        }

        case ConfigureSubcommand::StopMotor:
            rc = stopMotor(motor_id);
            break;

        case ConfigureSubcommand::ResetMotor:
            rc = resetMotor(motor_id);
            break;

        default:
            rc = ResultCode::NotSupported;
            break;
    }

    return sendConfigureResponse(subcommand, motor_id, rc);
}

ResultCode MotorManager::sendConfigureResponse(ConfigureSubcommand subcommand, uint8_t motor_id, ResultCode code)
{
    uint8_t payload[5] = {
        kProtocolVersion,
        static_cast<uint8_t>(subcommand),
        motor_id,
        static_cast<uint8_t>(code),
        0x00
    };

    const esp_err_t ret = m_uart.sendCommand(m_config.configure_response_msg_type, payload, sizeof(payload));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send configure response (msg=0x%02X): %s",
                 m_config.configure_response_msg_type, esp_err_to_name(ret));
        return ResultCode::InternalError;
    }
    return code;
}

const char *resultCodeToString(ResultCode code)
{
    switch (code) {
        case ResultCode::Ok:
            return "OK";
        case ResultCode::InvalidArgument:
            return "INVALID_ARGUMENT";
        case ResultCode::NotFound:
            return "NOT_FOUND";
        case ResultCode::AlreadyExists:
            return "ALREADY_EXISTS";
        case ResultCode::CapacityReached:
            return "CAPACITY_REACHED";
        case ResultCode::InvalidState:
            return "INVALID_STATE";
        case ResultCode::NotSupported:
            return "NOT_SUPPORTED";
        case ResultCode::InternalError:
        default:
            return "INTERNAL_ERROR";
    }
}

esp_err_t resultCodeToEspErr(ResultCode code)
{
    switch (code) {
        case ResultCode::Ok:
            return ESP_OK;
        case ResultCode::InvalidArgument:
            return ESP_ERR_INVALID_ARG;
        case ResultCode::NotFound:
            return ESP_ERR_NOT_FOUND;
        case ResultCode::AlreadyExists:
            return ESP_ERR_INVALID_STATE;
        case ResultCode::CapacityReached:
            return ESP_ERR_NO_MEM;
        case ResultCode::InvalidState:
            return ESP_ERR_INVALID_STATE;
        case ResultCode::NotSupported:
            return ESP_ERR_NOT_SUPPORTED;
        case ResultCode::InternalError:
        default:
            return ESP_FAIL;
    }
}

} // namespace motor_manager
