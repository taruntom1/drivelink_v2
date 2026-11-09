#include "motor_manager.hpp"

#include "esp_log.h"

#include <cstring>

static const char *TAG = "motor_mgr_uart";

namespace motor_manager {

namespace {

constexpr size_t kConfigureHeaderBytes = 4U; // version + seq + subcommand + motor_id
constexpr size_t kConfigureResponseHeaderBytes = 5U; // version + seq + subcommand + motor_id + result

// Exact v3 body sizes (bytes after the configure header).
constexpr size_t kCreateConfigureGpioBytes = 4U;
constexpr size_t kCreateConfigureFloatCount = 15U;
constexpr size_t kCreateConfigureBodyBytes =
    kCreateConfigureGpioBytes + kCreateConfigureFloatCount * sizeof(float);     // 64
constexpr size_t kSetModeBodyBytes = 1U;                                        // mode
constexpr size_t kSetPidParamsBodyBytes = 1U + 4U * sizeof(float);              // kind + kp ki kd d_cutoff
constexpr size_t kSetSetpointBodyBytes = 1U + sizeof(float);                    // kind + value
constexpr size_t kSetTelemetryGlobalBodyBytes = 2U;                             // enabled + field_mask
constexpr size_t kSetTelemetryMotorBodyBytes = 3U;                              // enabled + field_mask + apply
constexpr size_t kSetTimingBodyBytes = 2U * sizeof(uint32_t);                   // control_us + telemetry_us
constexpr size_t kSetTimingResponseBodyBytes = 2U * sizeof(uint32_t);

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

    // Exact match: trailing bytes mean the host and firmware disagree on the layout.
    const size_t expected_len = 2U + static_cast<size_t>(count) * kSetpointEntrySize;
    if (frame.len != expected_len) {
        ESP_LOGW(TAG, "Setpoint stream length mismatch: len=%u expected=%u", frame.len, static_cast<unsigned>(expected_len));
        return;
    }

