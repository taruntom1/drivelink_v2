// core: reboot_required system settings. Staging, SAVE, reboot, DISCARD and
// REVERT, the limits they give the pending layout, power loss during SAVE, and
// the boot order: crashes before and after the core is built both count, and
// safe mode runs with the defaults. The setting is test-only. The limits.*
// settings follow: their capacity and RAM checks, safe mode with a layout over
// the default limits, and the live diag.* timing.

#include "doctest.h"

#include <array>
#include <memory>
#include <vector>

#include "core/boot.hpp"
#include "core/command_service.hpp"
#include "core/core.hpp"
#include "core/ram_estimate.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/native_task.hpp"
#include "core_os/native_diag.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/type_fusion_mock.hpp"
#include "mod_mock/modules.hpp"
#include "drivelink/protocol/gen/type_system.hpp"

using namespace testmod;
using core::Detail;
using core::Result;

namespace {

namespace sys = drivelink::protocol::defs::system;
constexpr uint8_t kSys = drivelink::protocol::defs::kSystemTypeId;

/// `test.max_instances`, u16, 1..64, default 16: sizes CoreConfig::max_instances.
constexpr uint16_t kSetting = 0x7F00;
constexpr uint32_t kDefault = 16;
/// `test.enabled`, bool, default true, and `test.gain`, f32, default 2: settings
/// whose saved bits can be invalid for the type.
constexpr uint16_t kFlag = 0x7F01;
constexpr uint16_t kGain = 0x7F02;

const defs::TypeDesc &testSystemType()
{
    static const std::vector<defs::ItemDesc> items = [] {
        std::vector<defs::ItemDesc> all(sys::kDesc.items.begin(), sys::kDesc.items.end());
        const auto flags = defs::flagBits(defs::ItemFlag::RebootRequired, defs::ItemFlag::Persist);
        defs::ItemDesc other{};
        other.kind = defs::ItemKind::Param;
        other.flags = flags;
        other.unit = other.description = "";
        other.id = kFlag;
        other.type = defs::ValueType::Bool;
        other.name = "test.enabled";
        other.default_bits = 1;
        all.push_back(other);
        other.id = kGain;
        other.type = defs::ValueType::F32;
        other.name = "test.gain";
        other.default_bits = f(2.0f);
        all.push_back(other);
        defs::ItemDesc item{};
        item.id = kSetting;
        item.kind = defs::ItemKind::Param;
        item.type = defs::ValueType::U16;
        item.flags = flags;
        item.name = "test.max_instances";
        item.unit = item.description = "";
        item.min_bits = 1;
        item.max_bits = 64;
        item.default_bits = kDefault;
        item.has_min = item.has_max = true;
        all.push_back(item);
        return all;
    }();
    static const defs::TypeDesc type{sys::kDesc.name, sys::kDesc.type_id, sys::kDesc.loops, items};
    return type;
}

core::CoreConfig settingConfig()
{
    core::CoreConfig cfg = testConfig();
    cfg.system_type = &testSystemType();
    cfg.configure_system = [](core::CoreConfig &config, std::span<const uint32_t> values) {
        config.max_instances = static_cast<uint16_t>(values.back());
    };
    return cfg;
}

/// One reboot, through the path main and vdev use.
std::unique_ptr<core::Core> reboot(core_os::KvStore &store)
{
    core::BringUp up = core::bringUp(store, settingConfig(), kTypes);
    REQUIRE(up.status.ok());
    REQUIRE(up.core != nullptr);
    return std::move(up.core);
}

core::Status stage(core::Core &c, uint32_t value, uint32_t *applied = nullptr)
{
    const core::SetEntry e = entry(kSys, 0, kSetting, value);
    uint32_t bits = 0;
    const core::Status st = c.params().set({&e, 1}, {&bits, 1});
    if (applied != nullptr) {
        *applied = bits;
    }
    return st;
}

uint32_t running(core::Core &c)
{
    return getParam(c, kSys, 0, kSetting);
}

uint32_t pending(core::Core &c, uint16_t id = kSetting)
{
    const core::Ref r = ref(kSys, 0, id);
    uint32_t v = 0;
    REQUIRE(c.getPending({&r, 1}, {&v, 1}).ok());
    return v;
}

} // namespace

