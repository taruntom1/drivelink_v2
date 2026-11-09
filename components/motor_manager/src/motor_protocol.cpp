#include "motor_protocol.hpp"

#include <cmath>
#include <initializer_list>

namespace motor_manager::protocol {

namespace {

using uart_comm::wire::Reader;
using uart_comm::wire::Writer;

bool allFinite(std::initializer_list<float> values)
{
    for (const float v : values) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

bool decodeControlMode(uint8_t raw, ControlModeCode &out)
{
    if (raw > static_cast<uint8_t>(ControlModeCode::PositionControl)) {
        return false;
    }
    out = static_cast<ControlModeCode>(raw);
    return true;
}

void readMotorConfig(Reader &r, MotorConfigPayload &cfg)
{
    cfg.pwm_gpio = r.readU8();
    cfg.dir_gpio = r.readU8();
    cfg.enc_a = r.readU8();
    cfg.enc_b = r.readU8();
    cfg.counts_per_rev = r.readF32Le();
    cfg.gear_ratio = r.readF32Le();
    cfg.output_min = r.readF32Le();
    cfg.output_max = r.readF32Le();
    cfg.vel_kp = r.readF32Le();
    cfg.vel_ki = r.readF32Le();
    cfg.vel_kd = r.readF32Le();
    cfg.vel_d_cutoff_hz = r.readF32Le();
    cfg.pos_kp = r.readF32Le();
    cfg.pos_ki = r.readF32Le();
    cfg.pos_kd = r.readF32Le();
    cfg.pos_d_cutoff_hz = r.readF32Le();
    cfg.velocity_feedforward = r.readF32Le();
    cfg.position_feedforward = r.readF32Le();
    cfg.vel_filter_cutoff_hz = r.readF32Le();
}

// Decode the body of a known subcommand. Every body has a fixed size and must
// match it exactly: a different length means host and firmware disagree on
// the layout. Untrusted values that cannot be clamped (non-finite floats,
// unknown enum values, unknown telemetry field bits) are rejected here, so
// nothing is applied.
DecodeStatus decodeBody(Reader &body, ConfigureRequest &out)
{
    switch (out.subcommand) {
        case ConfigureSubcommand::CreateMotor:
        case ConfigureSubcommand::ConfigureMotor:
            if (body.remaining() != kMotorConfigBodySize) {
                return DecodeStatus::BadBody;
            }
            readMotorConfig(body, out.motor);
            if (!isMotorConfigFinite(out.motor)) {
                return DecodeStatus::BadBody;
            }
            break;

        case ConfigureSubcommand::DeleteMotor:
        case ConfigureSubcommand::StopMotor:
        case ConfigureSubcommand::ResetMotor:
            break; // empty body

        case ConfigureSubcommand::SetMode:
            if (body.remaining() != kSetModeBodySize || !decodeControlMode(body.readU8(), out.mode)) {
                return DecodeStatus::BadBody;
            }
            break;

        case ConfigureSubcommand::SetPidParams:
            if (body.remaining() != kSetPidParamsBodySize) {
                return DecodeStatus::BadBody;
            }
            out.pid_kind = static_cast<PidKind>(body.readU8());
            out.kp = body.readF32Le();
            out.ki = body.readF32Le();
            out.kd = body.readF32Le();
            out.d_cutoff_hz = body.readF32Le();
            if (!isValidPidKind(out.pid_kind) || !allFinite({out.kp, out.ki, out.kd, out.d_cutoff_hz})) {
                return DecodeStatus::BadBody;
            }
            break;

        case ConfigureSubcommand::SetSetpoint:
            if (body.remaining() != kSetSetpointBodySize) {
                return DecodeStatus::BadBody;
            }
            out.setpoint_kind = static_cast<SetpointKind>(body.readU8());
            out.setpoint_value = body.readF32Le();
            if (!isValidSetpointKind(out.setpoint_kind) || !std::isfinite(out.setpoint_value)) {
                return DecodeStatus::BadBody;
            }
            break;

        case ConfigureSubcommand::SetTelemetryGlobal:
            if (body.remaining() != kSetTelemetryGlobalBodySize) {
                return DecodeStatus::BadBody;
            }
            out.telemetry_enabled = (body.readU8() != 0U);
            out.telemetry_field_mask = body.readU8();
            if ((out.telemetry_field_mask & ~kTelemetryFieldAll) != 0U) {
                return DecodeStatus::BadBody;
            }
            break;

        case ConfigureSubcommand::SetTelemetryMotor:
            if (body.remaining() != kSetTelemetryMotorBodySize) {
                return DecodeStatus::BadBody;
            }
            out.telemetry_enabled = (body.readU8() != 0U);
            out.telemetry_field_mask = body.readU8();
            out.telemetry_apply_mask = (body.readU8() != 0U);
            if ((out.telemetry_field_mask & ~kTelemetryFieldAll) != 0U) {
                return DecodeStatus::BadBody;
            }
            break;

        case ConfigureSubcommand::SetTiming:
            if (body.remaining() != kSetTimingBodySize) {
                return DecodeStatus::BadBody;
            }
            out.control_period_us = body.readU32Le();
            out.telemetry_period_us = body.readU32Le();
            break;

        default:
            return DecodeStatus::UnknownSubcommand;
    }
    return body.done() ? DecodeStatus::Ok : DecodeStatus::BadBody;
}

} // namespace

bool isMotorConfigFinite(const MotorConfigPayload &c)
{
    return allFinite({c.counts_per_rev, c.gear_ratio, c.output_min, c.output_max,
                      c.vel_kp, c.vel_ki, c.vel_kd, c.vel_d_cutoff_hz,
                      c.pos_kp, c.pos_ki, c.pos_kd, c.pos_d_cutoff_hz,
                      c.velocity_feedforward, c.position_feedforward, c.vel_filter_cutoff_hz});
}

DecodeStatus decodeConfigure(const uint8_t *payload, size_t len, ConfigureRequest &out)
{
    out = ConfigureRequest{};
    Reader r(payload, len);
    const uint8_t version = r.readU8();
    const bool version_ok = r.ok() && version == kProtocolVersion;
    if (!version_ok || len < kConfigureHeaderSize) {
        // The header layout is unknown, so subcommand and motor_id cannot be
        // echoed. seq is echoed when the version matches and it is present.
        if (version_ok && len >= 2U) {
            out.header.seq = payload[1];
        }
        return DecodeStatus::BadHeader;
    }

    out.header.seq = r.readU8();
    out.header.subcommand = r.readU8();
    out.header.motor_id = r.readU8();
    out.subcommand = static_cast<ConfigureSubcommand>(out.header.subcommand);

    Reader body(&payload[kConfigureHeaderSize], len - kConfigureHeaderSize);
    return decodeBody(body, out);
}

ResultCode resultForDecodeStatus(DecodeStatus status)
{
    switch (status) {
        case DecodeStatus::Ok:
            return ResultCode::Ok;
        case DecodeStatus::UnknownSubcommand:
            return ResultCode::NotSupported;
        case DecodeStatus::BadHeader:
        case DecodeStatus::BadBody:
        default:
            return ResultCode::InvalidArgument;
    }
}

size_t encodeConfigureResponse(const ConfigureHeader &header, ResultCode result,
                               const uint8_t *body, size_t body_len,
                               uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    w.appendU8(kProtocolVersion);
    w.appendU8(header.seq);
    w.appendU8(header.subcommand);
    w.appendU8(header.motor_id);
    w.appendU8(static_cast<uint8_t>(result));
    w.appendBytes(body, body_len);
    return w.ok() ? w.size() : 0U;
}

size_t encodeSetTimingResponseBody(uint32_t control_period_us, uint32_t telemetry_period_us,
                                   uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    w.appendU32Le(control_period_us);
    w.appendU32Le(telemetry_period_us);
    return w.ok() ? w.size() : 0U;
}

bool decodeSetpointStreamHeader(const uint8_t *payload, size_t len, uint8_t &entry_count)
{
    Reader r(payload, len);
    const uint8_t version = r.readU8();
    const uint8_t count = r.readU8();
    if (!r.ok() || version != kProtocolVersion) {
        return false;
    }
    // Exact match: trailing bytes mean the host and firmware disagree on the layout.
    if (len != kSetpointStreamHeaderSize + static_cast<size_t>(count) * kSetpointEntrySize) {
        return false;
    }
    entry_count = count;
    return true;
}

SetpointEntry decodeSetpointEntry(const uint8_t *payload, size_t len, uint8_t index)
{
    SetpointEntry entry{};
    const size_t offset = kSetpointStreamHeaderSize + static_cast<size_t>(index) * kSetpointEntrySize;
    if (payload == nullptr || offset > len) {
        return entry;
    }
    Reader r(&payload[offset], len - offset);
    const uint8_t motor_id = r.readU8();
    const uint8_t kind = r.readU8();
    const float value = r.readF32Le();
    if (r.ok()) {
        entry.motor_id = motor_id;
        entry.kind = kind;
        entry.value = value;
    }
    return entry;
}

size_t telemetryEntrySize(uint8_t field_mask)
{
    size_t size = kTelemetryEntryBaseSize;
    if ((field_mask & kTelemetryFieldAngle) != 0U) {
        size += 4U;
    }
    if ((field_mask & kTelemetryFieldVelocity) != 0U) {
        size += 4U;
    }
    if ((field_mask & kTelemetryFieldOutput) != 0U) {
        size += 4U;
    }
    return size;
}

TelemetryEncoder::TelemetryEncoder(uint8_t *out, size_t cap, uint64_t timestamp_us)
    : m_writer(out, cap)
{
    m_writer.appendU8(kProtocolVersion);
    m_writer.appendU8(0); // entry count, patched by finish()
    m_writer.appendU64Le(timestamp_us);
}

bool TelemetryEncoder::append(const TelemetryEntry &entry)
{
    if (!m_writer.ok() || m_count == UINT8_MAX ||
        telemetryEntrySize(entry.field_mask) > m_writer.remaining()) {
        return false;
    }
    m_writer.appendU8(entry.motor_id);
    m_writer.appendU8(entry.field_mask);
    if ((entry.field_mask & kTelemetryFieldAngle) != 0U) {
        m_writer.appendF32Le(entry.angle_rad);
    }
    if ((entry.field_mask & kTelemetryFieldVelocity) != 0U) {
        m_writer.appendF32Le(entry.velocity_rad_s);
    }
    if ((entry.field_mask & kTelemetryFieldOutput) != 0U) {
        m_writer.appendF32Le(entry.output);
    }
    ++m_count;
    return true;
}

size_t TelemetryEncoder::finish()
{
    m_writer.patchU8(1, m_count);
    if (!m_writer.ok() || m_writer.size() > UINT8_MAX) {
        return 0U;
    }
    return m_writer.size();
}

} // namespace motor_manager::protocol
