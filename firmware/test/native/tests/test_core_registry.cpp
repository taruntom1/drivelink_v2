// core: the module registry, and the boot's checks of the layout entries.

#include "doctest.h"

#include <array>
#include <vector>

#include "core/core.hpp"
#include "core/registry.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/types.hpp"

using namespace testmod;
using core::Detail;
using core::Result;

TEST_CASE("Registry: lookup")
{
    core::Registry reg;
    REQUIRE(reg.init(kTypes).ok());
    CHECK(reg.size() == 5);
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

TEST_CASE("Boot: entries are checked for type, loop, uniqueness and capacity")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {
                           {0x55, 0, core::kFactoryControlLoop, "x"},             // unknown type
                           {fxs::kTypeId, 0, core::kFactoryControlLoop, "s"},     // sensor-only type
                           {fx::kTypeId, 0, core::kFactoryControlLoop, "ok"},     // fine
                           {fx::kTypeId, 0, core::kFactorySensorLoop, "again"},   // (type, inst) twice
                           {fx::kTypeId, 1, core::kFactorySensorLoop, "full"},    // beyond the capacity
                           {fx::kTypeId, 2, 7, "bad loop"},                // also beyond it
                       });
    core::Core c(testConfig(4));
    REQUIRE(bootCore(c, store).ok());
    CHECK(c.instanceCount() == 3);
    CHECK(c.configuredCount() == 4); // ok and the sensor-only one: the others have no record
    CHECK(c.instancesFailed() == 5);
    CHECK(c.layoutInvalid());
    CHECK(stateOf(c, fx::kTypeId, 0) == core::InstanceState::Running);
    CHECK(stateOf(c, fxs::kTypeId, 0) == core::InstanceState::Invalid);
    CHECK(c.loop(core::kFactoryControlLoop).instanceCount() == 1);
    CHECK(c.loop(core::kFactorySensorLoop).instanceCount() == 0);

    namespace codes = drivelink::protocol::defs::diag::codes;
    std::vector<uint32_t> details;
    for (const auto &r : core_os::native::recordedReports()) {
        if (r.code.code == codes::core::kLayoutInvalid.code) {
            details.push_back(r.arg1);
        }
    }
    CHECK(details == std::vector<uint32_t>{static_cast<uint32_t>(Detail::UnknownType),
                                           static_cast<uint32_t>(Detail::LoopMismatch),
                                           static_cast<uint32_t>(Detail::None), // Exists
                                           static_cast<uint32_t>(Detail::Capacity),
                                           static_cast<uint32_t>(Detail::Capacity)});
}

TEST_CASE("Core::boot runs once, before start")
{
    reset();
    core_os::KvStore store;
    {
        core::Core c(testConfig());
        REQUIRE(c.init(kTypes).ok());
        CHECK(c.start().detail == Detail::NotAtBoot);
        REQUIRE(c.boot(core::prepareBoot(store)).ok());
        REQUIRE(c.start().ok());
        CHECK(c.isRunning());
        const core::Status st = c.boot(core::prepareBoot(store));
        CHECK(st.result == Result::Busy);
        CHECK(st.detail == Detail::NotAtBoot);
        c.stop();
        CHECK_FALSE(c.isRunning());
        c.stop(); // idempotent
    }
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK(c.boot(core::prepareBoot(store)).detail == Detail::NotAtBoot);
}
