#pragma once
// Remote mode (H3b-1): the pure parts of a connection through drivelink-agent
// (docs/protocol/agent.md). The lease as this window sees it, the
// exclusive-operation rule, the rows the Connect, Device and faults views
// show, and the recent agents. No I/O; tested in gui-backend.
#include <drivelink/agent.hpp>
#include <drivelink/model.hpp>
#include <QDateTime>
#include <QString>
#include <QVariantList>
#include <optional>

namespace drivelink::gui {

// DLLeaseChip.Variant order.
enum class LeaseChip { Available=0, Mine, Ros, Other, Requesting, Revoked };

struct LeaseHolder {
    uint16_t id=0; // 0: nobody
    QString name;
    uint8_t kind=0; // AgentClientKind
    bool operator==(const LeaseHolder &) const = default;
};

// What this window knows about its device's lease. The agent's Lease events
// are the truth; a Take that timed out may still be granted later.
class LeaseTracker {
public:
    // At Hello: this connection's id and the holder in the device record.
    void reset(uint16_t self,const LeaseHolder &holder);
    void clear() {*this={};}
    struct Change {
        bool gained=false;    // this window holds the lease now and did not before
        bool lost=false;      // this window held it and does not any more
        bool revoked=false;   // lost to another client's Take (not a release)
    };
    Change apply(const agent::LeaseEvent &event);
    // LeaseTake sent / answered. ok: the agent granted it (the Lease event
    // may come before or after the answer); uncertain: no answer in time,
    // so keep requesting until a Lease event decides. An answer after a
    // Lease event already decided the request changes nothing (the events
    // that followed the grant are newer).
    void takeSent() {m_requesting=true;}
    Change takeAnswered(bool ok,bool uncertain);
    // LeaseRelease sent: the agent answers Ok at once and ends the lease.
    Change released();
    // The agent's ClientList, a newer view than a Lease event that was
    // missed: adopts its holder (holder_kind from the record), reporting a
    // loss to another client as a revocation. No change when they agree.
    // sentAt: events() when the list was requested. A Lease event applied
    // since may be newer than the list, so then the list changes nothing
    // (the next list decides).
    Change reconcile(const std::vector<agent::ClientRecord> &clients,uint64_t sentAt);
    // Lease events applied (and lists adopted) so far.
    uint64_t events() const {return m_events;}
    // The user saw the revocation (dialog, banner or the Clients card).
    void acknowledgeRevoked() {m_revokedBy.reset();m_revoked=false;}

    uint16_t self() const {return m_self;}
    const LeaseHolder &holder() const {return m_holder;}
    bool mine() const {return m_self!=0 && m_holder.id==m_self;}
    bool requesting() const {return m_requesting;}
    bool revoked() const {return m_revoked;}
    // Who took it (known once the grant after the revocation arrived).
    std::optional<LeaseHolder> revokedBy() const {return m_revokedBy;}
    // Another client holds it (not this window, not nobody).
    bool otherHolds() const {return m_holder.id!=0 && !mine();}
    LeaseChip chip() const;
    // The name shown for the holder: "this GUI", the client name, or "".
    QString holderText() const;
private:
    uint16_t m_self=0;
    LeaseHolder m_holder;
    bool m_requesting=false,m_revoked=false;
    std::optional<LeaseHolder> m_revokedBy;
    uint64_t m_events=0;
};

// "kind" text for AgentClientKind: GUI, CLI, ROS or Other.
QString clientKindText(uint8_t kind);

// The agent's exclusive-operation rule (agent.md "The lease"), as the reason
// an operation is refused now; empty when the agent would forward it:
// this window holds the lease, or nobody holds it and the device is not
// armed. remote false: always empty.
QString leaseBlock(bool remote,const LeaseTracker &lease,int armState,bool arming);

// ListDevices records for the Agent tab: {alias, id, name, endpoint, online,
// clients, holderId, holderName, holderKind, holderKindText, device} where
// device is what the Hello names (the alias).
QVariantList agentDeviceRows(const std::vector<agent::DeviceRecord> &devices);

// ClientList for the Clients and lease card: {id, name, kind, kindText,
// holder, self}, this window first.
QVariantList clientRows(const std::vector<agent::ClientRecord> &clients,uint16_t self);

// Agent history (History events, oldest first) for the faults drawer and the
// Device card, newest first: {time (QDateTime, UTC), kind (diag, arm, lease,
// online, offline), text, source: "agent"}. Diag entries are named from the
// device's code table when the snapshot has it.
QVariantList historyRows(const std::vector<agent::HistoryEntry> &entries,const DeviceSnapshot *snapshot,uint16_t self);

// A refused or failed agent connection as the Agent tab shows it:
// {state: unreachable, version, token, unknownDevice, offline, capacity,
// other; text; agentVersion; coreVersion}.
QVariantMap agentFailure(const Error &error);

// Recent agents, newest first, in the GUI's INI file (beside the recent
// devices). The token is stored only when the user asked to remember it.
class RecentAgents final {
public:
    static constexpr int kLimit=8;
    explicit RecentAgents(QString path={});
    // {address, device, deviceId, deviceName, token, rememberToken, lastSeen}
    QVariantList list() const {return m_cache;}
    QVariantMap find(const QString &address) const;
    void remember(const QString &address,const QString &device,const QString &deviceId,const QString &deviceName,
                  const QString &token,bool rememberToken,const QDateTime &when=QDateTime::currentDateTimeUtc());
    void forget(const QString &address);
    QString path() const {return m_path;}
private:
    QString m_path;
    QVariantList m_cache;
    QVariantList read() const;
    void write(const QVariantList &);
};

// "host:port" or "tcp://host:port" (port defaults to the agent's 47620) as
// the tcp:// endpoint a Session takes; empty when it is not one.
QString agentEndpoint(const QString &address);

} // namespace drivelink::gui