TEST_CASE("System settings: SET stages, GET stays, DISCARD drops")
{
    reset();
    core_os::KvStore store;
    auto c = reboot(store);
    REQUIRE(c->start().ok());
    CHECK(running(*c) == kDefault);
    CHECK(pending(*c) == kDefault);
    CHECK_FALSE(c->pendingReboot());

    CHECK(stage(*c, 20).result == Result::PendingReboot);
    CHECK(running(*c) == kDefault);
    CHECK(pending(*c) == 20);
    CHECK(pending(*c, sys::kWatchdogMs) == 200); // a live param: the running value
    CHECK(c->nvsDirty());
    CHECK(c->pendingReboot());
    CHECK(c->arm().detail == Detail::RebootPending);

    uint32_t applied = 0;
    CHECK(stage(*c, 1000, &applied).result == Result::PendingReboot); // clamped, as any param
    CHECK(applied == 64);
    CHECK(pending(*c) == 64);

    // A live and a staged param never share a batch.
    const std::array<core::SetEntry, 2> mixed{entry(kSys, 0, sys::kStatusRateHz, 20), entry(kSys, 0, kSetting, 30)};
    std::array<uint32_t, 2> values{};
    CHECK(c->params().set(mixed, values).detail == Detail::CrossLoopBatch);
    CHECK(pending(*c) == 64);

    REQUIRE(c->discardPending().ok());
    CHECK(pending(*c) == kDefault);
    CHECK_FALSE(c->pendingReboot());
    c->stop();
}

TEST_CASE("System settings: an unsaved value is gone after a reboot, a saved one runs and sizes the core")
{
    reset();
    core_os::KvStore store;
    {
        auto c = reboot(store);
        CHECK(stage(*c, 20).accepted());
    }
    {
        auto c = reboot(store);
        CHECK(running(*c) == kDefault);
        CHECK(stage(*c, 20).accepted());
        REQUIRE(c->save().ok());
        CHECK(running(*c) == kDefault); // until the reboot
        CHECK(pending(*c) == 20);
        CHECK(c->pendingReboot());
        CHECK_FALSE(c->nvsDirty());
        // DISCARD after SAVE, as for the layout: the copy is gone, NVS is not.
        REQUIRE(c->discardPending().ok());
        CHECK(pending(*c) == kDefault);
        CHECK(c->pendingReboot());
    }
    auto c = reboot(store);
    CHECK(running(*c) == 20);
    CHECK(pending(*c) == 20);
    CHECK(getParam(*c, kSys, 0, sys::kLimitsInstances) == 20); // bootConfig() used it
    CHECK_FALSE(c->pendingReboot());
    REQUIRE(c->save().ok()); // a SAVE for another reason keeps it
    c.reset();
    CHECK(running(*reboot(store)) == 20);
}

TEST_CASE("System settings: FACTORY_RESET drops the saved setting; a staged one needs a SAVE to survive it")
{
    for (const bool save_again : {false, true}) {
        CAPTURE(save_again);
        reset();
        core_os::KvStore store;
        writeLayout(store, {}, 1, {{kSetting, 8}});
        auto c = reboot(store);
        CHECK(stage(*c, 20).accepted());
        REQUIRE(c->factoryReset().ok());
        CHECK(running(*c) == 8);
        CHECK(pending(*c) == 20); // as the pending layout: until the reboot
        CHECK(c->pendingReboot());
        if (save_again) {
            REQUIRE(c->save().ok());
        } else {
            REQUIRE(c->discardPending().ok());
            CHECK(pending(*c) == 8);
        }
        c.reset();
        c = reboot(store);
        CHECK(c->configLoadedFromNvs() == save_again);
        CHECK(running(*c) == (save_again ? 20 : kDefault));
        CHECK(pending(*c) == running(*c));
        CHECK_FALSE(c->pendingReboot());
    }
}

TEST_CASE("System settings: a batch with module params is staged whole or not at all")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {{act::kTypeId, 0, 0, "a"}, {act::kTypeId, 1, 0, "b"}}, 1, {{kSetting, 8}});
    auto c = reboot(store);
    const core::Ref pin = ref(act::kTypeId, 0, act::kPin);
    const auto pinPending = [&] {
        uint32_t v = 0;
        REQUIRE(c->getPending({&pin, 1}, {&v, 1}).ok());
        return v;
    };
    std::array<uint32_t, 3> applied{};
    const auto batch = [&](uint32_t setting, uint32_t pin_value) {
        const std::array<core::SetEntry, 3> entries{entry(kSys, 0, kFlag, 0), entry(kSys, 0, kSetting, setting),
                                                    entry(act::kTypeId, 0, act::kPin, pin_value)};
        return c->params().set(entries, applied);
    };

    // A pin the policy refuses: the settings beside it are not staged either.
    core::Status st = batch(20, 13);
    CHECK(st.detail == Detail::GpioNotAllowed);
    CHECK(st.index == 2);
    CHECK(pending(*c, kFlag) == 1);
    CHECK(pending(*c) == 8);
    CHECK(pinPending() == defs::kNoGpio);
    CHECK_FALSE(c->pendingReboot());
    CHECK_FALSE(c->nvsDirty());

    // Both valid: both are staged.
    CHECK(batch(20, 5).accepted());
    CHECK(pending(*c, kFlag) == 0);
    CHECK(pending(*c) == 20);
    CHECK(pinPending() == 5);

    // A setting too low for the layout: an active copy keeps what it had.
    const std::array<core::SetEntry, 3> low{entry(kSys, 0, kFlag, 1), entry(kSys, 0, kSetting, 1),
                                            entry(act::kTypeId, 0, act::kPin, 6)};
    CHECK(c->params().set(low, applied).detail == Detail::Capacity);
    CHECK(pending(*c, kFlag) == 0);
    CHECK(pending(*c) == 20);
    CHECK(pinPending() == 5);

    REQUIRE(c->save().ok());
    c.reset();
    c = reboot(store);
    CHECK(running(*c) == 20);
    CHECK(getParam(*c, kSys, 0, kFlag) == 0);
    CHECK(getParam(*c, act::kTypeId, 0, act::kPin) == 5);
}

