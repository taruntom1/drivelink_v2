// The lease state machine, the history ring and the status-stream arm parse.
#include "history.hpp"
#include "lease.hpp"
#include <doctest.h>

using namespace drivelink::agent;

TEST_CASE("lease: a free lease is granted at once and taken again as a no-op")
{
    Lease l;
    CHECK(l.phase() == Lease::Phase::Free);
    CHECK(l.holder() == 0);
    CHECK(l.mayExclusive(7, false));
    CHECK_FALSE(l.mayExclusive(7, true)); // nobody holds it, but the device is armed
    CHECK_FALSE(l.mayCommand(7));
    CHECK(l.take(7) == Lease::Take::Granted);
    CHECK(l.holder() == 7);
    CHECK(l.mayCommand(7));
    CHECK(l.mayExclusive(7, true)); // the holder, armed or not
    CHECK_FALSE(l.mayCommand(8));
    CHECK_FALSE(l.mayExclusive(8, false)); // someone else holds it
    CHECK(l.take(7) == Lease::Take::AlreadyHolder);
    CHECK(l.holder() == 7);
}

TEST_CASE("lease: Take preempts; nobody commands until the end completes")
{
    Lease l;
    REQUIRE(l.take(1) == Lease::Take::Granted);
    CHECK(l.take(2) == Lease::Take::Revoked);
    CHECK(l.phase() == Lease::Phase::Ending);
    CHECK(l.ending() == 1);
    CHECK(l.pending() == 2);
    CHECK(l.holder() == 0);
    CHECK_FALSE(l.mayCommand(1));
    CHECK_FALSE(l.mayCommand(2));
    CHECK_FALSE(l.mayExclusive(1, false));
    CHECK_FALSE(l.mayExclusive(3, false)); // a handoff is running
    CHECK(l.take(3) == Lease::Take::Busy);
    CHECK(l.take(2) == Lease::Take::Busy); // a second Take from the waiting client
    CHECK_FALSE(l.release(1));             // the old holder lost it already
    auto granted = l.finish();
    REQUIRE(granted);
    CHECK(*granted == 2);
    CHECK(l.holder() == 2);
    CHECK(l.mayCommand(2));
    CHECK(l.pending() == 0);
}

TEST_CASE("lease: release and disconnect end it without a successor")
{
    Lease l;
    REQUIRE(l.take(1) == Lease::Take::Granted);
    CHECK_FALSE(l.release(2)); // not the holder
    CHECK(l.release(1));
    CHECK(l.phase() == Lease::Phase::Ending);
    CHECK_FALSE(l.mayExclusive(2, false));
    CHECK_FALSE(l.finish());
    CHECK(l.phase() == Lease::Phase::Free);
    CHECK(l.mayExclusive(2, false));

    REQUIRE(l.take(3) == Lease::Take::Granted);
    CHECK(l.leave(4) == Lease::Left::None);
    CHECK(l.leave(3) == Lease::Left::Holder);
    CHECK(l.phase() == Lease::Phase::Ending);
    CHECK_FALSE(l.finish());
    CHECK(l.phase() == Lease::Phase::Free);
}

TEST_CASE("lease: a Take while a lease ends waits; its client leaving drops it")
{
    Lease l;
    REQUIRE(l.take(1) == Lease::Take::Granted);
    REQUIRE(l.release(1));
    CHECK(l.take(2) == Lease::Take::Waiting);
    CHECK(l.pending() == 2);
    CHECK(l.leave(2) == Lease::Left::Pending);
    CHECK(l.pending() == 0);
    CHECK_FALSE(l.finish());
    CHECK(l.phase() == Lease::Phase::Free);

    REQUIRE(l.take(1) == Lease::Take::Granted);
    REQUIRE(l.take(2) == Lease::Take::Revoked);
    CHECK(l.leave(1) == Lease::Left::None); // the old holder leaving changes nothing
    CHECK(l.leave(2) == Lease::Left::Pending);
    CHECK_FALSE(l.finish());
    CHECK(l.phase() == Lease::Phase::Free);
}

TEST_CASE("lease: finish outside an end and reset")
{
    Lease l;
    CHECK_FALSE(l.finish());
    REQUIRE(l.take(1) == Lease::Take::Granted);
    CHECK_FALSE(l.finish()); // held, nothing ending
    CHECK(l.holder() == 1);
    REQUIRE(l.take(2) == Lease::Take::Revoked);
    l.reset();
    CHECK(l.phase() == Lease::Phase::Free);
    CHECK(l.pending() == 0);
    CHECK(l.take(3) == Lease::Take::Granted);
}

