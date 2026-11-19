// Remote mode (H3b-1): a LocalConnection through drivelink-agent
// (docs/protocol/agent.md). Everything else (Layout, Monitor, Tune, OTA) is
// the same through the agent; this file adds the lease, the agent's events
// (Lease, History, Closing), the client list, the Agent tab's lobby listing
// and the confirmation of live edits while another client holds the lease.
#include "connection_internal.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <QPointer>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
}

QString LocalConnection::exclusiveBlocked() const {
    return leaseBlock(m_agent.has_value(),m_lease,armState(),m_arming);
}

QVariantMap LocalConnection::lease() const {
    if(!m_agent) {return {};}
    const auto &h=m_lease.holder();
    QString take,release;
    if(!ready()) {take=release=tr("Wait for the device to finish loading");}
    else if(m_leasePending) {take=release=tr("Waiting for the agent");}
    if(take.isEmpty()) {
        if(m_lease.mine()) {take=tr("This window holds the lease");}
        else if(m_lease.requesting()) {take=tr("Waiting for the agent to grant the lease");}
    }
    if(release.isEmpty() && !m_lease.mine()) {release=tr("This window does not hold the lease");}
    const auto by=m_lease.revokedBy();
    return {{"chip",int(m_lease.chip())},{"holderId",int(h.id)},{"holderName",m_lease.holderText()},
        {"holderKind",int(h.kind)},{"holderKindText",h.id?clientKindText(h.kind):QString()},
        {"mine",m_lease.mine()},{"requesting",m_lease.requesting()},{"revoked",m_lease.revoked()},
        {"revokedBy",by?(by->name.isEmpty()?tr("client %1").arg(by->id):by->name):QString()},
        {"revokedAt",m_revokedAt},
        {"ros",m_lease.otherHolds() && h.kind==uint8_t(defs::AgentClientKind::Ros)},
        {"takeBlocked",take},{"releaseBlocked",release}};
}

QVariantMap LocalConnection::agent() const {
    if(!m_agent) {return {};}
    const bool hasId=std::any_of(m_agentRecord.device_id.begin(),m_agentRecord.device_id.end(),[](uint8_t b){return b!=0;});
    return {{"address",m_agent->address},{"device",m_agent->device},
        {"deviceId",hasId?QString::fromStdString(drivelink::agent::formatDeviceId(m_agentRecord.device_id)):QString()},
        {"deviceName",QString::fromStdString(m_agentRecord.name)},{"link",QString::fromStdString(m_agentRecord.endpoint)},
        {"clientId",int(m_lease.self())},{"since",m_agentSince},
        {"protocol",tr("agent protocol v%1").arg(drivelink::agent::kVersion)},{"closing",m_agentClosing}};
}

QVariantMap LocalConnection::liveEdit() const {
    if(!m_liveEdit) {return {};}
    return {{"key",m_liveEdit->key},{"serial",QString::number(m_liveEditSerial)},{"what",m_liveEdit->what},{"holder",m_lease.holderText()},
        {"ros",m_lease.holder().kind==uint8_t(defs::AgentClientKind::Ros)}};
}

bool LocalConnection::holdLiveEdit(const QString &key,const QString &what,std::function<void()> send,std::function<void()> cancel) {
    // No connection (closing, or between connections): nothing to hold; the
    // caller's send finds no session and reports it.
    if(!m_agent || !m_context || m_closing || !m_lease.otherHolds() || m_liveEditOptOut) {return false;}
    // The edit the operator just confirmed goes on while it continues (a
    // drag or a run of steps on the same param).
    if(key==m_consentKey && m_consentSeen.isValid() && m_consentSeen.elapsed()<1500) {m_consentSeen.restart();return false;}
    // While the operator answers one question (the dialog covers the pages)
    // another edit is dropped unsent. Replacing the held one instead could
    // re-enter through its cancel callback (Tune sends its next value then).
    if(m_liveEdit) {
        if(cancel) {cancel();}
        return true;
    }
    m_liveEdit=LiveEditHold{key,what,std::move(send),std::move(cancel)};
    ++m_liveEditSerial;
    emit changed();
    return true;
}
void LocalConnection::dropLiveEdit() {
    if(!m_liveEdit) {return;}
    auto held=std::move(*m_liveEdit);
    m_liveEdit.reset();
    // The view sees the question answered before the callback can ask anew.
    emit changed();
    if(held.cancel) {held.cancel();}
}
void LocalConnection::applyLiveEdit(bool dontAskAgain) {
    if(!m_liveEdit) {return;}
    auto held=std::move(*m_liveEdit);
    m_liveEdit.reset();
    if(dontAskAgain) {m_liveEditOptOut=true;}
    m_consentKey=held.key;m_consentSeen.restart();
    emit changed();
    if(held.send) {held.send();}
}
void LocalConnection::cancelLiveEdit() {dropLiveEdit();}

