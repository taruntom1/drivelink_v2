#include "doctest.h"

#include "motor_protocol.hpp"
#include "wire_codec.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace motor_manager;
using namespace motor_manager::protocol;
using uart_comm::wire::Writer;

namespace {

std::vector<uint8_t> header(uint8_t seq, ConfigureSubcommand sub, uint8_t motor_id)
{
    return {kProtocolVersion, seq, static_cast<uint8_t>(sub), motor_id};
}

void appendF32(std::vector<uint8_t> &v, float f)
{
    uint8_t b[4];
    Writer w(b, sizeof(b));
    w.appendF32Le(f);
    v.insert(v.end(), b, b + 4);
}

void appendU32(std::vector<uint8_t> &v, uint32_t x)
{
    uint8_t b[4];
    Writer w(b, sizeof(b));
    w.appendU32Le(x);
    v.insert(v.end(), b, b + 4);
}

std::vector<uint8_t> createMotorRequest(uint8_t seq = 9, uint8_t motor_id = 3)
{
    auto p = header(seq, ConfigureSubcommand::CreateMotor, motor_id);
    p.insert(p.end(), {4, 5, 6, 7}); // pwm, dir, enc_a, enc_b
    for (int i = 0; i < 15; ++i) {
        appendF32(p, 1.5f + static_cast<float>(i));
    }
    return p;
}

Result decode(const std::vector<uint8_t> &p, ConfigureRequest &req)
{
    return decodeConfigure(p.data(), p.size(), req);
}

} // namespace

TEST_CASE("Wire sizes match the v4 spec")
{
    CHECK(kProtocolVersion == 4);
    CHECK(kConfigureResponseHeaderSize == 6);
    CHECK(kMotorConfigBodySize == 64);
    CHECK(kSetPidParamsBodySize == 17);
    CHECK(kSetSetpointBodySize == 5);
    CHECK(kSetTimingBodySize == 8);
}

TEST_CASE("CreateMotor decodes every field in wire order")
{
    const auto p = createMotorRequest();
    REQUIRE(p.size() == kConfigureHeaderSize + kMotorConfigBodySize);

    ConfigureRequest req{};
    REQUIRE((decode(p, req)).ok());
    CHECK(req.header.seq == 9);
    CHECK(req.header.subcommand == static_cast<uint8_t>(ConfigureSubcommand::CreateMotor));
    CHECK(req.header.motor_id == 3);
    CHECK(req.subcommand == ConfigureSubcommand::CreateMotor);
    CHECK(req.motor.pwm_gpio == 4);
    CHECK(req.motor.dir_gpio == 5);
    CHECK(req.motor.enc_a == 6);
    CHECK(req.motor.enc_b == 7);
    CHECK(req.motor.counts_per_rev == 1.5f);
    CHECK(req.motor.gear_ratio == 2.5f);
    CHECK(req.motor.output_min == 3.5f);
    CHECK(req.motor.output_max == 4.5f);
    CHECK(req.motor.vel_kp == 5.5f);
    CHECK(req.motor.vel_ki == 6.5f);
    CHECK(req.motor.vel_kd == 7.5f);
    CHECK(req.motor.vel_d_cutoff_hz == 8.5f);
    CHECK(req.motor.pos_kp == 9.5f);
    CHECK(req.motor.pos_ki == 10.5f);
    CHECK(req.motor.pos_kd == 11.5f);
    CHECK(req.motor.pos_d_cutoff_hz == 12.5f);
    CHECK(req.motor.velocity_feedforward == 13.5f);
    CHECK(req.motor.position_feedforward == 14.5f);
    CHECK(req.motor.vel_filter_cutoff_hz == 15.5f);
}

TEST_CASE("CreateMotor body must be exactly 64 bytes")
{
    auto p = createMotorRequest();
    ConfigureRequest req{};

    auto shorter = p;
    shorter.pop_back();
    CHECK((decode(shorter, req)).detail == ResultDetail::BadLength);
    CHECK(req.header.seq == 9); // header still echoed
    CHECK(req.header.motor_id == 3);

    auto longer = p;
    longer.push_back(0);
    CHECK((decode(longer, req)).detail == ResultDetail::BadLength);
    CHECK(decode(longer, req).code == ResultCode::InvalidArgument);
}

