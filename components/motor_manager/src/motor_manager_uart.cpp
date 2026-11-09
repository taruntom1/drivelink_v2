#include "motor_manager.hpp"

#include "diag.hpp"
#include "esp_log.h"
#include "motor_protocol.hpp"

static const char *const TAG = "motor_mgr_uart";

namespace motor_manager {

namespace {

motor_controller::ControlMode toControlMode(ControlModeCode code)
{
    switch (code) {
        case ControlModeCode::VelocityControl:
            return motor_controller::ControlMode::VelocityControl;
        case ControlModeCode::PositionControl:
            return motor_controller::ControlMode::PositionControl;
        case ControlModeCode::OpenLoop:
        default:
            return motor_controller::ControlMode::OpenLoop;
    }
}

} // namespace

void MotorManager::handleSetpointStream(const uart_comm::RxFrame &frame)
{
    if (!isValid()) {
        return;
    }

    // This runs on the UART RX task (a real-time path): problems are counted,
    // not logged. diag sends the counts to the host.
    uint8_t count = 0;
    if (!protocol::decodeSetpointStreamHeader(frame.payload, frame.len, count)) {
        m_bad_setpoint_frames.fetch_add(1U, std::memory_order_relaxed);
        DIAG_COUNT(diag::codes::motor_manager::kSetpointBadFrame);
        return;
    }

    // With the update task running, setpoints go into the mailbox and are
    // applied on its next tick, so this RX-task callback never waits on
    // m_mutex. Without it, apply them directly as before.
    const bool use_mailbox = m_update_task.isRunning();

    for (uint8_t i = 0; i < count; ++i) {
        const protocol::SetpointEntry entry = protocol::decodeSetpointEntry(frame.payload, frame.len, i);
        const auto kind = static_cast<SetpointKind>(entry.kind);
        const ResultCode rc = use_mailbox ? queueSetpoint(entry.motor_id, kind, entry.value)
                                          : setSetpoint(entry.motor_id, kind, entry.value);
        if (rc != ResultCode::Ok) {
            m_rejected_setpoints.fetch_add(1U, std::memory_order_relaxed);
            DIAG_COUNT(diag::codes::motor_manager::kSetpointRejected);
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
        // RX task: count, don't log.
        DIAG_COUNT(diag::codes::motor_manager::kConfigQueueFull);
    }
}

ResultCode MotorManager::parseAndDispatchConfigure(const uint8_t *payload, uint8_t len)
{
    protocol::ConfigureRequest req{};
    const protocol::DecodeStatus status = protocol::decodeConfigure(payload, len, req);
    const protocol::ConfigureHeader &hdr = req.header;

    if (status != protocol::DecodeStatus::Ok) {
        if (status == protocol::DecodeStatus::BadHeader) {
            ESP_LOGW(TAG, "Configure header rejected: len=%u version=%u expected=%u",
                     len, (len >= 1U) ? payload[0] : 0U, kProtocolVersion);
        } else {
            ESP_LOGD(TAG, "Configure subcommand 0x%02X rejected (len=%u)", hdr.subcommand, len);
        }
        return sendConfigureResponse(hdr.seq, hdr.subcommand, hdr.motor_id,
                                     protocol::resultForDecodeStatus(status));
    }

    ResultCode rc = ResultCode::InvalidArgument;
    uint8_t resp_body[kSetTimingResponseBodySize] = {};
    size_t resp_body_len = 0;

    switch (req.subcommand) {
        case ConfigureSubcommand::CreateMotor:
            rc = createMotor(hdr.motor_id, req.motor);
            break;
        case ConfigureSubcommand::ConfigureMotor:
            rc = configureMotor(hdr.motor_id, req.motor);
            break;
        case ConfigureSubcommand::DeleteMotor:
            rc = deleteMotor(hdr.motor_id);
            break;
        case ConfigureSubcommand::SetMode:
            rc = setMode(hdr.motor_id, toControlMode(req.mode));
            break;
        case ConfigureSubcommand::SetPidParams:
            rc = setPidParams(hdr.motor_id, req.pid_kind, req.kp, req.ki, req.kd, req.d_cutoff_hz);
            break;
        case ConfigureSubcommand::SetSetpoint:
            rc = setSetpoint(hdr.motor_id, req.setpoint_kind, req.setpoint_value);
            break;
        case ConfigureSubcommand::StopMotor:
            rc = stopMotor(hdr.motor_id);
            break;
        case ConfigureSubcommand::ResetMotor:
            rc = resetMotor(hdr.motor_id);
            break;
        case ConfigureSubcommand::SetTelemetryGlobal:
            rc = setTelemetryGlobal(req.telemetry_enabled, req.telemetry_field_mask);
            break;
        case ConfigureSubcommand::SetTelemetryMotor:
            rc = setTelemetryForMotor(hdr.motor_id, req.telemetry_enabled, req.telemetry_field_mask,
                                      req.telemetry_apply_mask);
            break;
        case ConfigureSubcommand::SetTiming: {
            // Global: motor_id is ignored (and echoed back unchanged).
            uint32_t applied_control_us = 0;
            uint32_t applied_telemetry_us = 0;
            rc = setTiming(req.control_period_us, req.telemetry_period_us,
                           applied_control_us, applied_telemetry_us);
            if (rc == ResultCode::Ok) {
                resp_body_len = protocol::encodeSetTimingResponseBody(applied_control_us, applied_telemetry_us,
                                                                      resp_body, sizeof(resp_body));
            }
            break;
        }
        default:
            rc = ResultCode::NotSupported; // unreachable: the decoder rejects unknown subcommands
            break;
    }

    if (rc == ResultCode::InvalidArgument) {
        ESP_LOGD(TAG, "Configure subcommand 0x%02X rejected by the manager", hdr.subcommand);
    }
    return sendConfigureResponse(hdr.seq, hdr.subcommand, hdr.motor_id, rc, resp_body, resp_body_len);
}

ResultCode MotorManager::sendConfigureResponse(uint8_t seq, uint8_t subcommand, uint8_t motor_id, ResultCode code,
                                               const uint8_t *body, size_t body_len)
{
    uint8_t payload[kConfigureResponseHeaderSize + kSetTimingResponseBodySize] = {};
    protocol::ConfigureHeader hdr{};
    hdr.seq = seq;
    hdr.subcommand = subcommand;
    hdr.motor_id = motor_id;
    const size_t payload_len =
        protocol::encodeConfigureResponse(hdr, code, body, body_len, payload, sizeof(payload));
    if (payload_len == 0U) {
        ESP_LOGE(TAG, "Configure response body too large (%u bytes)", static_cast<unsigned>(body_len));
        return ResultCode::InternalError;
    }

    const esp_err_t ret = m_uart.sendCommand(m_config.configure_response_msg_type, payload,
                                             static_cast<uint8_t>(payload_len));
    if (ret != ESP_OK) {
        // The command's own result stands: it may have been applied.
        DIAG_COUNT(diag::codes::motor_manager::kResponseTxDropped);
        ESP_LOGW(TAG, "Failed to send configure response (msg=0x%02X result=%s): %s",
                 m_config.configure_response_msg_type, resultCodeToString(code), esp_err_to_name(ret));
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
