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
        const bool applied = use_mailbox ? (queueSetpoint(entry.motor_id, kind, entry.value) == ResultCode::Ok)
                                         : setSetpoint(entry.motor_id, kind, entry.value).ok();
        if (!applied) {
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
        // RX task: count, don't log. Answer so the host is not left waiting:
        // the command was not applied.
        DIAG_COUNT(diag::codes::motor_manager::kConfigQueueFull);
        const protocol::ConfigureHeader hdr = protocol::decodeConfigureHeader(frame.payload, frame.len);
        (void)sendConfigureResponse(hdr.seq, hdr.subcommand, hdr.motor_id,
                                    Result{ResultCode::InvalidState, ResultDetail::QueueFull});
    }
}

Result MotorManager::parseAndDispatchConfigure(const uint8_t *payload, uint8_t len)
{
    protocol::ConfigureRequest req{};
    const Result decode_rc = protocol::decodeConfigure(payload, len, req);
    const protocol::ConfigureHeader &hdr = req.header;

    if (!decode_rc.ok()) {
        if (decode_rc.detail == ResultDetail::BadVersion || len < kConfigureHeaderSize) {
            ESP_LOGW(TAG, "Configure header rejected: len=%u version=%u expected=%u",
                     len, (len >= 1U) ? payload[0] : 0U, kProtocolVersion);
        } else {
            ESP_LOGD(TAG, "Configure subcommand 0x%02X rejected (len=%u): %s", hdr.subcommand, len,
                     resultDetailToString(decode_rc.detail));
        }
        return sendConfigureResponse(hdr.seq, hdr.subcommand, hdr.motor_id, decode_rc);
    }

    Result rc{};
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
            if (rc.ok()) {
                resp_body_len = protocol::encodeSetTimingResponseBody(applied_control_us, applied_telemetry_us,
                                                                      resp_body, sizeof(resp_body));
            }
            break;
        }
        default:
            // unreachable: the decoder rejects unknown subcommands
            rc = Result{ResultCode::NotSupported, ResultDetail::UnknownSubcommand};
            break;
    }

    if (rc.code == ResultCode::InvalidArgument) {
        ESP_LOGD(TAG, "Configure subcommand 0x%02X rejected by the manager: %s", hdr.subcommand,
                 resultDetailToString(rc.detail));
    }
    return sendConfigureResponse(hdr.seq, hdr.subcommand, hdr.motor_id, rc, resp_body, resp_body_len);
}

Result MotorManager::sendConfigureResponse(uint8_t seq, uint8_t subcommand, uint8_t motor_id, Result result,
                                           const uint8_t *body, size_t body_len)
{
    // Also called on the RX task (config queue full): no logging before the
    // send, and sendCommand() does not block.
    uint8_t payload[kConfigureResponseHeaderSize + kSetTimingResponseBodySize] = {};
    protocol::ConfigureHeader hdr{};
    hdr.seq = seq;
    hdr.subcommand = subcommand;
    hdr.motor_id = motor_id;
    const size_t payload_len =
        protocol::encodeConfigureResponse(hdr, result, body, body_len, payload, sizeof(payload));
    if (payload_len == 0U) {
        ESP_LOGE(TAG, "Configure response body too large (%u bytes)", static_cast<unsigned>(body_len));
        return Result{ResultCode::InternalError, ResultDetail::None};
    }

    const esp_err_t ret = m_uart.sendCommand(m_config.configure_response_msg_type, payload,
                                             static_cast<uint8_t>(payload_len));
    if (ret != ESP_OK) {
        // The command's own result stands: it may have been applied. Counted
        // only: this may run on the RX task.
        DIAG_COUNT(diag::codes::motor_manager::kResponseTxDropped);
    }
    return result;
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

const char *resultDetailToString(ResultDetail detail)
{
    switch (detail) {
        case ResultDetail::None:
            return "none";
        case ResultDetail::BadVersion:
            return "bad version";
        case ResultDetail::BadLength:
            return "bad length";
        case ResultDetail::UnknownSubcommand:
            return "unknown subcommand";
        case ResultDetail::NonFiniteFloat:
            return "non-finite float";
        case ResultDetail::BadEnum:
            return "bad enum value";
        case ResultDetail::BadFieldMask:
            return "bad field mask";
        case ResultDetail::BadGpio:
            return "bad GPIO";
        case ResultDetail::GpioInUse:
            return "GPIO in use";
        case ResultDetail::UnknownMotor:
            return "unknown motor";
        case ResultDetail::MotorExists:
            return "motor exists";
        case ResultDetail::MotorLimit:
            return "motor limit";
        case ResultDetail::NotReady:
            return "not ready";
        case ResultDetail::QueueFull:
            return "queue full";
        case ResultDetail::LockTimeout:
            return "lock timeout";
        case ResultDetail::MotorDriverFailed:
            return "motor driver failed";
        case ResultDetail::EncoderFailed:
            return "encoder failed";
        case ResultDetail::TimerFailed:
            return "timer failed";
        default:
            return "?";
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