TEST_CASE("Unparsable configure headers")
{
    ConfigureRequest req{};

    SUBCASE("empty payload")
    {
        CHECK((decodeConfigure(nullptr, 0, req)).detail == ResultDetail::BadLength);
        CHECK(req.header.seq == 0);
    }
    SUBCASE("version only")
    {
        const std::vector<uint8_t> p = {kProtocolVersion};
        CHECK((decode(p, req)).detail == ResultDetail::BadLength);
        CHECK(req.header.seq == 0);
    }
    SUBCASE("right version, seq present, header still short: seq is echoed")
    {
        const std::vector<uint8_t> p = {kProtocolVersion, 0x42, 0x01};
        CHECK((decode(p, req)).detail == ResultDetail::BadLength);
        CHECK(req.header.seq == 0x42);
    }
    SUBCASE("wrong version: nothing is echoed")
    {
        auto p = createMotorRequest(0x42);
        p[0] = kProtocolVersion - 1;
        CHECK((decode(p, req)).detail == ResultDetail::BadVersion);
        CHECK(req.header.seq == 0);
    }
    CHECK(req.header.subcommand == kUnparsedHeaderByte);
    CHECK(req.header.motor_id == kUnparsedHeaderByte);
}

TEST_CASE("decodeConfigureHeader gives the same echo bytes as decodeConfigure")
{
    const auto full = createMotorRequest(0x42, 5);
    ConfigureHeader hdr = decodeConfigureHeader(full.data(), full.size());
    CHECK(hdr.seq == 0x42);
    CHECK(hdr.subcommand == static_cast<uint8_t>(ConfigureSubcommand::CreateMotor));
    CHECK(hdr.motor_id == 5);

    // A body is not needed: only the header is read.
    hdr = decodeConfigureHeader(full.data(), kConfigureHeaderSize);
    CHECK(hdr.motor_id == 5);

    const std::vector<uint8_t> short_hdr = {kProtocolVersion, 0x17};
    hdr = decodeConfigureHeader(short_hdr.data(), short_hdr.size());
    CHECK(hdr.seq == 0x17);
    CHECK(hdr.subcommand == kUnparsedHeaderByte);
    CHECK(hdr.motor_id == kUnparsedHeaderByte);

    hdr = decodeConfigureHeader(nullptr, 0);
    CHECK(hdr.seq == 0);
    CHECK(hdr.subcommand == kUnparsedHeaderByte);
}

TEST_CASE("Unknown subcommand is NotSupported, with the header echoed")
{
    const std::vector<uint8_t> p = {kProtocolVersion, 1, 0x7E, 2};
    ConfigureRequest req{};
    CHECK((decode(p, req)).detail == ResultDetail::UnknownSubcommand);
    CHECK(req.header.subcommand == 0x7E);
    CHECK(decode(p, req).code == ResultCode::NotSupported);
}

TEST_CASE("Empty-body subcommands reject any body")
{
    for (auto sub : {ConfigureSubcommand::DeleteMotor, ConfigureSubcommand::StopMotor,
                     ConfigureSubcommand::ResetMotor}) {
        auto p = header(1, sub, 2);
        ConfigureRequest req{};
        CHECK((decode(p, req)).ok());
        p.push_back(0);
        CHECK((decode(p, req)).detail == ResultDetail::BadLength);
    }
}

TEST_CASE("SetMode accepts modes 0..2 only")
{
    for (uint8_t mode = 0; mode <= 2; ++mode) {
        auto p = header(1, ConfigureSubcommand::SetMode, 2);
        p.push_back(mode);
        ConfigureRequest req{};
        REQUIRE((decode(p, req)).ok());
        CHECK(static_cast<uint8_t>(req.mode) == mode);
    }
    auto p = header(1, ConfigureSubcommand::SetMode, 2);
    p.push_back(3);
    ConfigureRequest req{};
    CHECK((decode(p, req)).detail == ResultDetail::BadEnum);
}

