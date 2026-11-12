// core, end to end with threads: the native PeriodicTask runs the loops
// while the param service sets values from the test thread.

#include "doctest.h"

#include <array>
#include <chrono>
#include <thread>

#include "core/core.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"

using namespace testmod;
using namespace std::chrono_literals;

namespace {
namespace sys = drivelink::protocol::defs::system;
namespace lp = drivelink::protocol::defs::loop;
constexpr uint8_t kSys = drivelink::protocol::defs::kSystemTypeId;
} // namespace

TEST_CASE("Threaded: set() completes with Ok while the loops run")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    bootWith(c, store, {{fx::kTypeId, 0, core::kFactoryControlLoop, "ctl"}, {fx::kTypeId, 1, core::kFactorySensorLoop, "sen"}});
    // 100 Hz: the 35 ms staging timeout is well above a coarse host
    // scheduler's sleep granularity.
    std::array<uint32_t, 2> applied{};
    const std::array<core::SetEntry, 2> rates{entry(defs::kLoopTypeId, 0, lp::kRateHz, 100),
                                              entry(defs::kLoopTypeId, 1, lp::kRateHz, 100)};
    REQUIRE(c.params().set(rates, applied).ok());
    REQUIRE(c.start().ok());

    const uint32_t hb = c.loop(core::kFactoryControlLoop).heartbeat();
    for (int i = 1; i <= 20; ++i) {
        CAPTURE(i);
        const auto gain = static_cast<float>(i);
        const std::array<core::SetEntry, 2> batch{entry(fx::kTypeId, 0, fx::kGain, f(gain)),
                                                  entry(fx::kTypeId, 0, fx::kLevel, static_cast<uint32_t>(i % 10))};
        REQUIRE(c.params().set(batch, applied).ok());
        CHECK(applied[0] == f(gain));
        const core::SetEntry sensor_entry = entry(fx::kTypeId, 1, fx::kOffset, static_cast<uint32_t>(i));
        REQUIRE(c.params().set({&sensor_entry, 1}, applied).ok());

        // get() right after returns the new values.
        const std::array<core::Ref, 2> refs{ref(fx::kTypeId, 0, fx::kGain), ref(fx::kTypeId, 1, fx::kOffset)};
        std::array<uint32_t, 2> values{};
        REQUIRE(c.params().get(refs, values).ok());
        CHECK(values[0] == f(gain));
        CHECK(values[1] == static_cast<uint32_t>(i));
    }
    CHECK(c.loop(core::kFactoryControlLoop).heartbeat() > hb);
    c.stop();

    // With the loops stopped, the storage is safe to read.
    FullModule &ctl = *world().created[0];
    FullModule &sen = *world().created[1];
    CHECK(ctl.gain == 20.0f);
    CHECK(ctl.level == 1); // 20 % 10 = 0, clamped to min 1
    CHECK(sen.offset == 20);
    CHECK(ctl.hook_ids.size() == 40); // gain and level, 20 times
    CHECK(ctl.ticks > 0);
    CHECK(sen.ticks > 0);
}
