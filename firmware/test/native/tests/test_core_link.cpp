// core: the link settings and their trial. Staging and its refusals, the trial a
// boot runs at most once, confirm (late, twice, with a failing store), timeout,
// crash, UART failure, power loss at every write, safe mode, FACTORY_RESET, OTA
// and the wire.

#include "doctest.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "core/boot.hpp"
#include "core/command_service.hpp"
#include "core/core.hpp"
#include "core/link_settings.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/clock.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/native_kv.hpp"
#include "core_os/native_ota.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/link.hpp"

using namespace testmod;
using core::Detail;
using core::LinkSettings;
using core::Result;
namespace linkstore = core::linkstore;
namespace native = core_os::native;

namespace {

namespace sys = drivelink::protocol::defs::system;
namespace cw = drivelink::protocol::core_wire;
constexpr uint8_t kSys = drivelink::protocol::defs::kSystemTypeId;
constexpr uint8_t kCtl = core::kFactoryControlLoop;
constexpr int64_t kSecond = 1000000;

const LinkSettings kFactory{3, 43, 44};

struct Device {
    core_os::KvStore store;
    core::LinkBoot link; ///< what the last boot decided
    std::unique_ptr<core::Core> core;

    Device()
    {
        reset();
        native::resetBootInfo();
        native::setOtaImageUnconfirmed(false);
    }
    ~Device()
    {
        native::setOtaImageUnconfirmed(false);
        native::resetBootInfo();
    }

    /// Reset and boot; `fail_uart` links fail to open, in order.
    core::Core &boot(int fail_uart = 0)
    {
        core.reset();
        const uint32_t crashes = core_os::loadCrashCount();
        const bool crash = core_os::resetWasCrash();
        reset(); // a power-on; put the reset cause back
        core_os::storeCrashCount(crashes);
        native::setResetWasCrash(crash);
        core::BootPlan plan = core::prepareBoot(store);
        for (; fail_uart > 0; --fail_uart) {
            CHECK(core::fallBackLink(plan.link, -5));
        }
        link = plan.link;
        core::CoreConfig cfg;
        cfg.gpio_allowed_mask = core::gpio_policy::kEsp32s3ModulePins;
        core::BringUp up = core::bringUp(std::move(plan), cfg, kTypes);
        REQUIRE(up.status.ok());
        core = std::move(up.core);
        REQUIRE(core->start().ok());
        return *core;
    }

    core::Status set(std::vector<std::pair<uint16_t, uint32_t>> values)
    {
        std::vector<core::SetEntry> entries;
        for (const auto &[id, value] : values) {
            entries.push_back({ref(kSys, 0, id), value});
        }
        std::vector<uint32_t> applied(entries.size());
        return core->params().set(entries, applied);
    }
    core::Status setPin(uint8_t inst, uint16_t id, uint32_t pin)
    {
        const core::SetEntry e = entry(act::kTypeId, inst, id, pin);
        uint32_t applied = 0;
        return core->params().set({&e, 1}, {&applied, 1});
    }
    void stage(const LinkSettings &l)
    {
        REQUIRE(set({{sys::kLinkBaud, l.baud}, {sys::kLinkTxPin, l.tx_pin}, {sys::kLinkRxPin, l.rx_pin}}).result ==
                Result::PendingReboot);
    }
    void stageAndSave(const LinkSettings &l)
    {
        stage(l);
        REQUIRE(core->save().ok());
    }
    LinkSettings running()
    {
        return read([&](std::span<const core::Ref> r, std::span<uint32_t> v) { return core->params().get(r, v); });
    }
    LinkSettings pending()
    {
        return read([&](std::span<const core::Ref> r, std::span<uint32_t> v) { return core->getPending(r, v); });
    }
    linkstore::Records records() { return linkstore::load(store); }

private:
    template <typename Get> LinkSettings read(Get get)
    {
        const std::array<core::Ref, 3> refs{ref(kSys, 0, sys::kLinkBaud), ref(kSys, 0, sys::kLinkTxPin),
                                            ref(kSys, 0, sys::kLinkRxPin)};
        std::array<uint32_t, 3> v{};
        REQUIRE(get(refs, v).ok());
        return {static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]), static_cast<uint8_t>(v[2])};
    }
};

