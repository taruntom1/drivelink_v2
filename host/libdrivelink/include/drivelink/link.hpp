#pragma once
#include "drivelink/client.hpp"
#include <chrono>
#include <functional>
#include <optional>
#include <string>

// The serial link settings and their trial ([core.md](../../../../docs/protocol/core.md)):
// a saved link change runs once after a reboot and reverts unless a host
// confirms it in time. An open connection (the GUI's or the agent's) uses
// linkState(), settleLinkTrial() and confirmLinkTrial(); followLinkTrial() is
// the whole wait on sessions of its own.
namespace drivelink {
struct LinkSettings
{
    unsigned baud = 921600; // bits per second, one of protocol::kLinkBaudRates
    uint8_t tx_pin = 43, rx_pin = 44;
    bool operator==(const LinkSettings &) const = default;
    bool samePins(const LinkSettings &other) const { return tx_pin == other.tx_pin && rx_pin == other.rx_pin; }
};
struct LinkState
{
    DeviceInfo info;       // GET_INFO, read last: its trial time is the newest
    LinkSettings running;  // GET: the trial's values during one
    LinkSettings next;     // GET_PENDING: the next boot's; the confirmed ones during a trial
    bool trial() const { return info.link_trial_ms != 0; }
    // Staged or saved and not tried yet: after a SAVE, the next boot tries `next`.
    bool changed() const { return !trial() && next != running; }
};
// Reads only. Throws Protocol for a device without link settings.
LinkState linkState(Client &client);
// SET the three link settings in one batch (staged; SAVE writes them, and the
// next boot tries them). Throws Usage for a rate the setting does not offer.
ParamResult stageLink(Client &client, const LinkSettings &link);

// LINK_CONFIRM, sent at most once, and only after GET_INFO showed `device` in a
// trial. A lost or malformed reply is settled by reading, never by sending it
// again.
enum class LinkConfirmation
{
    Confirmed,   // the running link is stored as the confirmed one
    Unconfirmed, // the request was lost and the trial still runs: may be called again
    NoTrial,     // nothing to confirm (none runs, already confirmed, or too late)
    Reverted,    // the trial ended while the outcome was unknown
    WrongDevice  // another device answered; nothing was sent
};
// Throws DeviceError when the device refuses (Fault/StorageFailed: the trial
// goes on, and may be confirmed again), and Timeout or Connection with
// outcome_unknown when a lost reply could not be settled on this connection:
// read linkState() on the next one.
LinkConfirmation confirmLinkTrial(Client &client, const protocol::DeviceId &device);

// What a reboot into a saved link change should lead to. Made before the
// REBOOT; it covers the trial.
struct LinkExpectation
{
    protocol::DeviceId device{};
    LinkSettings trial, confirmed;
    // How long the device may need to settle on one of the two: the trial, the
    // reboot after it and some margin.
    std::chrono::milliseconds window() const;
};
// From the state before the reboot, or during the trial; nullopt when no link
// change waits.
std::optional<LinkExpectation> expectLinkTrial(const LinkState &state);

enum class LinkOutcome
{
    Confirmed,    // the changed link runs and no trial does
    Unconfirmed,  // the trial runs
    Reverted,     // the confirmed link runs again and the change is gone
    NotRebooted,  // still the old link, and the change still waits for a reboot
    Deferred,     // safe mode runs the confirmed link; the change waits for a normal boot
    WrongDevice,  // another device answered
    OtherLink,    // neither link: somebody changed it meanwhile
    NotReachable  // no device answered in time
};
// Never NotReachable.
LinkOutcome classifyLinkTrial(const LinkExpectation &expected, const LinkState &state);
std::string linkOutcomeText(LinkOutcome outcome);

struct LinkTrialResult
{
    LinkOutcome outcome = LinkOutcome::NotReachable;
    unsigned baud = 0;              // the rate the device answered at (serial endpoints)
    std::optional<LinkState> state; // as last read
    std::string problem;            // the last failure, when nothing was settled
};
// One look at an open connection; with `confirm`, a running trial is confirmed.
// Throws what linkState() and confirmLinkTrial() throw.
LinkTrialResult settleLinkTrial(Client &client, const LinkExpectation &expected, bool confirm = true);

struct LinkFollowOptions
{
    bool confirm = true;                 // false: return Unconfirmed once the trial answers
    std::chrono::milliseconds window{0}; // 0: expected.window()
};
// After the REBOOT, with every session on the endpoint closed: reconnect until
// the trial is settled or the window ends. A serial endpoint is opened with
// auto-detection, the trial's rate first, whatever `options.baud` says. Throws
// Cancelled when `cancelled` (asked between attempts) says so, and errors that
// a retry cannot cure.
LinkTrialResult followLinkTrial(const std::string &endpoint, const SessionOptions &options,
                                const LinkExpectation &expected, const LinkFollowOptions &follow = {},
                                const std::function<bool()> &cancelled = {});
} // namespace drivelink
