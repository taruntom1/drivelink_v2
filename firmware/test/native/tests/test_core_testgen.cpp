#include "doctest.h"
#include "core/command_service.hpp"
#include "core_os/clock.hpp"
#include "core_os/native_task.hpp"
#include "core_os/realtime.hpp"
#include "core_test_modules.hpp"
#include <cmath>
#include <limits>
#include <thread>

using namespace testmod;
namespace {
using Wave = core::TestgenWaveform;
using State = core::TestgenState;
using Reason = core::TestgenReason;
struct Manual {
    Manual() { core_os::native::setManualScheduling(true); reset(); }
    ~Manual() { core_os::native::setManualScheduling(false); }
};
struct Generator {
    Manual manual;
    core_os::KvStore store;
    core::Core c{testConfig()};
    core::TestgenConfig cfg;
    uint32_t run = 0;
    int64_t now = 0;
    explicit Generator(uint8_t other_loop=1) {
        bootWith(c, store, {{fx::kTypeId,0,0,"a"},{fx::kTypeId,1,other_loop,"b"},{fx::kTypeId,2,0,"c"},
            {5,other_loop,255,other_loop==1 ? "sensor" : "sparse_loop",{{1,100},{2,1}}}});
        cfg.loop = 0; cfg.target = {fx::kTypeId,0,fx::kVelocitySp}; cfg.amplitude = 2;
        const std::array<core::Ref,2> refs{cfg.target, core::Ref{fx::kTypeId,2,fx::kVelocitySp}};
        REQUIRE(c.groups().stageConfig(core::GroupKind::Command,0,0,1,refs).ok());
        c.loop(0).runTick(1); REQUIRE(c.groups().complete(core::GroupKind::Command,0,0).ok());
        REQUIRE(c.start().ok()); REQUIRE(c.arm().ok()); c.loop(0).runTick(1); c.loop(1).runTick(1);
        // Synthetic device tick times stay beyond the real test runner clock;
        // termination also fences RX commands at the actual execution instant.
        now = core_os::nowUs() + 1000000000;
    }
    core::TestgenService &g() { return c.testgen(); }
    void tick(int64_t t, uint8_t loop=0, uint32_t word=1) {
        core_os::RealtimeScope scope(true);
        g().beginTick(loop,t,word); g().sample(loop,t);
    }
    core::TestgenSnapshot snap(uint8_t loop=0) {
        core::TestgenSnapshot s; REQUIRE(g().snapshot(loop,s).ok()); return s;
    }
    void start() { REQUIRE(g().stageStart(cfg,run).ok()); tick(now); REQUIRE(g().complete(cfg.loop,0).ok()); }
    bool send(float a, float b, int64_t at) {
        std::array<uint8_t,12> bytes{}; drivelink::protocol::wire::Writer w(bytes.data(),bytes.size());
        w.appendU8(drivelink::protocol::core_wire::kVersion); w.appendU8(1); w.appendU16Le(0);
        w.appendU32Le(f(a)); w.appendU32Le(f(b));
        return c.groups().receiveCommand(0,bytes,at);
    }
    float value(uint8_t instance=0) { return world().created[instance]->velocity_sp; }
    void doneEvent(Reason reason) {
        uint8_t loop; core::TestgenSnapshot s;
        REQUIRE(g().takeDone(loop,s)); CHECK(loop==0); CHECK(s.run==run); CHECK(s.reason==reason);
        CHECK_FALSE(g().takeDone(loop,s));
    }
};
}

