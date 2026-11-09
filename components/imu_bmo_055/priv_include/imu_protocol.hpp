#pragma once

/// IMU payload codec: pure byte-in/byte-out functions with no ESP-IDF or
/// FreeRTOS dependency, so the host unit tests cover them. The manager
/// decodes and encodes through these and keeps only the dispatch and state.

extern "C" {
#include "bno055.h"
}

#include "imu_bmo_055_protocol.hpp"

#include <cstddef>
#include <cstdint>

namespace imu_bmo_055 {

/// One set of sensor readings, as sent in a stream packet. Only the fields
/// selected by the stream field mask are filled in and encoded.
struct ImuStreamSample {
    uint64_t timestamp_us = 0;

    bno055_euler_t euler{};
    bno055_quaternion_t quaternion{};
    bno055_linear_accel_t linear_accel{};
    bno055_gravity_t gravity{};
    bno055_accel_t raw_accel{};
    bno055_gyro_t raw_gyro{};
    bno055_mag_t raw_mag{};
    int8_t temperature = 0;
    CalibrationStatus calibration_status{};
};

namespace protocol {

/// Echoed in the configure response. When the header could not be parsed,
/// subcommand is kUnparsedHeaderByte, and seq is echoed only if the version
/// matched and a seq byte was present (0 otherwise).
struct ConfigureHeader {
    uint8_t seq = 0;
    uint8_t subcommand = kUnparsedHeaderByte;
};

/// Decode the configure request header (version, seq, subcommand).
/// @param[out] header  Always filled in, so a response can be sent.
/// @param[out] body, body_len  The bytes after the header, on success.
/// @return false if the payload is too short or has the wrong version.
bool decodeConfigureHeader(const uint8_t *payload, size_t len, ConfigureHeader &header,
                           const uint8_t *&body, size_t &body_len);

// Body decoders. Each requires the exact body size and rejects values outside
// the allowed set; on failure `out` is left unchanged.
bool decodeSetStreamEnabled(const uint8_t *body, size_t len, bool &enabled);
bool decodeSetStreamTiming(const uint8_t *body, size_t len, uint32_t &sample_period_us,
                           uint32_t &telemetry_period_us);
bool decodeSetStreamFieldMask(const uint8_t *body, size_t len, uint16_t &field_mask);
bool decodeSetOperationMode(const uint8_t *body, size_t len, uint8_t &mode);

/// Decode an exactly kCalibrationBlobSize-byte blob. Rejects a has_sic byte
/// other than 0 or 1.
bool decodeCalibrationBlob(const uint8_t *body, size_t len, CalibrationBlob &out);

/// @return kCalibrationBlobSize, or 0 if it does not fit in `cap`.
size_t encodeCalibrationBlob(const CalibrationBlob &blob, uint8_t *out, size_t cap);

/// @return kSetStreamTimingResponseBodySize, or 0 if it does not fit in `cap`.
size_t encodeSetStreamTimingResponseBody(uint32_t sample_period_us, uint32_t telemetry_period_us,
                                         uint8_t *out, size_t cap);

/// Encode a configure response: the 4-byte header, then `body` (may be empty).
/// @return The payload length, or 0 if it does not fit in `cap`.
size_t encodeConfigureResponse(const ConfigureHeader &header, ResultCode result,
                               const uint8_t *body, size_t body_len, uint8_t *out, size_t cap);

/// Encode one stream packet with the fields in `field_mask`.
/// @return The payload length, or 0 if it does not fit in `cap` or is
///         longer than 255 bytes.
size_t encodeStreamSample(const ImuStreamSample &sample, uint16_t field_mask, uint8_t *out, size_t cap);

} // namespace protocol
} // namespace imu_bmo_055