    // With the update task running, setpoints go into the mailbox and are
    // applied on its next tick, so this RX-task callback never waits on
    // m_mutex. Without it, apply them directly as before.
    const bool use_mailbox = m_update_task.isRunning();

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
        const ResultCode rc = use_mailbox ? queueSetpoint(motor_id, kind, value)
                                          : setSetpoint(motor_id, kind, value);
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
    // Short or wrong-version payloads are forwarded too, so the config task
    // can answer them with a "header not parsed" response.
    if (m_config_queue == nullptr) {
        return;
    }
    if (xQueueSend(m_config_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Configure queue full; dropping command");
    }
}

ResultCode MotorManager::parseAndDispatchConfigure(const uint8_t *payload, uint8_t len)
{
    if (len < kConfigureHeaderBytes || payload[0] != kProtocolVersion) {
        // The header layout is unknown, so subcommand and motor_id cannot be
        // echoed. seq is echoed when the version matches and it is present.
        const bool version_ok = (len >= 1U) && (payload[0] == kProtocolVersion);
        const uint8_t seq = (version_ok && len >= 2U) ? payload[1] : 0U;
        ESP_LOGW(TAG, "Configure header rejected: len=%u version=%u expected=%u",
                 len, (len >= 1U) ? payload[0] : 0U, kProtocolVersion);
        return sendConfigureResponse(seq, kUnparsedHeaderByte, kUnparsedHeaderByte,
                                     ResultCode::InvalidArgument);
    }

    const uint8_t seq = payload[1];
    const uint8_t subcommand_raw = payload[2];
    const auto subcommand = static_cast<ConfigureSubcommand>(subcommand_raw);
    const uint8_t motor_id = payload[3];
    // Every body has a fixed size in v3 and must match it exactly: a
    // different length means host and firmware disagree on the layout.
    const size_t body_len = len - kConfigureHeaderBytes;
    size_t offset = kConfigureHeaderBytes;

    ResultCode rc = ResultCode::InvalidArgument;
    uint8_t resp_body[kSetTimingResponseBodyBytes] = {};
    size_t resp_body_len = 0;

    switch (subcommand) {
        case ConfigureSubcommand::CreateMotor:
        case ConfigureSubcommand::ConfigureMotor: {
            if (body_len != kCreateConfigureBodyBytes) {
                break;
            }
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
                !readFloatLe(payload, len, offset, cfg.vel_d_cutoff_hz) ||
                !readFloatLe(payload, len, offset, cfg.pos_kp) ||
                !readFloatLe(payload, len, offset, cfg.pos_ki) ||
                !readFloatLe(payload, len, offset, cfg.pos_kd) ||
                !readFloatLe(payload, len, offset, cfg.pos_d_cutoff_hz) ||
                !readFloatLe(payload, len, offset, cfg.velocity_feedforward) ||
                !readFloatLe(payload, len, offset, cfg.position_feedforward) ||
                !readFloatLe(payload, len, offset, cfg.vel_filter_cutoff_hz)) {
                break;
            }

            if (subcommand == ConfigureSubcommand::CreateMotor) {
                rc = createMotor(motor_id, cfg);
            } else {
                rc = configureMotor(motor_id, cfg);
            }
            break;
        }

        case ConfigureSubcommand::DeleteMotor:
            if (body_len == 0U) {
                rc = deleteMotor(motor_id);
            }
            break;

        case ConfigureSubcommand::SetMode: {
            if (body_len != kSetModeBodyBytes) {
                break;
            }
            motor_controller::ControlMode mode{};
            if (!parseControlMode(payload[offset], mode)) {
                break;
            }
            rc = setMode(motor_id, mode);
            break;
        }

        case ConfigureSubcommand::SetPidParams: {
            if (body_len != kSetPidParamsBodyBytes) {
                break;
            }
            const auto pid_kind = static_cast<PidKind>(payload[offset++]);
            float kp = 0.0f;
            float ki = 0.0f;
            float kd = 0.0f;
            float d_cutoff_hz = 0.0f;
            if (!readFloatLe(payload, len, offset, kp) ||
                !readFloatLe(payload, len, offset, ki) ||
                !readFloatLe(payload, len, offset, kd) ||
                !readFloatLe(payload, len, offset, d_cutoff_hz)) {
                break;
            }
            rc = setPidParams(motor_id, pid_kind, kp, ki, kd, d_cutoff_hz);
            break;
        }

        case ConfigureSubcommand::SetSetpoint: {
            if (body_len != kSetSetpointBodyBytes) {
                break;
            }
            const auto kind = static_cast<SetpointKind>(payload[offset++]);
            float value = 0.0f;
            if (!readFloatLe(payload, len, offset, value)) {
                break;
            }
            rc = setSetpoint(motor_id, kind, value);
            break;
        }

        case ConfigureSubcommand::StopMotor:
            if (body_len == 0U) {
                rc = stopMotor(motor_id);
            }
            break;

        case ConfigureSubcommand::ResetMotor:
            if (body_len == 0U) {
                rc = resetMotor(motor_id);
            }
            break;

        case ConfigureSubcommand::SetTelemetryGlobal: {
            if (body_len != kSetTelemetryGlobalBodyBytes) {
                break;
            }
            const bool enabled = (payload[offset++] != 0U);
            const uint8_t field_mask = payload[offset++];
            rc = setTelemetryGlobal(enabled, field_mask);
            break;
        }

        case ConfigureSubcommand::SetTelemetryMotor: {
            if (body_len != kSetTelemetryMotorBodyBytes) {
                break;
            }
            const bool enabled = (payload[offset++] != 0U);
            const uint8_t field_mask = payload[offset++];
            const bool update_field_mask = (payload[offset++] != 0U);
            rc = setTelemetryForMotor(motor_id, enabled, field_mask, update_field_mask);
            break;
        }

        case ConfigureSubcommand::SetTiming: {
            // Global: motor_id is ignored (and echoed back unchanged).
            if (body_len != kSetTimingBodyBytes) {
                break;
            }
            uint32_t control_us = 0;
            uint32_t telemetry_us = 0;
            if (!readU32Le(payload, len, offset, control_us) ||
                !readU32Le(payload, len, offset, telemetry_us)) {
                break;
            }
            uint32_t applied_control_us = 0;
            uint32_t applied_telemetry_us = 0;
            rc = setTiming(control_us, telemetry_us, applied_control_us, applied_telemetry_us);
            if (rc == ResultCode::Ok) {
                std::memcpy(&resp_body[0], &applied_control_us, sizeof(uint32_t));
                std::memcpy(&resp_body[sizeof(uint32_t)], &applied_telemetry_us, sizeof(uint32_t));
                resp_body_len = kSetTimingResponseBodyBytes;
            }
            break;
        }

        default:
            rc = ResultCode::NotSupported;
            break;
    }

    if (rc == ResultCode::InvalidArgument) {
        ESP_LOGD(TAG, "Configure subcommand 0x%02X rejected (body_len=%u)",
                 subcommand_raw, static_cast<unsigned>(body_len));
    }
    return sendConfigureResponse(seq, subcommand_raw, motor_id, rc, resp_body, resp_body_len);
}

ResultCode MotorManager::sendConfigureResponse(uint8_t seq, uint8_t subcommand, uint8_t motor_id, ResultCode code,
                                               const uint8_t *body, size_t body_len)
{
    uint8_t payload[kConfigureResponseHeaderBytes + kSetTimingResponseBodyBytes] = {
        kProtocolVersion,
        seq,
        subcommand,
        motor_id,
        static_cast<uint8_t>(code)
    };
    if (body_len > (sizeof(payload) - kConfigureResponseHeaderBytes) || (body_len > 0U && body == nullptr)) {
        ESP_LOGE(TAG, "Configure response body too large (%u bytes)", static_cast<unsigned>(body_len));
        return ResultCode::InternalError;
    }
    if (body_len > 0U) {
        std::memcpy(&payload[kConfigureResponseHeaderBytes], body, body_len);
    }
    const auto payload_len = static_cast<uint8_t>(kConfigureResponseHeaderBytes + body_len);

    const esp_err_t ret = m_uart.sendCommand(m_config.configure_response_msg_type, payload, payload_len);
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
