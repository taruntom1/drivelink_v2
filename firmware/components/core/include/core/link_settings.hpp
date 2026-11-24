#pragma once

#include <cstdint>
#include <span>

#include "core/status.hpp"
#include "core_os/kv_store.hpp"
#include "core_os/types.hpp"
#include "drivelink/protocol/defs.hpp"

namespace core {

namespace defs = drivelink::protocol::defs;

/// The serial link: the system settings `link.baud`, `link.tx_pin` and
/// `link.rx_pin`.
struct LinkSettings {
    uint8_t baud = 0; ///< a `link.baud` value
    uint8_t tx_pin = 0;
    uint8_t rx_pin = 0;

    bool operator==(const LinkSettings &) const = default;
    uint32_t rate() const; ///< bits per second
    bool samePins(const LinkSettings &o) const { return tx_pin == o.tx_pin && rx_pin == o.rx_pin; }
    uint64_t pinMask() const;
    uint32_t diagArg() const; ///< arg0 of `core.LinkReverted`
};

/// The defaults of the settings (protocol/defs/system.yaml).
LinkSettings factoryLink();

/// True for a rate the enum has and two different pins inside `pin_mask`.
bool linkValid(const LinkSettings &link, uint64_t pin_mask);

/// True for a system item that belongs to the link.
bool isLinkItem(uint16_t id);

/// The link in the system values `values` (by item index of `type`), and back.
LinkSettings linkFrom(const defs::TypeDesc &type, std::span<const uint32_t> values);
void linkInto(const defs::TypeDesc &type, const LinkSettings &link, std::span<uint32_t> values);

namespace linkstore {

// NVS: the link has a namespace of its own, apart from the configuration's
// generations: `conf_a` / `conf_b`, the confirmed link by its own generation,
// and `trial`.
inline constexpr const char *kNamespace = "link";
inline constexpr const char *kConfirmedKeys[2] = {"conf_a", "conf_b"};
inline constexpr const char *kTrialKey = "trial";

/// `core.NvsWriteFailed` arg0 for this namespace (diag contract).
inline constexpr uint32_t kNvsArgLink = 0xFFFE;

enum class TrialState : uint8_t {
    None = 0,
    Armed = 1, ///< saved; the next normal boot runs it
    Used = 2,  ///< a boot ran it (or is running it): never run again
};

/// What the namespace holds. Anything missing or damaged reads as the factory
/// link and no trial.
struct Records {
    LinkSettings confirmed;
    uint32_t gen = 0; ///< of the confirmed record, 0 when there is none
    TrialState state = TrialState::None;
    LinkSettings trial; ///< meaningful unless `state` is None
};

Records load(core_os::KvStore &store);

/// Write the trial record, and note it in `records` on success.
core_os::Err writeTrial(core_os::KvStore &store, Records &records, TrialState state, const LinkSettings &trial);

/// Write `link` as the confirmed one into the slot the current one is not in,
/// and read it back. On an error the current one stays.
core_os::Err writeConfirmed(core_os::KvStore &store, Records &records, const LinkSettings &link);

} // namespace linkstore

/// `core.LinkReverted` arg1 (diag contract, protocol/defs/diag_codes.yaml).
inline constexpr uint32_t kLinkRevertTimeout = 1;
inline constexpr uint32_t kLinkRevertReset = 2;
inline constexpr uint32_t kLinkRevertUart = 3;
inline constexpr uint32_t kLinkRevertStore = 4;

/// What the boot decided about the link.
struct LinkBoot {
    linkstore::Records records;
    LinkSettings running;  ///< what the UART opens with
    bool trial = false;    ///< `running` is a trial
    bool deferred = false; ///< a saved trial waits for a later boot (safe mode, an unconfirmed image)

    /// The link of the next boot if nothing changes: what GET_PENDING shows.
    LinkSettings next() const { return deferred ? records.trial : records.confirmed; }
};

/// Read the records and decide. A trial is marked as used **before** this
/// returns, so before the UART opens on it: whatever ends it, the boot after it
/// uses the confirmed link. `hold` defers a saved trial. Reports
/// `core.LinkReverted` for a trial that an earlier boot did not confirm.
LinkBoot bootLink(core_os::KvStore &store, bool hold);

/// The UART failed to open on `link.running`: go to the link to try next
/// (a trial's confirmed link, then the factory one) and report it.
/// @return false when there is none left.
bool fallBackLink(LinkBoot &link, core_os::Err err);

} // namespace core
