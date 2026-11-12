#include "doctest.h"

#include "imu_protocol.hpp"
#include "drivelink/protocol/wire_codec.hpp"

#include <array>
#include <vector>

using namespace imu_bmo_055;
using namespace imu_bmo_055::protocol;

namespace {

CalibrationBlob sampleBlob()
{
    CalibrationBlob blob{};
    blob.has_sic = true;
    blob.accel_offset_x = -1;
    blob.accel_offset_y = 2;
    blob.accel_offset_z = -300;
    blob.accel_radius = 1000;
    blob.gyro_offset_x = 4;
    blob.gyro_offset_y = -5;
    blob.gyro_offset_z = 6;
    blob.mag_offset_x = 32767;
    blob.mag_offset_y = -32768;
    blob.mag_offset_z = 7;
    blob.mag_radius = 800;
    for (int i = 0; i < 9; ++i) {
        blob.sic[i] = static_cast<int16_t>(i * 100 - 400);
    }
    return blob;
}

} // namespace

TEST_CASE("IMU configure header")
{
    ConfigureHeader hdr{};
    const uint8_t *body = nullptr;
    size_t body_len = 99;

    SUBCASE("valid header with a body")
    {
        const std::vector<uint8_t> p = {kProtocolVersion, 0x21, 0x02, 0xAA, 0xBB};
        REQUIRE(decodeConfigureHeader(p.data(), p.size(), hdr, body, body_len).ok());
        CHECK(hdr.seq == 0x21);
        CHECK(hdr.subcommand == 0x02);
        CHECK(body == &p[3]);
        CHECK(body_len == 2);
    }
    SUBCASE("valid header, empty body")
    {
        const std::vector<uint8_t> p = {kProtocolVersion, 1, 0x05};
        REQUIRE(decodeConfigureHeader(p.data(), p.size(), hdr, body, body_len).ok());
        CHECK(body_len == 0);
    }
    SUBCASE("short, version and seq present: seq echoed")
    {
        const std::vector<uint8_t> p = {kProtocolVersion, 0x33};
        CHECK(decodeConfigureHeader(p.data(), p.size(), hdr, body, body_len).detail == ResultDetail::BadLength);
        CHECK(hdr.seq == 0x33);
        CHECK(hdr.subcommand == kUnparsedHeaderByte);
    }
    SUBCASE("wrong version: nothing echoed")
    {
        const std::vector<uint8_t> p = {kProtocolVersion + 1, 0x33, 0x01};
        CHECK(decodeConfigureHeader(p.data(), p.size(), hdr, body, body_len).detail == ResultDetail::BadVersion);
        CHECK(hdr.seq == 0);
        CHECK(hdr.subcommand == kUnparsedHeaderByte);
        CHECK(body == nullptr);
        CHECK(body_len == 0);
    }
    SUBCASE("empty")
    {
        CHECK(decodeConfigureHeader(nullptr, 0, hdr, body, body_len).detail == ResultDetail::BadLength);
        CHECK(hdr.seq == 0);
    }
}

TEST_CASE("SetStreamEnabled accepts only 0 or 1, exact length")
{
    bool enabled = false;
    const uint8_t on[] = {1};
    REQUIRE(decodeSetStreamEnabled(on, 1, enabled).ok());
    CHECK(enabled);
    const uint8_t off[] = {0};
    REQUIRE(decodeSetStreamEnabled(off, 1, enabled).ok());
    CHECK_FALSE(enabled);

    const uint8_t two[] = {2};
    enabled = true;
    CHECK(decodeSetStreamEnabled(two, 1, enabled).detail == ResultDetail::BadEnum);
    CHECK(enabled); // unchanged
    const uint8_t longer[] = {1, 0};
    CHECK(decodeSetStreamEnabled(longer, 2, enabled).detail == ResultDetail::BadLength);
    CHECK(decodeSetStreamEnabled(nullptr, 0, enabled).detail == ResultDetail::BadLength);
}

TEST_CASE("SetStreamTiming request and response bodies")
{
    const std::vector<uint8_t> body = {0x10, 0x27, 0, 0, 0x20, 0x4E, 0, 0}; // 10000, 20000
    uint32_t sample = 0;
    uint32_t telemetry = 0;
    REQUIRE(decodeSetStreamTiming(body.data(), body.size(), sample, telemetry).ok());
    CHECK(sample == 10000);
    CHECK(telemetry == 20000);
    CHECK(decodeSetStreamTiming(body.data(), body.size() - 1, sample, telemetry).detail == ResultDetail::BadLength);

    std::array<uint8_t, kSetStreamTimingResponseBodySize> out{};
    REQUIRE(encodeSetStreamTimingResponseBody(10000, 20000, out.data(), out.size()) == out.size());
    CHECK(std::vector<uint8_t>(out.begin(), out.end()) == body);
    CHECK(encodeSetStreamTimingResponseBody(1, 2, out.data(), out.size() - 1) == 0);
}

TEST_CASE("SetStreamFieldMask and SetOperationMode bodies")
{
    const uint8_t mask_body[] = {0xFF, 0x01};
    uint16_t mask = 0;
    REQUIRE(decodeSetStreamFieldMask(mask_body, sizeof(mask_body), mask).ok());
    CHECK(mask == 0x01FF);
    CHECK(decodeSetStreamFieldMask(mask_body, 1, mask).detail == ResultDetail::BadLength);

    const uint8_t mode_body[] = {0x0C, 0x00};
    uint8_t mode = 0;
    REQUIRE(decodeSetOperationMode(mode_body, 1, mode).ok());
    CHECK(mode == 0x0C);
    CHECK(decodeSetOperationMode(mode_body, 2, mode).detail == ResultDetail::BadLength);
}