void LocalConnection::pollAgentEvents() {
    auto c=m_context;auto session=c?c->getSession():nullptr;
    // Until the Hello's answer is applied (the opener's post), events wait.
    if(!session || m_lease.self()==0) {return;}
    // The Session's own agent queue (stream traffic never pushes an agent
    // event out of it); in arrival order, from the Hello on.
    Frame frame;
    while(session->nextAgentEvent(frame)) {
        agentEvent(frame);
        if(!current(c)) {return;}
    }
}

void LocalConnection::agentEvent(const Frame &frame) {
    const auto event=drivelink::agent::decodeEvent(frame.payload);
    if(!event) {return;}
    switch(defs::AgentEvent(event->event)) {
    case defs::AgentEvent::Lease: {
        const auto body=drivelink::agent::decodeLease(event->body);
        if(!body) {return;}
        const bool hadRevokedBy=m_lease.revokedBy().has_value();
        const auto change=m_lease.apply(*body);
        if(change.gained) {leaseGained();}
        if(change.lost) {leaseLost(change.revoked);}
        if(!hadRevokedBy && m_lease.revokedBy()) {emit leaseRevoked(m_lease.revokedBy()->name);}
        refreshClients();
        emit changed();
        return;
    }
    case defs::AgentEvent::History: {
        const auto chunk=drivelink::agent::decodeHistory(event->body);
        if(!chunk) {return;}
        m_historyEntries.insert(m_historyEntries.end(),chunk->entries.begin(),chunk->entries.end());
        // Named once the whole history is in (up to 4096 entries).
        if(chunk->last) {m_historyDone=true;rebuildHistory();emit changed();}
        return;
    }
    case defs::AgentEvent::Closing: {
        const auto reason=drivelink::agent::decodeClosing(event->body);
        m_agentClosing=reason && *reason==uint8_t(defs::AgentCloseReason::Shutdown)
            ?tr("The agent is shutting down"):tr("The agent lost the device");
        emit changed();
        return;
    }
    default:
        return; // ClockModel: the Session composes the clock itself
    }
}

void LocalConnection::rebuildHistory() {
    if(!m_historyDone) {return;}
    m_agentHistory=historyRows(m_historyEntries,m_snapshot.get(),m_lease.self());
}

void LocalConnection::leaseGained() {
    auto c=m_context;if(!c) {return;}
    // The agent disarmed the device for the handoff (or nobody held the
    // lease, so nothing could be armed): this window may set up its command
    // groups. It never arms by itself (G6).
    m_explicitDisarmAcknowledged=true;m_observingArmed=false;m_setupAttempted=false;
    m_revokedAt={};++c->leaseEpoch;
    emit notification(tr("This window holds the control lease; the device stays disarmed until you press ARM"),1);
    refresh(c);
}

void LocalConnection::leaseLost(bool revoked) {
    auto c=m_context;if(!c) {return;}
    // The agent stopped this window's command frames, disarmed the device
    // and cleared its command groups: forget them here, without CMD_CLEAR.
    // An ARM in flight is cancelled by the fence (the agent disarms after it).
    ++c->safety;++c->leaseEpoch;m_ownArm=false;m_arming=false;
    c->releaseOwned();
    m_groupsReady=false;m_setupAttempted=false;
    m_command.clearGroups();m_command.setBlocked(commandBlocked());
    if(revoked) {
        m_revokedAt=QDateTime::currentDateTime();
        emit notification(tr("Control lease revoked: another client took it and the device was disarmed. This window keeps observing."),2);
    } else {
        emit notification(tr("Control lease released; the device was disarmed"),0);
    }
    // A run under way ends on the disarm the status shows, as on any DISARM.
    refresh(c);
}

void LocalConnection::acknowledgeRevoked() {m_lease.acknowledgeRevoked();emit changed();}

void LocalConnection::takeLease() {
    if(!m_agent || !ready() || m_lease.mine() || m_lease.requesting() || m_leasePending) {return;}
    auto c=m_context;auto session=c->getSession();
    m_lease.takeSent();m_leasePending=true;emit changed();
    const QPointer<LocalConnection> self(this);
    try {
        // Up to 15 s while the agent disarms and clears a previous holder.
        session->async<int>([session] {agentLeaseTake(*session);return 0;},[self,c](std::expected<int,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_leasePending=false;
            const bool uncertain=!result && result.error().outcome_unknown;
            const auto change=self->m_lease.takeAnswered(bool(result),uncertain);
            if(!result) {
                if(uncertain) {
                    emit self->notification(tr("The agent has not answered the lease request yet; it may still grant it"),0);
                } else {self->report(result.error());}
            }
            if(change.gained) {self->leaseGained();}
            emit self->changed();
        });
    } catch(const Error &e) {m_leasePending=false;(void)m_lease.takeAnswered(false,false);report(e);emit changed();}
}

