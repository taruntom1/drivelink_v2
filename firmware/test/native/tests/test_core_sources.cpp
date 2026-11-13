#include "doctest.h"
#include "core/source_service.hpp"
#include "core_os/native_task.hpp"
#include "core_os/realtime.hpp"
#include "core_os/clock.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/type_fusion_mock.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "mod_mock/modules.hpp"
#include <atomic>
#include <thread>

namespace {
using namespace testmod;
namespace fusion = defs::fusion_mock;
struct Manual {
    Manual() { reset(); core_os::native::setManualScheduling(true); }
    ~Manual() { core_os::native::setManualScheduling(false); }
};
class Consumer final : public core::Instance {
public:
    core::Sampled<float> a, b;
    float combined = 0;
    bool start(core::InstanceContext &ctx) override {
        return ctx.bindSource(fusion::kFeedback, &a) && ctx.bindSource(fusion::kImu, &b) &&
               ctx.bindSignal(fusion::kCombined, &combined);
    }
    void tick(const core::TickContext &) override { combined = (a.valid ? a.value : 0) + (b.valid ? b.value : 0); }
};
core::Instance *makeConsumer(const core::InstanceConfig &) { return new Consumer; }
const core::ModuleType kConsumer{fusion::kDesc, makeConsumer};
const std::array<const core::ModuleType *, 2> kSourceTypes{&kFullType, &kConsumer};
uint32_t target(uint8_t instance = 0, uint16_t id = fx::kPosition) { return defs::sourceRef(fx::kTypeId, instance, id); }
Consumer &consumer(core::Core &c, uint8_t instance = 0) {
    const auto *r = c.findRecord(fusion::kTypeId, instance);
    REQUIRE(r); REQUIRE(r->state == core::InstanceState::Running);
    return static_cast<Consumer &>(*r->object);
}
void boot(core::Core &c, core_os::KvStore &store, const std::vector<Inst> &layout) {
    writeLayout(store, layout); REQUIRE(c.init(kSourceTypes).ok()); REQUIRE(c.boot(store).ok());
}
}

TEST_CASE("Sources: packed refs, generated associations and saturated ages") {
    const auto ref = defs::sourceRef(0xFE, 0xFD, 0xFFFF);
    CHECK(ref == 0xFEFDFFFFU); CHECK(defs::sourceRefType(ref) == 0xFE);
    CHECK(defs::sourceRefInstance(ref) == 0xFD); CHECK(defs::sourceRefId(ref) == 0xFFFF);
    CHECK(core::sampleAgeUs(100, 75) == 25); CHECK(core::sampleAgeUs(75, 100) == 0);
    CHECK(core::sampleAgeUs(INT64_MAX, 0) == core::kMaxAgeUs);
    CHECK(core::sampleAgeUs(0xFFFFFFFELL, 0) == core::kMaxAgeUs);
    CHECK(defs::sourceSignal(fusion::kDesc, fusion::kItems[0], ".value")->id == fusion::kFeedbackValue);
}

TEST_CASE("Sources: bindSource validates storage, descriptors, types and duplicate binds") {
    auto record = [] {
        auto r = std::make_unique<core::detail::InstanceRecord>(); r->desc = &fusion::kDesc;
        r->bindings = std::make_unique<core::detail::Binding[]>(fusion::kItems.size());
        r->shadow = std::make_unique<uint32_t[]>(fusion::kItems.size()); return r;
    };
    SUBCASE("good and duplicate") {
        auto r = record(); core::InstanceContext ctx(*r); core::Sampled<float> sample;
        REQUIRE(ctx.bindSource(1, &sample)); CHECK(sample.age_us == core::kInvalidAgeUs);
        CHECK_FALSE(sample.valid); CHECK(r->sources.size() == 1);
        CHECK_FALSE(ctx.bindSource(1, &sample)); CHECK(ctx.bindError() == core::Detail::AlreadyBound);
    }
    SUBCASE("wrong type") {
        auto r = record(); core::InstanceContext ctx(*r); core::Sampled<uint32_t> sample;
        CHECK_FALSE(ctx.bindSource(1, &sample)); CHECK(ctx.bindError() == core::Detail::WrongType);
    }
    SUBCASE("null") {
        auto r = record(); core::InstanceContext ctx(*r);
        CHECK_FALSE(ctx.bindSource<float>(1, nullptr)); CHECK(ctx.bindError() == core::Detail::NullStorage);
    }
    SUBCASE("signal is not a source") {
        auto r = record(); core::InstanceContext ctx(*r); core::Sampled<float> sample;
        CHECK_FALSE(ctx.bindSource(fusion::kCombined, &sample)); CHECK(ctx.bindError() == core::Detail::WrongKind);
    }
}

