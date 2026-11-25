#include "drivelink/link.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/link.hpp"
#include <algorithm>
#include <array>
#include <thread>

namespace drivelink {
namespace p=protocol;
namespace {
namespace item=p::defs::system;
constexpr auto kConfirm=static_cast<uint8_t>(p::defs::SystemOp::LinkConfirm);
constexpr uint8_t kSafeMode=8; // GET_INFO flags
// The device's trial times, and what the reboots and the reconnects may add.
constexpr std::chrono::seconds kBaudTrial{30},kPinTrial{120},kSettle{15};
// How long a REBOOT may take to show. It comes at once after its reply.
constexpr std::chrono::seconds kRebootGrace{10};

std::array<Ref,3> linkRefs() {
    return {Ref{item::kTypeId,0,item::kLinkBaud},Ref{item::kTypeId,0,item::kLinkTxPin},Ref{item::kTypeId,0,item::kLinkRxPin}};
}
LinkSettings decode(const ParamResult &values) {
    const auto rate=values.values.size()==3 ? p::linkBaudRate(values.values[0]) : 0U;
    if (!rate || values.values[1]>255 || values.values[2]>255) { throw Error(ErrorKind::Protocol,"malformed link settings"); }
    return {rate,static_cast<uint8_t>(values.values[1]),static_cast<uint8_t>(values.values[2])};
}
// The request was sent and nobody knows whether it took effect: a lost or
// malformed reply, a Boot that interrupted it, or a lost connection.
bool uncertain(const Error &e) {
    return e.outcome_unknown || e.boot_changed || e.kind==ErrorKind::Timeout || e.kind==ErrorKind::Protocol;
}
// Worth another connection: nothing answered, or the device was in between.
bool transient(const Error &e) {
    return e.kind==ErrorKind::Connection || e.kind==ErrorKind::Timeout || e.boot_changed;
}
// The device could not store the confirmation. The trial goes on.
bool storageFailed(const Error &e) {
    return e.kind==ErrorKind::Rejected && e.detail==static_cast<uint8_t>(p::defs::Detail::StorageFailed);
}
}
LinkState linkState(Client &client) {
    const auto refs=linkRefs();
    // A trial that ends between the reads would mix two boots: read again.
    for (unsigned attempt=0;attempt<3;++attempt) {
        const auto generation=client.session().generation();
        const bool before=client.info().link_trial_ms!=0;
        LinkState state;
        state.running=decode(client.get(refs));
        state.next=decode(client.get(refs,true));
        state.info=client.info();
        if (state.trial()==before && client.session().generation()==generation) { return state; }
    }
    throw Error(ErrorKind::Timeout,"the link kept changing while it was read");
}
ParamResult stageLink(Client &client,const LinkSettings &link) {
    const auto baud=p::linkBaudValue(link.baud);
    if (!baud) { throw Error(ErrorKind::Usage,"the link has no rate of "+std::to_string(link.baud)+" baud"); }
    const std::array<uint32_t,3> values{*baud,link.tx_pin,link.rx_pin};
    return client.set(linkRefs(),values);
}
LinkConfirmation confirmLinkTrial(Client &client,const p::DeviceId &device) {
    const auto before=linkState(client);
    if (before.info.device_id!=device) { return LinkConfirmation::WrongDevice; }
    if (!before.trial()) { return LinkConfirmation::NoTrial; }
    try {
        (void)client.system(kConfirm);
        return LinkConfirmation::Confirmed;
    } catch (const Error &e) {
        if (e.kind==ErrorKind::Rejected && e.detail==static_cast<uint8_t>(p::defs::Detail::NoLinkTrial)) { return LinkConfirmation::NoTrial; }
        if (!uncertain(e)) { throw; }
        LinkState after;
        try { after=linkState(client); }
        catch (Error &failed) {
            if (transient(failed)) {
                failed.setMessage("link confirmation uncertain: "+std::string(failed.what()));
                failed.family=static_cast<uint8_t>(p::defs::Family::System);failed.op=kConfirm;
                failed.has_request_context=true;failed.outcome_unknown=true;
            }
            throw;
        }
        if (after.info.device_id!=device) { return LinkConfirmation::WrongDevice; }
        if (after.trial()) { return LinkConfirmation::Unconfirmed; }
        return after.running==before.running ? LinkConfirmation::Confirmed : LinkConfirmation::Reverted;
    }
}
std::chrono::milliseconds LinkExpectation::window() const {
    return (trial.samePins(confirmed) ? kBaudTrial : kPinTrial)+kSettle;
}
std::optional<LinkExpectation> expectLinkTrial(const LinkState &state) {
    if (state.trial()) { return LinkExpectation{state.info.device_id,state.running,state.next}; }
    if (!state.changed()) { return std::nullopt; }
    return LinkExpectation{state.info.device_id,state.next,state.running};
}
LinkOutcome classifyLinkTrial(const LinkExpectation &expected,const LinkState &state) {
    if (state.info.device_id!=expected.device) { return LinkOutcome::WrongDevice; }
    if (state.running==expected.trial) { return state.trial() ? LinkOutcome::Unconfirmed : LinkOutcome::Confirmed; }
    if (state.running!=expected.confirmed || state.trial()) { return LinkOutcome::OtherLink; }
    if (state.next!=expected.trial) { return LinkOutcome::Reverted; }
    return (state.info.flags&kSafeMode) ? LinkOutcome::Deferred : LinkOutcome::NotRebooted;
}
std::string linkOutcomeText(LinkOutcome outcome) {
    switch (outcome) {
    case LinkOutcome::Confirmed: return "the new link is confirmed";
    case LinkOutcome::Unconfirmed: return "the new link runs and is not confirmed yet";
    case LinkOutcome::Reverted: return "the device went back to the old link";
    case LinkOutcome::NotRebooted: return "the device has not rebooted into the new link";
    case LinkOutcome::Deferred: return "the device is in safe mode; a normal boot tries the new link";
    case LinkOutcome::WrongDevice: return "another device answered";
    case LinkOutcome::OtherLink: return "the device runs neither the old link nor the new one";
    case LinkOutcome::NotReachable: return "no device answered on the old link or the new one";
    }
    return {};
}
LinkTrialResult settleLinkTrial(Client &client,const LinkExpectation &expected,bool confirm) {
    LinkTrialResult result;
    result.baud=client.session().baud();
    result.state=linkState(client);
    result.outcome=classifyLinkTrial(expected,*result.state);
    if (result.outcome!=LinkOutcome::Unconfirmed || !confirm) { return result; }
    (void)confirmLinkTrial(client,expected.device);
    // Whatever it said, the device has the last word.
    result.state=linkState(client);
    result.outcome=classifyLinkTrial(expected,*result.state);
    return result;
}
LinkTrialResult followLinkTrial(const std::string &endpoint,const SessionOptions &options,const LinkExpectation &expected,
                                const LinkFollowOptions &follow,const std::function<bool()> &cancelled) {
    if (expected.trial==expected.confirmed) { throw Error(ErrorKind::Usage,"no link change to follow"); }
    auto reconnect=options;
    reconnect.baud=kBaudAuto;
    reconnect.last_baud=expected.trial.baud;
    reconnect.timeout=std::min(options.timeout,std::chrono::milliseconds(1500));
    reconnect.detect_timeout=std::min(options.detect_timeout,std::chrono::milliseconds(5000));
    const auto started=std::chrono::steady_clock::now();
    const auto deadline=started+(follow.window.count()>0 ? follow.window : expected.window());
    LinkTrialResult last;
    while (std::chrono::steady_clock::now()<deadline) {
        if (cancelled && cancelled()) { throw Error(ErrorKind::Cancelled,"link trial left alone; it reverts unless confirmed"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        try {
            Session session(endpoint,reconnect);
            Client client(session);
            last=settleLinkTrial(client,expected,follow.confirm);
            const bool wait=(last.outcome==LinkOutcome::Unconfirmed && follow.confirm) ||
                (last.outcome==LinkOutcome::NotRebooted && std::chrono::steady_clock::now()-started<kRebootGrace);
            if (!wait) { return last; }
        } catch (const Error &e) {
            if (!transient(e) && !storageFailed(e)) { throw; }
            last.problem=e.what();
            last.outcome=transient(e) ? LinkOutcome::NotReachable : LinkOutcome::Unconfirmed;
        }
    }
    return last;
}
} // namespace drivelink
