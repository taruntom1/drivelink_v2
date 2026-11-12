#include "doctest.h"

#include "gpio_policy.hpp"
#include "motor_protocol.hpp"
#include "drivelink/protocol/wire_codec.hpp"

#include <cmath>
#include <limits>
#include <vector>

using namespace motor_manager;
using namespace motor_manager::protocol;
namespace gp = motor_manager::gpio_policy;

namespace {

// uart_comm's link pins in sdkconfig.defaults.
constexpr uint64_t kLink = gp::gpioBit(43) | gp::gpioBit(44);

MotorConfigPayload pins(uint8_t pwm, uint8_t dir, uint8_t a, uint8_t b)
{
    MotorConfigPayload cfg{};
    cfg.pwm_gpio = pwm;
    cfg.dir_gpio = dir;
    cfg.enc_a = a;
    cfg.enc_b = b;
    return cfg;
}

std::vector<uint8_t> withHeader(ConfigureSubcommand sub)
{
    return {kProtocolVersion, 1, static_cast<uint8_t>(sub), 2};
}

void appendF32(std::vector<uint8_t> &v, float f)
{
    uint8_t b[4];
    drivelink::protocol::wire::Writer w(b, sizeof(b));
    w.appendF32Le(f);
    v.insert(v.end(), b, b + 4);
}

std::vector<uint8_t> createWithFloat(int index, float bad)
{
    auto p = withHeader(ConfigureSubcommand::CreateMotor);
    p.insert(p.end(), {4, 5, 6, 7});
    for (int i = 0; i < 15; ++i) {
        appendF32(p, (i == index) ? bad : 1.0f);
    }
    return p;
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

} // namespace

TEST_CASE("GPIO policy: allowed and reserved pins on the ESP32-S3")
{
    for (unsigned g : {0u, 1u, 4u, 18u, 21u, 38u, 42u, 45u, 48u}) {
        CAPTURE(g);
        CHECK(gp::isAllowed(static_cast<uint8_t>(g), kLink));
    }
    for (unsigned g : {19u, 20u,                          // USB
                       22u, 23u, 24u, 25u,                // do not exist
                       26u, 29u, 32u, 33u, 37u,           // flash / PSRAM
                       43u, 44u,                          // uart_comm link
                       49u, 63u, 64u, 200u, 255u}) {      // out of range
        CAPTURE(g);
        CHECK_FALSE(gp::isAllowed(static_cast<uint8_t>(g), kLink));
    }
    // The link pins are reserved only through the mask the manager passes.
    CHECK(gp::isAllowed(43, 0));
}

TEST_CASE("GPIO policy: the four motor pins must be allowed and distinct")
{
    CHECK(gp::areMotorPinsAllowed(pins(4, 5, 6, 7), kLink));
    CHECK_FALSE(gp::areMotorPinsAllowed(pins(4, 4, 6, 7), kLink));  // pwm == dir
    CHECK_FALSE(gp::areMotorPinsAllowed(pins(4, 5, 6, 6), kLink));  // enc_a == enc_b
    CHECK_FALSE(gp::areMotorPinsAllowed(pins(4, 5, 6, 4), kLink));  // enc_b == pwm
    CHECK_FALSE(gp::areMotorPinsAllowed(pins(4, 5, 6, 30), kLink)); // flash pin
    CHECK_FALSE(gp::areMotorPinsAllowed(pins(43, 5, 6, 7), kLink)); // host link
    CHECK_FALSE(gp::areMotorPinsAllowed(pins(4, 64, 6, 7), kLink)); // would be UB in 1ULL << 64
    CHECK(gp::motorPinMask(pins(4, 5, 6, 7)) == 0xF0U);
}

TEST_CASE("CreateMotor / ConfigureMotor reject any non-finite float")
{
    ConfigureRequest req{};
    for (int i = 0; i < 15; ++i) {
        CAPTURE(i);
        for (float bad : {kNaN, kInf, -kInf}) {
            const auto p = createWithFloat(i, bad);
            CHECK((decodeConfigure(p.data(), p.size(), req)).detail == ResultDetail::NonFiniteFloat);
        }
    }
    const auto ok = createWithFloat(0, 2.0f);
    CHECK((decodeConfigure(ok.data(), ok.size(), req)).ok());
    CHECK(isMotorConfigFinite(req.motor));
    req.motor.gear_ratio = kNaN;
    CHECK_FALSE(isMotorConfigFinite(req.motor));
}

TEST_CASE("SetPidParams rejects an unknown PID kind and non-finite gains")
{
    ConfigureRequest req{};
    auto make = [](uint8_t kind, float kp) {
        auto p = withHeader(ConfigureSubcommand::SetPidParams);
        p.push_back(kind);
        appendF32(p, kp);
        appendF32(p, 0.0f);
        appendF32(p, 0.0f);
        appendF32(p, 0.0f);
        return p;
    };
    auto p = make(1, 1.0f);
    CHECK((decodeConfigure(p.data(), p.size(), req)).ok());
    p = make(2, 1.0f);
    CHECK((decodeConfigure(p.data(), p.size(), req)).detail == ResultDetail::BadEnum);
    p = make(0, kNaN);
    CHECK((decodeConfigure(p.data(), p.size(), req)).detail == ResultDetail::NonFiniteFloat);
    p = make(0, kInf);
    CHECK((decodeConfigure(p.data(), p.size(), req)).detail == ResultDetail::NonFiniteFloat);
}

TEST_CASE("SetSetpoint rejects an unknown kind and a non-finite value")
{
    ConfigureRequest req{};
    auto make = [](uint8_t kind, float value) {
        auto p = withHeader(ConfigureSubcommand::SetSetpoint);
        p.push_back(kind);
        appendF32(p, value);
        return p;
    };
    auto p = make(2, -3.0f);
    CHECK((decodeConfigure(p.data(), p.size(), req)).ok());
    p = make(3, 1.0f);
    CHECK((decodeConfigure(p.data(), p.size(), req)).detail == ResultDetail::BadEnum);
    p = make(0, kNaN);
    CHECK((decodeConfigure(p.data(), p.size(), req)).detail == ResultDetail::NonFiniteFloat);
    p = make(1, -kInf);
    CHECK((decodeConfigure(p.data(), p.size(), req)).detail == ResultDetail::NonFiniteFloat);
}

TEST_CASE("Telemetry configure rejects unknown field bits")
{
    ConfigureRequest req{};
    auto global = withHeader(ConfigureSubcommand::SetTelemetryGlobal);
    global.insert(global.end(), {1, 0x08});
    CHECK((decodeConfigure(global.data(), global.size(), req)).detail == ResultDetail::BadFieldMask);

    auto motor = withHeader(ConfigureSubcommand::SetTelemetryMotor);
    motor.insert(motor.end(), {1, 0x80, 1});
    CHECK((decodeConfigure(motor.data(), motor.size(), req)).detail == ResultDetail::BadFieldMask);
}

TEST_CASE("Enum helpers")
{
    CHECK(isValidPidKind(PidKind::Velocity));
    CHECK(isValidPidKind(PidKind::Position));
    CHECK_FALSE(isValidPidKind(static_cast<PidKind>(2)));
    CHECK(isValidSetpointKind(SetpointKind::PositionRad));
    CHECK_FALSE(isValidSetpointKind(static_cast<SetpointKind>(3)));
}