/// The causes of the `core.LinkReverted` reports, in order.
std::vector<uint32_t> reverts()
{
    const auto &code = drivelink::protocol::defs::diag::codes::core::kLinkReverted;
    std::vector<uint32_t> out;
    for (const auto &r : native::recordedReports()) {
        if (r.code.code == code.code && r.code.source == code.source) {
            out.push_back(r.arg1);
        }
    }
    return out;
}

bool is(const core::Status &st, Result result, Detail detail)
{
    return st.result == result && st.detail == detail;
}

} // namespace

TEST_CASE("Link: the factory link runs, and nothing is written until a change is saved")
{
    Device d;
    d.boot();
    CHECK(core::factoryLink() == kFactory);
    CHECK(kFactory.rate() == 921600);
    CHECK(d.link.running == kFactory);
    CHECK_FALSE(d.link.trial);
    CHECK(d.running() == kFactory);
    CHECK(d.pending() == kFactory);
    CHECK(d.core->linkTrialRemainingMs(0) == 0);
    REQUIRE(d.core->save().ok());
    CHECK_FALSE(native::hasKey(d.store, "link", "trial"));
    CHECK_FALSE(native::hasKey(d.store, "link", "conf_a"));
    CHECK_FALSE(native::hasKey(d.store, "link", "conf_b"));
    CHECK(reverts().empty());
}

TEST_CASE("Link: a confirmed trial stays")
{
    Device d;
    d.boot();
    const LinkSettings fast{5, 43, 44};
    d.stage(fast);
    CHECK(d.running() == kFactory);
    CHECK(d.pending() == fast);
    CHECK(d.core->pendingReboot());
    CHECK(d.core->linkChangePending());
    CHECK(d.records().state == linkstore::TrialState::None); // SAVE writes it
    REQUIRE(d.core->save().ok());
    CHECK(d.records().state == linkstore::TrialState::Armed);
    CHECK(d.records().confirmed == kFactory);

    const int64_t before = core_os::nowUs();
    d.boot();
    CHECK(d.records().state == linkstore::TrialState::Used); // before the UART opens
    CHECK(d.link.trial);
    CHECK(d.link.running == fast);
    CHECK(d.running() == fast);
    CHECK(d.pending() == kFactory); // the next boot's, unless confirmed
    CHECK_FALSE(d.core->pendingReboot());
    CHECK(d.core->linkTrialActive());
    const uint32_t left = d.core->linkTrialRemainingMs(core_os::nowUs());
    CHECK(left <= 30000);
    CHECK(left > 30000 - 5000);
    CHECK(d.core->linkTrialRemainingMs(before + (40 * kSecond)) == 1); // never 0 during one

    // SAVE leaves the link alone during the trial.
    REQUIRE(d.core->setName("x").ok());
    REQUIRE(d.core->save().ok());
    CHECK(d.records().state == linkstore::TrialState::Used);

    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    CHECK_FALSE(d.core->linkTrialActive());
    CHECK(d.core->linkTrialRemainingMs(core_os::nowUs()) == 0);
    CHECK(d.pending() == fast);
    CHECK(d.records().confirmed == fast);
    CHECK(d.records().state == linkstore::TrialState::None);
    CHECK_FALSE(d.core->serviceLink(core_os::nowUs() + (500 * kSecond)));
    // Once: a second one has nothing to confirm.
    CHECK(is(d.core->confirmLink(core_os::nowUs()), Result::InvalidArgument, Detail::NoLinkTrial));

    d.boot();
    CHECK_FALSE(d.link.trial);
    CHECK(d.running() == fast);
    CHECK(d.pending() == fast);
    CHECK(reverts().empty());

    // And the next change starts from it, in the other slot.
    const LinkSettings slow{0, 5, 6};
    d.stageAndSave(slow);
    d.boot();
    CHECK(d.link.trial);
    const uint32_t pins_left = d.core->linkTrialRemainingMs(core_os::nowUs());
    CHECK(pins_left > 30000); // a pin changed: 120 s
    CHECK(pins_left <= 120000);
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    d.boot();
    CHECK(d.running() == slow);
    CHECK(d.records().gen == 2);
}

