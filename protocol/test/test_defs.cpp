#include "doctest.h"

#include "drivelink/protocol/defs.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/gen/types.hpp"

// Generated at build time from tools/gen/tests/fixtures (see
// firmware/test/native/CMakeLists.txt): they prove that the generator's type
// headers compile with -Werror and describe what the YAML says.
#include "type_fixture_full.hpp"
#include "type_fixture_sensor.hpp"
#include "type_fixture_wired.hpp"

#include <array>
#include <cstring>

using namespace drivelink::protocol;
using namespace drivelink::protocol::defs;

TEST_CASE("Registry MSG_TYPE constants")
{
    CHECK(msg::kCoreCommand == 0x01);
    CHECK(msg::kCoreResponse == 0x41);
    CHECK(msg::kDisarm == 0x03);
    CHECK(msg::kCoreEvent == 0xC1);
    CHECK(msg::kStatusStream == 0x80);
    CHECK(msg::kDiagQuery == 0x23);
    CHECK(msg::kDiagQueryResponse == 0x63);
    CHECK(msg::kDiagEvent == 0xC0);
    CHECK(msg::kTimeSyncRequest == 0x20);
    CHECK(msg::kTimeSyncResponse == 0x60);
    CHECK(kSystemTypeId == 0x00);
}

TEST_CASE("Registry values are unique and sorted, and none is reserved")
{
    for (size_t i = 0; i < kAllMsgTypes.size(); ++i) {
        CHECK(kAllMsgTypes[i] != 0x00);
        CHECK(kAllMsgTypes[i] != 0xFF);
        if (i > 0) {
            CHECK(kAllMsgTypes[i - 1] < kAllMsgTypes[i]);
        }
    }
    CHECK(isStream(msg::kStatusStream));
    CHECK(isEvent(msg::kDiagEvent));
    CHECK(isCommand(msg::kDisarm));
    CHECK(isResponse(msg::kCoreResponse));
}

TEST_CASE("Channel ranges: the legacy IMU stream cuts the command groups")
{
    CHECK(kSubGroupChannelFirst == 0x81);
    CHECK(kSubGroupChannelCount == 31);
    CHECK(kCmdGroupChannelFirst == 0xA0);
    CHECK(kCmdGroupChannelCount == 32);
    // The usable command channels end just below the legacy IMU stream (F6).
    CHECK(kSubGroupChannelFirst + kSubGroupChannelCount == kCmdGroupChannelFirst);
}

TEST_CASE("Descriptor hash is set, and the diag table comes through")
{
    CHECK(kDescriptorHash != 0);
    CHECK(diag::kCodeTable.size() == diag::kSlotCount);
    CHECK(diag::kSlotCount > 0);
    // System, loop, the controller, the two hardware motor types and four
    // mock types, ordered by type id.
    REQUIRE(kTypeTable.size() == 10);
    CHECK(kTypeTable[0] == &system::kDesc);
}

TEST_CASE("Built-in descriptors: system rates retired, configurable loop parameters")
{
    CHECK(system::kDesc.type_id == 0);
    CHECK(system::kDesc.loops == 0);
    CHECK(findItem(system::kDesc, 1) == nullptr);
    CHECK(findItem(system::kDesc, 2) == nullptr);
    CHECK(findItem(system::kDesc, system::kStatusRateHz)->default_bits == 10);
    CHECK(loop::kDesc.type_id == kLoopTypeId);
    CHECK(loop::kDesc.loops == 0);
    const auto *rate = findItem(loop::kDesc, loop::kRateHz);
    REQUIRE(rate);
    CHECK(rate->min_bits == 1);
    CHECK(rate->max_bits == 2000);
    CHECK(hasFlag(*rate, ItemFlag::LockedWhileArmed));
    CHECK(hasFlag(*findItem(loop::kDesc, loop::kRealtime), ItemFlag::RebootRequired));
}

TEST_CASE("f32 bits round trip")
{
    CHECK(f32Bits(1.0f) == 0x3F800000U);
    CHECK(f32Bits(-2.0f) == 0xC0000000U);
    CHECK(f32Bits(0.0f) == 0U);
    for (const float v : {0.0f, 1.5f, -0.5f, 1000.0f, 3.14159274f, 1e-20f}) {
        CHECK(f32FromBits(f32Bits(v)) == v);
    }
}

TEST_CASE("Fixture type: ids and tables")
{
    namespace fx = drivelink::protocol::defs::fixture_full;
    CHECK(fx::kTypeId == 0x01);
    CHECK(std::strcmp(fx::kDesc.name, "fixture.full") == 0);
    CHECK(fx::kDesc.type_id == 0x01);
    CHECK(fx::kDesc.items.size() == 15); // the retired id has no entry
    CHECK(runsIn(fx::kDesc, LoopClass::Realtime));
    CHECK(runsIn(fx::kDesc, LoopClass::NonRealtime));
    CHECK_FALSE(runsIn(fixture_sensor::kDesc, LoopClass::Realtime));
    CHECK(runsIn(fixture_sensor::kDesc, LoopClass::NonRealtime));
    CHECK(fx::kVelKp == 0x0030);
}

