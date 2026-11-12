// core: the module registry and Core::createInstance's checks.

#include "doctest.h"

#include <array>

#include "core/core.hpp"
#include "core/registry.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/types.hpp"

using namespace testmod;
using core::Detail;
using core::Result;

TEST_CASE("Registry: lookup")
{
    core::Registry reg;
    REQUIRE(reg.init(kTypes).ok());
    CHECK(reg.size() == 2);
    CHECK(reg.find(fx::kTypeId) == &kFullType);
    CHECK(reg.find(fxs::kTypeId) == &kSensorType);
    CHECK(reg.find(0x00) == nullptr);
    CHECK(reg.find(0x77) == nullptr);
}

TEST_CASE("Registry: rejects type id 0, duplicates, a null create and a null entry")
{
    const core::ModuleType system_type{drivelink::protocol::defs::system::kDesc, &createSensor};
    const core::ModuleType no_factory{fxs::kDesc, nullptr};

    core::Registry reg;

    const std::array<const core::ModuleType *, 2> with_system{&kFullType, &system_type};
    core::Status st = reg.init(with_system);
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::ReservedType);
    CHECK(st.index == 1);
    CHECK(reg.size() == 0); // stays empty on failure

    const std::array<const core::ModuleType *, 3> dup{&kFullType, &kSensorType, &kFullType};
    st = reg.init(dup);
    CHECK(st.detail == Detail::DuplicateType);
    CHECK(st.index == 2);

    const std::array<const core::ModuleType *, 1> null_create{&no_factory};
    st = reg.init(null_create);
    CHECK(st.detail == Detail::NoFactory);
    CHECK(st.index == 0);

    const std::array<const core::ModuleType *, 2> null_entry{&kFullType, nullptr};
    st = reg.init(null_entry);
    CHECK(st.detail == Detail::UnknownType);
    CHECK(st.index == 1);

    CHECK(reg.init({}).ok()); // an empty list is fine (F3a-1's main)
    CHECK(reg.size() == 0);
}

TEST_CASE("Registry: a desc must be the generated table entry")
{
    const std::array<const drivelink::protocol::defs::TypeDesc *, 2> fixture_table{&fx::kDesc, &fxs::kDesc};
    CHECK(core::descIsInTable(kFullType, fixture_table));
    CHECK(core::descIsInTable(kSensorType, fixture_table));
    // The real table holds only the system type: the fixtures aren't in it.
    CHECK_FALSE(core::descIsInTable(kFullType, drivelink::protocol::defs::kTypeTable));
}

TEST_CASE("Core::createInstance checks type, loop, uniqueness, label and capacity")
{
    reset();
    core::Core c(testConfig(2));
    REQUIRE(c.init(kTypes).ok());
    CHECK(c.instanceCount() == 0);

    core::Status st = c.createInstance(0x55, 0, core::kControlLoop, "x");
    CHECK(st.result == Result::NotFound);
    CHECK(st.detail == Detail::UnknownType);

    st = c.createInstance(fxs::kTypeId, 0, core::kControlLoop, "s"); // sensor-only type
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::LoopMismatch);
    CHECK(c.createInstance(fx::kTypeId, 0, 7, "bad loop").detail == Detail::LoopMismatch);

    st = c.createInstance(fx::kTypeId, 0, core::kControlLoop, "seventeen_chars__");
    CHECK(st.detail == Detail::BadLabel);

    REQUIRE(c.createInstance(fx::kTypeId, 0, core::kControlLoop, "sixteen_chars___").ok());
    CHECK(c.createInstance(fx::kTypeId, 0, core::kSensorLoop, "again").result == Result::Exists);
    REQUIRE(c.createInstance(fxs::kTypeId, 0, core::kSensorLoop, "s0").ok());
    CHECK(c.instanceCount() == 2);
    CHECK(c.loop(core::kControlLoop).instanceCount() == 1);
    CHECK(c.loop(core::kSensorLoop).instanceCount() == 1);

    st = c.createInstance(fx::kTypeId, 1, core::kSensorLoop, "full");
    CHECK(st.result == Result::NoMemory);
    CHECK(st.detail == Detail::Capacity);
}

TEST_CASE("Core::createInstance is boot only")
{
    reset();
    core::Core c(testConfig());
    REQUIRE(c.init(kTypes).ok());
    REQUIRE(c.start().ok());
    CHECK(c.isRunning());
    const core::Status st = c.createInstance(fx::kTypeId, 0, core::kControlLoop, "late");
    CHECK(st.result == Result::Busy);
    CHECK(st.detail == Detail::NotAtBoot);
    c.stop();
    CHECK_FALSE(c.isRunning());
    c.stop(); // idempotent
}