TEST_CASE("Link: an unconfirmed trial times out, disarms, reports and reverts; a late confirm changes nothing")
{
    Device d;
    d.boot();
    d.stageAndSave({7, 43, 44});
    d.boot();
    const int64_t now = core_os::nowUs();
    CHECK_FALSE(d.core->serviceLink(now + (29 * kSecond)));
    CHECK(reverts().empty());
    const int64_t end = now + (31 * kSecond);
    // Past its time, even before the service saw it.
    CHECK(is(d.core->confirmLink(end), Result::InvalidArgument, Detail::NoLinkTrial));
    CHECK_FALSE(d.core->serviceLink(end));
    CHECK(reverts() == std::vector<uint32_t>{core::kLinkRevertTimeout});
    CHECK(d.core->armStatus() == core::ArmStatus::Disarmed);
    CHECK(d.core->linkTrialActive()); // still no ARM
    CHECK(is(d.core->confirmLink(now), Result::InvalidArgument, Detail::NoLinkTrial));
    CHECK_FALSE(d.core->serviceLink(end + (kSecond / 4))); // the report leaves first
    CHECK(d.core->serviceLink(end + kSecond));
    CHECK(reverts().size() == 1);

    d.boot();
    CHECK_FALSE(d.link.trial);
    CHECK(d.running() == kFactory);
    CHECK(reverts() == std::vector<uint32_t>{core::kLinkRevertReset});
    CHECK(d.records().state == linkstore::TrialState::None);
    d.boot();
    CHECK(reverts().empty()); // told once
    CHECK(d.running() == kFactory);
}

TEST_CASE("Link: a crash, a reboot or a power loss during a trial reverts it; a crash loop ends on the confirmed link")
{
    Device d;
    d.boot();
    const LinkSettings wanted{4, 43, 44};
    d.stageAndSave(wanted);
    core_os::native::setResetWasCrash(true); // the trial boot itself follows a crash
    d.boot();
    CHECK(d.link.trial);
    for (int i = 0; i < 4; ++i) {
        d.boot(); // crashes again and again
        CHECK_FALSE(d.link.trial);
        CHECK(d.running() == kFactory);
    }
    CHECK(d.core->safeMode());
}

TEST_CASE("Link: safe mode runs the confirmed link and keeps a saved trial for the next normal boot")
{
    Device d;
    d.boot();
    const LinkSettings fast{6, 43, 44};
    d.stageAndSave(fast);
    d.boot();
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    const LinkSettings next{1, 43, 44};
    d.stageAndSave(next);
    core_os::storeCrashCount(5);
    native::setResetWasCrash(true);
    d.boot();
    REQUIRE(d.core->safeMode());
    CHECK(d.running() == fast); // not the default, as the other settings
    CHECK(d.pending() == next);
    CHECK(is(d.core->confirmLink(core_os::nowUs()), Result::InvalidArgument, Detail::NoLinkTrial));
    CHECK(d.core->linkChangePending());
    REQUIRE(d.core->save().ok()); // SAVE keeps it
    CHECK(d.records().state == linkstore::TrialState::Armed);
    REQUIRE(d.core->exitSafeMode().ok());
    native::setResetWasCrash(false);
    d.boot();
    CHECK(d.link.trial);
    CHECK(d.running() == next);
}