TEST_CASE("Calibration blob round-trips")
{
    const CalibrationBlob blob = sampleBlob();
    std::array<uint8_t, kCalibrationBlobSize> buf{};
    REQUIRE(encodeCalibrationBlob(blob, buf.data(), buf.size()) == kCalibrationBlobSize);
    CHECK(buf[0] == 1);    // has_sic
    CHECK(buf[1] == 0xFF); // accel_offset_x = -1, little-endian
    CHECK(buf[2] == 0xFF);

    CalibrationBlob out{};
    REQUIRE(decodeCalibrationBlob(buf.data(), buf.size(), out).ok());
    CHECK(out.has_sic == blob.has_sic);
    CHECK(out.accel_offset_x == blob.accel_offset_x);
    CHECK(out.accel_offset_z == blob.accel_offset_z);
    CHECK(out.accel_radius == blob.accel_radius);
    CHECK(out.gyro_offset_y == blob.gyro_offset_y);
    CHECK(out.mag_offset_x == blob.mag_offset_x);
    CHECK(out.mag_offset_y == blob.mag_offset_y);
    CHECK(out.mag_radius == blob.mag_radius);
    for (int i = 0; i < 9; ++i) {
        CHECK(out.sic[i] == blob.sic[i]);
    }
}

TEST_CASE("Calibration blob decode rejects bad input and leaves the output alone")
{
    std::array<uint8_t, kCalibrationBlobSize + 1> buf{};
    REQUIRE(encodeCalibrationBlob(sampleBlob(), buf.data(), buf.size()) == kCalibrationBlobSize);

    CalibrationBlob out{};
    out.mag_radius = 1234;
    CHECK(decodeCalibrationBlob(buf.data(), kCalibrationBlobSize - 1, out).detail == ResultDetail::BadLength);
    CHECK(decodeCalibrationBlob(buf.data(), kCalibrationBlobSize + 1, out).detail == ResultDetail::BadLength);
    buf[0] = 2; // has_sic must be 0 or 1
    CHECK(decodeCalibrationBlob(buf.data(), kCalibrationBlobSize, out).detail == ResultDetail::BadEnum);
    CHECK(out.mag_radius == 1234);

    std::array<uint8_t, kCalibrationBlobSize - 1> small{};
    CHECK(encodeCalibrationBlob(sampleBlob(), small.data(), small.size()) == 0);
}

TEST_CASE("Configure response layout")
{
    ConfigureHeader hdr{};
    hdr.seq = 7;
    hdr.subcommand = static_cast<uint8_t>(ConfigureSubcommand::ReadCalibration);
    const uint8_t body[] = {0xAA, 0xBB};
    std::array<uint8_t, 8> out{};
    CHECK(kProtocolVersion == 5);
    CHECK(kConfigureResponseHeaderSize == 5);
    REQUIRE(encodeConfigureResponse(hdr, Result{ResultCode::Busy, ResultDetail::QueueFull}, body, sizeof(body),
                                    out.data(), out.size()) == 7);
    CHECK(out[0] == kProtocolVersion);
    CHECK(out[1] == 7);
    CHECK(out[2] == 0x05);
    CHECK(out[3] == static_cast<uint8_t>(ResultCode::Busy));
    CHECK(out[4] == static_cast<uint8_t>(ResultDetail::QueueFull));
    CHECK(out[5] == 0xAA);
    CHECK(out[6] == 0xBB);

    CHECK(encodeConfigureResponse(hdr, Result{}, body, sizeof(body), out.data(), 6) == 0);
}

TEST_CASE("Stream sample: header and field order")
{
    ImuStreamSample s{};
    s.timestamp_us = 0x1122334455667788ULL;
    s.euler.h = 100;
    s.euler.r = -200;
    s.euler.p = 300;
    s.temperature = -5;
    s.calibration_status.sys = 3;
    s.calibration_status.gyro = 2;
    s.calibration_status.accel = 1;
    s.calibration_status.mag = 0;

    const uint16_t mask = static_cast<uint16_t>(StreamField::Euler) |
                          static_cast<uint16_t>(StreamField::Temperature) |
                          static_cast<uint16_t>(StreamField::CalibrationStatus);
    std::array<uint8_t, 64> out{};
    const size_t len = encodeStreamSample(s, mask, out.data(), out.size());
    REQUIRE(len == kStreamHeaderSize + 6 + 1 + 4);

    drivelink::protocol::wire::Reader r(out.data(), len);
    CHECK(r.readU8() == kProtocolVersion);
    CHECK(r.readU16Le() == mask);
    CHECK(r.readU64Le() == s.timestamp_us);
    CHECK(r.readI16Le() == 100);
    CHECK(r.readI16Le() == -200);
    CHECK(r.readI16Le() == 300);
    CHECK(static_cast<int8_t>(r.readU8()) == -5);
    CHECK(r.readU8() == 3);
    CHECK(r.readU8() == 2);
    CHECK(r.readU8() == 1);
    CHECK(r.readU8() == 0);
    CHECK(r.done());
}

TEST_CASE("Stream sample with every field")
{
    ImuStreamSample s{};
    s.quaternion.w = 16384;
    std::array<uint8_t, 128> out{};
    // euler 6 + quaternion 8 + linear accel 6 + gravity 6 + raw accel/gyro/mag 18 + temp 1 + calib 4
    CHECK(encodeStreamSample(s, kStreamFieldAll, out.data(), out.size()) == kStreamHeaderSize + 49);

    SUBCASE("a buffer one byte short fails")
    {
        CHECK(encodeStreamSample(s, kStreamFieldAll, out.data(), kStreamHeaderSize + 48) == 0);
    }
    SUBCASE("empty mask is just the header")
    {
        CHECK(encodeStreamSample(s, 0, out.data(), out.size()) == kStreamHeaderSize);
    }
}