TEST_CASE("Sources: cross-loop snapshots, freshness, aging and arm retention") {
    Manual manual; core_os::KvStore store; core::Core c{core::CoreConfig{}};
    boot(c, store, {{fusion::kTypeId,0,1,"consumer",{{1,target()},{2,target()}}}, {fx::kTypeId,0,0,"producer"}});
    auto &sample = consumer(c);
    c.loop(1).runTick(1); CHECK_FALSE(sample.a.valid); CHECK_FALSE(sample.a.fresh);
    CHECK(sample.a.age_us == core::kInvalidAgeUs);
    world().created[0]->position = 7.F;
    c.loop(0).runTick(1); c.loop(1).runTick(1);
    CHECK(sample.a.valid); CHECK(sample.a.fresh); CHECK(sample.a.value == 7.F);
    CHECK(sample.a.age_us == sample.b.age_us); CHECK(sample.combined == 14.F);
    const auto age = sample.a.age_us;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    c.loop(1).runTick(1); CHECK_FALSE(sample.a.fresh); CHECK(sample.a.age_us > age);
    // Paused producers keep the last snapshot, while age continues growing.
    c.loop(0).actionSlot().state.store(core::ActionState::Paused);
    c.loop(0).runTick(1); c.loop(1).runTick(1); CHECK_FALSE(sample.a.fresh); CHECK(sample.a.value == 7.F);
    c.loop(0).actionSlot().state.store(core::ActionState::Empty);
    REQUIRE(c.groups().stageConfig(core::GroupKind::Command,0,1,1,{}).ok());
    c.loop(1).runTick(1); REQUIRE(c.groups().complete(core::GroupKind::Command,0,0).ok());
    REQUIRE(c.start().ok()); REQUIRE(c.arm().ok()); c.loop(1).runTick(1);
    CHECK(sample.a.valid); CHECK(sample.a.value == 7.F);
    c.disarm(); c.loop(1).runTick(1); CHECK(sample.a.valid); CHECK(sample.a.value == 7.F);
    const core::Ref limit{0,0,defs::system::kLimitsLinkValues}; uint32_t value = 0;
    REQUIRE(c.params().get({&limit,1},{&value,1}).ok()); CHECK(value == 32);
    const core::SetEntry write{limit,1}; std::array<uint32_t,1> applied{};
    CHECK(c.params().set({&write,1},applied).detail == core::Detail::ReadOnly);
}

TEST_CASE("Sources: same-loop producer ticks first, sources can read command inputs") {
    Manual manual; core_os::KvStore store; core::Core c{core::CoreConfig{}};
    boot(c, store, {{fusion::kTypeId,0,0,"consumer",{{1,target()},{2,target(0,fx::kVelocitySp)}}},
                    {fx::kTypeId,0,0,"producer"}});
    auto &sample = consumer(c); world().created[0]->position = 4.F;
    world().created[0]->velocity_sp = 3.F;
    c.loop(0).runTick(1);
    CHECK(sample.a.value == 4.F); CHECK(sample.a.age_us == 0); CHECK(sample.a.fresh);
    CHECK(sample.b.value == 3.F); CHECK(sample.b.age_us == 0);
    CHECK(world().tick_log.size() == 1);
}

TEST_CASE("Sources: a failed producer leaves its consumer running and invalid") {
    Manual manual; world().mode = StartMode::ReturnFalse;
    core_os::KvStore store; core::Core c{core::CoreConfig{}};
    boot(c, store, {{fx::kTypeId,0,0,"failed"},{fusion::kTypeId,0,1,"consumer",{{1,target()}}}});
    CHECK(c.findRecord(fx::kTypeId,0)->state == core::InstanceState::Failed);
    auto &sample = consumer(c); c.loop(1).runTick(1);
    CHECK_FALSE(sample.a.valid); CHECK(sample.a.age_us == core::kInvalidAgeUs);
    CHECK_FALSE(sample.b.valid); CHECK(sample.combined == 0);
}

TEST_CASE("Sources: same-loop chains use current producer output despite reverse layout order") {
    Manual manual; core_os::KvStore store; core::Core c{core::CoreConfig{}};
    boot(c, store, {{fusion::kTypeId,0,0,"last",{{1,defs::sourceRef(fusion::kTypeId,1,fusion::kCombined)}}},
                    {fusion::kTypeId,1,0,"middle",{{1,target()}}}, {fx::kTypeId,0,0,"first"}});
    world().created[0]->position = 9.F;
    c.loop(0).runTick(1);
    CHECK(consumer(c,1).combined == 9.F); CHECK(consumer(c).combined == 9.F);
    CHECK(consumer(c).a.age_us == 0);
}