TEST_CASE("Link: a UART that does not open falls back: trial, confirmed, factory")
{
    Device d;
    d.boot();
    const LinkSettings confirmed{2, 5, 6};
    d.stageAndSave(confirmed);
    d.boot();
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    d.stageAndSave({7, 7, 8});

    d.boot(1); // the trial's UART
    CHECK_FALSE(d.link.trial);
    CHECK(d.running() == confirmed);
    CHECK(d.pending() == confirmed);
    CHECK_FALSE(d.core->linkTrialActive());
    CHECK(reverts() == std::vector<uint32_t>{core::kLinkRevertUart});
    d.boot();
    CHECK(d.running() == confirmed); // not tried again

    d.boot(1); // the confirmed one's
    CHECK(d.running() == kFactory);
    CHECK(d.pending() == confirmed); // nothing stored
    CHECK_FALSE(d.core->linkTrialActive());
    CHECK_FALSE(d.core->linkChangePending());
    CHECK(is(d.core->confirmLink(core_os::nowUs()), Result::InvalidArgument, Detail::NoLinkTrial));
    REQUIRE(d.core->save().ok());
    CHECK(d.records().confirmed == confirmed);

    core::LinkBoot last = d.link;
    CHECK_FALSE(core::fallBackLink(last, -5)); // the factory link is the last
    d.boot();
    CHECK(d.running() == confirmed);
}

TEST_CASE("Link: power loss at every write of SAVE leaves the old link, or a trial that reverts")
{
    const LinkSettings wanted{5, 10, 11};
    uint32_t writes = 0;
    {
        Device d;
        d.boot();
        d.stage(wanted);
        const uint32_t start = native::writeCount(d.store);
        REQUIRE(d.core->save().ok());
        writes = native::writeCount(d.store) - start;
    }
    REQUIRE(writes >= 5); // two loops, the system, the layout, the trial last
    for (uint32_t n = 0; n <= writes; ++n) {
        for (const bool tear : {false, true}) {
            CAPTURE(n);
            CAPTURE(tear);
            Device d;
            d.boot();
            d.stage(wanted);
            if (tear) {
                native::tearWriteAfter(d.store, n);
            } else {
                native::failWritesAfter(d.store, n);
            }
            const core::Status st = d.core->save();
            if (!tear) {
                CHECK(st.ok() == (n >= writes));
                if (n < writes) {
                    CHECK(is(st, Result::Fault, Detail::StorageFailed));
                    CHECK(d.core->nvsDirty());
                }
            }
            native::clearFaults(d.store);
            d.boot();
            const bool complete = n >= writes;
            CHECK(d.link.trial == (complete || (tear && n < writes - 1U)));
            CHECK(d.running() == (d.link.trial ? wanted : kFactory));
            d.boot(); // nobody confirmed
            CHECK(d.running() == kFactory);
            CHECK_FALSE(d.core->safeMode());
        }
    }
}

TEST_CASE("Link: a SAVE that failed at the trial record can be repeated")
{
    Device d;
    d.boot();
    const LinkSettings wanted{5, 43, 44};
    d.stage(wanted);
    native::failWriteAfter(d.store, 4, -7);
    CHECK(is(d.core->save(), Result::Fault, Detail::StorageFailed));
    CHECK(d.core->pendingReboot());
    REQUIRE(d.core->save().ok());
    d.boot();
    CHECK(d.link.trial);
    CHECK(d.running() == wanted);
}

TEST_CASE("Link: the boot that cannot store the start of a trial does not run it")
{
    Device d;
    d.boot();
    d.stageAndSave({5, 43, 44});
    native::failNextWrite(d.store, -7);
    d.boot();
    CHECK_FALSE(d.link.trial);
    CHECK(d.running() == kFactory);
    CHECK(d.core->linkChangePending());
    CHECK(reverts() == std::vector<uint32_t>{core::kLinkRevertStore});
    d.boot();
    CHECK(d.link.trial); // still saved

    // A damaged mark reads as no trial: the next boot is on the confirmed link.
    Device torn;
    torn.boot();
    torn.stageAndSave({5, 43, 44});
    native::tearNextWrite(torn.store);
    torn.boot();
    CHECK(torn.link.trial);
    torn.boot();
    CHECK_FALSE(torn.link.trial);
    CHECK(torn.running() == kFactory);
}