TEST_CASE("findItem")
{
    namespace fx = drivelink::protocol::defs::fixture_full;
    const ItemDesc *gain = findItem(fx::kDesc, fx::kGain);
    REQUIRE(gain != nullptr);
    CHECK(std::strcmp(gain->name, "gain") == 0);
    CHECK(gain->kind == ItemKind::Param);
    CHECK(findItem(fx::kDesc, 0x0400) == nullptr); // retired
    CHECK(findItem(fx::kDesc, 0x7777) == nullptr);
    for (const ItemDesc &item : fx::kDesc.items) {
        CHECK(findItem(fx::kDesc, item.id) == &item);
    }
}

TEST_CASE("findType")
{
    const std::array<const TypeDesc *, 2> table = {&fixture_full::kDesc, &fixture_sensor::kDesc};
    CHECK(typeIdsUnique(table));
    CHECK(findType(table, 0x01) == &fixture_full::kDesc);
    CHECK(findType(table, 0x02) == &fixture_sensor::kDesc);
    CHECK(findType(table, 0x03) == nullptr);
    CHECK(findType(table, kSystemTypeId) == nullptr);

    const std::array<const TypeDesc *, 2> dup = {&fixture_full::kDesc, &fixture_full::kDesc};
    CHECK_FALSE(typeIdsUnique(dup));
}

TEST_CASE("Fixture items: kinds, types and flags")
{
    namespace fx = drivelink::protocol::defs::fixture_full;
    const TypeDesc &t = fx::kDesc;

    const ItemDesc *actuator = findItem(t, fx::kActuator);
    REQUIRE(actuator != nullptr);
    CHECK(actuator->type == ValueType::U32);
    CHECK(hasFlag(*actuator, ItemFlag::RebootRequired));
    CHECK(hasFlag(*actuator, ItemFlag::Persist));
    CHECK(std::strcmp(actuator->ref, "actuator") == 0);

    const ItemDesc *enabled = findItem(t, fx::kEnabled);
    REQUIRE(enabled != nullptr);
    CHECK(enabled->type == ValueType::Bool);
    CHECK_FALSE(hasFlag(*enabled, ItemFlag::Persist)); // persist: false
    CHECK(enabled->default_bits == 1);
    CHECK(enabled->ref == nullptr);

    const ItemDesc *count = findItem(t, fx::kCount);
    REQUIRE(count != nullptr);
    CHECK(count->type == ValueType::U16);
    CHECK(hasFlag(*count, ItemFlag::ReadOnly));
    CHECK_FALSE(hasFlag(*count, ItemFlag::Persist)); // read-only params don't persist

    const ItemDesc *mode = findItem(t, fx::kMode);
    REQUIRE(mode != nullptr);
    CHECK(hasFlag(*mode, ItemFlag::LockedWhileArmed));
    CHECK_FALSE(hasFlag(*mode, ItemFlag::RebootRequired));

    const ItemDesc *vel_sp = findItem(t, fx::kVelocitySp);
    REQUIRE(vel_sp != nullptr);
    CHECK(vel_sp->kind == ItemKind::Input);
    CHECK(hasFlag(*vel_sp, ItemFlag::Testgen));
    CHECK(std::strcmp(vel_sp->unit, "rad/s") == 0);

    const ItemDesc *position = findItem(t, fx::kPosition);
    REQUIRE(position != nullptr);
    CHECK(position->kind == ItemKind::Signal);
    CHECK(position->flags == 0);
}

TEST_CASE("Fixture items: limits and defaults as slot bits")
{
    namespace fx = drivelink::protocol::defs::fixture_full;
    const TypeDesc &t = fx::kDesc;

    const ItemDesc *gain = findItem(t, fx::kGain);
    REQUIRE(gain != nullptr);
    CHECK(gain->has_min);
    CHECK(gain->has_max);
    CHECK(f32FromBits(gain->min_bits) == 0.0f);
    CHECK(f32FromBits(gain->max_bits) == 1000.0f);
    CHECK(gain->default_bits == f32Bits(1.5f));

    const ItemDesc *offset = findItem(t, fx::kOffset);
    REQUIRE(offset != nullptr);
    CHECK(static_cast<int32_t>(offset->min_bits) == -100);
    CHECK(static_cast<int32_t>(offset->max_bits) == 100);
    CHECK(static_cast<int32_t>(offset->default_bits) == -5);

    const ItemDesc *level = findItem(t, fx::kLevel);
    REQUIRE(level != nullptr);
    CHECK(level->min_bits == 1);
    CHECK(level->max_bits == 10);
    CHECK(level->default_bits == 3);

    const ItemDesc *count = findItem(t, fx::kCount);
    REQUIRE(count != nullptr);
    CHECK_FALSE(count->has_min);
    CHECK(count->has_max);
    CHECK(count->max_bits == 500);

    const ItemDesc *vel_kp = findItem(t, fx::kVelKp);
    REQUIRE(vel_kp != nullptr);
    CHECK(f32FromBits(vel_kp->min_bits) == -0.5f);
    CHECK(f32FromBits(vel_kp->max_bits) == 2.25f);

    const ItemDesc *raw = findItem(t, fx::kRaw);
    REQUIRE(raw != nullptr);
    CHECK_FALSE(raw->has_min);
    CHECK_FALSE(raw->has_max);
}

