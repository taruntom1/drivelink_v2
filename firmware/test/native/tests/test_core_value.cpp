// core: checkValue, storeValue / loadValue and valueTypeOf.

#include "doctest.h"

#include <cmath>
#include <limits>

#include "core/value.hpp"
#include "core_test_modules.hpp"

using namespace testmod;
using core::checkValue;
using core::Detail;

namespace {

const defs::ItemDesc &item(uint16_t id)
{
    const defs::ItemDesc *d = defs::findItem(fx::kDesc, id);
    REQUIRE(d != nullptr);
    return *d;
}

void checkClamp(uint16_t id, uint32_t in, uint32_t out)
{
    const core::ValueCheck c = checkValue(item(id), in);
    CHECK(c.ok);
    CHECK(c.applied_bits == out);
    CHECK(c.detail == Detail::None);
}

void checkReject(uint16_t id, uint32_t in, Detail detail)
{
    const core::ValueCheck c = checkValue(item(id), in);
    CHECK_FALSE(c.ok);
    CHECK(c.detail == detail);
}

uint32_t i(int32_t v)
{
    return static_cast<uint32_t>(v);
}

} // namespace

TEST_CASE("checkValue: bool")
{
    checkClamp(fx::kEnabled, 0, 0);
    checkClamp(fx::kEnabled, 1, 1);
    checkReject(fx::kEnabled, 2, Detail::BadValue);
    checkReject(fx::kEnabled, 0xFFFFFFFF, Detail::BadValue);
    checkClamp(fx::kReady, 1, 1); // a signal
}

TEST_CASE("checkValue: u8 must fit, then is clamped to min/max")
{
    // level: u8, 1..10
    checkReject(fx::kLevel, 300, Detail::BadValue);
    checkReject(fx::kLevel, 256, Detail::BadValue);
    checkClamp(fx::kLevel, 0, 1);
    checkClamp(fx::kLevel, 1, 1);
    checkClamp(fx::kLevel, 5, 5);
    checkClamp(fx::kLevel, 10, 10);
    checkClamp(fx::kLevel, 11, 10);
    checkClamp(fx::kLevel, 255, 10);
}

TEST_CASE("checkValue: u16 must fit, then is clamped")
{
    // count: u16, max 500, no min
    checkReject(fx::kCount, 70000, Detail::BadValue);
    checkClamp(fx::kCount, 0, 0);
    checkClamp(fx::kCount, 600, 500);
    checkClamp(fx::kCount, 0xFFFF, 500);
}

TEST_CASE("checkValue: u32 without limits takes anything")
{
    checkClamp(fx::kRaw, 0, 0);
    checkClamp(fx::kRaw, 0xFFFFFFFF, 0xFFFFFFFF);
}

TEST_CASE("checkValue: i32 is clamped as signed")
{
    // offset: i32, -100..100
    checkClamp(fx::kOffset, i(-5), i(-5));
    checkClamp(fx::kOffset, i(-500), i(-100));
    checkClamp(fx::kOffset, i(500), i(100));
    checkClamp(fx::kOffset, i(std::numeric_limits<int32_t>::min()), i(-100));
    checkClamp(fx::kOffset, i(std::numeric_limits<int32_t>::max()), i(100));
}

TEST_CASE("checkValue: f32 rejects NaN and infinities, and is clamped")
{
    // gain: f32, 0..1000
    checkReject(fx::kGain, f(std::numeric_limits<float>::quiet_NaN()), Detail::NonFiniteFloat);
    checkReject(fx::kGain, f(std::numeric_limits<float>::infinity()), Detail::NonFiniteFloat);
    checkReject(fx::kGain, f(-std::numeric_limits<float>::infinity()), Detail::NonFiniteFloat);
    checkClamp(fx::kGain, f(1.25f), f(1.25f));
    checkClamp(fx::kGain, f(-1.0f), f(0.0f));
    checkClamp(fx::kGain, f(2000.0f), f(1000.0f));
    // vel.kp: -0.5..2.25
    checkClamp(fx::kVelKp, f(-3.0f), f(-0.5f));
    checkClamp(fx::kVelKp, f(3.0f), f(2.25f));
    // position: a signal with no limits
    checkClamp(fx::kPosition, f(-1e30f), f(-1e30f));
    checkReject(fx::kPosition, f(std::nanf("")), Detail::NonFiniteFloat);
}

TEST_CASE("checkValue: an enum must be a listed value")
{
    // mode: a_mode=0, b_mode=1, c_mode=2
    checkClamp(fx::kMode, 0, 0);
    checkClamp(fx::kMode, 2, 2);
    checkReject(fx::kMode, 3, Detail::BadEnum);
    checkReject(fx::kMode, 0x101, Detail::BadEnum); // the low byte is listed, the value is not
    // state: idle=0, run=1
    checkReject(fx::kState, 2, Detail::BadEnum);
}

TEST_CASE("checkValue: an action has no value")
{
    checkReject(fx::kReset, 0, Detail::WrongKind);
}

TEST_CASE("storeValue / loadValue round trip for every type")
{
    bool b = false;
    core::storeValue(defs::ValueType::Bool, &b, 1);
    CHECK(b);
    CHECK(core::loadValue(defs::ValueType::Bool, &b) == 1);

    uint8_t u8 = 0;
    core::storeValue(defs::ValueType::U8, &u8, 200);
    CHECK(u8 == 200);
    CHECK(core::loadValue(defs::ValueType::U8, &u8) == 200);

    FxMode mode = FxMode::A;
    core::storeValue(defs::ValueType::Enum, &mode, 2);
    CHECK(mode == FxMode::C);
    CHECK(core::loadValue(defs::ValueType::Enum, &mode) == 2);

    uint16_t u16 = 0;
    core::storeValue(defs::ValueType::U16, &u16, 60000);
    CHECK(u16 == 60000);
    CHECK(core::loadValue(defs::ValueType::U16, &u16) == 60000);

    uint32_t u32 = 0;
    core::storeValue(defs::ValueType::U32, &u32, 0xDEADBEEF);
    CHECK(u32 == 0xDEADBEEF);

    int32_t i32 = 0;
    core::storeValue(defs::ValueType::I32, &i32, i(-42));
    CHECK(i32 == -42);
    CHECK(core::loadValue(defs::ValueType::I32, &i32) == i(-42));

    float f32 = 0.0f;
    core::storeValue(defs::ValueType::F32, &f32, f(-2.5f));
    CHECK(f32 == -2.5f);
    CHECK(core::loadValue(defs::ValueType::F32, &f32) == f(-2.5f));
}

TEST_CASE("valueTypeOf maps the bindable C++ types")
{
    static_assert(core::valueTypeOf<bool>() == defs::ValueType::Bool);
    static_assert(core::valueTypeOf<uint8_t>() == defs::ValueType::U8);
    static_assert(core::valueTypeOf<uint16_t>() == defs::ValueType::U16);
    static_assert(core::valueTypeOf<uint32_t>() == defs::ValueType::U32);
    static_assert(core::valueTypeOf<int32_t>() == defs::ValueType::I32);
    static_assert(core::valueTypeOf<float>() == defs::ValueType::F32);
    static_assert(core::valueTypeOf<FxMode>() == defs::ValueType::Enum);
    CHECK(true);
}