TEST_CASE("Link: power loss at every write of LINK_CONFIRM leaves one link or the other, and a failure is told")
{
    const LinkSettings first{5, 43, 44};
    const LinkSettings second{6, 43, 44};
    for (uint32_t n = 0; n < 3; ++n) {
        for (const bool tear : {false, true}) {
            CAPTURE(n);
            CAPTURE(tear);
            Device d;
            d.boot();
            d.stageAndSave(first);
            d.boot();
            REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
            d.stageAndSave(second);
            d.boot();
            REQUIRE(d.link.trial);
            if (tear) {
                native::tearWriteAfter(d.store, n, native::Tear::Corrupt);
            } else {
                native::failWritesAfter(d.store, n);
            }
            const core::Status st = d.core->confirmLink(core_os::nowUs());
            native::clearFaults(d.store);
            // Write 0 is the confirmed record: without it nothing is confirmed,
            // and what was confirmed before is whole.
            CHECK(st.ok() == (n > 0));
            CHECK(d.core->linkTrialActive() == (n == 0));
            if (n == 0) {
                CHECK(is(st, Result::Fault, Detail::StorageFailed));
            }
            d.boot();
            CHECK_FALSE(d.link.trial);
            CHECK(d.running() == (n > 0 ? second : first));
            // A trial whose record did not close is no revert.
            CHECK(reverts().empty() == (n > 0));
            d.boot();
            CHECK(d.running() == (n > 0 ? second : first));
            CHECK(reverts().empty());
        }
    }
}

TEST_CASE("Link: a failed LINK_CONFIRM write can be repeated within the trial")
{
    Device d;
    d.boot();
    d.stageAndSave({5, 43, 44});
    d.boot();
    native::failNextWrite(d.store, -7);
    CHECK(is(d.core->confirmLink(core_os::nowUs()), Result::Fault, Detail::StorageFailed));
    CHECK(d.core->linkTrialActive());
    CHECK(d.pending() == kFactory);
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    d.boot();
    CHECK(d.running() == LinkSettings{5, 43, 44});
}

TEST_CASE("Link: damaged records read as the factory link and no trial")
{
    Device d;
    d.boot();
    d.stageAndSave({5, 10, 11});
    d.boot();
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    const std::array<uint8_t, 12> junk{1, 1, 0, 0, 0, 5, 10, 10, 0, 0, 0, 0};
    REQUIRE(d.store.set("link", "conf_b", junk.data(), junk.size()) == core_os::kOk);
    REQUIRE(d.store.set("link", "trial", junk.data(), 9) == core_os::kOk);
    d.boot();
    CHECK(d.running() == kFactory);
    CHECK_FALSE(d.link.trial);
}

TEST_CASE("Link: SAVE of the confirmed link drops a saved change; DISCARD alone does not")
{
    Device d;
    d.boot();
    d.stageAndSave({5, 43, 44});
    REQUIRE(d.core->discardPending().ok());
    CHECK(d.pending() == kFactory);
    CHECK(d.records().state == linkstore::TrialState::Armed);
    CHECK(d.core->pendingReboot());
    CHECK(d.core->linkChangePending());
    REQUIRE(d.core->save().ok()); // writes back what GET_PENDING shows
    CHECK(d.records().state == linkstore::TrialState::None);
    CHECK_FALSE(d.core->linkChangePending());
    d.boot();
    CHECK_FALSE(d.link.trial);
    CHECK(d.running() == kFactory);
    CHECK(reverts().empty());

    // The same change saved twice is written once.
    d.stageAndSave({5, 43, 44});
    const uint32_t once = native::writeCount(d.store);
    REQUIRE(d.core->save().ok());
    const uint32_t twice = native::writeCount(d.store);
    CHECK(twice - once == 4); // two loops, the system, the layout
}