TEST_CASE("SetPidParams, SetSetpoint, telemetry and SetTiming bodies")
{
    ConfigureRequest req{};

    auto pid = header(1, ConfigureSubcommand::SetPidParams, 2);
    pid.push_back(static_cast<uint8_t>(PidKind::Position));
    appendF32(pid, 1.0f);
    appendF32(pid, 2.0f);
    appendF32(pid, 3.0f);
    appendF32(pid, 40.0f);
    REQUIRE((decode(pid, req)).ok());
    CHECK(req.pid_kind == PidKind::Position);
    CHECK(req.kp == 1.0f);
    CHECK(req.ki == 2.0f);
    CHECK(req.kd == 3.0f);
    CHECK(req.d_cutoff_hz == 40.0f);
    pid.pop_back();
    CHECK((decode(pid, req)).detail == ResultDetail::BadLength);

    auto sp = header(1, ConfigureSubcommand::SetSetpoint, 2);
    sp.push_back(static_cast<uint8_t>(SetpointKind::VelocityRadPerSec));
    appendF32(sp, -2.25f);
    REQUIRE((decode(sp, req)).ok());
    CHECK(req.setpoint_kind == SetpointKind::VelocityRadPerSec);
    CHECK(req.setpoint_value == -2.25f);

    auto tg = header(1, ConfigureSubcommand::SetTelemetryGlobal, 0);
    tg.insert(tg.end(), {1, kTelemetryFieldAngle});
    REQUIRE((decode(tg, req)).ok());
    CHECK(req.telemetry_enabled);
    CHECK(req.telemetry_field_mask == kTelemetryFieldAngle);

    auto tm = header(1, ConfigureSubcommand::SetTelemetryMotor, 4);
    tm.insert(tm.end(), {0, kTelemetryFieldAll, 1});
    REQUIRE((decode(tm, req)).ok());
    CHECK_FALSE(req.telemetry_enabled);
    CHECK(req.telemetry_field_mask == kTelemetryFieldAll);
    CHECK(req.telemetry_apply_mask);

    auto timing = header(1, ConfigureSubcommand::SetTiming, 0);
    appendU32(timing, 2000);
    appendU32(timing, 20000);
    REQUIRE((decode(timing, req)).ok());
    CHECK(req.control_period_us == 2000);
    CHECK(req.telemetry_period_us == 20000);
    timing.push_back(0);
    CHECK((decode(timing, req)).detail == ResultDetail::BadLength);
}

TEST_CASE("Configure response with the SetTiming body")
{
    uint8_t body[kSetTimingResponseBodySize] = {};
    REQUIRE(encodeSetTimingResponseBody(1000, 0x00012345, body, sizeof(body)) == kSetTimingResponseBodySize);

    ConfigureHeader hdr{};
    hdr.seq = 0x11;
    hdr.subcommand = static_cast<uint8_t>(ConfigureSubcommand::SetTiming);
    hdr.motor_id = 0;
    std::array<uint8_t, 16> out{};
    const size_t len = encodeConfigureResponse(hdr, Result{}, body, sizeof(body), out.data(), out.size());
    REQUIRE(len == kConfigureResponseHeaderSize + kSetTimingResponseBodySize);

    const std::array<uint8_t, 14> expected = {
        kProtocolVersion, 0x11, 0x0B, 0x00, 0x00, 0x00, // version seq subcommand motor_id result detail
        0xE8, 0x03, 0x00, 0x00,                         // 1000
        0x45, 0x23, 0x01, 0x00,                   // 0x12345
    };
    CHECK(std::memcmp(out.data(), expected.data(), expected.size()) == 0);
}

TEST_CASE("Configure response encode fails when it does not fit")
{
    const uint8_t body[8] = {};
    std::array<uint8_t, kConfigureResponseHeaderSize + 7> out{};
    CHECK(encodeConfigureResponse(ConfigureHeader{}, Result{}, body, sizeof(body), out.data(), out.size()) == 0);
    CHECK(encodeSetTimingResponseBody(1, 2, out.data(), 7) == 0);

    const size_t len = encodeConfigureResponse(ConfigureHeader{},
                                               Result{ResultCode::InvalidArgument, ResultDetail::BadVersion},
                                               nullptr, 0, out.data(), out.size());
    REQUIRE(len == kConfigureResponseHeaderSize);
    CHECK(out[2] == kUnparsedHeaderByte);
    CHECK(out[3] == kUnparsedHeaderByte);
    CHECK(out[4] == static_cast<uint8_t>(ResultCode::InvalidArgument));
    CHECK(out[5] == static_cast<uint8_t>(ResultDetail::BadVersion));
}

TEST_CASE("Setpoint stream framing")
{
    std::vector<uint8_t> p = {kProtocolVersion, 2};
    p.insert(p.end(), {1, static_cast<uint8_t>(SetpointKind::VelocityRadPerSec)});
    appendF32(p, 3.0f);
    p.insert(p.end(), {7, static_cast<uint8_t>(SetpointKind::PositionRad)});
    appendF32(p, -1.0f);

    uint8_t count = 0;
    REQUIRE(decodeSetpointStreamHeader(p.data(), p.size(), count));
    CHECK(count == 2);
    const SetpointEntry e0 = decodeSetpointEntry(p.data(), p.size(), 0);
    CHECK(e0.motor_id == 1);
    CHECK(e0.kind == static_cast<uint8_t>(SetpointKind::VelocityRadPerSec));
    CHECK(e0.value == 3.0f);
    const SetpointEntry e1 = decodeSetpointEntry(p.data(), p.size(), 1);
    CHECK(e1.motor_id == 7);
    CHECK(e1.value == -1.0f);

    SUBCASE("trailing byte drops the frame")
    {
        p.push_back(0);
        CHECK_FALSE(decodeSetpointStreamHeader(p.data(), p.size(), count));
    }
    SUBCASE("missing byte drops the frame")
    {
        p.pop_back();
        CHECK_FALSE(decodeSetpointStreamHeader(p.data(), p.size(), count));
    }
    SUBCASE("count larger than the payload drops the frame")
    {
        p[1] = 255;
        CHECK_FALSE(decodeSetpointStreamHeader(p.data(), p.size(), count));
    }
    SUBCASE("wrong version drops the frame")
    {
        p[0] = kProtocolVersion + 1;
        CHECK_FALSE(decodeSetpointStreamHeader(p.data(), p.size(), count));
    }
    SUBCASE("too short for a header")
    {
        CHECK_FALSE(decodeSetpointStreamHeader(p.data(), 1, count));
        CHECK_FALSE(decodeSetpointStreamHeader(nullptr, 0, count));
    }
    SUBCASE("an entry index past the end decodes as zeros")
    {
        const SetpointEntry e = decodeSetpointEntry(p.data(), p.size(), 5);
        CHECK(e.motor_id == 0);
        CHECK(e.value == 0.0f);
    }
}

