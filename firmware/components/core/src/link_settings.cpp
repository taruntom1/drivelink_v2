// The link settings and their NVS records (core/link_settings.hpp). Pure apart
// from the KvStore and the diag sink. Boot and config task only.

#include "core/link_settings.hpp"

#include <array>

#include "core/config_blob.hpp"
#include "core/gpio_policy.hpp"
#include "core/persistence.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/link.hpp"
#include "drivelink/protocol/wire_codec.hpp"

namespace core {

namespace {

namespace sys = defs::system;
namespace codes = defs::diag::codes::core;
namespace wire = drivelink::protocol::wire;

constexpr uint8_t kFormat = 1;
// confirmed: fmt u8 | gen u32 | baud u8 | tx u8 | rx u8 | crc32
// trial:     fmt u8 | state u8 | baud u8 | tx u8 | rx u8 | crc32
constexpr size_t kConfirmedBytes = 12;
constexpr size_t kTrialBytes = 9;

/// Read a record of exactly N bytes whose CRC and format check out.
template <size_t N> bool readRecord(core_os::KvStore &store, const char *key, std::array<uint8_t, N> &out)
{
    size_t len = 0;
    if (store.get(linkstore::kNamespace, key, nullptr, len) != core_os::kOk || len != N) {
        return false;
    }
    if (store.get(linkstore::kNamespace, key, out.data(), len) != core_os::kOk || len != N) {
        return false;
    }
    wire::Reader crc(out.data() + (N - 4U), 4);
    return crc.readU32Le() == crc32(out.data(), N - 4U) && out[0] == kFormat;
}

template <size_t N> core_os::Err writeRecord(core_os::KvStore &store, const char *key, std::array<uint8_t, N> &bytes)
{
    wire::Writer crc(bytes.data() + (N - 4U), 4);
    crc.appendU32Le(crc32(bytes.data(), N - 4U));
    const core_os::Err err = store.set(linkstore::kNamespace, key, bytes.data(), bytes.size());
    if (err != core_os::kOk) {
        core_os::reportDiag(codes::kNvsWriteFailed, err, linkstore::kNvsArgLink, persist::kNvsOpWrite);
    }
    return err;
}

LinkSettings readLink(wire::Reader &r)
{
    LinkSettings link;
    link.baud = r.readU8();
    link.tx_pin = r.readU8();
    link.rx_pin = r.readU8();
    return link;
}

void appendLink(wire::Writer &w, const LinkSettings &link)
{
    w.appendU8(link.baud);
    w.appendU8(link.tx_pin);
    w.appendU8(link.rx_pin);
}

bool readConfirmed(core_os::KvStore &store, size_t slot, uint32_t &gen, LinkSettings &link)
{
    std::array<uint8_t, kConfirmedBytes> bytes{};
    if (!readRecord(store, linkstore::kConfirmedKeys[slot], bytes)) {
        return false;
    }
    wire::Reader r(bytes.data() + 1, bytes.size() - 1U);
    gen = r.readU32Le();
    link = readLink(r);
    return gen != 0U && (gen & 1U) == slot && linkValid(link, gpio_policy::kEsp32s3Allowed);
}

void reportReverted(const LinkSettings &link, uint32_t cause, core_os::Err err = core_os::kOk)
{
    core_os::reportDiag(codes::kLinkReverted, err, link.diagArg(), cause);
}

} // namespace

uint32_t LinkSettings::rate() const
{
    return drivelink::protocol::linkBaudRate(baud);
}

uint64_t LinkSettings::pinMask() const
{
    return gpio_policy::gpioBit(tx_pin) | gpio_policy::gpioBit(rx_pin);
}

uint32_t LinkSettings::diagArg() const
{
    return ((rate() / 100U) << 16U) | (static_cast<uint32_t>(tx_pin) << 8U) | rx_pin;
}

LinkSettings factoryLink()
{
    LinkSettings link;
    for (const defs::ItemDesc &item : sys::kDesc.items) {
        const auto value = static_cast<uint8_t>(item.default_bits);
        switch (item.id) {
            case sys::kLinkBaud: link.baud = value; break;
            case sys::kLinkTxPin: link.tx_pin = value; break;
            case sys::kLinkRxPin: link.rx_pin = value; break;
            default: break;
        }
    }
    return link;
}

bool linkValid(const LinkSettings &link, uint64_t pin_mask)
{
    const uint64_t tx = gpio_policy::gpioBit(link.tx_pin);
    const uint64_t rx = gpio_policy::gpioBit(link.rx_pin);
    return link.rate() != 0U && link.tx_pin != link.rx_pin && (tx & pin_mask) != 0U && (rx & pin_mask) != 0U;
}

bool isLinkItem(uint16_t id)
{
    return id == sys::kLinkBaud || id == sys::kLinkTxPin || id == sys::kLinkRxPin;
}

LinkSettings linkFrom(const defs::TypeDesc &type, std::span<const uint32_t> values)
{
    LinkSettings link = factoryLink();
    for (size_t i = 0; i < type.items.size() && i < values.size(); ++i) {
        // A pin past u8 cannot be: its item is a u8.
        const auto value = static_cast<uint8_t>(values[i]);
        switch (type.items[i].id) {
            case sys::kLinkBaud: link.baud = value; break;
            case sys::kLinkTxPin: link.tx_pin = value; break;
            case sys::kLinkRxPin: link.rx_pin = value; break;
            default: break;
        }
    }
    return link;
}

void linkInto(const defs::TypeDesc &type, const LinkSettings &link, std::span<uint32_t> values)
{
    for (size_t i = 0; i < type.items.size() && i < values.size(); ++i) {
        switch (type.items[i].id) {
            case sys::kLinkBaud: values[i] = link.baud; break;
            case sys::kLinkTxPin: values[i] = link.tx_pin; break;
            case sys::kLinkRxPin: values[i] = link.rx_pin; break;
            default: break;
        }
    }
}

namespace linkstore {

Records load(core_os::KvStore &store)
{
    Records records;
    records.confirmed = factoryLink();
    for (size_t slot = 0; slot < 2U; ++slot) {
        uint32_t gen = 0;
        LinkSettings link;
        if (readConfirmed(store, slot, gen, link) && gen > records.gen) {
            records.gen = gen;
            records.confirmed = link;
        }
    }
    std::array<uint8_t, kTrialBytes> bytes{};
    if (readRecord(store, kTrialKey, bytes)) {
        wire::Reader r(bytes.data() + 1, bytes.size() - 1U);
        const uint8_t state = r.readU8();
        const LinkSettings link = readLink(r);
        const bool known =
            state == static_cast<uint8_t>(TrialState::Armed) || state == static_cast<uint8_t>(TrialState::Used);
        if (known && linkValid(link, gpio_policy::kEsp32s3Allowed)) {
            records.state = static_cast<TrialState>(state);
            records.trial = link;
        }
    }
    return records;
}

core_os::Err writeTrial(core_os::KvStore &store, Records &records, TrialState state, const LinkSettings &trial)
{
    std::array<uint8_t, kTrialBytes> bytes{};
    wire::Writer w(bytes.data(), bytes.size());
    w.appendU8(kFormat);
    w.appendU8(static_cast<uint8_t>(state));
    appendLink(w, trial);
    const core_os::Err err = writeRecord(store, kTrialKey, bytes);
    if (err == core_os::kOk) {
        records.state = state;
        records.trial = trial;
    }
    return err;
}

core_os::Err writeConfirmed(core_os::KvStore &store, Records &records, const LinkSettings &link)
{
    // gen + 1 is in the other slot than gen: the current record stays.
    const uint32_t gen = records.gen + 1U;
    const size_t slot = gen & 1U;
    std::array<uint8_t, kConfirmedBytes> bytes{};
    wire::Writer w(bytes.data(), bytes.size());
    w.appendU8(kFormat);
    w.appendU32Le(gen);
    appendLink(w, link);
    const core_os::Err err = writeRecord(store, kConfirmedKeys[slot], bytes);
    if (err != core_os::kOk) {
        return err;
    }
    uint32_t stored_gen = 0;
    LinkSettings stored;
    if (!readConfirmed(store, slot, stored_gen, stored) || stored_gen != gen || !(stored == link)) {
        core_os::reportDiag(codes::kNvsWriteFailed, -1, kNvsArgLink, persist::kNvsOpWrite);
        return core_os::Err{-1};
    }
    records.gen = gen;
    records.confirmed = link;
    return core_os::kOk;
}

} // namespace linkstore

LinkBoot bootLink(core_os::KvStore &store, bool hold)
{
    using linkstore::TrialState;
    LinkBoot link;
    link.records = linkstore::load(store);
    linkstore::Records &records = link.records;
    link.running = records.confirmed;
    if (records.state == TrialState::None) {
        return link;
    }
    const bool same = records.trial == records.confirmed; // confirmed, only the closing write is missing
    if (same || records.state == TrialState::Used) {
        if (!same) {
            reportReverted(records.trial, kLinkRevertReset);
        }
        (void)linkstore::writeTrial(store, records, TrialState::None, records.trial);
        records.state = TrialState::None; // a failed write only repeats this at the next boot
        return link;
    }
    if (hold) {
        link.deferred = true;
        return link;
    }
    const core_os::Err err = linkstore::writeTrial(store, records, TrialState::Used, records.trial);
    if (err != core_os::kOk) {
        reportReverted(records.trial, kLinkRevertStore, err);
        link.deferred = true; // still armed: a later boot tries again
        return link;
    }
    link.running = records.trial;
    link.trial = true;
    return link;
}

bool fallBackLink(LinkBoot &link, core_os::Err err)
{
    const LinkSettings failed = link.running;
    if (link.trial) {
        link.trial = false;
        link.running = link.records.confirmed;
    } else if (!(link.running == factoryLink())) {
        link.running = factoryLink();
    } else {
        return false;
    }
    reportReverted(failed, kLinkRevertUart, err);
    return true;
}

} // namespace core