TEST_CASE("Link: FACTORY_RESET keeps the confirmed link and drops an untried change")
{
    Device d;
    d.boot();
    const LinkSettings confirmed{2, 5, 6};
    d.stageAndSave(confirmed);
    d.boot();
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    d.stageAndSave({7, 43, 44});
    native::failNextWrite(d.store, -7);
    CHECK(is(d.core->factoryReset(), Result::Fault, Detail::StorageFailed));
    REQUIRE(d.core->factoryReset().ok());
    d.boot();
    CHECK_FALSE(d.core->configLoadedFromNvs());
    CHECK_FALSE(d.link.trial);
    CHECK(d.running() == confirmed);
    CHECK(d.pending() == confirmed);

    // During a trial it reverts like any other reset.
    d.stageAndSave({7, 43, 44});
    d.boot();
    REQUIRE(d.link.trial);
    REQUIRE(d.core->factoryReset().ok());
    d.boot();
    CHECK(d.running() == confirmed);
}

TEST_CASE("Link: pins are checked against each other, the policy and every module, both ways")
{
    Device d;
    d.boot();
    CHECK(is(d.set({{sys::kLinkTxPin, 44}}), Result::InvalidArgument, Detail::GpioConflict));
    CHECK(is(d.set({{sys::kLinkRxPin, 20}}), Result::InvalidArgument, Detail::GpioNotAllowed)); // USB
    CHECK(is(d.set({{sys::kLinkRxPin, 30}}), Result::InvalidArgument, Detail::GpioNotAllowed)); // flash
    CHECK(is(d.set({{sys::kLinkRxPin, 255}}), Result::InvalidArgument, Detail::GpioNotAllowed));
    CHECK(is(d.set({{sys::kLinkBaud, 8}}), Result::InvalidArgument, Detail::BadEnum));
    CHECK(d.pending() == kFactory);
    CHECK_FALSE(d.core->pendingReboot());
    REQUIRE(d.set({{sys::kLinkTxPin, 44}, {sys::kLinkRxPin, 43}}).result == Result::PendingReboot); // swapped is fine
    REQUIRE(d.core->discardPending().ok());

    // A module's pin, live or only pending.
    uint8_t inst = 0;
    REQUIRE(d.core->addInstance(act::kTypeId, kCtl, "a", inst).accepted());
    REQUIRE(d.setPin(inst, act::kPin, 10).result == Result::PendingReboot);
    CHECK(is(d.set({{sys::kLinkTxPin, 10}}), Result::InvalidArgument, Detail::GpioConflict));
    // UART0's pins and the running link's are never a module's.
    CHECK(is(d.setPin(inst, act::kPin, 43), Result::InvalidArgument, Detail::GpioNotAllowed));
    CHECK(is(d.setPin(inst, act::kPin, 44), Result::InvalidArgument, Detail::GpioNotAllowed));
    // Nor the staged one's.
    REQUIRE(d.set({{sys::kLinkTxPin, 11}, {sys::kLinkRxPin, 12}}).result == Result::PendingReboot);
    CHECK(is(d.setPin(inst, act::kPin, 11), Result::InvalidArgument, Detail::GpioNotAllowed));
    CHECK(is(d.setPin(inst, act::kPin, 12), Result::InvalidArgument, Detail::GpioNotAllowed));
    REQUIRE(d.core->save().ok());

    // During the trial: the running and the confirmed pins; afterwards UART0's stay closed.
    d.boot();
    REQUIRE(d.link.trial);
    CHECK(stateOf(*d.core, act::kTypeId, inst) == core::InstanceState::Running);
    for (const uint32_t pin : {11U, 12U, 43U, 44U}) {
        CHECK(is(d.setPin(inst, act::kPin, pin), Result::InvalidArgument, Detail::GpioNotAllowed));
    }
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    d.boot();
    for (const uint32_t pin : {11U, 12U, 43U, 44U}) {
        CHECK(is(d.setPin(inst, act::kPin, pin), Result::InvalidArgument, Detail::GpioNotAllowed));
    }
    // Back to the factory link, saved: the confirmed pins stay closed until it ran.
    d.stageAndSave(kFactory);
    CHECK(is(d.setPin(inst, act::kPin, 11), Result::InvalidArgument, Detail::GpioNotAllowed));
    d.boot();
    CHECK(is(d.setPin(inst, act::kPin, 11), Result::InvalidArgument, Detail::GpioNotAllowed)); // the fallback
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    d.boot();
    CHECK(d.setPin(inst, act::kPin, 11).result == Result::PendingReboot);
}