void LocalConnection::releaseLease() {
    if(!m_agent || !ready() || !m_lease.mine() || m_leasePending) {return;}
    auto c=m_context;auto session=c->getSession();
    m_leasePending=true;emit changed();
    const QPointer<LocalConnection> self(this);
    try {
        session->async<int>([session] {agentLeaseRelease(*session);return 0;},[self,c](std::expected<int,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_leasePending=false;
            if(!result) {self->report(result.error());emit self->changed();return;}
            const auto change=self->m_lease.released();
            if(change.lost) {self->leaseLost(false);}
            self->refreshClients();
            emit self->changed();
        });
    } catch(const Error &e) {m_leasePending=false;report(e);emit changed();}
}

void LocalConnection::refreshClients() {
    if(!m_agent) {return;}
    // One read at a time; a Lease event meanwhile asks for one more, so a
    // list that could not decide is followed by one that can.
    if(m_clientsPending) {m_clientsAgain=true;return;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session || m_lease.self()==0) {return;}
    m_clientsPending=true;m_clientsAgain=false;
    const auto sentAt=m_lease.events();
    const QPointer<LocalConnection> self(this);
    try {
        session->async<std::vector<drivelink::agent::ClientRecord>>([session] {return agentClients(*session);},
            [self,c,sentAt](std::expected<std::vector<drivelink::agent::ClientRecord>,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_clientsPending=false;
            // A failed read keeps the last list: the next tick reads again.
            if(result) {
                // Lease events sent before this answer are already queued:
                // apply them first. When no event was applied since the
                // request (here or by the timer's poll), the list and this
                // view describe the same moment and the list corrects a view
                // that missed an event. Otherwise one may be newer than the
                // list: the read it asked for decides.
                self->pollAgentEvents();
                if(!self->current(c)) {return;}
                const bool hadRevokedBy=self->m_lease.revokedBy().has_value();
                const auto change=self->m_lease.reconcile(*result,sentAt);
                if(change.gained) {self->leaseGained();}
                if(change.lost) {self->leaseLost(change.revoked);}
                if(!hadRevokedBy && self->m_lease.revokedBy()) {emit self->leaseRevoked(self->m_lease.revokedBy()->name);}
                self->m_clients=clientRows(*result,self->m_lease.self());
                emit self->changed();
            }
            if(self->m_clientsAgain) {self->refreshClients();}
        });
    } catch(const Error &) {m_clientsPending=false;}
}

void LocalConnection::listAgent(const QString &address,const QString &token) {
    const auto endpoint=agentEndpoint(address);
    if(endpoint.isEmpty()) {
        m_listing={{"state","failed"},{"address",address},
                   {"failure",QVariantMap{{"state","other"},{"text",tr("Enter the agent as host or host:port")}}}};
        emit changed();return;
    }
    if(token.isEmpty()) {
        m_listing={{"state","failed"},{"address",address},
                   {"failure",QVariantMap{{"state","token"},{"text",tr("Enter the agent's token")}}}};
        emit changed();return;
    }
    if(m_listing.value("state")==QLatin1String("listing")) {return;}
    m_listing={{"state","listing"},{"address",address}};
    emit changed();
    m_listTarget={address,token};
    // A quiet refresh still running: this list starts when it ends (its
    // answer is dropped), so the GUI thread never waits on a lobby.
    if(m_listBusy) {m_listQueued=true;return;}
    startList(false);
}

void LocalConnection::refreshAgentList() {
    // Only an answered list is refreshed, quietly (it stays shown meanwhile).
    if(m_listBusy || m_listing.value("state")!=QLatin1String("ok") || m_context || m_closing) {return;}
    if(m_listing.value("address").toString()!=m_listTarget.first) {return;}
    startList(true);
}