TEST_CASE("System settings: an invalid saved value loads as the default, an out-of-range one clamped")
{
    struct Case {
        uint32_t saved, loaded; // 0x10000 is no u16
    };
    for (const Case &k : {Case{9999, 64}, Case{0, 1}, Case{0x10000, kDefault}}) {
        CAPTURE(k.saved);
        reset();
        core_os::KvStore store;
        writeLayout(store, {}, 1, {{kSetting, k.saved}, {kFlag, 7}, {kGain, 0x7FC00000}, {0x7E00, 5}});
        auto c = reboot(store);
        CHECK(c->configLoadedFromNvs());
        CHECK(running(*c) == k.loaded);
        CHECK(pending(*c) == k.loaded);
        CHECK(getParam(*c, kSys, 0, sys::kLimitsInstances) == k.loaded);
        CHECK(getParam(*c, kSys, 0, kFlag) == 1); // 7 is no bool
        CHECK(pending(*c, kFlag) == 1);
        CHECK(getParam(*c, kSys, 0, kGain) == f(2.0f)); // a NaN
        CHECK(pending(*c, kGain) == f(2.0f));
        CHECK_FALSE(c->pendingReboot()); // nothing is staged by the repair
        REQUIRE(c->save().ok());
        for (const core::ParamPair &p : core::persist::loadConfig(store).system) {
            CHECK(p.id != 0x7E00); // an item that is gone
            CHECK((p.id != kSetting || p.value == k.loaded));
        }
    }
}

TEST_CASE("System settings: a torn generation falls back to the setting of the one before it")
{
    for (const auto mode : {core_os::native::Tear::Truncate, core_os::native::Tear::Corrupt}) {
        CAPTURE(static_cast<int>(mode));
        reset();
        core_os::KvStore store;
        writeLayout(store, {{fx::kTypeId, 0, 0, "a"}}, 1, {{kSetting, 8}});
        {
            auto c = reboot(store);
            CHECK(stage(*c, 24).accepted());
            // Three instance blobs (two loops, the module), then the system's:
            // it is torn, and the layout blob after it commits the generation.
            core_os::native::tearWriteAfter(store, 3, mode);
            REQUIRE(c->save().ok());
            core_os::native::clearFaults(store);
        }
        auto c = reboot(store);
        CHECK(c->configLoadedFromNvs());
        CHECK(running(*c) == 8);
        CHECK(pending(*c) == 8);
        CHECK(getParam(*c, kSys, 0, sys::kLimitsInstances) == 8);
        CHECK(c->findRecord(fx::kTypeId, 0) != nullptr);
        CHECK_FALSE(c->pendingReboot());

        // The next SAVE goes past the torn generation and holds.
        CHECK(stage(*c, 30).accepted());
        REQUIRE(c->save().ok());
        c.reset();
        c = reboot(store);
        CHECK(running(*c) == 30);

        // Both generations unusable: the default.
        core_os::native::removeKey(store, "t00i00", core::persist::kParamKeys[0]);
        core_os::native::removeKey(store, "t00i00", core::persist::kParamKeys[1]);
        c.reset();
        c = reboot(store);
        CHECK_FALSE(c->configLoadedFromNvs());
        CHECK(running(*c) == kDefault);
        CHECK(pending(*c) == kDefault);
    }
}

TEST_CASE("System settings: REVERT restores live params and keeps the staged setting")
{
    reset();
    core_os::KvStore store;
    auto c = reboot(store);
    REQUIRE(c->save().ok());
    const core::SetEntry live = entry(kSys, 0, sys::kStatusRateHz, 50);
    uint32_t applied = 0;
    REQUIRE(c->params().set({&live, 1}, {&applied, 1}).ok());
    CHECK(stage(*c, 20).accepted());
    REQUIRE(c->revert().ok());
    CHECK(getParam(*c, kSys, 0, sys::kStatusRateHz) == 10);
    CHECK(pending(*c) == 20);
    CHECK(running(*c) == kDefault);
    CHECK(c->nvsDirty());
}