TEST_CASE("Setpoint stream with zero entries is valid")
{
    const std::vector<uint8_t> p = {kProtocolVersion, 0};
    uint8_t count = 99;
    REQUIRE(decodeSetpointStreamHeader(p.data(), p.size(), count));
    CHECK(count == 0);
}

TEST_CASE("telemetryEntrySize")
{
    CHECK(telemetryEntrySize(0) == 2);
    CHECK(telemetryEntrySize(kTelemetryFieldAngle) == 6);
    CHECK(telemetryEntrySize(kTelemetryFieldAngle | kTelemetryFieldOutput) == 10);
    CHECK(telemetryEntrySize(kTelemetryFieldAll) == 14);
}

TEST_CASE("Telemetry packet layout")
{
    std::array<uint8_t, 64> out{};
    TelemetryEncoder enc(out.data(), out.size(), 0x0102030405060708ULL);

    TelemetryEntry a{};
    a.motor_id = 1;
    a.field_mask = kTelemetryFieldAll;
    a.angle_rad = 1.0f;
    a.velocity_rad_s = -2.0f;
    a.output = 0.5f;
    REQUIRE(enc.append(a));

    TelemetryEntry b{};
    b.motor_id = 2;
    b.field_mask = kTelemetryFieldVelocity;
    b.angle_rad = 99.0f; // not in the mask: not encoded
    b.velocity_rad_s = 4.0f;
    REQUIRE(enc.append(b));

    const size_t len = enc.finish();
    REQUIRE(len == kTelemetryPacketHeaderSize + 14 + 6);
    CHECK(out[0] == kProtocolVersion);
    CHECK(out[1] == 2); // entry count patched in
    CHECK(out[2] == 0x08); // timestamp, little-endian
    CHECK(out[9] == 0x01);

    uart_comm::wire::Reader r(&out[kTelemetryPacketHeaderSize], len - kTelemetryPacketHeaderSize);
    CHECK(r.readU8() == 1);
    CHECK(r.readU8() == kTelemetryFieldAll);
    CHECK(r.readF32Le() == 1.0f);
    CHECK(r.readF32Le() == -2.0f);
    CHECK(r.readF32Le() == 0.5f);
    CHECK(r.readU8() == 2);
    CHECK(r.readU8() == kTelemetryFieldVelocity);
    CHECK(r.readF32Le() == 4.0f);
    CHECK(r.done());
}

TEST_CASE("Telemetry stops adding entries when the buffer is full")
{
    // Room for the header and exactly one full entry.
    std::array<uint8_t, kTelemetryPacketHeaderSize + 14 + 5> out{};
    TelemetryEncoder enc(out.data(), out.size(), 0);
    TelemetryEntry e{};
    e.field_mask = kTelemetryFieldAll;
    CHECK(enc.append(e));
    CHECK_FALSE(enc.append(e)); // 14 more bytes do not fit
    TelemetryEntry small{};
    small.field_mask = 0;
    CHECK(enc.append(small)); // 2 bytes still fit
    CHECK(enc.entryCount() == 2);
    CHECK(enc.finish() == kTelemetryPacketHeaderSize + 14 + 2);
}

TEST_CASE("Telemetry with no entries is just the header")
{
    std::array<uint8_t, kTelemetryPacketHeaderSize> out{};
    TelemetryEncoder enc(out.data(), out.size(), 5);
    CHECK(enc.finish() == kTelemetryPacketHeaderSize);
    CHECK(out[1] == 0);

    std::array<uint8_t, kTelemetryPacketHeaderSize - 1> tiny{};
    TelemetryEncoder bad(tiny.data(), tiny.size(), 5);
    CHECK(bad.finish() == 0);
}
