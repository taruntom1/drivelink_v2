// core: safe mode. The counter rule, three crash boots, the 30 s clear,
// editing and saving in safe mode, and EXIT_SAFE_MODE.

#include "doctest.h"

#include <array>
#include <chrono>
#include <thread>

#include "core/core.hpp"
#include "core/safe_mode.hpp"
#include "core_os/boot_info.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

using namespace testmod;
using namespace std::chrono_literals;
using core::InstanceState;

namespace {

namespace codes = drivelink::protocol::defs::diag::codes;

/// A config that clears the crash counter quickly.
core::CoreConfig quickClear()
{
    core::CoreConfig cfg = testConfig();
    cfg.crash_clear_delay_us = 20000;
    return cfg;
}

} // namespace

TEST_CASE("Safe mode: the counter rule")
{
    static_assert(core::decideSafeMode(0, false).counter == 0);
    CHECK(core::decideSafeMode(5, false).counter == 0); // any other reset clears it
    CHECK_FALSE(core::decideSafeMode(5, false).safe_mode);
    CHECK(core::decideSafeMode(0, true).counter == 1);
    CHECK_FALSE(core::decideSafeMode(1, true).safe_mode);
    CHECK(core::decideSafeMode(2, true).counter == 3);
    CHECK(core::decideSafeMode(2, true).safe_mode);
    CHECK(core::decideSafeMode(9, true).safe_mode);
    CHECK(core::decideSafeMode(UINT32_MAX, true).counter == UINT32_MAX); // saturates
}

TEST_CASE("Safe mode: three crash boots in a row; editing and SAVE keep the layout; a normal reset ends it")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {{fx::kTypeId, 0, core::kControlLoop, "a", {{fx::kGain, f(2.0f)}}}});
    core_os::native::setResetWasCrash(true);
    for (uint32_t boot = 1; boot <= 2; ++boot) {
        CAPTURE(boot);
        core::Core c(testConfig());
        REQUIRE(bootCore(c, store).ok());
        CHECK_FALSE(c.safeMode());
        CHECK(core_os::loadCrashCount() == boot);
        CHECK(c.instanceCount() == 1);
    } // destroyed long before 30 s: the counter stays
    {
        world().created.clear();
        core::Core c(quickClear());
        REQUIRE(bootCore(c, store).ok());
        CHECK(c.safeMode());
        CHECK(core_os::loadCrashCount() == 3);
        CHECK(stateOf(c, fx::kTypeId, 0) == InstanceState::Offline);
        CHECK(c.instanceCount() == 0);
        CHECK(c.instancesFailed() == 0);
        CHECK(world().created.empty());
        CHECK(c.loop(core::kControlLoop).instanceCount() == 0);
        REQUIRE(core_os::native::reportCount(codes::core::kSafeModeEntered) == 1);
        CHECK(core_os::native::recordedReports().back().arg0 == 3);
        CHECK(c.configLoadedFromNvs());

        // The loops still run, empty.
        REQUIRE(c.start().ok());
        // Params can be read and edited, and SAVE keeps the layout.
        CHECK(getParam(c, fx::kTypeId, 0, fx::kGain) == f(2.0f));
        std::array<uint32_t, 1> applied{};
        const core::SetEntry gain = entry(fx::kTypeId, 0, fx::kGain, f(4.0f));
        REQUIRE(c.params().set({&gain, 1}, applied).ok());
        REQUIRE(c.save().ok());
        std::this_thread::sleep_for(100ms);
        CHECK(core_os::loadCrashCount() == 3); // not cleared in safe mode
        c.stop();
    }

    // A reset that is not a crash ends it.
    core_os::native::setResetWasCrash(false);
    reset();
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK_FALSE(c.safeMode());
    CHECK(core_os::loadCrashCount() == 0);
    CHECK(stateOf(c, fx::kTypeId, 0) == InstanceState::Running);
    CHECK(world().created[0]->gain == 4.0f);
}

TEST_CASE("Safe mode: the counter is cleared a while after a normal boot")
{
    reset();
    core_os::KvStore store;
    core_os::native::setResetWasCrash(true);
    core::Core c(quickClear());
    REQUIRE(bootCore(c, store).ok());
    CHECK(core_os::loadCrashCount() == 1);
    for (int i = 0; i < 100 && core_os::loadCrashCount() != 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(core_os::loadCrashCount() == 0);
}

TEST_CASE("Safe mode: EXIT_SAFE_MODE clears the counter")
{
    reset();
    core_os::KvStore store;
    core_os::storeCrashCount(5);
    core_os::native::setResetWasCrash(true);
    {
        core::Core c(testConfig());
        REQUIRE(bootCore(c, store).ok());
        CHECK(c.safeMode());
        REQUIRE(c.exitSafeMode().ok());
        CHECK(core_os::loadCrashCount() == 0);
        CHECK(core_os::native::rebootCount() == 0); // the caller reboots
    }
    // Even if that reboot is reported as a crash, it starts counting from 0.
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK_FALSE(c.safeMode());
    CHECK(core_os::loadCrashCount() == 1);
}