TEST_CASE("System settings: the pending layout is checked against the pending settings")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {{fx::kTypeId, 0, 0, "a"}, {fx::kTypeId, 1, 0, "b"}}, 1, {{kSetting, 2}});
    auto c = reboot(store);
    REQUIRE(c->instanceCount() == 4); // two loops (counted apart) and two modules: full

    const core::Status low = stage(*c, 1); // below what the layout uses
    CHECK(low.result == Result::NoMemory);
    CHECK(low.detail == Detail::Capacity);
    CHECK(low.index == 0);
    CHECK(pending(*c) == 2);
    CHECK_FALSE(c->pendingReboot());
    CHECK(c->pendingLayout().entries().empty());

    uint8_t id = 0;
    CHECK(c->addInstance(fx::kTypeId, 0, "c", id).detail == Detail::Capacity);
    CHECK(stage(*c, 3).accepted()); // raise, then add, in one pending session
    REQUIRE(c->addInstance(fx::kTypeId, 0, "c", id).ok());
    CHECK(stage(*c, 2).detail == Detail::Capacity); // the staged instance counts too
    CHECK(pending(*c) == 3);
    REQUIRE(c->save().ok());
    c.reset();
    c = reboot(store);
    CHECK(c->instanceCount() == 5);
    CHECK(c->instancesFailed() == 0);
    CHECK_FALSE(c->layoutInvalid());
}

TEST_CASE("System settings: a power loss during SAVE leaves the old setting or the new one, with its layout")
{
    for (const bool torn : {false, true}) {
        bool committed = false;
        for (uint32_t write = 0; write < 12 && !committed; ++write) {
            CAPTURE(torn);
            CAPTURE(write);
            reset();
            core_os::KvStore store;
            writeLayout(store, {{fx::kTypeId, 0, 0, "a"}}, 1, {{kSetting, 8}});
            bool saved = false;
            {
                auto c = reboot(store);
                CHECK(stage(*c, 24).accepted());
                uint8_t id = 0;
                REQUIRE(c->addInstance(fx::kTypeId, 0, "b", id).ok());
                if (torn) {
                    core_os::native::tearWriteAfter(store, write);
                } else {
                    core_os::native::failWritesAfter(store, write);
                }
                saved = c->save().ok(); // a torn write reports success
                core_os::native::clearFaults(store);
            }
            auto c = reboot(store);
            CHECK(c->configLoadedFromNvs());
            committed = c->findRecord(fx::kTypeId, 1) != nullptr;
            CHECK(running(*c) == (committed ? 24 : 8));
            CHECK(pending(*c) == running(*c));
            CHECK((torn || committed == saved));
            CHECK((!committed || write >= 6)); // two loops, two modules, the system, the layout
        }
        CHECK(committed);
    }
}

TEST_CASE("System settings: the crash counter goes up before the first NVS read")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {}, 1, {{kSetting, 60}});
    core_os::native::setResetWasCrash(true);
    struct Seen {
        uint32_t reads = 0, first = 0;
    } seen;
    core_os::native::setReadObserver(
        store,
        [](void *ctx) {
            auto *s = static_cast<Seen *>(ctx);
            if (s->reads++ == 0) {
                s->first = core_os::loadCrashCount();
            }
        },
        &seen);
    for (uint32_t boot = 1; boot <= 3; ++boot) {
        CAPTURE(boot);
        seen = {};
        const core::BootPlan plan = core::prepareBoot(store);
        CHECK(seen.reads > 0);
        CHECK(seen.first == boot);
        CHECK(plan.safe.safe_mode == (boot == 3));
    }
}