TEST_CASE("Testgen waveforms have explicit phase, duration and chirp integration") {
    core::TestgenConfig c; c.duration_ms=2000; c.offset=3; c.amplitude=2;
    CHECK(core::testgenValue(c,0)==5);
    c.waveform=Wave::Ramp;
    CHECK(core::testgenValue(c,0)==3); CHECK(core::testgenValue(c,1)==4); CHECK(core::testgenValue(c,2)==5);
    c.waveform=Wave::Square; c.frequency_start=1;
    CHECK(core::testgenValue(c,0)==5); CHECK(core::testgenValue(c,.5)==1); CHECK(core::testgenValue(c,1)==5);
    c.waveform=Wave::Sine;
    CHECK(core::testgenValue(c,0)==3); CHECK(core::testgenValue(c,.25)==doctest::Approx(5));
    CHECK(core::testgenValue(c,.75)==doctest::Approx(1));
    c.waveform=Wave::ChirpLinear; c.frequency_end=3;
    CHECK(core::testgenValue(c,.5)==doctest::Approx(3+2*std::sin(2*std::acos(-1.)*.625)));
    c.waveform=Wave::ChirpLog; c.frequency_end=4;
    CHECK(core::testgenValue(c,1)==doctest::Approx(3+2*std::sin(2*std::acos(-1.)/std::log(2.))));
    c.frequency_end=1;
    CHECK(core::testgenValue(c,.25)==doctest::Approx(5));
    c.frequency_start=1e-30F; c.frequency_end=499;
    CHECK(std::isfinite(core::testgenValue(c,1.999)));
}
TEST_CASE("Testgen validates START without altering inputs and can withdraw it") {
    Generator f; auto invalid=f.cfg; uint32_t id=99;
    invalid.loop=9; CHECK(f.g().stageStart(invalid,id).detail==core::Detail::LoopMismatch);
    invalid=f.cfg; invalid.target.id=fx::kGain; CHECK(f.g().stageStart(invalid,id).detail==core::Detail::WrongKind);
    invalid.target.id=fx::kRaw; CHECK(f.g().stageStart(invalid,id).detail==core::Detail::Unsupported);
    invalid=f.cfg; invalid.target.instance=1; CHECK(f.g().stageStart(invalid,id).detail==core::Detail::LoopMismatch);
    invalid=f.cfg; invalid.duration_ms=0; CHECK_FALSE(f.g().stageStart(invalid,id).ok());
    invalid=f.cfg; invalid.offset=std::numeric_limits<float>::infinity(); CHECK_FALSE(f.g().stageStart(invalid,id).ok());
    invalid=f.cfg; invalid.waveform=Wave::Sine; invalid.frequency_start=500;
    CHECK_FALSE(f.g().stageStart(invalid,id).ok()); invalid.frequency_start=0;
    CHECK_FALSE(f.g().stageStart(invalid,id).ok());
    invalid=f.cfg; invalid.waveform=Wave::ChirpLog; invalid.frequency_start=1;
    CHECK_FALSE(f.g().stageStart(invalid,id).ok());
    CHECK(id==99); CHECK(f.value()==0); CHECK(f.snap().state==State::Idle);
    REQUIRE(f.g().stageStart(f.cfg,id).ok()); CHECK(f.g().stageStop(0).result==core::Result::Busy);
    CHECK(f.g().complete(0,0).detail==core::Detail::Withdrawn);
    f.tick(f.now); CHECK_FALSE(f.g().running()); CHECK(f.value()==0);
    f.c.disarm(); CHECK(f.g().stageStart(f.cfg,id).result==core::Result::NotArmed);
}
TEST_CASE("Testgen completion holds the last emitted value and events fence restart") {
    Generator f; f.cfg.waveform=Wave::Ramp; f.cfg.duration_ms=10; f.cfg.amplitude=2000;
    const auto violations=core_os::realtimeViolations(); f.start();
    CHECK(f.value()==0); CHECK(f.g().running());
    uint32_t other; CHECK(f.g().stageStart(f.cfg,other).detail==core::Detail::WrongState);
    f.tick(f.now+9000); CHECK(f.value()==1000); // descriptor clamp
    f.tick(f.now+10000); CHECK(f.snap().state==State::Done); CHECK(f.snap().reason==Reason::Completed);
    CHECK(f.value()==1000); CHECK_FALSE(f.g().running());
    CHECK(f.g().stageStart(f.cfg,other).detail==core::Detail::WrongState); // pending event cannot be lost
    f.doneEvent(Reason::Completed);
    CHECK(f.g().stageStart(f.cfg,other).ok()); CHECK(other!=f.run);
    CHECK(f.g().complete(0,0).detail==core::Detail::Withdrawn);
    REQUIRE(f.g().stageStop(0).ok()); f.tick(f.now+11000); REQUIRE(f.g().complete(0,0).ok());
    CHECK(core_os::realtimeViolations()==violations);
}
TEST_CASE("Testgen repeats by device time and an overrun does not slow the waveform") {
    Generator f; f.cfg.waveform=Wave::Ramp; f.cfg.duration_ms=10; f.cfg.repeat=3; f.start();
    f.tick(f.now+25000); CHECK(f.value()==doctest::Approx(1)); CHECK(f.snap().repetitions==2);
    f.tick(f.now+30000); CHECK(f.snap().reason==Reason::Completed); CHECK(f.value()==doctest::Approx(1));
    f.doneEvent(Reason::Completed);
    f.cfg.repeat=0; f.start(); f.tick(f.now+100005000); CHECK(f.g().running());
    CHECK(f.value()==doctest::Approx(1));
    REQUIRE(f.g().stageStop(0).ok()); f.tick(f.now+100006000); REQUIRE(f.g().complete(0,0).ok());
    CHECK(f.value()==doctest::Approx(1)); f.doneEvent(Reason::Stopped);
}
TEST_CASE("Testgen suppresses only its target and discards delayed mailboxes after STOP") {
    Generator f; f.start();
    REQUIRE(f.send(77,88,f.now+1)); f.c.groups().beginTick(0,true,0);
    CHECK(f.value()==2); CHECK(f.value(2)==88);
    int64_t receipt=0; REQUIRE(f.c.groups().lastCommandUs(0,receipt)); CHECK(receipt==f.now+1);
    // A mailbox received during generation is intentionally not consumed until STOP.
    REQUIRE(f.send(99,66,f.now+2)); REQUIRE(f.g().stageStop(0).ok());
    f.tick(f.now+10); REQUIRE(f.g().complete(0,0).ok());
    f.c.groups().beginTick(0,true,0); CHECK(f.value()==2); CHECK(f.value(2)==66);
    f.doneEvent(Reason::Stopped);
    // New target does not remove the old target's handoff gate.
    f.cfg.target.instance=2; f.start(); REQUIRE(f.send(111,222,f.now+3));
    f.c.groups().beginTick(0,true,0); CHECK(f.value()==2); CHECK(f.value(2)==2);
    REQUIRE(f.send(7,8,f.now+11)); f.c.groups().beginTick(0,true,0);
    CHECK(f.value()==7); CHECK(f.value(2)==2);
    f.c.disarm(); f.c.loop(0).runTick(1);
    CHECK(f.value()==0); CHECK(f.value(2)==0); CHECK_FALSE(f.g().running());
}
TEST_CASE("Testgen binds runs to arm generation, including a rapid disarm and re-arm") {
    Generator f; REQUIRE(f.g().stageStart(f.cfg,f.run).ok());
    f.c.disarm(); REQUIRE(f.c.arm().ok()); f.tick(f.now,0,9);
    CHECK(f.g().complete(0,0).detail==core::Detail::DisarmRaced); CHECK(f.value()==0);
    // Start through runTick so production arm snapshot is used.
    REQUIRE(f.g().stageStart(f.cfg,f.run).ok()); f.c.loop(0).runTick(1); REQUIRE(f.g().complete(0,0).ok());
    CHECK(f.g().running()); f.c.disarm(); CHECK_FALSE(f.g().running());
    CHECK(f.snap().reason==Reason::Disarmed);
    REQUIRE(f.c.arm().ok()); f.c.loop(0).runTick(1);
    CHECK(f.value()==0); f.doneEvent(Reason::Disarmed); CHECK_FALSE(f.g().running());
}
TEST_CASE("Testgen capture trigger belongs to the loop and consumes the next due sample") {
    Generator f; core::CaptureTrigger trigger{core::CaptureTriggerKind::TestgenStart,{},core::CaptureEdge::Rising,0};
    const core::Ref ref=f.cfg.target;
    REQUIRE(f.c.capture().configure(0,2,3,0,trigger,{&ref,1}).ok()); REQUIRE(f.c.capture().arm().ok());
    f.c.capture().sample(0,f.now-1000,1000); // first due sample, next tick is divided out
    auto other=f.cfg; other.loop=1; other.target.instance=1; uint32_t id;
    REQUIRE(f.g().stageStart(other,id).ok()); f.tick(f.now,1); REQUIRE(f.g().complete(1,0).ok());
    f.c.capture().sample(0,f.now,1000); CHECK(f.c.capture().snapshot().state==core::CaptureState::Armed);
    f.start(); f.c.capture().sample(0,f.now+1000,1000);
    CHECK(f.c.capture().snapshot().state==core::CaptureState::Triggered);
    f.c.disarm(); f.tick(f.now+2000,0,8);
    f.c.capture().sample(0,f.now+2000,1000); f.c.capture().sample(0,f.now+3000,1000);
    f.c.capture().sample(0,f.now+4000,1000); f.c.capture().sample(0,f.now+5000,1000);
    CHECK(f.c.capture().snapshot().state==core::CaptureState::Done);
}
TEST_CASE("Testgen command service validates exact lengths and status/events") {
    Generator f; core::CommandService service(f.c,{});
    std::array<uint8_t,128> response{};
    std::vector<uint8_t> request{drivelink::protocol::core_wire::kVersion,3,7,3,0};
    auto n=service.handle(request,response); CHECK(n==54); CHECK(response[4]==0);
    request.push_back(0); service.handle(request,response); CHECK(response[4]==1);
    request={drivelink::protocol::core_wire::kVersion,3,7,1,0};
    service.handle(request,response); CHECK(response[4]==1); CHECK(f.snap().state==State::Idle);
    f.start(); REQUIRE(f.g().stageStop(0).ok()); f.tick(f.now+1); REQUIRE(f.g().complete(0,0).ok());
    std::array<uint8_t,8> event{}; CHECK(core::testgenDoneEvent(f.c,event)==8);
    CHECK(event[1]==2); CHECK(event[2]==0); CHECK(event[7]==static_cast<uint8_t>(Reason::Stopped));
    CHECK(core::testgenDoneEvent(f.c,event)==0);
}
TEST_CASE("Testgen snapshots stay coherent with independent concurrent loop writers") {
    Generator f; f.cfg.repeat=0; f.start();
    auto other=f.cfg; other.loop=1; other.target.instance=1; other.amplitude=7;
    uint32_t id; REQUIRE(f.g().stageStart(other,id).ok()); f.tick(f.now,1); REQUIRE(f.g().complete(1,0).ok());
    std::atomic<bool> stop{false};
    auto writer=[&](uint8_t loop) {
        int64_t now=f.now;
        while (!stop.load()) { now+=1000; f.tick(now,loop); }
    };
    std::thread a(writer,0), b(writer,1);
    bool coherent=true;
    for (unsigned i=0; i<2000; ++i) {
        for (uint8_t loop : {0,1}) {
            core::TestgenSnapshot s;
            if (f.g().snapshot(loop,s).ok()) {
                coherent &= s.state==State::Running && s.reason==Reason::None &&
                    s.value_bits==defs::f32Bits(loop ? 7.F : 2.F);
            }
        }
    }
    stop.store(true); a.join(); b.join(); CHECK(coherent);
    CHECK(f.g().running()); f.c.disarm(); CHECK_FALSE(f.g().running());
    f.tick(f.now,0,8); f.tick(f.now,1,8);
    uint8_t loop; core::TestgenSnapshot s;
    REQUIRE(f.g().takeDone(loop,s)); CHECK(s.reason==Reason::Disarmed);
    REQUIRE(f.g().takeDone(loop,s)); CHECK(s.reason==Reason::Disarmed);
    CHECK_FALSE(f.g().takeDone(loop,s));
}
TEST_CASE("Testgen routes by sparse loop id rather than capacity index") {
    Generator f(7); f.c.loop(7).runTick(1);
    f.cfg.loop=7; f.cfg.target.instance=1;
    REQUIRE(f.g().stageStart(f.cfg,f.run).ok()); f.tick(f.now,7); REQUIRE(f.g().complete(7,0).ok());
    CHECK(f.snap(7).state==State::Running); CHECK(f.snap().state==State::Idle);
    REQUIRE(f.g().stageStop(7).ok()); f.tick(f.now+1,7); REQUIRE(f.g().complete(7,0).ok());
    uint8_t loop; core::TestgenSnapshot s; REQUIRE(f.g().takeDone(loop,s)); CHECK(loop==7);
}
TEST_CASE("Testgen terminal event cannot be replaced by a concurrent restart") {
    Generator f; f.cfg.duration_ms=1;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        int64_t now=f.now;
        while (!stop.load()) { f.tick(now); now+=1000; std::this_thread::yield(); }
    });
    bool valid=true;
    for (unsigned i=0; i<100; ++i) {
        uint32_t run=0;
        auto status=f.g().start(f.cfg,run);
        if (!status.ok()) { valid=false; break; }
        uint8_t loop=255; core::TestgenSnapshot s;
        const auto deadline=core_os::nowUs()+1000000;
        while (!f.g().takeDone(loop,s) && core_os::nowUs()<deadline) { std::this_thread::yield(); }
        if (loop!=0 || s.run!=run || s.reason!=Reason::Completed) { valid=false; break; }
        // takeDone may observe the terminal mailbox just before live Done.
        core::TestgenSnapshot live;
        while ((!f.g().snapshot(0,live).ok() || live.state==State::Running) && core_os::nowUs()<deadline) {
            std::this_thread::yield();
        }
    }
    stop.store(true); writer.join(); CHECK(valid);
}
