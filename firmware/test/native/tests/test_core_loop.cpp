// core: the loop tick (heartbeat, tick order, dt, overruns) and loop rates.

#include "doctest.h"

#include <chrono>
#include <thread>

#include "core/core.hpp"
#include "core/loop.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"

using namespace testmod;
using namespace std::chrono_literals;

namespace {

namespace sys = drivelink::protocol::defs::system;
namespace lp = drivelink::protocol::defs::loop;
constexpr uint8_t kSys = drivelink::protocol::defs::kSystemTypeId;

class Recorder final : public core::Instance {
public:
    explicit Recorder(int id) : m_id(id) {}
    bool start(core::InstanceContext &ctx) override
    {
        (void)ctx;
        return true;
    }
    void tick(const core::TickContext &t) override
    {
        world().tick_log.push_back(m_id);
        last = t;
    }
    core::TickContext last{};

private:
    int m_id;
};

} // namespace

TEST_CASE("Loop: heartbeat, dt from elapsed × period, overruns and stats")
{
    reset();
    core::Loop loop(core::kFactoryControlLoop, "control", true, 2, 1000);
    Recorder r(1);
    REQUIRE(loop.addInstance(&r));
    CHECK(loop.heartbeat() == 0);

    loop.runTick(1);
    CHECK(loop.heartbeat() == 1);
    CHECK(r.last.elapsed == 1);
    CHECK(r.last.dt_s == doctest::Approx(0.001));
    CHECK(r.last.now_us > 0);

    loop.runTick(3); // two periods were missed
    CHECK(loop.heartbeat() == 2);
    CHECK(r.last.elapsed == 3);
    CHECK(r.last.dt_s == doctest::Approx(0.003));

    loop.setPeriodUs(2500);
    loop.runTick(2);
    CHECK(r.last.dt_s == doctest::Approx(0.005));

    const core::LoopStats s = loop.stats();
    CHECK(s.ticks == 3);
    CHECK(s.overruns == 3); // (3 - 1) + (2 - 1)
    CHECK(s.max_run_us >= s.last_run_us);
}

TEST_CASE("Loop: instances tick in insertion order; the list has a fixed capacity")
{
    reset();
    core::Loop loop(core::kFactorySensorLoop, "sensor", false, 3, 10000);
    Recorder a(10);
    Recorder b(20);
    Recorder c(30);
    Recorder d(40);
    REQUIRE(loop.addInstance(&b));
    REQUIRE(loop.addInstance(&a));
    REQUIRE(loop.addInstance(&c));
    CHECK_FALSE(loop.addInstance(&d)); // full
    CHECK_FALSE(loop.addInstance(nullptr));
    CHECK(loop.instanceCount() == 3);

    loop.runTick(1);
    loop.runTick(1);
    CHECK(world().tick_log == std::vector<int>{20, 10, 30, 20, 10, 30});
}

TEST_CASE("Loop: the core's loops tick their instances, with the loop's period")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    // No wiring: the tick order is the layout order.
    bootWith(c, store,
             {{fx::kTypeId, 4, core::kFactoryControlLoop, "b"},
              {fx::kTypeId, 2, core::kFactoryControlLoop, "a"},
              {fx::kTypeId, 3, core::kFactorySensorLoop, "s"}});

    c.loop(core::kFactoryControlLoop).runTick(1);
    const int t = fx::kTypeId << 8;
    CHECK(world().tick_log == std::vector<int>{t | 4, t | 2}); // not the sensor instance
    CHECK(world().created[0]->last_dt == doctest::Approx(0.001));

    world().tick_log.clear();
    c.loop(core::kFactorySensorLoop).runTick(2);
    CHECK(world().tick_log == std::vector<int>{t | 3});
    CHECK(world().created[2]->last_dt == doctest::Approx(0.02));
    CHECK(world().created[2]->last_elapsed == 2);
}

TEST_CASE("Loop: a system rate change updates the period and the task's timing")
{
    reset();
    core::Core c(testConfig());
    core_os::KvStore store;
    REQUIRE(bootCore(c, store).ok());
    REQUIRE(c.start().ok());
    core::Loop &sensor = c.loop(core::kFactorySensorLoop);
    std::array<uint32_t, 1> applied{};

    auto periods = [&] {
        const core::LoopStats s = sensor.stats();
        return s.ticks + s.overruns;
    };

    const core::SetEntry slow = entry(defs::kLoopTypeId, 1, lp::kRateHz, 10);
    REQUIRE(c.params().set({&slow, 1}, applied).ok());
    CHECK(sensor.periodUs() == 100000);
    uint32_t before = periods();
    std::this_thread::sleep_for(300ms);
    const uint32_t at_10hz = periods() - before;
    CHECK(at_10hz <= 6); // about 3

    const core::SetEntry fast = entry(defs::kLoopTypeId, 1, lp::kRateHz, 500);
    REQUIRE(c.params().set({&fast, 1}, applied).ok());
    CHECK(sensor.periodUs() == 2000);
    before = periods();
    std::this_thread::sleep_for(300ms);
    const uint32_t at_500hz = periods() - before;
    CHECK(at_500hz >= 60); // about 150
    c.stop();
}