TEST_CASE("Sources: pending validation is atomic, deduplicates refs and protects targets") {
    Manual manual; core_os::KvStore store; core::CoreConfig config; config.link_values = 1; core::Core c(config);
    boot(c, store, {{fx::kTypeId,0,0,"producer"},{fusion::kTypeId,0,1,"consumer"}});
    std::array<uint32_t,2> applied{};
    const std::array<core::SetEntry,2> duplicates{{{{fusion::kTypeId,0,1},target()},{{fusion::kTypeId,0,2},target()}}};
    CHECK(c.params().set(duplicates,applied).accepted());
    const core::SetEntry overflow{{fusion::kTypeId,0,2},target(0,fx::kVelocitySp)};
    CHECK(c.params().set({&overflow,1},applied).detail == core::Detail::Capacity);
    CHECK(c.removeInstance(fx::kTypeId,0).detail == core::Detail::Referenced);
    const core::Ref pending{fusion::kTypeId,0,2}; uint32_t bits = 0;
    REQUIRE(c.getPending({&pending,1},{&bits,1}).ok()); CHECK(bits == target());
    for (auto bad : {defs::sourceRef(fx::kTypeId,9,fx::kPosition), target(0,fx::kGain), target(0,fx::kRaw)}) {
        const core::SetEntry invalid{{fusion::kTypeId,0,1},bad};
        CHECK(c.params().set({&invalid,1},applied).detail == core::Detail::BadRef);
    }
    REQUIRE(c.save().ok()); c.stop();
    core::Core reloaded(config); REQUIRE(reloaded.init(kSourceTypes).ok()); REQUIRE(reloaded.boot(store).ok());
    const core::Ref saved{fusion::kTypeId,0,1};
    REQUIRE(reloaded.params().get({&saved,1},{&bits,1}).ok()); CHECK(bits == target());
    reloaded.loop(0).runTick(1); reloaded.loop(1).runTick(1); CHECK(consumer(reloaded).a.valid);
}

TEST_CASE("Sources: same-loop cycles rejected, cross-loop cycles allowed, sparse ids supported") {
    Manual manual;
    for (bool same_loop : {true, false}) {
        core_os::KvStore store; core::Core c{core::CoreConfig{}};
        boot(c, store, {{fusion::kTypeId,0,0,"a"}, {fusion::kTypeId,1,static_cast<uint8_t>(same_loop?0:19),"b"},
             {defs::kLoopTypeId,19,255,"extra",{{1,100},{2,0}}}});
        const std::array<core::SetEntry,2> entries{{
            {{fusion::kTypeId,0,1},defs::sourceRef(fusion::kTypeId,1,fusion::kCombined)},
            {{fusion::kTypeId,1,1},defs::sourceRef(fusion::kTypeId,0,fusion::kCombined)}}};
        std::array<uint32_t,2> applied{};
        auto status = c.params().set(entries,applied);
        if (same_loop) { CHECK(status.detail == core::Detail::WiringCycle); CHECK_FALSE(c.pendingReboot()); }
        else { CHECK(status.accepted()); }
    }
}

TEST_CASE("Sources: triple buffer stays coherent under a faster writer and latest wins") {
    core::detail::SourceBuffer buffer(16);
    CHECK_FALSE(buffer.take()); CHECK_FALSE(buffer.valid());
    auto publish = [&](uint32_t seq) {
        for (size_t i = 0; i < 16; ++i) { buffer.backValues()[i] = seq; }
        buffer.publish(seq);
    };
    publish(1); publish(2); REQUIRE(buffer.take()); CHECK(buffer.values()[0] == 2);
    CHECK_FALSE(buffer.take()); CHECK(buffer.timestampUs() == 2);
    std::atomic<bool> done{false};
    std::thread writer([&] { for (uint32_t seq=3; seq<100000; ++seq) { publish(seq); } done.store(true); });
    uint32_t last = 2; bool coherent = true;
    do {
        if (buffer.take()) {
            const auto seq = buffer.values()[0]; coherent &= seq >= last;
            for (auto value : buffer.values()) { coherent &= value == seq; }
            coherent &= buffer.timestampUs() == seq; last = seq;
        }
    } while (!done.load());
    writer.join();
    if (buffer.take()) { last = buffer.values()[0]; }
    CHECK(coherent); CHECK(last == 99999);
}

TEST_CASE("Sources: production fusion ticks under the real-time guard") {
    Manual manual; core_os::KvStore store; core::Core c{core::CoreConfig{}};
    writeLayout(store, {{115,0,0,"fusion",{{2,defs::sourceRef(114,0,256)}}},{114,0,1,"imu"}});
    REQUIRE(c.init(mod_mock::kTypes).ok()); REQUIRE(c.boot(store).ok());
    c.loop(1).runTick(1); c.loop(0).runTick(1);
    const auto *r = c.findRecord(115,0); REQUIRE(r->state == core::InstanceState::Running);
    CHECK(core::loadValue(defs::ValueType::U32,r->bindings[r->itemIndex(259)].ptr) != core::kInvalidAgeUs);
}
