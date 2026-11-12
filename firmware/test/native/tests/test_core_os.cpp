// core_os, native backend: Mutex, Event, PeriodicTask, the clock and the
// recording diag sink.

#include "doctest.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/event.hpp"
#include "core_os/mutex.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/periodic_task.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

using namespace std::chrono_literals;

namespace {

namespace codes = drivelink::protocol::defs::diag::codes;

struct Counter {
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> periods{0};
    std::atomic<uint32_t> max_elapsed{0};
    std::atomic<int> block_first_ms{0};
};

void countTick(void *ctx, uint32_t elapsed)
{
    auto *c = static_cast<Counter *>(ctx);
    if (c->calls.fetch_add(1) == 0) {
        const int ms = c->block_first_ms.load();
        if (ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }
    c->periods.fetch_add(elapsed);
    if (elapsed > c->max_elapsed.load()) {
        c->max_elapsed.store(elapsed);
    }
}

} // namespace

TEST_CASE("core_os clock is monotonic")
{
    const int64_t a = core_os::nowUs();
    std::this_thread::sleep_for(2ms);
    const int64_t b = core_os::nowUs();
    CHECK(b - a >= 2000);
}

TEST_CASE("core_os Mutex: tryLock fails while another thread holds it")
{
    core_os::Mutex m;
    m.lock();
    bool other = true;
    std::thread t([&] { other = m.tryLock(); });
    t.join();
    CHECK_FALSE(other);
    m.unlock();

    std::thread t2([&] {
        other = m.tryLock();
        if (other) {
            m.unlock();
        }
    });
    t2.join();
    CHECK(other);
}

TEST_CASE("core_os Event: timeout, signal, clear")
{
    core_os::Event e;

    const int64_t start = core_os::nowUs();
    CHECK_FALSE(e.wait(5000));
    CHECK(core_os::nowUs() - start >= 5000); // at least the timeout

    CHECK_FALSE(e.wait(0));
    e.signal();
    e.signal(); // signals collapse
    CHECK(e.wait(0));
    CHECK_FALSE(e.wait(0)); // wait() took it

    e.signal();
    e.clear();
    CHECK_FALSE(e.wait(0));

    // A signal from another thread ends the wait early.
    std::thread t([&] {
        std::this_thread::sleep_for(20ms);
        e.signal();
    });
    const int64_t t0 = core_os::nowUs();
    CHECK(e.wait(2000000));
    CHECK(core_os::nowUs() - t0 < 1000000);
    t.join();
}

TEST_CASE("core_os PeriodicTask: ticks at its period, then stops")
{
    core_os::PeriodicTask task;
    Counter c;
    CHECK_FALSE(task.isRunning());
    REQUIRE(task.start("t", 4096, 5, core_os::kNoAffinity, 10000, &countTick, &c) == core_os::kOk);
    CHECK(task.isRunning());
    CHECK(task.periodUs() == 10000);
    // A second start is refused while running.
    CHECK(task.start("t", 4096, 5, core_os::kNoAffinity, 10000, &countTick, &c) != core_os::kOk);

    std::this_thread::sleep_for(200ms);
    task.stop();
    CHECK_FALSE(task.isRunning());
    // About 20 periods; loose bounds for a coarse host scheduler. The
    // catch-up count keeps the period total close to the wall time even when
    // wake-ups are late.
    const uint32_t periods = c.periods.load();
    CHECK(periods >= 8);
    CHECK(periods <= 40);
    CHECK(c.calls.load() <= periods);

    const uint32_t after = c.calls.load();
    std::this_thread::sleep_for(50ms);
    CHECK(c.calls.load() == after); // nothing after stop()
    task.stop(); // idempotent
}

TEST_CASE("core_os PeriodicTask: a late tick gets the catch-up count")
{
    core_os::PeriodicTask task;
    Counter c;
    c.block_first_ms = 60; // the first call overruns about 6 periods of 10 ms
    REQUIRE(task.start("t", 4096, 5, core_os::kNoAffinity, 10000, &countTick, &c) == core_os::kOk);
    std::this_thread::sleep_for(150ms);
    task.stop();
    CHECK(c.max_elapsed.load() >= 3);
}

TEST_CASE("core_os PeriodicTask: setPeriod changes the rate")
{
    core_os::PeriodicTask task;
    Counter c;
    CHECK(task.start("t", 4096, 5, core_os::kNoAffinity, 0, &countTick, &c) != core_os::kOk); // zero period
    CHECK(task.start("t", 4096, 5, core_os::kNoAffinity, 1000, nullptr, &c) != core_os::kOk);  // no fn

    REQUIRE(task.start("t", 4096, 5, core_os::kNoAffinity, 100000, &countTick, &c) == core_os::kOk);
    std::this_thread::sleep_for(150ms);
    const uint32_t slow = c.periods.exchange(0);
    CHECK(slow <= 3);

    CHECK(task.setPeriod(0) != core_os::kOk);
    CHECK(task.periodUs() == 100000); // unchanged on an error
    REQUIRE(task.setPeriod(5000) == core_os::kOk);
    CHECK(task.periodUs() == 5000);
    std::this_thread::sleep_for(200ms);
    task.stop();
    CHECK(c.periods.load() >= 20); // about 40
}

TEST_CASE("core_os native diag sink records reports and counts")
{
    core_os::native::resetRecordedDiag();
    core_os::reportDiag(codes::core::kInitFailed, -1, 2, 0);
    core_os::countDiag(codes::uart_comm::kCrcError, 3);
    core_os::countDiag(codes::uart_comm::kCrcError, 2);

    const auto reports = core_os::native::recordedReports();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].code.code == codes::core::kInitFailed.code);
    CHECK(reports[0].err == -1);
    CHECK(reports[0].arg0 == 2);
    CHECK(core_os::native::reportCount(codes::core::kInitFailed) == 1);
    CHECK(core_os::native::reportCount(codes::core::kTimerRestartFailed) == 0);
    CHECK(core_os::native::counterTotal(codes::uart_comm::kCrcError) == 5);

    core_os::native::resetRecordedDiag();
    CHECK(core_os::native::recordedReports().empty());
    CHECK(core_os::native::counterTotal(codes::uart_comm::kCrcError) == 0);
}