TEST_CASE("System settings: the crash counter goes up before the core is sized; safe mode uses the defaults")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {{fx::kTypeId, 0, 0, "a"}}, 1, {{kSetting, 60}});

    core_os::native::setResetWasCrash(true);
    {
        const core::BootPlan plan = core::prepareBoot(store);
        CHECK(core_os::loadCrashCount() == 1);
        CHECK_FALSE(plan.safe.safe_mode);
        CHECK(core::bootConfig(plan, settingConfig()).max_instances == 60);
    } // crashes before Core exists
    {
        auto c = reboot(store);
        CHECK(core_os::loadCrashCount() == 2);
        CHECK(running(*c) == 60);
    } // crashes after it does, within the 30 s
    auto c = reboot(store);
    REQUIRE(c->safeMode());
    CHECK(core_os::loadCrashCount() == 3);
    CHECK(running(*c) == kDefault);
    CHECK(getParam(*c, kSys, 0, sys::kLimitsInstances) == kDefault);
    CHECK(pending(*c) == 60);
    CHECK_FALSE(c->pendingReboot()); // nothing is staged
    CHECK(c->findRecord(fx::kTypeId, 0) != nullptr);

    // A SAVE in safe mode keeps the saved setting; the user can lower it.
    REQUIRE(c->save().ok());
    CHECK(pending(*c) == 60);
    CHECK(stage(*c, 40).accepted());
    CHECK(running(*c) == kDefault);
    REQUIRE(c->save().ok());
    REQUIRE(c->exitSafeMode().ok());
    c.reset();
    core_os::native::setResetWasCrash(false);
    c = reboot(store);
    CHECK_FALSE(c->safeMode());
    CHECK(running(*c) == 40);
    CHECK(c->instancesFailed() == 0);
}

TEST_CASE("System settings: the wire describes, stages and reads them; LINK_CONFIRM is reserved")
{
    reset();
    core_os::KvStore store;
    auto c = reboot(store);
    core::CommandService service(*c, {});
    std::array<uint8_t, 1024> out{};
    const auto call = [&](std::vector<uint8_t> req) {
        req.insert(req.begin(), {drivelink::protocol::core_wire::kVersion, 7});
        return service.handle(req, out);
    };
    const auto last = static_cast<uint8_t>(testSystemType().items.size() - 1U);
    REQUIRE(call({3, 2, 0, last, 0}) > 13); // GET_ITEMS of the system, its last item
    CHECK(out[4] == 0);
    CHECK((out[11] | (out[12] << 8)) == kSetting);
    REQUIRE(call({4, 2, 1, 0, 0, 0x00, 0x7F, 20, 0, 0, 0}) == 11); // SET
    CHECK(out[4] == static_cast<uint8_t>(Result::PendingReboot));
    CHECK(out[7] == 20);
    REQUIRE(call({4, 1, 1, 0, 0, 0x00, 0x7F}) == 11); // GET
    CHECK(out[7] == kDefault);
    REQUIRE(call({4, 3, 1, 0, 0, 0x00, 0x7F}) == 11); // GET_PENDING
    CHECK(out[7] == 20);
    REQUIRE(call({2, 5}) == 6); // DISCARD_PENDING
    REQUIRE(call({4, 3, 1, 0, 0, 0x00, 0x7F}) == 11);
    CHECK(out[7] == kDefault);

    REQUIRE(call({1, 11}) == 6);
    CHECK(out[4] == static_cast<uint8_t>(Result::InvalidArgument));
    CHECK(out[5] == static_cast<uint8_t>(Detail::Unsupported));
    CHECK(service.takeAfterReply() == core::AfterReply::None);
}

// -- limits.* ------------------------------------------------------------------

namespace {

/// A boot with the real system type and `heap` bytes free before Core is built.
std::unique_ptr<core::Core> limitsBoot(core_os::KvStore &store, size_t heap = SIZE_MAX)
{
    core_os::native::setFreeInternalBytes(heap);
    core::CoreConfig cfg;
    cfg.gpio_allowed_mask = kTestGpioMask;
    core::BringUp up = core::bringUp(store, cfg, kTypes);
    core_os::native::setFreeInternalBytes(SIZE_MAX);
    REQUIRE(up.status.ok());
    return std::move(up.core);
}

core::Status setOne(core::Core &c, const core::Ref &r, uint32_t value)
{
    const core::SetEntry e{r, value};
    uint32_t bits = 0;
    return c.params().set({&e, 1}, {&bits, 1});
}

core::Status setLimit(core::Core &c, uint16_t id, uint32_t value)
{
    return setOne(c, ref(kSys, 0, id), value);
}

/// The estimate for the pending settings and layout, as the check sees it.
size_t pendingNeed(const core::Core &c)
{
    core::CoreConfig cfg;
    std::vector<uint32_t> values;
    for (size_t i = 0; i < sys::kDesc.items.size(); ++i) {
        values.push_back(c.pendingLayout().systemValue(i));
    }
    core::configureLimits(cfg, values);
    std::vector<core::LayoutEntry> view;
    for (const core::PendingEntry &e : c.pendingLayout().entries()) {
        view.push_back({e.desc, e.type_id, e.instance, e.loop, e.label, e.values.data()});
    }
    return core::estimateRam(cfg, view);
}

constexpr size_t kReserve = core::RamBudget{}.reserve_bytes;

std::vector<Inst> modules(size_t count)
{
    std::vector<Inst> all;
    for (size_t i = 0; i < count; ++i) {
        all.push_back({fx::kTypeId, static_cast<uint8_t>(i), 1, "m" + std::to_string(i)});
    }
    return all;
}

const std::vector<core::ParamPair> kRaisedLimits{
    {sys::kLimitsLoops, 6}, {sys::kLimitsInstances, 24}, {sys::kLimitsSubGroups, 12}, {sys::kLimitsCmdGroups, 12}};

} // namespace