void LocalConnection::startList(bool quiet) {
    if(m_lister.joinable()) {m_lister.join();} // ended: m_listBusy was false
    const auto serial=++m_listSerial;
    m_listBusy=true;m_listQueued=false;
    const QString address=m_listTarget.first,token=m_listTarget.second,endpoint=agentEndpoint(address);
    const auto queue=m_dispatch;const QPointer<LocalConnection> self(this);
    const auto ep=endpoint.toStdString(),tok=token.toStdString(),name=m_clientName.toStdString();
    m_lister=std::thread([queue,self,serial,quiet,address,ep,tok,name] {
        QVariantMap listing{{"address",address}};
        try {
            const auto devices=listAgentDevices(resolveTcpEndpoint(ep),tok,std::chrono::milliseconds(3000),name,
                                                uint8_t(defs::AgentClientKind::Gui));
            listing["state"]=QStringLiteral("ok");listing["devices"]=agentDeviceRows(devices);
        } catch(const std::exception &e) {
            const auto *typed=dynamic_cast<const Error *>(&e);
            listing["state"]=QStringLiteral("failed");
            listing["failure"]=gui::agentFailure(typed?*typed:Error(ErrorKind::Connection,e.what()));
        }
        post(queue,[self,serial,quiet,listing] {
            if(!self || self->m_listSerial!=serial) {return;}
            self->m_listBusy=false;
            if(self->m_listQueued) {self->startList(false);return;}
            // A quiet refresh lands only on the list it refreshes.
            if(quiet && self->m_listing.value("state")!=QLatin1String("ok")) {return;}
            self->m_listing=listing;emit self->changed();
        });
    });
}

void LocalConnection::connectAgent(const QString &address,const QString &token,const QString &device,bool rememberToken) {
    if(m_context || m_closing) {return;}
    const auto endpoint=agentEndpoint(address);
    QString problem;
    if(endpoint.isEmpty()) {problem=tr("Enter the agent as host or host:port");}
    else if(token.isEmpty()) {problem=tr("Enter the agent's token");}
    else if(device.isEmpty()) {problem=tr("Choose one of the agent's devices");}
    if(!problem.isEmpty()) {m_agentFailure={{"state","other"},{"text",problem}};emit changed();return;}
    m_agent=AgentTarget{address.trimmed(),endpoint,token,device,rememberToken};
    m_agentFailure.clear();
    open(endpoint,921600);
}

void LocalConnection::forgetAgent(const QString &address) {
    m_recentAgents.forget(address);
    emit recentDevicesChanged();
}

QVariantMap LocalConnection::agentBrowse() const {
    return {{"state",!m_browseOn ? QStringLiteral("off") : m_browseBusy ? QStringLiteral("searching") : QStringLiteral("idle")},
            {"protocol",int(drivelink::agent::kVersion)},
            {"agents",m_found.rows(drivelink::agent::kVersion)}};
}

void LocalConnection::startAgentBrowse() {
    if(m_browseOn) {return;}
    m_browseOn=true;m_browseRounds=0;
    browseRound();
    emit changed();
}

void LocalConnection::stopAgentBrowse() {
    if(!m_browseOn) {return;}
    m_browseOn=false;m_browseNow=false;
    m_browseTimer.stop();
    m_found.clear();
    ++m_browseSerial; // a round still running lands nowhere
    m_browseBusy=false;
    emit changed();
}

void LocalConnection::browseAgentsNow() {
    if(!m_browseOn) {startAgentBrowse();return;}
    m_browseRounds=0;
    if(m_browseBusy) {m_browseNow=true;return;}
    m_browseTimer.stop();
    browseRound();
    emit changed();
}

void LocalConnection::browseRound() {
    if(!m_browseOn || m_browseBusy) {return;}
    // A stopped round may still be running (its result is dropped by
    // serial): wait for it without blocking the GUI thread.
    if(!m_fakeAgents && m_browserDone && !m_browserDone->load()) {m_browseTimer.start(100);return;}
    const auto serial=++m_browseSerial;
    m_browseBusy=true;
    const QPointer<LocalConnection> self(this);
    auto land=[self,serial](std::vector<discovery::FoundAgent> found) {
        if(!self || self->m_browseSerial!=serial) {return;}
        self->m_browseBusy=false;
        self->m_found.round(found);
        ++self->m_browseRounds;
        if(self->m_browseNow) {self->m_browseNow=false;self->m_browseRounds=0;self->browseRound();}
        else {self->m_browseTimer.start(browseDelay(self->m_browseRounds));}
        emit self->changed();
    };
    if(m_fakeAgents) {
        // Screenshots: no network; "searching" for a moment like a real round.
        QTimer::singleShot(300,this,[land,found=*m_fakeAgents] {land(found);});
        return;
    }
    if(m_browser.joinable()) {m_browser.join();} // finished: m_browserDone is set
    const auto queue=m_dispatch;
    m_browserDone=std::make_shared<std::atomic<bool>>(false);
    m_browser=std::thread([queue,land,done=m_browserDone] {
        std::vector<discovery::FoundAgent> found;
        try {found=discovery::browseAgents(std::chrono::milliseconds(1000));}
        catch(const std::exception &) {} // a network failure is an empty round
        post(queue,[land,found=std::move(found)] {land(found);});
        done->store(true);
    });
}

} // namespace drivelink::gui
