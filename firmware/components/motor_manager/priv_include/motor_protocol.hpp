#pragma once

/// motor_manager payload codec: pure byte-in/byte-out functions with no
/// ESP-IDF or FreeRTOS dependency, so the host unit tests cover them.
/// MotorManager decodes and encodes through these and keeps only the
/// dispatch and the state.

#include "motor_manager_protocol.hpp"
#include "drivelink/protocol/wire_codec.hpp"

#include <cstddef>
#include <cstdint>

namespace motor_manager::protocol {

/// Echoed in the configure response. When the header could not be parsed,
/// subcommand and motor_id are kUnparsedHeaderByte, and seq is echoed only if
/// the version matched and a seq byte was present (0 otherwise).
struct ConfigureHeader {
    uint8_t seq = 0;
    uint8_t subcommand = kUnparsedHeaderByte;
    uint8_t motor_id = kUnparsedHeaderByte;
};

/// A decoded configure request. Only the fields of header.subcommand are set.
struct ConfigureRequest {
    ConfigureHeader header{};
    ConfigureSubcommand subcommand = ConfigureSubcommand::CreateMotor;

    MotorConfigPayload motor{};                        ///< CreateMotor, ConfigureMotor
    ControlModeCode mode = ControlModeCode::OpenLoop;  ///< SetMode
    PidKind pid_kind = PidKind::Velocity;              ///< SetPidParams
    float kp = 0.0f;
    float ki = 0.0f;
    float kd = 0.0f;
    float d_cutoff_hz = 0.0f;
    SetpointKind setpoint_kind = SetpointKind::OpenLoopDuty; ///< SetSetpoint
    float setpoint_value = 0.0f;
    bool telemetry_enabled = false;                    ///< SetTelemetryGlobal, SetTelemetryMotor
    uint8_t telemetry_field_mask = 0;
    bool telemetry_apply_mask = false;                 ///< SetTelemetryMotor
    uint32_t control_period_us = 0;                    ///< SetTiming
    uint32_t telemetry_period_us = 0;
};

/// Decode a configure request payload. out.header is always filled in, so a
/// response can be sent for every result. On failure the result is
/// NotSupported / UnknownSubcommand, or InvalidArgument with the reason
/// (BadVersion, BadLength, NonFiniteFloat, BadEnum, BadFieldMask).
Result decodeConfigure(const uint8_t *payload, size_t len, ConfigureRequest &out);

/// Decode only the echo bytes of the header, as decodeConfigure() does. Used
/// to answer a command that is not dispatched (config queue full).
ConfigureHeader decodeConfigureHeader(const uint8_t *payload, size_t len);

/// True if every float in a CreateMotor / ConfigureMotor body is finite.
bool isMotorConfigFinite(const MotorConfigPayload &cfg);

/// Encode a configure response: the 6-byte header, then `body` (may be empty).
/// @return The payload length, or 0 if it does not fit in `cap`.
size_t encodeConfigureResponse(const ConfigureHeader &header, Result result,
                               const uint8_t *body, size_t body_len,
                               uint8_t *out, size_t cap);

/// Encode the SetTiming response body (applied control and telemetry periods).
/// @return kSetTimingResponseBodySize, or 0 if it does not fit in `cap`.
size_t encodeSetTimingResponseBody(uint32_t control_period_us, uint32_t telemetry_period_us,
                                   uint8_t *out, size_t cap);

struct SetpointEntry {
    uint8_t motor_id = 0;
    uint8_t kind = 0; ///< raw SetpointKind byte, not validated
    float value = 0.0f;
};

/// Check a setpoint stream frame: the version must match and the length must
/// be exactly 2 + entry_count * kSetpointEntrySize. A frame that fails is
/// dropped as a whole.
bool decodeSetpointStreamHeader(const uint8_t *payload, size_t len, uint8_t &entry_count);

/// Entry `index` of a frame that passed decodeSetpointStreamHeader().
SetpointEntry decodeSetpointEntry(const uint8_t *payload, size_t len, uint8_t index);

struct TelemetryEntry {
    uint8_t motor_id = 0;
    uint8_t field_mask = 0;
    float angle_rad = 0.0f;
    float velocity_rad_s = 0.0f;
    float output = 0.0f;
};

/// Encoded size of one telemetry entry with this field mask.
size_t telemetryEntrySize(uint8_t field_mask);

/// Builds one telemetry packet: header, then entries until the buffer is
/// full. Writes only into the caller's buffer (no allocation).
class TelemetryEncoder {
public:
    TelemetryEncoder(uint8_t *out, size_t cap, uint64_t timestamp_us);

    /// Append one entry. If it does not fit, nothing is written and it
    /// returns false; later, smaller entries may still fit.
    bool append(const TelemetryEntry &entry);

    /// Write the entry count into the header.
    /// @return The packet length, or 0 if the header did not fit or the
    ///         packet is longer than 255 bytes.
    size_t finish();

    uint8_t entryCount() const { return m_count; }

private:
    drivelink::protocol::wire::Writer m_writer;
    uint8_t m_count = 0;
};

} // namespace motor_manager::protocol
