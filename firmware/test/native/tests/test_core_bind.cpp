// core: the InstanceContext bind API, and what a failed start does.

#include "doctest.h"

#include <memory>

#include "core/core.hpp"
#include "core/instance.hpp"
#include "core_os/native_diag.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"

using namespace testmod;
using core::Detail;
using core::Result;

namespace {

namespace codes = drivelink::protocol::defs::diag::codes;

/// A record for fixture.full, as the core makes one (defaults in the shadow).
std::unique_ptr<core::detail::InstanceRecord> makeRecord()
{
    auto r = std::make_unique<core::detail::InstanceRecord>();
    r->desc = &fx::kDesc;
    r->type_id = fx::kTypeId;
    r->instance = 3;
    r->loop = core::kFactoryControlLoop;
    const size_t n = fx::kDesc.items.size();
    r->bindings = std::make_unique<core::detail::Binding[]>(n);
    r->shadow = std::make_unique<uint32_t[]>(n);
    for (size_t i = 0; i < n; ++i) {
        r->shadow[i] = fx::kDesc.items[i].kind == defs::ItemKind::Param ? fx::kDesc.items[i].default_bits : 0U;
    }
    return r;
}

} // namespace

TEST_CASE("Bind: a good bind writes the boot value into the storage")
{
    auto rec = makeRecord();
    core::InstanceContext ctx(*rec);
    float gain = 0.0f;
    CHECK(ctx.bindParam(fx::kGain, &gain));
    CHECK(gain == 1.5f); // the YAML default
    FxMode mode = FxMode::A;
    CHECK(ctx.bindParam(fx::kMode, &mode));
    CHECK(mode == FxMode::B);
    int32_t offset = 0;
    CHECK(ctx.bindParam(fx::kOffset, &offset));
    CHECK(offset == -5);
    bool enabled = false;
    CHECK(ctx.bindParam(fx::kEnabled, &enabled));
    CHECK(enabled);
    const float position = 0.0f;
    CHECK(ctx.bindSignal(fx::kPosition, &position));
    uint32_t raw = 0;
    CHECK(ctx.bindInput(fx::kRaw, &raw));
    CHECK(ctx.bindError() == Detail::None);

    CHECK(ctx.param(fx::kLevel) == 3);
    CHECK(ctx.param(fx::kPosition) == 0); // not a param
    CHECK(ctx.param(0x7777) == 0);
    CHECK(ctx.typeId() == fx::kTypeId);
    CHECK(ctx.instanceId() == 3);
}

TEST_CASE("Bind: unknown id, wrong kind, wrong type, double bind and null storage are rejected")
{
    SUBCASE("unknown id")
    {
        auto rec = makeRecord();
        core::InstanceContext ctx(*rec);
        float v = 0.0f;
        CHECK_FALSE(ctx.bindParam(uint16_t{0x7777}, &v));
        CHECK(ctx.bindError() == Detail::UnknownRef);
        CHECK_FALSE(ctx.bindParam(uint16_t{0x0400}, &v)); // a retired id
    }
    SUBCASE("wrong kind")
    {
        auto rec = makeRecord();
        core::InstanceContext ctx(*rec);
        float v = 0.0f;
        CHECK_FALSE(ctx.bindSignal(fx::kGain, &v)); // a param
        CHECK(ctx.bindError() == Detail::WrongKind);
        CHECK_FALSE(ctx.bindParam(fx::kPosition, &v)); // a signal
        CHECK_FALSE(ctx.bindInput(fx::kPosition, &v));
        CHECK_FALSE(ctx.bindParam(fx::kReset, &v)); // an action
    }
    SUBCASE("wrong type")
    {
        auto rec = makeRecord();
        core::InstanceContext ctx(*rec);
        uint8_t u8 = 0;
        CHECK_FALSE(ctx.bindParam(fx::kGain, &u8));
        CHECK(ctx.bindError() == Detail::WrongType);
        uint8_t raw_enum = 0;
        CHECK_FALSE(ctx.bindParam(fx::kMode, &raw_enum)); // an enum needs an enum class
        int32_t i32 = 0;
        CHECK_FALSE(ctx.bindParam(fx::kActuator, &i32)); // u32
    }
    SUBCASE("double bind")
    {
        auto rec = makeRecord();
        core::InstanceContext ctx(*rec);
        float a = 0.0f;
        float b = 0.0f;
        CHECK(ctx.bindParam(fx::kGain, &a));
        CHECK_FALSE(ctx.bindParam(fx::kGain, &b));
        CHECK(ctx.bindError() == Detail::AlreadyBound);
    }
    SUBCASE("null storage")
    {
        auto rec = makeRecord();
        core::InstanceContext ctx(*rec);
        CHECK_FALSE(ctx.bindParam(fx::kGain, static_cast<float *>(nullptr)));
        CHECK(ctx.bindError() == Detail::NullStorage);
    }
}

TEST_CASE("Bind: fail() keeps the first stage")
{
    auto rec = makeRecord();
    core::InstanceContext ctx(*rec);
    CHECK_FALSE(ctx.failed());
    ctx.fail(4, 0x103);
    ctx.fail(9, 1);
    CHECK(ctx.failed());
    CHECK(ctx.failStage() == 4);
    CHECK(ctx.failErr() == 0x103);
}

TEST_CASE("Start: a failed instance is reported with its stage and left out")
{
    struct Case {
        StartMode mode;
        uint32_t stage;
        int32_t err;
    };
    const Case cases[] = {
        {StartMode::SkipGain, core::kStartStageUnbound, 0},
        {StartMode::BadBind, core::kStartStageBind, 0},
        {StartMode::FailStage, 7, -3},
        {StartMode::ReturnFalse, core::kStartStageNoStage, 0},
        {StartMode::CreateNull, core::kStartStageCreate, 0},
    };
    for (const Case &k : cases) {
        CAPTURE(static_cast<int>(k.mode));
        reset();
        world().mode = k.mode;
        core_os::KvStore store;
        core::Core c(testConfig());
        bootWith(c, store, {{fx::kTypeId, 2, core::kFactoryControlLoop, "m"}});

        CHECK(c.instanceCount() == 2);
        CHECK(c.instancesFailed() == 1);
        CHECK(stateOf(c, fx::kTypeId, 2) == core::InstanceState::Failed);
        CHECK(c.loop(core::kFactoryControlLoop).instanceCount() == 0);

        const auto reports = core_os::native::recordedReports();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].code.code == codes::core::kInstanceStartFailed.code);
        CHECK(reports[0].code.source == codes::core::kInstanceStartFailed.source);
        CHECK(reports[0].arg0 == ((uint32_t{fx::kTypeId} << 8) | 2U));
        CHECK(reports[0].arg1 == k.stage);
        CHECK(reports[0].err == k.err);

        // Its config stays: the params read from the shadow.
        CHECK(getParam(c, fx::kTypeId, 2, fx::kGain) == f(1.5f));
    }
}

TEST_CASE("Start: a good instance reads its boot values")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    bootWith(c, store, {{fx::kTypeId, 0, core::kFactoryControlLoop, "m"}});
    REQUIRE(world().created.size() == 1);
    const FullModule &m = *world().created[0];
    CHECK(m.boot_actuator == 0);
    CHECK(m.gain == 1.5f);
    CHECK(m.level == 3);
    CHECK(m.mode_value == FxMode::B);
    CHECK(core_os::native::recordedReports().empty());
}