TEST_CASE("history: keeps the newest entries, oldest first")
{
    CHECK_THROWS(History(0));
    History h(3);
    CHECK(h.entries().empty());
    for (uint8_t i = 1; i <= 5; ++i) { h.push({i, 1000u + i, {i}}); }
    CHECK(h.size() == 3);
    CHECK(h.capacity() == 3);
    const auto e = h.entries();
    REQUIRE(e.size() == 3);
    CHECK(e[0].kind == 3);
    CHECK(e[1].kind == 4);
    CHECK(e[2].kind == 5);
    CHECK(e[2].unix_ms == 1005);
    History big(4);
    big.push({1, 1, drivelink::agent::Bytes(300, 7)});
    CHECK(big.entries().front().body.size() == 255);
}

TEST_CASE("parseStatusArm: arm state and last disarm reason")
{
    // version, epoch, counter, timestamp, arm_state, flags, errors, cpu[2], loop_count, loops, reason
    std::vector<uint8_t> status{1, 0, 5, 0, 1, 2, 3, 4, 5, 6, 7, 8, /*arm*/ 1, 0, 0, 0xFF, 0xFF, /*loops*/ 1,
                                0, 0, 0, 0, 0, /*reason*/ 2};
    auto parsed = parseStatusArm(status);
    REQUIRE(parsed);
    CHECK(parsed->state == 1);
    CHECK(parsed->armed());
    CHECK(parsed->last_disarm_reason == 2);
    status[12] = 2;
    CHECK_FALSE(parseStatusArm(status)->armed()); // Fault
    status.pop_back();
    CHECK_FALSE(parseStatusArm(status));          // length does not match loop_count
    CHECK_FALSE(parseStatusArm(std::vector<uint8_t>(18, 0)));
    std::vector<uint8_t> none(19, 0);
    CHECK(parseStatusArm(none));
}

TEST_CASE("lease end: CLEARs wait for the DISARM, then the grant waits for the CLEARs")
{
    LeaseEnd e;
    CHECK_FALSE(e.ending());
    CHECK_FALSE(e.readyToClear());
    e.start();
    CHECK(e.ending());
    CHECK_FALSE(e.readyToClear()); // its DISARM is in flight
    e.disarmDone();
    REQUIRE(e.readyToClear());
    e.clearsStarted(2);
    CHECK_FALSE(e.readyToClear()); // the CLEARs went out once
    CHECK_FALSE(e.readyToFinish());
    e.clearDone();
    CHECK_FALSE(e.readyToFinish());
    e.clearDone();
    REQUIRE(e.readyToFinish());
    e.finished();
    CHECK_FALSE(e.ending());
    CHECK_FALSE(e.readyToFinish());
}

TEST_CASE("lease end: no command slots to clear finishes at once")
{
    LeaseEnd e;
    e.start();
    e.disarmDone();
    REQUIRE(e.readyToClear());
    e.clearsStarted(0);
    CHECK(e.readyToFinish());
}

TEST_CASE("lease end: an ARM in flight and its extra DISARM, in either order of completion")
{
    // The old holder's ARM completes before the end's DISARM: its extra
    // DISARM must still complete before the CLEARs (review finding 1).
    LeaseEnd e;
    e.armSent();
    e.start();
    e.armDone();
    e.redisarmSent();
    CHECK_FALSE(e.readyToClear());
    e.disarmDone();
    CHECK_FALSE(e.readyToClear()); // the extra DISARM is still in flight
    e.redisarmDone();
    CHECK(e.readyToClear());

    // The end's DISARM completes first, then the ARM and its extra DISARM.
    LeaseEnd f;
    f.armSent();
    f.start();
    f.disarmDone();
    CHECK_FALSE(f.readyToClear()); // the ARM is in flight
    f.armDone();
    f.redisarmSent();
    CHECK_FALSE(f.readyToClear());
    f.redisarmDone();
    CHECK(f.readyToClear());
}

TEST_CASE("lease end: a backstop DISARM (armed with nobody holding) holds the CLEARs too")
{
    LeaseEnd e;
    e.start();
    e.redisarmSent(); // the status showed Armed during the end
    e.disarmDone();
    CHECK_FALSE(e.readyToClear());
    e.redisarmDone();
    CHECK(e.readyToClear());
    // Counters never go below zero; reset drops everything.
    e.armDone();
    e.redisarmDone();
    CHECK(e.arms() == 0);
    e.armSent();
    e.reset();
    CHECK(e.arms() == 0);
    CHECK_FALSE(e.ending());
}

TEST_CASE("lease end: an extra DISARM from before this end still counts and releases it")
{
    // Review finding A: a backstop DISARM sent while nobody held the lease
    // completes after a new end started; its completion must still count.
    LeaseEnd e;
    e.redisarmSent(); // backstop in flight, no lease
    e.start();        // someone took and released the lease meanwhile
    e.disarmDone();
    CHECK_FALSE(e.readyToClear());
    e.redisarmDone(); // the backstop completes during the new end
    REQUIRE(e.readyToClear());
    e.clearsStarted(0);
    CHECK(e.readyToFinish());
}