TEST_CASE("Link: no ARM, no link edit and no OTA during a trial; no link edit on an unconfirmed image")
{
    Device d;
    d.boot();
    const drivelink::protocol::Sha256Digest digest{};
    CHECK(d.core->ota().begin(0, digest).detail == Detail::Capacity); // past the link checks
    d.stage({5, 43, 44});
    CHECK(is(d.core->ota().begin(100, digest), Result::Busy, Detail::RebootPending)); // staged
    REQUIRE(d.core->save().ok());
    REQUIRE(d.core->discardPending().ok());
    CHECK(is(d.core->ota().begin(100, digest), Result::Busy, Detail::RebootPending)); // saved

    d.boot();
    REQUIRE(d.link.trial);
    CHECK(is(d.core->arm(), Result::Busy, Detail::LinkTrialActive));
    CHECK(is(d.core->ota().begin(100, digest), Result::Busy, Detail::LinkTrialActive));
    for (const uint32_t value : {5U, 6U}) { // the running value too
        const core::Status st = d.set({{sys::kLimitsLoops, 5}, {sys::kLinkBaud, value}});
        CHECK(is(st, Result::Busy, Detail::LinkTrialActive));
        CHECK(st.index == 1);
    }
    CHECK(d.set({{sys::kLimitsLoops, 5}}).result == Result::PendingReboot); // the rest is free
    REQUIRE(d.core->confirmLink(core_os::nowUs()).ok());
    CHECK(d.pending() == LinkSettings{5, 43, 44}); // the pending copy follows
    REQUIRE(d.core->discardPending().ok());
    CHECK(d.core->arm().detail != Detail::LinkTrialActive);

    // Unconfirmed image: a saved trial waits for the boot after it.
    d.stageAndSave({6, 43, 44});
    native::setOtaImageUnconfirmed(true);
    d.boot();
    CHECK_FALSE(d.link.trial);
    CHECK(d.running() == LinkSettings{5, 43, 44});
    CHECK(d.pending() == LinkSettings{6, 43, 44});
    CHECK(d.core->linkChangePending());
    native::setOtaImageUnconfirmed(false);
    d.boot();
    CHECK(d.link.trial);
}

TEST_CASE("Link: GET_INFO reports the trial, LINK_CONFIRM ends it, with an exact body")
{
    Device d;
    d.boot();
    auto service = std::make_unique<core::CommandService>(*d.core, core::DeviceInfo{});
    std::array<uint8_t, 1024> out{};
    const auto call = [&](std::vector<uint8_t> req) {
        req.insert(req.begin(), {cw::kVersion, 7});
        return service->handle(req, out);
    };
    const auto trialMs = [&] {
        const size_t n = call({1, 1});
        REQUIRE(n > 10);
        REQUIRE(out[4] == 0);
        drivelink::protocol::wire::Reader r(out.data() + n - 4U, 4);
        return r.readU32Le();
    };
    CHECK(trialMs() == 0);
    REQUIRE(call({1, 11}) == 6);
    CHECK(out[4] == static_cast<uint8_t>(Result::InvalidArgument));
    CHECK(out[5] == static_cast<uint8_t>(Detail::NoLinkTrial));

    d.stageAndSave({5, 43, 44});
    service.reset();
    d.boot();
    service = std::make_unique<core::CommandService>(*d.core, core::DeviceInfo{});
    CHECK(trialMs() > 20000);
    CHECK(trialMs() <= 30000);
    REQUIRE(call({1, 11, 0}) == 6); // a body is refused
    CHECK(out[5] == static_cast<uint8_t>(Detail::BadValue));
    CHECK(trialMs() > 20000);
    REQUIRE(call({1, 11}) == 6);
    CHECK(out[4] == 0);
    CHECK(out[5] == 0);
    CHECK(service->takeAfterReply() == core::AfterReply::None);
    CHECK(trialMs() == 0);
}
