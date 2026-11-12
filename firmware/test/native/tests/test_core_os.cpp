// core_os, native backend: Mutex, Event, PeriodicTask, the clock, the
// recording diag sink, the in-memory KvStore with its fault injection, the
// boot info fakes and the one-shot timer.

#include "doctest.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "core_os/boot_info.hpp"
#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/event.hpp"
#include "core_os/mutex.hpp"
#include "core_os/kv_store.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/native_kv.hpp"
#include "core_os/one_shot.hpp"
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

namespace {

std::vector<uint8_t> readAll(core_os::KvStore &store, const char *ns, const char *key)
{
    size_t len = 0;
    if (store.get(ns, key, nullptr, len) != core_os::kOk) {
        return {};
    }
    std::vector<uint8_t> out(len);
    REQUIRE(store.get(ns, key, out.data(), len) == core_os::kOk);
    return out;
}

void collect(void *ctx, const char *ns)
{
    static_cast<std::set<std::string> *>(ctx)->insert(ns);
}

} // namespace

TEST_CASE("KvStore: get, set, size, erase and list")
{
    core_os::KvStore store;
    const uint8_t data[] = {1, 2, 3, 4, 5};
    size_t len = 0;
    CHECK(store.get("ns", "k", nullptr, len) != core_os::kOk); // missing
    REQUIRE(store.set("ns", "k", data, sizeof(data)) == core_os::kOk);
    REQUIRE(store.get("ns", "k", nullptr, len) == core_os::kOk);
    CHECK(len == 5);
    uint8_t small[3] = {};
    len = sizeof(small);
    CHECK(store.get("ns", "k", small, len) != core_os::kOk); // too small: nothing copied
    CHECK(small[0] == 0);
    CHECK(readAll(store, "ns", "k") == std::vector<uint8_t>{1, 2, 3, 4, 5});
    REQUIRE(store.set("ns", "k", data, 2) == core_os::kOk); // overwrite
    CHECK(readAll(store, "ns", "k").size() == 2);
    CHECK(store.set("a_name_too_long_", "k", data, 1) != core_os::kOk);
    CHECK(store.set("ns", "", data, 1) != core_os::kOk);

    REQUIRE(store.set("other", "k", data, 1) == core_os::kOk);
    std::set<std::string> names;
    REQUIRE(store.listNamespaces(&collect, &names) == core_os::kOk);
    CHECK(names == std::set<std::string>{"ns", "other"});
    REQUIRE(store.eraseNamespace("ns") == core_os::kOk);
    REQUIRE(store.eraseNamespace("never") == core_os::kOk);
    names.clear();
    REQUIRE(store.listNamespaces(&collect, &names) == core_os::kOk);
    CHECK(names == std::set<std::string>{"other"});
    CHECK(core_os::native::writeCount(store) == 5); // 3 sets, 2 erases
    CHECK(core_os::nvsInit().err == core_os::kOk);
}

TEST_CASE("KvStore: power loss, a failing write and torn writes")
{
    core_os::KvStore store;
    const uint8_t data[] = {1, 2, 3, 4, 5, 6};

    core_os::native::failWritesAfter(store, 2);
    CHECK(store.set("n", "a", data, 6) == core_os::kOk);
    CHECK(store.eraseNamespace("x") == core_os::kOk);
    CHECK(store.set("n", "b", data, 6) != core_os::kOk); // the power is gone
    CHECK(store.eraseNamespace("n") != core_os::kOk);
    CHECK_FALSE(core_os::native::hasKey(store, "n", "b"));
    CHECK(core_os::native::hasKey(store, "n", "a"));
    core_os::native::clearFaults(store);
    CHECK(store.set("n", "b", data, 6) == core_os::kOk);

    core_os::native::failWriteAfter(store, 1, 0x1234);
    CHECK(store.set("n", "c", data, 6) == core_os::kOk);
    CHECK(store.set("n", "d", data, 6) == 0x1234);
    CHECK(store.set("n", "d", data, 6) == core_os::kOk); // only once
    core_os::native::failNextWrite(store, 7);
    CHECK(store.set("n", "a", data, 1) == 7);
    CHECK(readAll(store, "n", "a").size() == 6); // unchanged

    core_os::native::tearNextWrite(store);
    CHECK(store.set("n", "t", data, 6) == core_os::kOk); // says yes
    CHECK(readAll(store, "n", "t") == std::vector<uint8_t>{1, 2, 3});
    core_os::native::tearWriteAfter(store, 1, core_os::native::Tear::Corrupt);
    CHECK(store.set("n", "u", data, 6) == core_os::kOk);
    CHECK(store.set("n", "v", data, 6) == core_os::kOk);
    CHECK(readAll(store, "n", "u") == std::vector<uint8_t>{1, 2, 3, 4, 5, 6});
    CHECK(readAll(store, "n", "v") == std::vector<uint8_t>{1, 2, 3, 4, 5, 0xF9});

    core_os::native::removeKey(store, "n", "v");
    CHECK_FALSE(core_os::native::hasKey(store, "n", "v"));
    CHECK(core_os::native::namespaceCount(store) == 1);
}

TEST_CASE("Boot info fakes: crash reset, RTC counter, reboot")
{
    core_os::native::resetBootInfo();
    CHECK_FALSE(core_os::resetWasCrash());
    CHECK(core_os::loadCrashCount() == 0);
    core_os::native::setResetWasCrash(true);
    CHECK(core_os::resetWasCrash());
    core_os::storeCrashCount(2);
    CHECK(core_os::loadCrashCount() == 2);
    core_os::reboot();
    CHECK(core_os::native::rebootCount() == 1);
    core_os::native::resetBootInfo();
    CHECK(core_os::loadCrashCount() == 0);
    CHECK(core_os::native::rebootCount() == 0);
}

TEST_CASE("OneShotTimer: fires once after the delay, and cancel stops it")
{
    std::atomic<int> calls{0};
    auto fn = [](void *ctx) { static_cast<std::atomic<int> *>(ctx)->fetch_add(1); };
    {
        core_os::OneShotTimer t;
        CHECK(t.start("t", 0, nullptr, nullptr) != core_os::kOk);
        const int64_t t0 = core_os::nowUs();
        REQUIRE(t.start("t", 20000, fn, &calls) == core_os::kOk);
        for (int i = 0; i < 200 && calls.load() == 0; ++i) {
            std::this_thread::sleep_for(5ms);
        }
        CHECK(calls.load() == 1);
        CHECK(core_os::nowUs() - t0 >= 20000);
        std::this_thread::sleep_for(50ms);
        CHECK(calls.load() == 1); // once

        REQUIRE(t.start("t", 200000, fn, &calls) == core_os::kOk);
        t.cancel();
        t.cancel(); // idempotent
        std::this_thread::sleep_for(50ms);
        CHECK(calls.load() == 1);
        REQUIRE(t.start("t", 300000, fn, &calls) == core_os::kOk);
    } // the destructor cancels
    CHECK(calls.load() == 1);
}