TEST_CASE("Fixture items: enums")
{
    namespace fx = drivelink::protocol::defs::fixture_full;
    const ItemDesc *mode = findItem(fx::kDesc, fx::kMode);
    REQUIRE(mode != nullptr);
    CHECK(mode->type == ValueType::Enum);
    REQUIRE(mode->enum_values.size() == 3);
    CHECK(std::strcmp(mode->enum_values[0].name, "a_mode") == 0);
    CHECK(mode->enum_values[0].value == 0);
    CHECK(std::strcmp(mode->enum_values[2].name, "c_mode") == 0);
    CHECK(mode->enum_values[2].value == 2);
    CHECK(mode->default_bits == 1); // b_mode

    const ItemDesc *state = findItem(fx::kDesc, fx::kState);
    REQUIRE(state != nullptr);
    CHECK(state->enum_values.size() == 2);

    const ItemDesc *gain = findItem(fx::kDesc, fx::kGain);
    REQUIRE(gain != nullptr);
    CHECK(gain->enum_values.empty());
}

TEST_CASE("Fixture items: actions")
{
    namespace fx = drivelink::protocol::defs::fixture_full;
    const ItemDesc *reset = findItem(fx::kDesc, fx::kReset);
    REQUIRE(reset != nullptr);
    CHECK(reset->kind == ItemKind::Action);
    CHECK(reset->type == ValueType::None);
    CHECK(reset->runs_in == ActionPlacement::Loop);
    CHECK(reset->args.empty());
    CHECK(reset->results.empty());
    CHECK(reset->blob == BlobDir::None);

    const ItemDesc *cal = findItem(fx::kDesc, fx::kCalibrate);
    REQUIRE(cal != nullptr);
    CHECK(cal->runs_in == ActionPlacement::Paused);
    CHECK(cal->blob == BlobDir::Out);
    REQUIRE(cal->args.size() == 2);
    CHECK(std::strcmp(cal->args[0].name, "seconds") == 0);
    CHECK(cal->args[0].type == ValueType::F32);
    CHECK(std::strcmp(cal->args[0].unit, "s") == 0);
    CHECK(cal->args[1].type == ValueType::U8);
    REQUIRE(cal->results.size() == 1);
    CHECK(cal->results[0].type == ValueType::I32);
}

TEST_CASE("Flag and loop helpers")
{
    CHECK(flagBits() == 0);
    CHECK(flagBits(ItemFlag::RebootRequired, ItemFlag::Persist) == 0x03);
    CHECK(loopMask(LoopClass::Realtime, LoopClass::NonRealtime) == 0x03);
}

TEST_CASE("Defs: gpio and ref_order flags, wiring values and categories")
{
    using namespace drivelink::protocol::defs;
    namespace wd = fixture_wired;
    CHECK(static_cast<uint8_t>(ItemFlag::Gpio) == 0x20);
    CHECK(static_cast<uint8_t>(ItemFlag::RefTicksAfter) == 0x40);
    const ItemDesc *pin = findItem(wd::kDesc, wd::kPinA);
    REQUIRE(pin != nullptr);
    CHECK(hasFlag(*pin, ItemFlag::Gpio));
    CHECK(hasFlag(*pin, ItemFlag::RebootRequired));
    CHECK(pin->default_bits == kNoGpio);
    CHECK_FALSE(pin->has_max);
    CHECK(hasFlag(*findItem(wd::kDesc, wd::kActuator), ItemFlag::RefTicksAfter));
    CHECK_FALSE(hasFlag(*findItem(wd::kDesc, wd::kFeedback), ItemFlag::RefTicksAfter));

    CHECK(wiringRef(0x20, 3) == 0x2003);
    CHECK(wiringRefType(0x2003) == 0x20);
    CHECK(wiringRefInstance(0x2003) == 3);
    CHECK(isWiringRef(0xFFFF));
    CHECK_FALSE(isWiringRef(0x10000));
    CHECK(kUnwired == 0);

    CHECK(inCategory("actuator.pwm_dir", "actuator"));
    CHECK_FALSE(inCategory("actuator", "actuator"));
    CHECK_FALSE(inCategory("actuators.x", "actuator"));
    CHECK_FALSE(inCategory("act.x", "actuator"));
    CHECK_FALSE(inCategory("system", "system"));
    CHECK_FALSE(inCategory("a.b", ""));
    CHECK_FALSE(inCategory(nullptr, "a"));
}