TEST_CASE("Limits: every one is saved and sizes the next boot; frame and memory caps stay")
{
    reset();
    core_os::KvStore store;
    {
        auto c = limitsBoot(store);
        CHECK(getParam(*c, kSys, 0, sys::kLimitsCaptureKb) == 4096);
        const std::array<core::SetEntry, 8> set{
            entry(kSys, 0, sys::kLimitsLoops, 6),         entry(kSys, 0, sys::kLimitsInstances, 24),
            entry(kSys, 0, sys::kLimitsSubGroups, 31),    entry(kSys, 0, sys::kLimitsCmdGroups, 32),
            entry(kSys, 0, sys::kLimitsGroupEntries, 10), entry(kSys, 0, sys::kLimitsLinkValues, 5),
            entry(kSys, 0, sys::kLimitsCaptureRefs, 64),  entry(kSys, 0, sys::kLimitsCaptureKb, 64)};
        std::array<uint32_t, 8> applied{};
        REQUIRE(c->params().set(set, applied).result == Result::PendingReboot);
        CHECK(getParam(*c, kSys, 0, sys::kLimitsLoops) == 4);
        CHECK(c->groups().capacity(core::GroupKind::Subscription) == 8);
        REQUIRE(c->save().ok());
    }
    auto c = limitsBoot(store);
    CHECK(c->loopCapacity() == 6);
    CHECK(c->groups().capacity(core::GroupKind::Subscription) == 31);
    CHECK(c->groups().capacity(core::GroupKind::Command) == 32);
    CHECK(c->groups().info(core::GroupKind::Subscription, 30) != nullptr);
    CHECK(c->groups().entryCapacity() == 10);
    CHECK(c->capture().refCapacity() == 64);
    CHECK(c->capture().capacityBytes() == 64U * 1024U);
    CHECK(getParam(*c, kSys, 0, sys::kLimitsCaptureKb) == 64);
    CHECK(pending(*c, sys::kLimitsInstances) == 24);
    CHECK_FALSE(c->pendingReboot());
    c.reset();

    // A small frame caps the two limits it carries: GET shows the cap.
    core::CoreConfig small;
    small.group_entries_cap = 9;
    small.capture_refs_cap = 19;
    core::BringUp up = core::bringUp(store, small, kTypes);
    REQUIRE(up.core != nullptr);
    CHECK(getParam(*up.core, kSys, 0, sys::kLimitsGroupEntries) == 9);
    CHECK(getParam(*up.core, kSys, 0, sys::kLimitsCaptureRefs) == 19);
    CHECK(pending(*up.core, sys::kLimitsGroupEntries) == 10);
}

TEST_CASE("Limits: one below what the pending layout uses is refused; a raised one can be used at once")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, modules(3));
    auto c = limitsBoot(store);
    core::Status st = setLimit(*c, sys::kLimitsLoops, 1); // two loops
    CHECK(st.result == Result::NoMemory);
    CHECK(st.detail == Detail::Capacity);
    CHECK(st.index == 0);
    CHECK(setLimit(*c, sys::kLimitsInstances, 2).detail == Detail::Capacity);
    CHECK_FALSE(c->pendingReboot());

    REQUIRE(setLimit(*c, sys::kLimitsLoops, 2).accepted());
    uint8_t id = 0;
    CHECK(c->addInstance(defs::kLoopTypeId, core::kNoLoop, "third", id).detail == Detail::Capacity);
    REQUIRE(setLimit(*c, sys::kLimitsLoops, 5).accepted());
    REQUIRE(c->addInstance(defs::kLoopTypeId, core::kNoLoop, "third", id).ok());
    REQUIRE(c->addInstance(defs::kLoopTypeId, core::kNoLoop, "fourth", id).ok());
    REQUIRE(c->addInstance(defs::kLoopTypeId, core::kNoLoop, "fifth", id).ok()); // past the running 4
    CHECK(setLimit(*c, sys::kLimitsLoops, 4).detail == Detail::Capacity);
    REQUIRE(c->save().ok());
    c.reset();
    c = limitsBoot(store);
    CHECK(c->loopCount() == 5);
    CHECK(c->instancesFailed() == 0);
}

