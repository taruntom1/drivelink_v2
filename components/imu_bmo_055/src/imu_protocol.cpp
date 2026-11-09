#include "imu_protocol.hpp"

#include "wire_codec.hpp"

namespace imu_bmo_055::protocol {

namespace {

using uart_comm::wire::Reader;
using uart_comm::wire::Writer;

bool hasField(uint16_t field_mask, StreamField field)
{
    return (field_mask & static_cast<uint16_t>(field)) != 0U;
}

} // namespace

bool decodeConfigureHeader(const uint8_t *payload, size_t len, ConfigureHeader &header,
                           const uint8_t *&body, size_t &body_len)
{
    header = ConfigureHeader{};
    body = nullptr;
    body_len = 0;

    Reader r(payload, len);
    const uint8_t version = r.readU8();
    const bool version_ok = r.ok() && version == kProtocolVersion;
    if (!version_ok || len < kConfigureRequestHeaderSize) {
        // The header layout is unknown, so the subcommand cannot be echoed.
        // seq is echoed when the version matches and it is present.
        if (version_ok && len >= 2U) {
            header.seq = payload[1];
        }
        return false;
    }

    header.seq = r.readU8();
    header.subcommand = r.readU8();
    body = &payload[kConfigureRequestHeaderSize];
    body_len = len - kConfigureRequestHeaderSize;
    return true;
}

bool decodeSetStreamEnabled(const uint8_t *body, size_t len, bool &enabled)
{
    Reader r(body, len);
    const uint8_t raw = r.readU8();
    if (len != kSetStreamEnabledBodySize || !r.done() || raw > 1U) {
        return false;
    }
    enabled = (raw != 0U);
    return true;
}

bool decodeSetStreamTiming(const uint8_t *body, size_t len, uint32_t &sample_period_us,
                           uint32_t &telemetry_period_us)
{
    Reader r(body, len);
    const uint32_t sample = r.readU32Le();
    const uint32_t telemetry = r.readU32Le();
    if (len != kSetStreamTimingBodySize || !r.done()) {
        return false;
    }
    sample_period_us = sample;
    telemetry_period_us = telemetry;
    return true;
}

bool decodeSetStreamFieldMask(const uint8_t *body, size_t len, uint16_t &field_mask)
{
    Reader r(body, len);
    const uint16_t mask = r.readU16Le();
    if (len != kSetStreamFieldMaskBodySize || !r.done()) {
        return false;
    }
    field_mask = mask;
    return true;
}

bool decodeSetOperationMode(const uint8_t *body, size_t len, uint8_t &mode)
{
    Reader r(body, len);
    const uint8_t raw = r.readU8();
    if (len != kSetOperationModeBodySize || !r.done()) {
        return false;
    }
    mode = raw;
    return true;
}

bool decodeCalibrationBlob(const uint8_t *body, size_t len, CalibrationBlob &out)
{
    if (len != kCalibrationBlobSize) {
        return false;
    }

    Reader r(body, len);
    const uint8_t has_sic = r.readU8();
    if (has_sic > 1U) {
        return false;
    }

    CalibrationBlob blob{};
    blob.has_sic = (has_sic != 0U);
    blob.accel_offset_x = r.readI16Le();
    blob.accel_offset_y = r.readI16Le();
    blob.accel_offset_z = r.readI16Le();
    blob.accel_radius = r.readI16Le();

    blob.gyro_offset_x = r.readI16Le();
    blob.gyro_offset_y = r.readI16Le();
    blob.gyro_offset_z = r.readI16Le();

    blob.mag_offset_x = r.readI16Le();
    blob.mag_offset_y = r.readI16Le();
    blob.mag_offset_z = r.readI16Le();
    blob.mag_radius = r.readI16Le();

    for (int16_t &v : blob.sic) {
        v = r.readI16Le();
    }

    if (!r.done()) {
        return false;
    }
    out = blob;
    return true;
}

size_t encodeCalibrationBlob(const CalibrationBlob &blob, uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    w.appendU8(blob.has_sic ? 1U : 0U);
    w.appendI16Le(blob.accel_offset_x);
    w.appendI16Le(blob.accel_offset_y);
    w.appendI16Le(blob.accel_offset_z);
    w.appendI16Le(blob.accel_radius);

    w.appendI16Le(blob.gyro_offset_x);
    w.appendI16Le(blob.gyro_offset_y);
    w.appendI16Le(blob.gyro_offset_z);

    w.appendI16Le(blob.mag_offset_x);
    w.appendI16Le(blob.mag_offset_y);
    w.appendI16Le(blob.mag_offset_z);
    w.appendI16Le(blob.mag_radius);

    for (const int16_t v : blob.sic) {
        w.appendI16Le(v);
    }
    return w.ok() ? w.size() : 0U;
}

size_t encodeSetStreamTimingResponseBody(uint32_t sample_period_us, uint32_t telemetry_period_us,
                                         uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    w.appendU32Le(sample_period_us);
    w.appendU32Le(telemetry_period_us);
    return w.ok() ? w.size() : 0U;
}

size_t encodeConfigureResponse(const ConfigureHeader &header, ResultCode result,
                               const uint8_t *body, size_t body_len, uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    w.appendU8(kProtocolVersion);
    w.appendU8(header.seq);
    w.appendU8(header.subcommand);
    w.appendU8(static_cast<uint8_t>(result));
    w.appendBytes(body, body_len);
    return w.ok() ? w.size() : 0U;
}

size_t encodeStreamSample(const ImuStreamSample &sample, uint16_t field_mask, uint8_t *out, size_t cap)
{
    Writer w(out, cap);
    w.appendU8(kProtocolVersion);
    w.appendU16Le(field_mask);
    w.appendU64Le(sample.timestamp_us);

    const auto append_xyz = [&w](int16_t x, int16_t y, int16_t z) {
        w.appendI16Le(x);
        w.appendI16Le(y);
        w.appendI16Le(z);
    };

    if (hasField(field_mask, StreamField::Euler)) {
        append_xyz(sample.euler.h, sample.euler.r, sample.euler.p);
    }
    if (hasField(field_mask, StreamField::Quaternion)) {
        w.appendI16Le(sample.quaternion.w);
        w.appendI16Le(sample.quaternion.x);
        w.appendI16Le(sample.quaternion.y);
        w.appendI16Le(sample.quaternion.z);
    }
    if (hasField(field_mask, StreamField::LinearAccel)) {
        append_xyz(sample.linear_accel.x, sample.linear_accel.y, sample.linear_accel.z);
    }
    if (hasField(field_mask, StreamField::Gravity)) {
        append_xyz(sample.gravity.x, sample.gravity.y, sample.gravity.z);
    }
    if (hasField(field_mask, StreamField::RawAccel)) {
        append_xyz(sample.raw_accel.x, sample.raw_accel.y, sample.raw_accel.z);
    }
    if (hasField(field_mask, StreamField::RawGyro)) {
        append_xyz(sample.raw_gyro.x, sample.raw_gyro.y, sample.raw_gyro.z);
    }
    if (hasField(field_mask, StreamField::RawMag)) {
        append_xyz(sample.raw_mag.x, sample.raw_mag.y, sample.raw_mag.z);
    }
    if (hasField(field_mask, StreamField::Temperature)) {
        w.appendU8(static_cast<uint8_t>(sample.temperature));
    }
    if (hasField(field_mask, StreamField::CalibrationStatus)) {
        w.appendU8(sample.calibration_status.sys);
        w.appendU8(sample.calibration_status.gyro);
        w.appendU8(sample.calibration_status.accel);
        w.appendU8(sample.calibration_status.mag);
    }

    if (!w.ok() || w.size() > static_cast<size_t>(UINT8_MAX)) {
        return 0U;
    }
    return w.size();
}

} // namespace imu_bmo_055::protocol