TEST_CASE("Limits: an entry that is invalid for another reason still counts")
{
    reset();
    core_os::KvStore store;
    // Three modules, the last in no loop: it already fails, for another reason.
    auto layout = modules(3);
    layout[2].loop = 7;
    writeLayout(store, layout);
    auto c = limitsBoot(store);
    REQUIRE(stateOf(*c, fx::kTypeId, 2) == core::InstanceState::Invalid);
    CHECK(setLimit(*c, sys::kLimitsInstances, 2).detail == Detail::Capacity);
    CHECK(setLimit(*c, sys::kLimitsInstances, 3).accepted());
}

TEST_CASE("Limits: link_values below what a loop pair uses is refused")
{
    reset();
    namespace fus = defs::fusion_mock;
    static const std::array<const core::ModuleType *, 2> types{&kFullType, mod_mock::kTypes[3]};
    core_os::KvStore store;
    writeLayout(store, {{fx::kTypeId, 0, 0, "producer"},
                        {fus::kTypeId, 0, 1, "consumer",
                         {{1, defs::sourceRef(fx::kTypeId, 0, fx::kPosition)},
                          {2, defs::sourceRef(fx::kTypeId, 0, fx::kVelocitySp)}}}});
    core::BringUp up = core::bringUp(store, core::CoreConfig{}, types);
    REQUIRE(up.core != nullptr);
    REQUIRE(up.core->instancesFailed() == 0);
    CHECK(setLimit(*up.core, sys::kLimitsLinkValues, 1).detail == Detail::Capacity);
    CHECK(setLimit(*up.core, sys::kLimitsLinkValues, 2).accepted());
}

TEST_CASE("RAM check: limits, loops, stacks and modules that do not fit are refused")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, modules(1));
    size_t base = 0;
    {
        auto c = limitsBoot(store);
        REQUIRE(setLimit(*c, sys::kLimitsSubGroups, 8).accepted()); // the default: only makes the copy
        base = pendingNeed(*c);
    }
    const size_t group = core::GroupService::slotBytes();
    // Room for three more command groups, and not for four.
    auto c = limitsBoot(store, base + kReserve + (3U * (group + 64U)));
    const core::Status st = setLimit(*c, sys::kLimitsCmdGroups, 12);
    CHECK(st.result == Result::NoMemory);
    CHECK(st.detail == Detail::NotEnoughMemory);
    CHECK(st.index == 0);
    CHECK(pending(*c, sys::kLimitsCmdGroups) == 8);
    CHECK_FALSE(c->pendingReboot());
    uint8_t id = 0;
    REQUIRE(c->addInstance(defs::kLoopTypeId, core::kNoLoop, "l", id).ok()); // about 7 KB
    const core::Ref stack = ref(defs::kLoopTypeId, id, defs::loop::kStackBytes);
    const uint8_t extra = id;
    CHECK(setOne(*c, stack, 16384).detail == Detail::NotEnoughMemory);
    CHECK(c->addInstance(defs::kLoopTypeId, core::kNoLoop, "l2", id).detail == Detail::NotEnoughMemory);
    REQUIRE(c->removeInstance(defs::kLoopTypeId, extra).ok());
    REQUIRE(setLimit(*c, sys::kLimitsCmdGroups, 11).accepted());
    CHECK(setLimit(*c, sys::kLimitsSubGroups, 9).detail == Detail::NotEnoughMemory);
    // Raised and lowered in one batch: what counts is the result.
    const std::array<core::SetEntry, 2> swap{entry(kSys, 0, sys::kLimitsSubGroups, 9),
                                             entry(kSys, 0, sys::kLimitsCmdGroups, 9)};
    std::array<uint32_t, 2> applied{};
    CHECK(c->params().set(swap, applied).accepted());
    bool refused = false;
    for (unsigned i = 0; i < 12 && !refused; ++i) {
        const core::Status add = c->addInstance(fx::kTypeId, 1, ("x" + std::to_string(i)).c_str(), id);
        refused = add.detail == Detail::NotEnoughMemory;
        CHECK((add.ok() || refused));
    }
    CHECK(refused);
}

TEST_CASE("RAM check: a saved layout that no longer fits boots, and can be made smaller")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, modules(6), 1, kRaisedLimits);
    auto c = limitsBoot(store, 1000); // nothing fits
    CHECK(c->groups().capacity(core::GroupKind::Subscription) == 12);
    CHECK(c->instanceCount() == 8);
    CHECK(setLimit(*c, sys::kLimitsSubGroups, 13).detail == Detail::NotEnoughMemory);
    uint8_t id = 0;
    CHECK(c->addInstance(fx::kTypeId, 1, "more", id).detail == Detail::NotEnoughMemory);
    CHECK(c->setLabel(fx::kTypeId, 0, "renamed").ok()); // costs nothing
    CHECK(setLimit(*c, sys::kLimitsSubGroups, 11).accepted());
    CHECK(c->removeInstance(fx::kTypeId, 1).ok());
    REQUIRE(c->save().ok());
}

TEST_CASE("Limits: safe mode runs the defaults and keeps a layout that needs the raised limits")
{
    reset();
    core_os::KvStore store;
    // 24 modules and 6 loops: over the default 16 and 4.
    auto layout = modules(24);
    for (uint8_t id = 2; id < 6; ++id) {
        layout.push_back({defs::kLoopTypeId, id, core::kNoLoop, "loop" + std::to_string(id)});
    }
    writeLayout(store, layout, 1, kRaisedLimits);
    core_os::native::setResetWasCrash(true);
    core_os::storeCrashCount(2);
    auto c = limitsBoot(store);
    REQUIRE(c->safeMode());
    CHECK(getParam(*c, kSys, 0, sys::kLimitsInstances) == 16);
    CHECK(c->groups().capacity(core::GroupKind::Command) == 8);
    CHECK(c->loopCapacity() == 4);
    CHECK(pending(*c, sys::kLimitsInstances) == 24);
    CHECK(c->configuredCount() == 30);
    CHECK_FALSE(c->layoutInvalid()); // judged by its own limits
    CHECK(c->instancesFailed() == 0);
    REQUIRE(c->start().ok());

    // Its edits are checked against the saved limits.
    CHECK(setLimit(*c, sys::kLimitsInstances, 23).detail == Detail::Capacity);
    uint8_t id = 0;
    CHECK(c->addInstance(fx::kTypeId, 1, "extra", id).detail == Detail::Capacity);
    REQUIRE(c->save().ok()); // with no pending copy
    REQUIRE(setLimit(*c, sys::kLimitsSubGroups, 4).accepted());
    REQUIRE(c->save().ok()); // from the pending copy
    REQUIRE(c->exitSafeMode().ok());
    c.reset();
    core_os::native::setResetWasCrash(false);
    c = limitsBoot(store);
    CHECK_FALSE(c->safeMode());
    CHECK(c->instanceCount() == 30);
    CHECK(c->loopCount() == 6);
    CHECK(c->groups().capacity(core::GroupKind::Subscription) == 4);
    CHECK(c->groups().capacity(core::GroupKind::Command) == 12);
}

TEST_CASE("Limits: a layout saved without its limits keeps its extra instances through SAVE")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, modules(18)); // e.g. from a build with a raised Kconfig limit
    auto c = limitsBoot(store);
    CHECK(c->configuredCount() == 20);
    CHECK(stateOf(*c, fx::kTypeId, 17) == core::InstanceState::Invalid);
    CHECK(c->instancesFailed() == 2);
    REQUIRE(c->save().ok());
    REQUIRE(setLimit(*c, sys::kLimitsInstances, 18).accepted());
    REQUIRE(c->save().ok());
    c.reset();
    c = limitsBoot(store);
    CHECK(c->instanceCount() == 20);
    CHECK(c->instancesFailed() == 0);
}

TEST_CASE("Diagnostic timing: live, saved and handed to the platform, at the defaults too")
{
    using core_os::DiagTiming;
    using core_os::native::diagTiming;
    reset();
    core_os::KvStore store;
    {
        auto c = limitsBoot(store);
        CHECK(diagTiming(DiagTiming::CounterPeriodMs) == 1000);
        CHECK(diagTiming(DiagTiming::PollMs) == 20);
        CHECK(diagTiming(DiagTiming::StackLogS) == 30);
        CHECK(setLimit(*c, sys::kDiagCounterPeriodMs, 250).result == Result::Ok);
        CHECK(setLimit(*c, sys::kDiagPollMs, 1).result == Result::Ok); // clamped
        CHECK(setLimit(*c, sys::kDiagStackLogS, 0).result == Result::Ok);
        CHECK(diagTiming(DiagTiming::CounterPeriodMs) == 250);
        CHECK(diagTiming(DiagTiming::PollMs) == 10);
        CHECK(diagTiming(DiagTiming::StackLogS) == 0);
        CHECK_FALSE(c->pendingReboot());
        REQUIRE(c->save().ok());
        CHECK(setLimit(*c, sys::kDiagPollMs, 500).result == Result::Ok);
        REQUIRE(c->revert().ok());
        CHECK(diagTiming(DiagTiming::PollMs) == 10);
    }
    core_os::setDiagTiming(DiagTiming::PollMs, 0);
    auto c = limitsBoot(store);
    CHECK(diagTiming(DiagTiming::PollMs) == 10);
    CHECK(getParam(*c, kSys, 0, sys::kDiagCounterPeriodMs) == 250);
}
