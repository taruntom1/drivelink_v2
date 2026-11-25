#include "connection_internal.hpp"
#include <drivelink/buffers.hpp>
#include <drivelink/command.hpp>
#include <drivelink/protocol/core_codec.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <drivelink/protocol/link.hpp>
#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QStringDecoder>
#include <QUrl>
#include <QPointer>
#include <QSysInfo>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
QString formatLink(const LinkSettings &l) {
    return QStringLiteral("%1 baud · TX %2 / RX %3").arg(l.baud).arg(l.tx_pin).arg(l.rx_pin);
}
std::optional<LinkSettings> readLink(const std::map<uint16_t,Value> &params) {
    const auto b=params.find(defs::system::kLinkBaud);
    const auto tx=params.find(defs::system::kLinkTxPin);
    const auto rx=params.find(defs::system::kLinkRxPin);
    if(b==params.end() || tx==params.end() || rx==params.end()) {return std::nullopt;}
    const auto rate=protocol::linkBaudRate(valueBits(b->second));
    const auto txv=valueBits(tx->second),rxv=valueBits(rx->second);
    if(!rate || txv>255 || rxv>255) {return std::nullopt;}
    return LinkSettings{rate,uint8_t(txv),uint8_t(rxv)};
}
std::optional<LinkState> snapshotLink(const DeviceSnapshot &s) {
    const auto running=readLink(s.system_params);
    if(!running) {return std::nullopt;}
    LinkState state;
    state.info=s.info;
    state.running=*running;
    state.next=readLink(s.pending_system_params).value_or(*running);
    return state;
}
}

LocalConnection::LocalConnection(QObject *parent):Connection(parent),
    // Application ownership matters at Qt shutdown: deleting a receiver from
    // the destruction of one of its own posted events re-enters Qt's queue
    // mutex. The app deletes this receiver after its posted-event cleanup.
    m_dispatch(std::make_shared<GuiDispatch>()),m_instances(this),m_loops(this),m_faults(this) {
    // Action rows follow every state change (arm state, OTA, a configuration
    // action, outcomes); unchanged rows are left alone.
    connect(this,&Connection::changed,this,[this] {updateActions();});
    m_browseTimer.setSingleShot(true);
    connect(&m_browseTimer,&QTimer::timeout,this,[this] {browseRound();});
    if(qEnvironmentVariableIsSet("DRIVELINK_GUI_FAKE_AGENTS")) {
        m_fakeAgents=parseFakeAgents(qgetenv("DRIVELINK_GUI_FAKE_AGENTS"));
        if(!m_fakeAgents) {qWarning("DRIVELINK_GUI_FAKE_AGENTS is not a JSON array of agents; ignored");}
    }
    connect(&m_monitor,&MonitorView::plansChanged,this,[this] {syncMonitor();});
    connect(&m_monitor,&MonitorView::notification,this,&Connection::notification);
    // Manual command frames go only to this connection's value groups of the
    // current boot; the GUI thread is their single producer.
    m_command.setSettingsPath(m_recent.path());
    m_command.setBlocked(tr("Not connected"));
    m_command.setWriter([this](uint8_t group,std::span<const uint32_t> bits) {
        if(!m_context || !m_snapshot || !commandBlocked().isEmpty()) {return false;}
        std::shared_ptr<CommandGroup> keeper;
        {
            std::lock_guard lock(m_context->mutex);
            for(const auto &o:m_context->owned) {
                if(o.plan.id==group && o.plan.values && o.generation==m_snapshot->generation) {keeper=o.keeper;}
            }
        }
        return keeper && keeper->writeBits(bits);
    });
    // Monitor's samples are popped at 20 Hz: plots move smoothly and a
    // 1 kHz group stays far from its 2048-sample buffer.
    m_monitorTimer.setInterval(50);
    connect(&m_monitorTimer,&QTimer::timeout,this,[this] {pollMonitor();});
    m_monitorTimer.start();
    setupTune();
    // Shown to the agent's other clients (at most 32 bytes on the wire).
    m_clientName=QStringLiteral("drivelink-gui@")+QSysInfo::machineHostName();
    while(m_clientName.toUtf8().size()>32) {m_clientName.chop(1);}
    m_timer.setInterval(100);
    connect(&m_timer,&QTimer::timeout,this,[this] {
        if(m_closing) {
            if(m_closeElapsed.elapsed()>3500) {finishClose();}
            return;
        }
        // A reboot that never came (refused drain, lost reply) ends the
        // expectation, so a later loss is again an unrequested one.
        const int waitLimit=m_linkExpectation?(m_linkWaitOverrideMs>0?m_linkWaitOverrideMs:m_linkWaitMs):m_reconnectWindowMs;
        if(m_expectReboot && !m_reconnecting && m_rebootElapsed.isValid() && m_rebootElapsed.elapsed()>waitLimit) {
            clearRebootExpectation();m_linkExpectation.reset();
            if(m_ota && (m_ota->phase==OtaJob::Phase::Rebooting || m_ota->phase==OtaJob::Phase::Reconnecting)) {
                otaStop(OtaJob::Phase::Failed,m_ota->rollback?tr("The device did not reboot; the reboot was not repeated and the build stays unconfirmed.")
                    :tr("The device did not reboot; the reboot was not repeated. The prepared image is still waiting: abort it in the Firmware card."));
            }
            emit notification(tr("The device did not reboot; it was not asked again"),2);emit changed();
        }
        // While a requested reboot is under way on a link that stays open,
        // wait for Boot instead of reopening the port (opening a tty can
        // pulse an ESP auto-reset line on Linux). For a link-settings change
        // on a direct serial port, close promptly on silence so auto-detect
        // can probe the new baud rate or pins.
        const bool linkSerialWait=m_expectReboot && m_linkExpectation && !m_agent && m_endpoint.startsWith(QLatin1String("serial://"));
        const bool awaitingBoot=m_expectReboot && m_rebootElapsed.isValid() && armState()==0;
        const qint64 silence=linkSerialWait?1500:(awaitingBoot?waitLimit:3000);
        if(m_context && m_snapshot && !m_loading && m_seen.isValid() && m_seen.elapsed()>silence) {
            if(!m_expectReboot) {report(Error(ErrorKind::Connection,"Device stopped sending status"));}
            finishClose(true);return;
        }
        if(++m_pollTick%3==0) {pollLoopStats();}
        if(m_pollTick%2==0) {pollLive();}
        // Live countdown tick for an active link trial or a link-trial wait.
        if(m_pollTick%5==0 && (linkTrialActive() || m_linkExpectation.has_value())) {emit changed();}
        if(m_agent && m_context) {
            pollAgentEvents();
            // The client list has no event: read it every 2 s while Device shows it.
            if(++m_clientsTick%20==0 && m_page==QLatin1String("device")) {refreshClients();}
        }
    });
    m_timer.start();
}
LocalConnection::~LocalConnection() {
    finishClose();
    if(m_lister.joinable()) {m_lister.join();}
    if(m_browser.joinable()) {m_browser.join();} // a round ends within ~1.3 s
}
bool LocalConnection::current(const std::shared_ptr<Context> &c) const {return m_context==c && !c->retired;}
bool LocalConnection::linked() const {return m_context && m_context->getSession() && !m_linkLost && (!m_snapshot || m_snapshot->connected);}
bool LocalConnection::ready() const {
    return linked() && m_snapshot && m_snapshot->ready && !m_loading && !m_closing &&
        m_context->getSession()->generation()==m_snapshot->generation;
}
int LocalConnection::armState() const {return m_snapshot && m_snapshot->status?m_snapshot->status->arm_state:0;}
bool LocalConnection::controllingMotion() const {
    if(!linked() || (m_agent && !m_lease.mine())) {return false;}
    // An in-flight ARM can still take effect. A direct observer can see an
    // armed device without owning its ARM. An agent lease ends with DISARM.
    return m_arming || m_armPending || (armState()==1 && (m_agent || m_ownArm));
}
bool LocalConnection::safeMode() const {return m_snapshot && m_snapshot->safeMode();}
bool LocalConnection::unsaved() const {return m_snapshot && m_snapshot->status && (m_snapshot->status->flags&8);}
// STATUS carries the live flag; GET_INFO's copy is from the last metadata load.
bool LocalConnection::pendingReboot() const {
    if(!m_snapshot) {return false;}
    return m_snapshot->status?(m_snapshot->status->flags&4)!=0:(m_snapshot->info.flags&1)!=0;
}

QString LocalConnection::reason() const {
    if(m_linkLost) {return tr("Link lost");}
    if(m_loading) {return tr("Loading device");}
    if(m_snapshot && m_snapshot->status) {return disarmReasonText(m_snapshot->status->last_disarm_reason);}
    return {};
}
bool LocalConnection::armEnabled() const {
    return ready() && (!m_agent || m_lease.mine()) && m_groupsReady && !m_setupPending && !m_arming && !m_armPending && !m_disarmPending && !m_waitingDisarm && !m_expectReboot &&
        !(m_ota && m_ota->active()) && !linkTrialActive() &&
        (!m_observingArmed || m_explicitDisarmAcknowledged) &&
        !safeMode() && m_snapshot->status && armState()==0 && !(m_snapshot->status->flags&2) &&
        !(m_snapshot->status->flags&4);
}
QString LocalConnection::armBlockedReason() const {
    if(!ready()) {return tr("Wait for current device metadata and status");}
    if(m_agent && !m_lease.mine()) {return tr("ARM needs the control lease");}
    if(safeMode()) {return tr("Device is in safe mode");}
    if(linkTrialActive()) {return tr("Serial link is on trial: confirm it or wait for it to revert");}
    if(m_expectReboot) {return tr("The device is rebooting");}
    if(m_ota && m_ota->active()) {return tr("A firmware update is in progress");}
    if(m_observingArmed && !m_explicitDisarmAcknowledged) {return tr("Press DISARM before taking command ownership");}
    if(m_arming || m_armPending || m_disarmPending || m_waitingDisarm) {return tr("Waiting for arm/disarm confirmation");}
    if(!m_groupsReady) {return m_blocked.isEmpty()?tr("Preparing command keepalives"):m_blocked;}
    if(!m_snapshot->status) {return tr("Waiting for device status");}
    if(m_snapshot->status->flags&2) {return tr("Device reports an ARM blocking condition");}
    // The device refuses ARM with a reboot pending (G7b: pending layouts
    // disable ARM).
    if(m_snapshot->status->flags&4) {return tr("A reboot is pending: save and reboot, or discard the pending changes");}
    return {};
}
void LocalConnection::report(const Error &e) {
    m_error=errorFields(e);m_error["text"]=errorText(e);
    qWarning("DriveLink: %s (family %d, op %d, result %d, detail %d)",e.what(),e.family,e.op,e.result,e.detail);
    // Reconnect attempts after a reboot fail until the device is back; the
    // overlay shows that state instead of a toast per attempt.
    if(!m_reconnecting) {emit notification(m_error["text"].toString(),2);}
    emit changed();
}
void LocalConnection::connectTo(const QString &endpoint,int baud,int lastBaud) {
    if(m_context || m_closing || endpoint.trimmed().isEmpty() || baud<0) {return;}
    // A direct connection (serial, or TCP to vdev): no agent.
    m_agent.reset();m_lease.clear();m_agentFailure.clear();
    open(endpoint,baud,lastBaud);
}
void LocalConnection::open(const QString &endpoint,int baud,int lastBaud) {
    if(m_context || m_closing || endpoint.trimmed().isEmpty() || baud<0) {return;}
    // A new connection by the user ends any reboot follow-up of the last one.
    if(!m_reconnecting && !m_afterLoss) {
        clearRebootExpectation();clearLinkTrialState();m_previous.reset();m_notice.clear();m_staleSubs.clear();
        if(m_ota && m_ota->terminal()) {m_ota.reset();}
    }
    ++m_connectionSerial;m_otaStatus.reset();m_otaStatusPending=false;m_otaStatusFailed=false;
    auto c=std::make_shared<Context>();m_context=c;m_endpoint=endpoint.trimmed();
    m_requestedBaud=baud;
    if(lastBaud>0) {m_lastBaud=lastBaud;}
    else if(baud>0) {m_lastBaud=baud;}
    else if(m_lastBaud<=0 && m_endpoint.startsWith(QLatin1String("serial://"))) {
        m_lastBaud=m_recent.onEndpoint(m_endpoint).value(QStringLiteral("baud")).toInt();
    }
    m_baud=baud>0?baud:m_lastBaud;
    const int hintBaud=m_linkExpectation && !m_agent && m_endpoint.startsWith(QLatin1String("serial://"))
        ?int(m_linkExpectation->trial.baud):m_lastBaud;
    const int openBaud=m_linkExpectation && !m_agent && m_endpoint.startsWith(QLatin1String("serial://"))
        ?int(kBaudAuto):baud;
    const bool reconnecting=m_reconnecting;
    m_loading=true;m_linkLost=false;m_linkNotReachable=false;
    m_error.clear();m_mismatch.clear();m_blocked.clear();m_subsNote.clear();m_snapshot.reset();m_statusCounter.reset();
    m_setupAttempted=false;m_subsAttempted=false;m_recorded=false;m_diag.reset();m_loadStep=0;
    m_observingArmed=false;m_explicitDisarmAcknowledged=false;
    m_disarmWhenOpened=false;
    m_loops.reset({});m_faults.reset({});
    // Remote state belongs to one connection; the opt-out of live-edit
    // questions too (G6).
    m_lease.clear();m_agentRecord={};m_clients.clear();m_historyEntries.clear();m_agentHistory.clear();
    m_agentClosing.clear();m_agentFailure.clear();m_liveEditOptOut=false;m_consentKey.clear();m_leasePending=false;m_clientsPending=false;
    m_clientsAgain=false;m_historyDone=false;m_liveEdit.reset(); // a hold of the last connection never outlives it
    std::optional<AgentHello> hello;
    if(m_agent) {
        hello=AgentHello{m_agent->token.toStdString(),m_agent->device.toStdString(),m_clientName.toStdString(),
                         uint8_t(protocol::defs::AgentClientKind::Gui)};
    }
    emit changed();
    const auto queue=m_dispatch;const QPointer<LocalConnection> self(this);const auto address=m_endpoint.toStdString();
    // Start after this GUI turn. Safety requests during opening are retained
    // until publication, including an Esc immediately after Connect.
    QTimer::singleShot(0,this,[this,c,queue,self,address,openBaud,hintBaud,reconnecting,hello] {
    if(!current(c)) {return;}
    m_opener=std::thread([c,queue,self,address,openBaud,hintBaud,reconnecting,hello] {
        try {
            SessionOptions options;options.dispatcher=[queue](Task task){post(queue,std::move(task));};
            options.baud=unsigned(openBaud);
            options.last_baud=unsigned(std::max(0,hintBaud));
            // Keep auto-detect bounded short in a reconnect loop (blocks ctor).
            options.detect_timeout=reconnecting?std::chrono::milliseconds(2000):std::chrono::milliseconds(5000);
            std::string target=address;
            if(hello) {
                // Through drivelink-agent: a typed host name resolves here,
                // off the GUI thread. Lease and History events arrive on the
                // event queue (read by the GUI), so give it room.
                target=resolveTcpEndpoint(address);
                options.agent=*hello;
            }
            auto session=std::make_shared<Session>(target,options);
            const int detectedBaud=int(session->baud());
            // Discovery can outlast an already-armed owner's watchdog. Keep
            // the early status history independently of DeviceModel, which
            // installs its observer only after its initial metadata load.
            auto noteArmed=[weak=std::weak_ptr<Context>(c)](const Frame &frame) {
                if(auto state=weak.lock();state && !state->retired) {
                    try {
                        const auto event=Client::decodeEvent(frame);
                        if(const auto *status=std::get_if<StatusFrame>(&event);status && status->arm_state==1) {state->armedSeen=frame.generation;}
                        if(const auto *boot=std::get_if<BootEvent>(&event)) {state->bootReason=boot->reset_reason;}
                    } catch(const Error &) {}
                }
            };
            session->observe(noteArmed);
            Frame early;
            for(size_t i=0;i<options.event_capacity && !c->retired;++i) {
                if(!session->nextEvent(early,std::chrono::milliseconds(0))) {break;}
                noteArmed(early);
            }
            // Agent events (History, Lease) wait in the Session's own agent
            // queue from the Hello on; the GUI reads them there.
            const auto accepted=session->agentHello();
            {std::lock_guard lock(c->mutex);c->session=session;}
            if(c->retired) {session->close();return;}
            post(queue,[self,c,accepted,detectedBaud] {
                if(!self || !self->current(c)) {return;}
                if(detectedBaud>0) {self->m_baud=detectedBaud;self->m_lastBaud=detectedBaud;}
                if(accepted) {
                    // An observer first (G6): the holder named by the Hello's
                    // device record; Lease events update it from here on.
                    const auto &d=accepted->device;
                    self->m_agentRecord=d;self->m_agentSince=QDateTime::currentDateTime();
                    self->m_lease.reset(accepted->client_id,LeaseHolder{d.holder_id,QString::fromStdString(d.holder_name),d.holder_kind});
                    self->pollAgentEvents();self->refreshClients();
                }
                self->m_loadStep=1;
                if(self->m_disarmWhenOpened) {self->disarm();}
                emit self->changed();
            });
            Client client(*session);
            // The reset reason and uptime feed the reboot notice. Diagnostics
            // are optional: their failure does not block the connection.
            std::optional<DiagInfo> diag;
            try {diag=client.diagInfo();} catch(const Error &) {}
            post(queue,[self,c,diag] {
                if(!self || !self->current(c)) {return;}
                self->m_loadStep=2;self->m_diag=diag;self->m_diagElapsed.start();emit self->changed();
            });
            auto model=std::make_shared<DeviceModel>(client);
            if(c->retired) {return;}
            model->onChange([self,weak=std::weak_ptr<Context>(c)](DeviceModel::Change change) {
                if(auto state=weak.lock();self && state && self->current(state)) {
                    self->refresh(state,change==DeviceModel::Change::Model);
                }
            });
            {std::lock_guard lock(c->mutex);c->model=std::move(model);}
            post(queue,[self,c] {
                if(!self || !self->current(c)) {return;}
                if(self->m_opener.joinable()) {self->m_opener.join();}
                self->m_loading=false;self->m_seen.start();self->refresh(c);
            });
        } catch(const std::exception &e) {
            const auto *typed=dynamic_cast<const Error *>(&e);
            Error error=typed?*typed:Error(ErrorKind::Connection,e.what());
            post(queue,[self,c,error] {
                if(!self || !self->current(c)) {return;}
                if(self->m_agent && !self->m_reconnecting) {
                    // The Agent tab shows why: unreachable, versions, token, device.
                    self->m_agentFailure=gui::agentFailure(error);
                } else if(error.peer_version) {
                    self->m_mismatch={{"device",error.peer_version},{"host",protocol::core_wire::kVersion},
                        {"endpoint",self->m_endpoint}};
                }
                self->report(error);self->finishClose();
            });
        }
    });
    });
}
void LocalConnection::refresh(const std::shared_ptr<Context> &c,bool metadataEvent) {
    if(!current(c) || m_closing) {return;}
    const auto model=c->getModel();if(!model) {return;}
    auto snapshot=model->snapshotShared();
    const bool metadata=metadataEvent || !m_snapshot || m_snapshot->generation!=snapshot->generation ||
        m_snapshot->ready!=snapshot->ready || m_snapshot->reloading!=snapshot->reloading;
    const auto before=m_snapshot;
    const bool booted=before && before->generation!=snapshot->generation;
    if(!m_snapshot || m_snapshot->generation!=snapshot->generation) {
        // Boot released every group on the device, including loop statistics.
        // A subscription record already under the new boot is a stray slot
        // from a setup that crossed the Boot (releaseStray); keep it.
        c->stopKeepers();
        {std::lock_guard lock(c->mutex);c->owned.clear();std::erase_if(c->subs,[&](const OwnedSub &o){return o.generation!=snapshot->generation;});}
        m_groupsReady=false;m_setupAttempted=false;m_subsAttempted=false;m_statusCounter.reset();++c->safety;
        m_arming=false;m_armReplied=false;m_ownArm=false;m_waitingDisarm=false;
        if(m_snapshot) {m_explicitDisarmAcknowledged=false;}
        // Row states belong to the previous boot's layout.
        m_edits.clear();
        resetLive();resetMonitorGroups();m_command.clearGroups();
        // The groups and the capture went with the boot: so does a run.
        if(m_tuneRun) {endTuneRun(tr("The device rebooted during the run; nothing was recorded"),TuneEnd::DeviceGone);}
        m_tuneRelease.reset(); // the reboot ended the signal; clearGroups ended the hold
        dropModeRestore();     // and restored the saved mode
        for(auto it=m_actionResults.begin();it!=m_actionResults.end();) {
            if(it.key()!=m_actionRunning) {it=m_actionResults.erase(it);} else {++it;}
        }
    }
    // Another client staged or dropped a setting: the reboot flag moved, but the
    // pending values only change with a configuration read.
    const bool stagedElsewhere=before && !booted && before->status && snapshot->status &&
        ((before->status->flags^snapshot->status->flags)&4);
    m_snapshot=std::move(snapshot);
    if(!m_snapshot->connected) {finishClose(true);return;}
    if(stagedElsewhere && ready()) {refreshConfig();}
    // A reboot pending with nothing unsaved: a SAVE wrote pending settings. It
    // holds until the reboot, whatever is staged or discarded afterwards.
    if(!before || booted || !pendingReboot()) {m_savedPending=false;}
    if(pendingReboot() && m_snapshot->status && !unsaved()) {m_savedPending=true;}
    if(metadata) {m_instances.reset(m_snapshot);m_loops.reset(m_snapshot);}
    const auto sameDiagnostics=[&] {
        if(!before || before->diagnostics.size()!=m_snapshot->diagnostics.size()) {return false;}
        return std::equal(before->diagnostics.begin(),before->diagnostics.end(),m_snapshot->diagnostics.begin(),
            [](const DiagEntry &a,const DiagEntry &b) {
                return a.source==b.source && a.instance==b.instance && a.code==b.code && a.state==b.state && a.count==b.count;
            });
    };
    if(metadata || !sameDiagnostics()) {m_faults.reset(m_snapshot);}
    if(booted) {
        const int reason=c->bootReason.exchange(-1);
        showRebootNotice(reason>=0?std::optional<uint8_t>(uint8_t(reason)):std::nullopt,
            before->status && (before->status->flags&8),QString::fromStdString(hex(before->info.device_id)));
    }
    if(m_snapshot->status && (!m_statusCounter || *m_statusCounter!=m_snapshot->status->counter)) {
        m_statusCounter=m_snapshot->status->counter;m_seen.restart();
        // A status after the ARM reply ends "arming" whatever it shows: a
        // fault or watchdog can disarm before any status reports Armed.
        if(armState()==1 || (m_armReplied && !m_armPending)) {m_arming=false;m_armReplied=false;}
        if(armState()!=1 && !m_armPending) {m_ownArm=false;}
        if(armState()==0) {m_waitingDisarm=false;}
        m_loops.setStatus(&*m_snapshot->status);
    }
    if(ready() && (!m_recorded || m_recordedName!=deviceName())) {
        const bool first=!m_recorded;m_recorded=true;m_recordedName=deviceName();
        if(m_agent) {
            m_recentAgents.remember(m_agent->address,m_agent->device,deviceId(),deviceName(),m_agent->token,m_agent->rememberToken);
        } else {
            const int savedBaud=m_baud>0?m_baud:(m_lastBaud>0?m_lastBaud:921600);
            m_recent.remember(deviceId(),deviceName(),m_endpoint,savedBaud,QString::fromStdString(m_snapshot->info.firmware_version));
        }
        emit recentDevicesChanged();
        if(first && (m_reconnecting || m_afterLoss)) {
            // A reconnect after a requested reboot, or a Retry that finds the
            // same device with less uptime than the outage: it rebooted.
            const bool same=m_previous && m_previous->id==deviceId();
            bool rebooted=m_reconnecting;
            // Uptime lower than the device's uptime at the loss means it booted.
            if(!rebooted && same && m_diag) {
                rebooted=m_previous->uptimeUs?m_diag->uptime_us<*m_previous->uptimeUs
                                             :m_diag->uptime_us/1000<uint64_t(m_lossElapsed.elapsed())+3000;
            }
            if(same && rebooted) {
                showRebootNotice(m_diag?std::optional<uint8_t>(m_diag->reset_reason):std::nullopt,m_previous->dirty,deviceId());
            }
            if(!same) {clearRebootExpectation();}
            // Stale statistics records apply only when the same device is
            // known not to have rebooted; otherwise another client may have
            // configured an identical group since.
            const bool provenSameBoot=same && m_diag && m_previous->uptimeUs && m_diag->uptime_us>=*m_previous->uptimeUs;
            if(!provenSameBoot) {m_staleSubs.clear();m_staleId.clear();}
            m_reconnecting=false;m_afterLoss=false;m_previous.reset();
        }
    }
    if(ready() && m_snapshot) {
        if(m_snapshot->info.link_trial_ms!=m_linkTrialBaseMs || m_snapshot->generation!=m_linkTrialGen) {
            m_linkTrialBaseMs=m_snapshot->info.link_trial_ms;
            m_linkTrialGen=m_snapshot->generation;
            if(m_linkTrialBaseMs>0) {m_linkTrialElapsed.restart();}
            else {m_linkTrialElapsed.invalidate();}
        }
        if(m_linkExpectation && !m_linkSettlePending &&
           (m_connectionSerial!=m_linkRebootSerial || m_snapshot->generation!=m_linkRebootGeneration)) {
            settlePendingLinkTrial();
        }
    }
    if(!m_groupsReady && !m_setupPending && !c->owns() &&
       (c->armedSeen==m_snapshot->generation || (m_snapshot->status && armState()==1))) {
        m_observingArmed=true;
    }
    // Through an agent, command groups exist only while this window holds the lease.
    if(ready() && (!m_agent || m_lease.mine()) && m_snapshot->status && armState()==0 && !safeMode() && !m_setupPending &&
       (!m_observingArmed || m_explicitDisarmAcknowledged) &&
       (!m_setupAttempted || m_setupGeneration!=m_snapshot->generation)) {setupGroups();}
    // Loop statistics follow command setup so the two never interleave
    // group mutations; an observer (no command setup) starts them at once.
    const bool commandsSettled=!m_setupPending &&
        ((m_setupAttempted && m_setupGeneration==m_snapshot->generation) || m_observingArmed || armState()!=0 ||
         // A remote observer sets up no command groups: nothing to wait for.
         (m_agent && !m_lease.mine()));
    if(ready() && m_snapshot->status && !safeMode() && !m_subsPending && commandsSettled &&
       (!m_subsAttempted || m_subsGeneration!=m_snapshot->generation)) {setupLoopStats();}
    otaRefresh();
    rebuildLayout(metadata);
    if(m_agent && metadata) {rebuildHistory();}
    // Monitor follows complete metadata only: a reload in progress never prunes its plots.
    if(ready()) {m_monitor.setSnapshot(m_snapshot);}
    syncLive();syncMonitor();
    if(ready()) {m_command.setSnapshot(m_snapshot,deviceId());}
    m_command.setBlocked(commandBlocked());
    syncTune();
    // The live view follows every refresh: Boot, safe mode, reload, run
    // states and staged targets all change it, not only new samples.
    emit changed();emit liveChanged();
}
void LocalConnection::showRebootNotice(std::optional<uint8_t> reason,bool dirty,const QString &id) {
    QStringList lost;
    if(dirty) {
        lost=m_changes.value(id);
        if(lost.isEmpty()) {lost<<tr("Unsaved changes made outside this window");}
    }
    // STATUS lags the SAVE reply, so a save just before the reboot wins.
    if(m_savedBeforeReboot) {lost.clear();}
    forgetChanges(id);
    m_notice={{"visible",true},{"expected",m_expectReboot},{"cause",m_rebootCause},
        {"reason",reason?resetReasonText(*reason):tr("Unknown")},{"reasonCode",reason?QVariant(*reason):QVariant()},
        {"lost",lost}};
    clearRebootExpectation();m_staleSubs.clear();
}
void LocalConnection::clearRebootExpectation() {
    m_expectReboot=false;m_rebootCause.clear();m_savedBeforeReboot=false;m_rebootElapsed.invalidate();
}
void LocalConnection::dismissRebootNotice() {m_notice.clear();emit changed();}
void LocalConnection::setupLoopStats() {
    auto c=m_context;auto session=c->getSession();const auto snapshot=m_snapshot;
    m_subsPending=true;m_subsAttempted=true;m_subsGeneration=snapshot->generation;m_subsNote.clear();
    const QPointer<LocalConnection> self(this);
    using Notes=std::map<uint8_t,std::string>;
    // Groups this GUI configured before an unrequested loss of the same
    // device; an exact match (id, loop, epoch, refs) can only be ours.
    std::vector<StaleSub> stale;
    if(m_staleId==deviceId()) {stale=std::move(m_staleSubs);}
    m_staleSubs.clear();m_staleId.clear();
    try {
        session->async<Notes>([c,session,snapshot,stale] {
            Notes notes;
            const auto type=snapshot->types.find(defs::kLoopTypeId);
            const bool described=type!=snapshot->types.end() && type->second.items.contains(protocol::defs::loop::kLastRunUs) &&
                type->second.items.contains(protocol::defs::loop::kMaxRunUs);
            if(!described) {
                for(const auto &loop:snapshot->info.loops) {notes[loop.id]="The device describes no run-time signals";}
                return notes;
            }
            Client client(*session);
            auto groups=client.groups();
            bool released=false;
            for(const auto &g:groups.groups) {
                const bool ours=std::any_of(stale.begin(),stale.end(),[&](const StaleSub &o) {
                    return g.kind==GroupKind::Subscription && g.configured && g.id==o.id && g.loop==o.loop &&
                        g.epoch==o.epoch && g.refs==o.refs;
                });
                if(ours) {
                    try {client.groupClear(g.id,false);released=true;}
                    catch(const Error &e) {if(e.kind==ErrorKind::Cancelled || e.kind==ErrorKind::Connection) {throw;}}
                }
            }
            if(released) {groups=client.groups();}
            std::vector<uint8_t> free;
            for(const auto &g:groups.groups) {if(g.kind==GroupKind::Subscription && !g.configured) {free.push_back(g.id);}}
            size_t next=0;
            for(const auto &loop:snapshot->info.loops) {
                if(c->retired || session->generation()!=snapshot->generation) {throw Error(ErrorKind::Cancelled,"Connection changed during loop statistics setup");}
                try {
                // Only free slots: another client's subscriptions are never reused.
                if(next>=free.size()) {notes[loop.id]="No free subscription slot";continue;}
                const auto id=free[next++];
                const std::vector<Ref> refs{{defs::kLoopTypeId,loop.id,protocol::defs::loop::kLastRunUs},
                                            {defs::kLoopTypeId,loop.id,protocol::defs::loop::kMaxRunUs}};
                const auto divider=uint16_t(std::clamp<uint32_t>(loop.rate_hz/10,1,65535));
                const auto slot=std::find_if(groups.groups.begin(),groups.groups.end(),[&](const auto &g){return g.kind==GroupKind::Subscription && g.id==id;});
                // Record intent first, as for command groups: close reconciles
                // the exact configuration even if this reply never arrives.
                {std::lock_guard lock(c->mutex);c->subs.push_back({id,loop.id,uint8_t(slot->epoch+1),snapshot->generation,refs,{}});}
                GroupMutation configured;
                for(int attempt=1;;++attempt) {
                    try {configured=client.subConfigure(id,loop.id,divider,refs);break;}
                    catch(const Error &e) {
                        // Busy/Withdrawn guarantees no change (the loop missed
                        // its claim window), so these informative groups retry.
                        const bool withdrawn=e.kind==ErrorKind::Rejected && e.result==uint8_t(defs::Result::Busy) &&
                            e.detail==uint8_t(defs::Detail::Withdrawn);
                        if(withdrawn && attempt<3 && !c->retired) {continue;}
                        if(!e.outcome_unknown) {
                            std::lock_guard lock(c->mutex);
                            std::erase_if(c->subs,[&](const auto &o){return o.statistics() && o.generation==snapshot->generation && o.id==id;});
                        }
                        throw;
                    }
                }
                if(configured.generation!=snapshot->generation) {
                    c->releaseStray(client,{id,loop.id,configured.epoch,configured.generation,refs,{}});
                    throw Error(ErrorKind::Cancelled,"Device rebooted during loop statistics setup");
                }
                auto buffer=session->subscribeBuffer(id,configured.epoch,refs.size(),8,configured.generation);
                {
                    std::lock_guard lock(c->mutex);
                    const auto owned=std::find_if(c->subs.begin(),c->subs.end(),[&](const auto &o){return o.statistics() && o.generation==snapshot->generation && o.id==id;});
                    if(owned==c->subs.end() || c->retired) {throw Error(ErrorKind::Cancelled,"Loop statistics ownership was invalidated");}
                    owned->epoch=configured.epoch;owned->buffer=std::move(buffer);
                    // The free slot now holds exactly this configuration: a stale
                    // record of another kind for it can no longer match it.
                    std::erase_if(c->subs,[&](const auto &o){return !o.statistics() && o.generation==snapshot->generation && o.id==id;});
                }
                // CONFIG enables the group; another mutation would advance its epoch.
                } catch(const Error &e) {
                    // One loop's refusal leaves only that loop without run time.
                    if(e.kind==ErrorKind::Cancelled || e.kind==ErrorKind::Connection) {throw;}
                    notes[loop.id]=errorText(e).toStdString();
                }
            }
            return notes;
        },[self,c](std::expected<Notes,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_subsPending=false;
            if(self->m_snapshot && self->m_snapshot->generation!=self->m_subsGeneration) {self->refresh(c);return;}
            // The selection's and Monitor's groups take slots only after the statistics groups.
            self->m_monitorListStale=true;
            self->syncLive();self->syncMonitor();
            // Run time is informative: a setup failure leaves the column empty
            // with its reason and never blocks the connection.
            if(!result) {
                self->m_subsNote=errorText(result.error());
                std::vector<uint8_t> owned;
                {
                    std::lock_guard lock(c->mutex);
                    for(const auto &o:c->subs) {if(o.buffer && o.statistics()) {owned.push_back(o.loop);}}
                }
                for(const auto &loop:self->m_snapshot->info.loops) {
                    if(std::find(owned.begin(),owned.end(),loop.id)==owned.end()) {self->m_loops.setRunTime(loop.id,std::nullopt,self->m_subsNote);}
                }
            } else {
                for(const auto &[loop,note]:*result) {self->m_loops.setRunTime(loop,std::nullopt,QString::fromStdString(note));}
            }
            emit self->changed();
        });
    } catch(const Error &e) {m_subsPending=false;m_subsNote=errorText(e);}
}
void LocalConnection::pollLoopStats() {
    if(!m_context) {return;}
    std::vector<std::pair<uint8_t,std::shared_ptr<GroupBuffer>>> buffers;
    {
        std::lock_guard lock(m_context->mutex);
        for(const auto &o:m_context->subs) {if(o.buffer && o.statistics()) {buffers.emplace_back(o.loop,o.buffer);}}
    }
    for(const auto &[loop,buffer]:buffers) {
        GroupSample sample;
        if(buffer->latest(sample) && sample.count>=2) {
            m_loops.setRunTime(loop,LoopRunTime{sample.values[0],sample.values[1],sample.counter});
        }
    }
    QHash<int,double> budgets;
    for(int i=0;i<m_loops.rowCount();++i) {
        const auto row=m_loops.index(i);
        if(row.data(LoopTable::HasRunTime).toBool()) {budgets.insert(row.data(LoopTable::LoopId).toInt(),row.data(LoopTable::Budget).toDouble());}
    }
    m_layoutTree.setBudgets(budgets);
}
bool LocalConnection::runOnDevice(std::function<void(Client &)> operation,std::function<void(QString)> done) {
    auto c=m_context;if(!c) {return false;}
    auto session=c->getSession();if(!session) {return false;}
    const QPointer<LocalConnection> self(this);
    try {
        session->async<OperationResult>([session,operation=std::move(operation)] {
            Client client(*session);operation(client);return OperationResult{};
        },[self,c,done=std::move(done)](auto result) {
            if(!self || !self->current(c)) {return;}
            done(result?QString():errorText(result.error()));
        });
    } catch(const Error &) {return false;}
    return true;
}
std::vector<uint8_t> LocalConnection::ownedSubscriptions() const {
    std::vector<uint8_t> ids;
    if(!m_context) {return ids;}
    std::lock_guard lock(m_context->mutex);
    for(const auto &o:m_context->subs) {if(o.statistics()) {ids.push_back(o.id);}}
    return ids;
}
void LocalConnection::setupGroups() {
    auto c=m_context;auto session=c->getSession();const auto snapshot=m_snapshot;
    m_setupPending=true;m_setupAttempted=true;m_setupGeneration=snapshot->generation;
    m_blocked.clear();const QPointer<LocalConnection> self(this);
    const auto leaseEpoch=c->leaseEpoch.load();
    try {
        session->async<OperationResult>([c,session,snapshot,leaseEpoch] {
            Client client(*session);
            // Remote: a group of ours that landed after a lost lease.
            if(c->leaseEpoch!=leaseEpoch) {throw Error(ErrorKind::Cancelled,"The control lease changed during command setup");}
            c->clearReleased(client);
            const auto groups=client.groups();const auto plans=planCommands(*snapshot,groups);
            const auto watchdog=valueBits(snapshot->system_params.at(defs::system::kWatchdogMs));
            if(watchdog<3 || watchdog>65535) {throw Error(ErrorKind::Usage,"Invalid command watchdog");}
            try {
                for(const auto &plan:plans) {
                    if(c->retired || session->generation()!=snapshot->generation) {throw Error(ErrorKind::Cancelled,"Connection changed during command setup");}
                    if(c->leaseEpoch!=leaseEpoch) {throw Error(ErrorKind::Cancelled,"The control lease changed during command setup");}
                    const auto before=std::find_if(groups.groups.begin(),groups.groups.end(),[&](const auto &g){return g.kind==GroupKind::Command && g.id==plan.id;});
                    Owned owned{plan,uint8_t(before->epoch+1),snapshot->generation,{}};
                    // Record intent before a sent mutation can time out. Cleanup
                    // reconciles exact configuration, even without a reply.
                    {std::lock_guard lock(c->mutex);c->owned.push_back(owned);}
                    GroupMutation configured;
                    try {
                        for(int attempt=1;;++attempt) {
                            try {configured=client.cmdConfigure(plan.id,plan.loop,plan.refs);break;}
                            catch(const Error &e) {
                                // Busy/Withdrawn guarantees no change (the loop
                                // missed its claim window): not a blind repeat.
                                const bool withdrawn=e.kind==ErrorKind::Rejected && e.result==uint8_t(defs::Result::Busy) &&
                                    e.detail==uint8_t(defs::Detail::Withdrawn);
                                if(!withdrawn || attempt>=3 || c->retired) {throw;}
                            }
                        }
                    }
                    catch(const Error &e) {
                        if(!e.outcome_unknown) {
                            std::lock_guard lock(c->mutex);
                            std::erase_if(c->owned,[&](const auto &o){return o.generation==snapshot->generation && o.plan.id==plan.id;});
                        }
                        throw;
                    }
                    if(configured.generation!=snapshot->generation) {throw Error(ErrorKind::Cancelled,"Device rebooted during command setup");}
                    // Keepalive-only groups: empty descriptors permit only prefix
                    // writes. Value groups (the hold rule): full descriptors and
                    // an explicit hold of 0, the value the firmware sets at ARM.
                    // Nothing is written here; input defaults never enter the queue.
                    std::vector<ItemDescriptor> descriptors;
                    if(plan.values) {
                        for(const auto &ref:plan.refs) {descriptors.push_back(snapshot->types.at(ref.type).items.at(ref.item).descriptor);}
                    }
                    const std::vector<uint32_t> zeros(descriptors.size(),0);
                    auto keeper=session->manageCommand(plan.id,configured.epoch,std::move(descriptors),zeros,uint16_t(watchdog),configured.generation);
                    std::lock_guard lock(c->mutex);
                    const auto slot=std::find_if(c->owned.begin(),c->owned.end(),[&](const auto &o) {
                        return o.generation==snapshot->generation && o.plan.id==plan.id;
                    });
                    if(slot==c->owned.end() || c->retired) {keeper->stop();throw Error(ErrorKind::Cancelled,"Command ownership was invalidated");}
                    slot->epoch=configured.epoch;slot->keeper=std::move(keeper);
                }
            } catch(...) {
                // Setup does not arm. Try cleanup once, retaining records if a
                // read or CLEAR is uncertain; never repeat CMD_CONFIG blindly.
                try {c->clearOwned(client);} catch(const Error &) {}
                throw;
            }
            return OperationResult{};
        },[self,c,leaseEpoch](auto result) {
            if(!self || !self->current(c)) {return;}
            self->m_setupPending=false;
            if(self->m_agent && (!self->m_lease.mine() || c->leaseEpoch!=leaseEpoch)) {
                // The lease ended during setup: the agent cleared this
                // window's command groups. A CMD_CONFIG that landed under a
                // lease held again since is cleared by the next setup.
                c->releaseOwned();
                self->m_groupsReady=false;self->m_setupAttempted=false;self->m_command.clearGroups();
                self->m_command.setBlocked(self->commandBlocked());
                // Held again since: set up afresh under this lease.
                self->refresh(c);emit self->changed();return;
            }
            if(self->m_snapshot && self->m_snapshot->generation!=self->m_setupGeneration) {self->refresh(c);return;}
            self->m_groupsReady=bool(result);
            if(result) {self->m_observingArmed=false;self->configureCommands();}
            if(!result) {self->m_command.clearGroups();self->m_blocked=errorText(result.error());self->report(result.error());}
            self->m_command.setBlocked(self->commandBlocked());
            emit self->changed();
        });
    } catch(const Error &e) {m_setupPending=false;m_blocked=errorText(e);report(e);}
}
void LocalConnection::configureCommands() {
    std::vector<CommandGroupSpec> groups;
    if(m_context && m_snapshot) {
        std::lock_guard lock(m_context->mutex);
        for(const auto &o:m_context->owned) {
            if(!o.plan.values || !o.keeper || o.generation!=m_snapshot->generation) {continue;}
            CommandGroupSpec group;group.id=o.plan.id;
            for(const auto &ref:o.plan.refs) {group.inputs.push_back({ref,m_snapshot->types.at(ref.type).items.at(ref.item).descriptor});}
            groups.push_back(std::move(group));
        }
    }
    m_command.setSnapshot(m_snapshot,deviceId());
    m_command.configure(groups);
}
void LocalConnection::discardCommands(uint8_t type,uint8_t instance) {
    if(!m_context) {return;}
    std::lock_guard lock(m_context->mutex);
    for(const auto &o:m_context->owned) {
        if(!o.plan.values || !o.keeper) {continue;}
        if(std::any_of(o.plan.refs.begin(),o.plan.refs.end(),[&](const Ref &r){return r.type==type && r.instance==instance;})) {
            o.keeper->discardQueued();
        }
    }
}
std::vector<CommandPlan> LocalConnection::commandGroups() const {
    std::vector<CommandPlan> plans;
    if(!m_context) {return plans;}
    std::lock_guard lock(m_context->mutex);
    for(const auto &o:m_context->owned) {plans.push_back(o.plan);}
    return plans;
}
QString LocalConnection::commandBlocked() const {
    if(!ready()) {return tr("Connect and wait for the device");}
    if(m_agent && !m_lease.mine()) {return tr("Take the control lease to command inputs");}
    if(m_observingArmed && !m_explicitDisarmAcknowledged) {return tr("Another owner armed the device; press DISARM first");}
    if(!m_groupsReady) {return m_blocked.isEmpty()?tr("Preparing command groups"):m_blocked;}
    if(m_disarmPending || m_waitingDisarm) {return tr("Disarming");}
    if(m_arming || m_armPending) {return tr("Arming");}
    if(armState()==2) {return tr("A fault is latched; acknowledge it and arm again");}
    if(armState()!=1) {return tr("Arm to command inputs");}
    if(!m_ownArm) {return tr("Another client armed the device; press DISARM, then ARM here");}
    return {};
}
void LocalConnection::arm() {
    if(!armEnabled()) {return;}
    auto c=m_context;auto session=c->getSession();const auto fence=c->safety.load();const auto generation=m_snapshot->generation;
    const auto leaseEpoch=c->leaseEpoch.load();
    m_arming=true;m_armPending=true;m_armReplied=false;emit changed();const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([c,fence,generation,leaseEpoch](Client &client) {
            if(c->retired || c->safety!=fence || client.session().generation()!=generation) {throw Error(ErrorKind::Cancelled,"ARM cancelled by DISARM or connection change");}
            auto result=client.arm();
            // Fenced by a lost lease (remote): the agent disarms after it;
            // a DISARM of ours could reach the new holder's device.
            const bool leaseMoved=c->leaseEpoch!=leaseEpoch;
            if(c->retired || client.session().generation()!=generation || (c->safety!=fence && !leaseMoved)) {client.disarm();}
            return result;
        },[self,c,fence,leaseEpoch](std::expected<SystemResult,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_armPending=false;self->m_armReplied=bool(result);self->m_ownArm=result && c->safety==fence;
            if(self->m_ownArm) {++self->m_armEpoch;}
            // Fenced by DISARM or a reconnect: disarm once more. Fenced by
            // a lost lease: the agent already disarmed after the ARM, and a
            // DISARM of ours now could disarm the new holder.
            if(c->safety!=fence) {
                self->m_arming=false;
                if(!(self->m_agent && c->leaseEpoch!=leaseEpoch)) {self->disarm();}
            }
            else if(!result) {
                self->m_arming=false;
                const auto &e=result.error();
                if(self->m_agent && e.kind==ErrorKind::Rejected && e.result==uint8_t(defs::Result::Busy) &&
                   e.detail==uint8_t(defs::Detail::None)) {
                    // The agent is still clearing the previous holder's
                    // command group; ARM is not repeated automatically.
                    const auto why=tr("The previous holder's command group is still being cleared; press ARM again in a moment");
                    emit self->notification(why,2);emit self->armRefused(why);
                } else {
                    self->report(e);
                    if(e.kind==ErrorKind::Rejected) {emit self->armRefused(errorText(e));}
                }
            }
            emit self->changed();
        });
    } catch(const Error &e) {m_armPending=false;m_arming=false;report(e);}
}
void LocalConnection::disarm() {
    auto c=m_context;if(!c) {return;}++c->safety;m_arming=false;m_waitingDisarm=true;
    // No manual value is queued after DISARM is requested, and none still
    // queued is sent (the firmware would discard it anyway).
    m_command.setBlocked(tr("Disarming"));m_ownArm=false;
    {
        std::lock_guard lock(c->mutex);
        for(const auto &o:c->owned) {if(o.plan.values && o.keeper) {o.keeper->discardQueued();}}
    }
    auto session=c->getSession();
    if(!session) {m_disarmWhenOpened=true;emit changed();return;}
    m_disarmWhenOpened=false;
    if(m_disarmPending) {m_disarmAgain=true;return;}
    m_disarmPending=true;emit changed();const QPointer<LocalConnection> self(this);
    const auto generation=session->generation();
    try {
        Client(*session).disarmAsync([self,c,generation](auto result) {
            if(!self || !self->current(c)) {return;}
            self->m_disarmPending=false;
            if(!result) {self->report(result.error());}
            else {self->m_explicitDisarmAcknowledged=generation==c->getSession()->generation();}
            // This is a later explicit user request, not an automatic retry
            // of the failed mutation. Never drop it on a timeout/refusal.
            if(self->m_disarmAgain) {self->m_disarmAgain=false;self->disarm();return;}
            if(!result) {if(self->m_closing) {self->finishClose();}return;}
            if(self->m_closing) {
                if(c->ownsAny() || self->m_setupPending || self->m_armPending || self->m_subsPending || self->m_livePending || self->m_monitorPending) {self->clearAndClose();}
                else {self->finishClose();}
            }
            else {self->refresh(c);}
            emit self->changed();
        });
    } catch(const Error &e) {m_disarmPending=false;report(e);if(m_closing) {finishClose();}}
}
void LocalConnection::acknowledge() {
    if(!ackEnabled()) {return;}
    auto c=m_context;auto session=c->getSession();m_ackPending=true;const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([](Client &client){return client.acknowledgeFault();},[self,c](std::expected<SystemResult,Error> result) {
            if(!self || !self->current(c)) {return;}self->m_ackPending=false;
            if(!result) {self->report(result.error());}emit self->changed();
        });
    } catch(const Error &e) {m_ackPending=false;report(e);}
}
void LocalConnection::disconnectDevice() {
    // Give plant identification time to stop the step and write back its mode.
    if(m_tune.identifying() || m_closing) {return;}
    // Disconnect also ends a lost link's overlay and any reboot follow-up.
    m_reconnecting=false;m_afterLoss=false;clearRebootExpectation();clearLinkTrialState();
    if(!m_context) {
        // Between reconnect attempts after an update's reboot: no link to
        // close, but the update must not wait for one any more.
        otaInterrupted(tr("Disconnected before the device came back."));
        m_linkLost=false;m_previous.reset();m_snapshot.reset();
        m_instances.reset({});m_loops.reset({});m_faults.reset({});
        m_monitor.reset(); // a lost link kept it for the reconnect
        m_command.reset();
        m_tune.reset();
        m_agent.reset();m_lease.clear();m_clients.clear();m_agentHistory.clear();m_historyEntries.clear();
        emit changed();emit disconnected();return;
    }
    m_closing=true;m_closeElapsed.start();emit changed();
    // An observer may have explicitly requested DISARM while opening. Await
    // that admitted request; closing must not turn it into a silent cancel.
    if(m_disarmPending) {return;}
    if((!m_agent || m_lease.mine()) && (m_context->owns() || m_setupPending || m_armPending)) {disarm();}
    // Loop statistics are released without a DISARM: they command nothing.
    else if(m_context->ownsAny() || m_subsPending || m_livePending || m_monitorPending) {clearAndClose();}
    else {finishClose();}
}
void LocalConnection::retry() {
    if(m_context || m_closing || m_endpoint.isEmpty()) {return;}
    const int baud=m_linkNotReachable?int(kBaudAuto):m_requestedBaud;
    m_linkNotReachable=false;
    m_afterLoss=true;open(m_endpoint,baud,m_lastBaud);
}
void LocalConnection::reconnectAfterReboot() {
    if(!m_reconnecting || m_context || m_closing) {return;}
    const int baud=m_linkExpectation && !m_agent?int(kBaudAuto):m_requestedBaud;
    open(m_endpoint,baud,m_lastBaud);
}
void LocalConnection::clearAndClose() {
    auto c=m_context;auto session=c->getSession();const QPointer<LocalConnection> self(this);
    try {
        session->async<OperationResult>([c,session] {Client client(*session);c->clearAll(client);return OperationResult{};},
            [self,c](auto result) {
                if(!self || !self->current(c)) {return;}
                if(!result) {self->report(result.error());}
                if(!self->m_disarmPending) {self->finishClose();}
            });
    } catch(const Error &e) {report(e);if(!m_disarmPending) {finishClose();}}
}
void LocalConnection::finishClose(bool lost) {
    const bool unsentDisarm=m_disarmWhenOpened;
    const bool unconfirmedDisarm=m_disarmPending;
    const bool actionInFlight=!m_actionRunning.isEmpty();
    // The agent's Closing event usually precedes the link's end by moments:
    // read its reason for the link-lost overlay before the session goes.
    if(m_agent && m_context) {
        if(auto s=m_context->getSession()) {
            Frame f;
            while(s->nextAgentEvent(f)) {
                const auto event=drivelink::agent::decodeEvent(f.payload);
                if(!event || event->event!=uint8_t(protocol::defs::AgentEvent::Closing)) {continue;}
                const auto reason=drivelink::agent::decodeClosing(event->body);
                m_agentClosing=reason && *reason==uint8_t(protocol::defs::AgentCloseReason::Shutdown)
                    ?tr("The agent is shutting down"):tr("The agent lost the device");
            }
        }
    }
    auto c=std::move(m_context);
    if(c) {
        c->retired=true;++c->safety;c->stopKeepers();
        if(auto s=c->getSession()) {s->close();}
    }
    if(m_opener.joinable()) {m_opener.join();}
    // close() has joined I/O and completed admitted requests; joining the
    // opener prevents further startup posts. Drain callback chains too. An
    // active callback can itself close, so exclude its delivery stack.
    while(m_dispatch->pending>m_dispatch->running) {
        QCoreApplication::sendPostedEvents(m_dispatch->receiver,QEvent::MetaCall);
        std::this_thread::yield();
    }
    dropLiveEdit();m_leasePending=false;m_clientsPending=false;m_clients.clear();
    m_loading=false;m_closing=false;m_groupsReady=false;m_setupPending=false;m_arming=false;m_armReplied=false;m_ownArm=false;
    m_armPending=false;m_disarmPending=false;m_disarmAgain=false;m_ackPending=false;m_waitingDisarm=false;
    m_disarmWhenOpened=false;m_subsPending=false;m_configAction.clear();m_loadStep=0;
    m_linkConfirmBusy=false;m_linkSettlePending=false;
    resetLive();m_livePending=false;m_actionRunning.clear();m_actionResults.clear();
    // A lost link keeps Monitor's plot set, as Layout keeps its selection.
    resetMonitorGroups();m_monitorPending=false;
    if(lost) {m_monitor.lose();} else {m_monitor.reset();}
    // A lost link keeps the card's inputs for the same device, like Monitor.
    if(lost) {m_command.clearGroups();} else {m_command.reset();}
    m_command.setBlocked(tr("Not connected"));
    // A lost link during plant identification may leave its mode set (unsaved).
    // The value it should have gone back to, e.g. "mode: velocity".
    QString modeBack;
    if(m_modeRestore) {modeBack=m_modeRestore->text;}
    // A queued write-back that is gone has ended (written back, or refused with its own toast).
    else if(m_tuneRun && m_tuneRun->identify && m_tuneRun->modeWritten && !m_tuneRun->restoreQueued && m_tuneRun->identify->mode &&
            m_tuneRun->identify->previous && m_snapshot) {
        const auto &id=*m_tuneRun->identify;
        modeBack=id.mode->label;
        for(const auto &choice:modeChoices(*m_snapshot,id.run.controller.key)) {
            if(choice.item==id.mode->item && choice.value==*id.previous) {modeBack=choice.label;}
        }
    }
    if(m_tuneRun) {endTuneRun(lost?tr("The link was lost during the run; nothing was recorded"):QString(),TuneEnd::DeviceGone);}
    m_tuneRelease.reset();m_tuneTimer.stop();
    if(!modeBack.isEmpty()) {
        dropModeRestore();
        emit notification(lost?tr("The link was lost before plant identification wrote back %1: check the mode after reconnecting (unsaved)").arg(modeBack)
                               :tr("Disconnected before plant identification wrote back %1: check the mode when you connect again (unsaved)").arg(modeBack),2);
    }
    m_tuneRefresh.stop();
    // Runs stay for the same device (H2e-2 saves them); the rest goes.
    m_tune.reset();
    if(lost && c) {
        // Remember the device for the reboot notice after a reconnect.
        std::optional<uint64_t> uptime;
        if(m_diag && m_diagElapsed.isValid()) {uptime=m_diag->uptime_us+uint64_t(m_diagElapsed.elapsed())*1000;}
        m_previous=Previous{deviceId(),deviceName(),unsaved(),uptime};m_lossElapsed.start();
        if(c->savedBeforeReboot) {m_savedBeforeReboot=true;forgetChanges(deviceId());}
        // A link that stalled could not release this GUI's statistics groups.
        std::lock_guard lock(c->mutex);
        m_staleSubs.clear();m_staleId=deviceId();
        for(const auto &o:c->subs) {m_staleSubs.push_back({o.id,o.loop,o.epoch,o.refs});}
    }
    m_linkLost=lost;
    if(!lost) {
        m_snapshot.reset();m_instances.reset({});m_loops.reset({});m_faults.reset({});
        m_edits.clear();m_selected.clear();rebuildLayout(true);
    }
    if(c && m_reconnecting) {
        // A reconnect attempt after a requested reboot failed: retry within
        // the window. Only the link is reopened; REBOOT is never repeated.
        const int window=m_linkExpectation?(m_linkWaitOverrideMs>0?m_linkWaitOverrideMs:m_linkWaitMs):m_reconnectWindowMs;
        const qint64 elapsed=m_linkExpectation && m_linkWaitElapsed.isValid()?m_linkWaitElapsed.elapsed():m_reconnectElapsed.elapsed();
        if(elapsed<window) {
            QTimer::singleShot(500,this,[this] {reconnectAfterReboot();});
        } else {
            const bool wasLink=m_linkExpectation.has_value();
            if(wasLink) {
                m_linkNotReachable=true;
                m_linkNotReachableOld=formatLink(m_linkExpectation->confirmed);
                m_linkNotReachableNew=formatLink(m_linkExpectation->trial);
                m_linkNotReachablePins=!m_linkExpectation->trial.samePins(m_linkExpectation->confirmed);
                m_linkExpectation.reset();
            }
            m_reconnecting=false;clearRebootExpectation();m_linkLost=true;
            otaInterrupted(tr("The device did not reconnect after its reboot."));
            report(Error(ErrorKind::Connection,wasLink?linkOutcomeText(LinkOutcome::NotReachable):"The device did not reconnect after its reboot"));
        }
    } else if(c && lost && m_expectReboot) {
        m_reconnecting=true;m_linkLost=false;m_reconnectElapsed.start();
        if(m_ota && m_ota->phase==OtaJob::Phase::Rebooting) {m_ota->phase=OtaJob::Phase::Reconnecting;}
        QTimer::singleShot(300,this,[this] {reconnectAfterReboot();});
    } else if(c && !lost && m_afterLoss) {
        // Retry failed: keep the link-lost overlay with its error.
        m_afterLoss=false;m_linkLost=true;
    }
    // An update's completions were drained above without a current context.
    if(c && !m_reconnecting && !m_expectReboot) {
        otaInterrupted(m_linkLost?tr("The link was lost."):tr("The connection closed."));
    } else if(c && !m_reconnecting) {
        otaInterrupted(tr("The connection closed."));
    }
    m_otaStatusPending=false;
    if(unsentDisarm) {report(Error(ErrorKind::Cancelled,"DISARM could not be sent before the connection closed"));}
    if(unconfirmedDisarm) {
        Error error(ErrorKind::Connection,"Connection closed before DISARM was confirmed");
        error.family=1;error.op=3;error.has_request_context=true;error.outcome_unknown=true;report(error);
    }
    // Remote mode ends with the connection, except while a reconnect or the
    // link-lost overlay (Retry) still needs the agent and its token.
    if(!m_linkLost && !m_reconnecting) {m_agent.reset();m_lease.clear();m_agentHistory.clear();m_historyEntries.clear();}
    else if(m_agent) {m_lease.clear();}
    // An action in flight may have run; its reply will never arrive.
    if(actionInFlight && c) {emit notification(tr("The connection closed while an action was running; it may have run. It was not retried."),2);}
    emit changed();emit liveChanged();if(c) {emit disconnected();}
}
namespace {
// A path or a local file:// URL; empty for any other URL.
QString panelFilePath(const QString &fileOrUrl) {
    if(!fileOrUrl.contains(QLatin1String("://"))) {return fileOrUrl;}
    const QUrl url(fileOrUrl);
    return url.isLocalFile()?url.toLocalFile():QString();
}
}
QVariantMap Connection::readTextFile(const QString &fileOrUrl) const {
    const auto fail=[](const QString &why) {return QVariantMap{{"ok",false},{"error",why}};};
    const auto path=panelFilePath(fileOrUrl);
    if(path.isEmpty()) {return fail(tr("Only local files can be read"));}
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) {return fail(tr("Cannot open %1: %2").arg(path,file.errorString()));}
    if(file.size()>kPanelFileBytes) {return fail(tr("The file is larger than %1 KiB").arg(kPanelFileBytes/1024));}
    const auto bytes=file.read(kPanelFileBytes+1);
    if(bytes.size()>kPanelFileBytes) {return fail(tr("The file is larger than %1 KiB").arg(kPanelFileBytes/1024));}
    QStringDecoder decode(QStringDecoder::Utf8);
    const QString text=decode(bytes);
    if(decode.hasError()) {return fail(tr("The file is not UTF-8 text"));}
    return {{"ok",true},{"text",text},{"path",path}};
}
QVariantMap Connection::writeTextFile(const QString &fileOrUrl,const QString &text) const {
    const auto fail=[](const QString &why) {return QVariantMap{{"ok",false},{"error",why}};};
    const auto path=panelFilePath(fileOrUrl);
    if(path.isEmpty()) {return fail(tr("Only local files can be written"));}
    const auto bytes=text.toUtf8();
    if(bytes.size()>kPanelFileBytes) {return fail(tr("The text is larger than %1 KiB").arg(kPanelFileBytes/1024));}
    // Written to a temporary file and renamed: never a half-written file.
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {
        return fail(tr("Cannot write %1: %2").arg(path,file.errorString()));
    }
    return {{"ok",true},{"path",path}};
}
QString Connection::nameProblem(const QString &name) const {
    const auto bytes=name.toUtf8();
    if(bytes.size()>16) {return tr("Use at most 16 bytes (%1 now)").arg(bytes.size());}
    for(const auto ch:name.toUcs4()) {
        if(ch<0x20 || (ch>=0x7F && ch<=0x9F)) {return tr("Control characters are not allowed");}
    }
    return {};
}
QString LocalConnection::deviceName() const {
    if(m_snapshot) {return QString::fromStdString(m_snapshot->info.name);}
    return m_previous?m_previous->name:QString();
}
QString LocalConnection::deviceId() const {
    if(m_snapshot) {return QString::fromStdString(hex(m_snapshot->info.device_id));}
    return m_previous?m_previous->id:QString();
}
QVariantMap LocalConnection::info() const {
    if(!m_snapshot) {return {};}
    const auto &i=m_snapshot->info;
    QVariantMap map{{"name",QString::fromStdString(i.name)},{"id",QString::fromStdString(hex(i.device_id))},
        {"firmware",QString::fromStdString(i.firmware_version)},{"buildId",QString::fromStdString(i.build_id)},
        {"protocol",i.protocol_version},{"hostProtocol",protocol::core_wire::kVersion},
        {"descriptorHash",QStringLiteral("%1").arg(i.descriptor_hash,8,16,QLatin1Char('0'))},
        {"captureCapacity",i.capture_capacity},{"maxPayload",i.max_payload},{"runningInstances",i.running_instances},
        {"layoutInvalid",(i.flags&2)!=0},{"configLoaded",(i.flags&4)!=0}};
    if(m_diag) {
        map["resetReason"]=resetReasonText(m_diag->reset_reason);
        map["instancesOk"]=m_diag->instances_ok;map["instancesFailed"]=m_diag->instances_failed;
    }
    return map;
}
QVariantList LocalConnection::limits() const {
    QVariantList rows;
    if(!m_snapshot) {return rows;}
    const auto type=m_snapshot->types.find(defs::kSystemTypeId);
    if(type==m_snapshot->types.end()) {return rows;}
    for(const auto &[id,item]:type->second.items) {
        const auto &d=item.descriptor;
        if(d.kind!=ItemKind::Param || !d.name.starts_with("limits.")) {continue;}
        const auto value=m_snapshot->system_params.find(id);
        if(value==m_snapshot->system_params.end()) {continue;}
        rows.push_back(QVariantMap{{"name",QString::fromStdString(d.name.substr(7))},{"value",valueBits(value->second)},
            {"unit",QString::fromStdString(d.unit)},{"description",QString::fromStdString(d.description)}});
    }
    return rows;
}
QVariantList LocalConnection::cpuLoad() const {
    QVariantList cores;
    if(!m_snapshot || !m_snapshot->status) {return cores;}
    for(const auto load:m_snapshot->status->cpu_load) {cores.push_back(load==0xFF?-1:int(load));}
    return cores;
}
int LocalConnection::activeFaults() const {
    // The device's own instance-aware count, as STATUS reports it.
    if(m_snapshot && m_snapshot->status) {return m_snapshot->status->active_errors;}
    return m_faults.activeCount();
}
int LocalConnection::faultHistory() const {return m_faults.rowCount();}
QStringList LocalConnection::unsavedChanges() const {return m_changes.value(deviceId());}
bool LocalConnection::ackEnabled() const {return ackBlockedReason().isEmpty();}
QString LocalConnection::ackBlockedReason() const {
    if(!ready()) {return tr("Wait for current device metadata and status");}
    if(armState()!=2) {return tr("No fault is latched");}
    if(m_ackPending) {return tr("Waiting for the acknowledgement");}
    // The device's own count gates the local check, so a missed Cleared
    // event cannot keep ACK disabled.
    if(!m_snapshot->status || m_snapshot->status->active_errors==0) {return {};}
    for(int i=0;i<m_faults.rowCount();++i) {
        const auto row=m_faults.index(i);
        if(row.data(FaultList::Active).toBool() && row.data(FaultList::Disarms).toBool()) {
            return tr("The fault condition is still active: %1").arg(row.data(FaultList::Name).toString());
        }
    }
    return {};
}
QVariantList LocalConnection::armChecks() const {
    auto check=[](const QString &label,const QString &state,const QString &detail={}) {
        return QVariantMap{{"label",label},{"state",state},{"detail",detail}};
    };
    QVariantList checks;
    const bool current=ready() && m_snapshot->status;
    checks<<check(tr("Device metadata and status are current"),current?"pass":"pending");
    if(!current) {return checks;}
    const auto flags=m_snapshot->status->flags;
    checks<<check(tr("Not in safe mode"),safeMode()?"fail":"pass",safeMode()?tr("Exit safe mode on the Device page"):QString());
    checks<<check(tr("No fault latched"),(flags&1)?"fail":"pass",(flags&1)?tr("Acknowledge the fault"):QString());
    checks<<check(tr("No active condition blocks arming"),(flags&2)?"fail":"pass",(flags&2)?tr("See the faults drawer"):QString());
    checks<<check(tr("No reboot pending"),(flags&4)?"fail":"pass",(flags&4)?tr("Save and reboot, or Discard all on Layout"):QString());
    if(linkTrialActive()) {
        checks<<check(tr("No serial link trial active"),"fail",tr("Confirm the serial link on the Device page, or wait for it to revert"));
    }
    if(m_agent) {
        checks<<check(tr("This window holds the control lease"),m_lease.mine()?"pass":"fail",
                      m_lease.mine()?QString():tr("Take the lease on the Device page (Clients and lease)"));
    }
    const bool gated=m_observingArmed && !m_explicitDisarmAcknowledged;
    checks<<check(tr("This window owns command keepalives"),gated?"fail":"pass",gated?tr("Another owner armed the device; press DISARM first"):QString());
    checks<<check(tr("Command keepalives are ready"),m_groupsReady?"pass":(m_blocked.isEmpty()?"pending":"fail"),m_blocked);
    return checks;
}
QString LocalConnection::commonBlock() const {
    if(!ready()) {return tr("Wait for the device to finish loading");}
    if(m_ota && m_ota->active()) {return tr("A firmware update is in progress");}
    if(!m_configAction.isEmpty()) {return tr("Wait for the current configuration action to finish");}
    return {};
}
QVariantMap LocalConnection::actionBlocked() const {
    // Plant identification writes its mode back itself: no SAVE (it would
    // keep the open-loop mode), REVERT, reboot or rename before that.
    const QString identifying=(m_tuneRun && m_tuneRun->identify) || m_modeRestore
        ?tr("Wait for plant identification to write the mode back"):QString();
    const auto exclusive=exclusiveBlocked();
    const auto common=!commonBlock().isEmpty()?commonBlock():!identifying.isEmpty()?identifying:exclusive;
    auto pick=[&](const QString &why) {return common.isEmpty()?why:common;};
    // The firmware refuses these while armed (LockedWhileArmed).
    const QString armed=armState()==1 || m_arming?tr("Not allowed while armed; disarm first"):QString();
    const QString nothing=!unsaved()?tr("There are no unsaved changes"):QString();
    // A requested reboot is in progress: no second reboot request.
    // REBOOT commits a prepared image, which would then run unchecked.
    const QString prepared=m_otaStatus && m_otaStatus->state==2 && m_otaStatus->boot_slot==m_otaStatus->running_slot
        ?tr("A prepared firmware update would be installed unchecked; abort it in the Firmware card first")
        :!m_otaStatus && m_otaStatusFailed?tr("The firmware update state could not be read; a prepared image could be installed unchecked"):QString();
    const QString rebooting=m_expectReboot?tr("The device is rebooting"):!armed.isEmpty()?armed:prepared;
    return {{"rename",pick({})},{"save",pick(armed.isEmpty()?nothing:armed)},{"revert",pick(armed.isEmpty()?nothing:armed)},
        {"reboot",pick(rebooting)},{"saveAndReboot",pick(rebooting)},
        {"factoryReset",pick(m_expectReboot?tr("The device is rebooting"):armed)},
        {"exitSafeMode",pick(!safeMode()?tr("The device is not in safe mode"):m_expectReboot?tr("The device is rebooting"):armed)},
        {"update",updateBlocked()}};
}
void LocalConnection::rename(const QString &name) {
    if(const auto problem=nameProblem(name);!problem.isEmpty()) {report(Error(ErrorKind::Usage,problem.toStdString()));return;}
    runOp(Op::Rename,name);
}
void LocalConnection::save() {runOp(Op::Save);}
void LocalConnection::revert() {runOp(Op::Revert);}
void LocalConnection::reboot() {runOp(Op::Reboot);}
void LocalConnection::saveAndReboot() {runOp(Op::SaveAndReboot);}
void LocalConnection::factoryReset() {runOp(Op::FactoryReset);}
void LocalConnection::exitSafeMode() {runOp(Op::ExitSafeMode);}
void LocalConnection::runOp(Op op,const QString &name) {
    static const char *actions[]={"rename","save","revert","reboot","saveAndReboot","factoryReset","exitSafeMode"};
    static const char *causes[]={"","","",QT_TR_NOOP("Reboot"),QT_TR_NOOP("Save and reboot"),QT_TR_NOOP("Factory reset"),QT_TR_NOOP("Exit safe mode")};
    const QString action=QString::fromLatin1(actions[int(op)]);
    if(const auto blocked=actionBlocked().value(action).toString();!blocked.isEmpty()) {
        report(Error(ErrorKind::Usage,blocked.toStdString()));return;
    }
    const bool reboots=op==Op::Reboot || op==Op::SaveAndReboot || op==Op::FactoryReset || op==Op::ExitSafeMode;
    auto c=m_context;auto session=c->getSession();const QPointer<LocalConnection> self(this);
    const auto id=deviceId();const auto oldName=deviceName();const auto utf8=name.toStdString();
    m_configAction=action;
    // Intent precedes sending: a link that drops after a reboot reply leads
    // to a reconnect, never to a repeated REBOOT.
    if(reboots) {
        m_expectReboot=true;m_rebootCause=tr(causes[int(op)]);
        m_linkExpectation.reset();m_linkOutcome.clear();m_linkNotReachable=false;
        if((op==Op::SaveAndReboot || (op==Op::Reboot && (!unsaved() || m_savedPending))) && m_snapshot) {
            if(const auto ls=snapshotLink(*m_snapshot);ls && ls->changed()) {
                if(auto exp=expectLinkTrial(*ls)) {
                    m_linkExpectation=*exp;
                    m_linkWaitMs=int(exp->window().count());
                    m_linkWaitElapsed.restart();
                    m_linkRebootSerial=m_connectionSerial;
                    m_linkRebootGeneration=m_snapshot->generation;
                }
            }
        }
    }
    emit changed();
    using defs::SystemOp;
    try {
        c->savedBeforeReboot=false;
        Client(*session).async([c,op,utf8](Client &client) {
            switch(op) {
            case Op::Rename:return client.setName(utf8);
            case Op::Save:return client.system(uint8_t(SystemOp::Save));
            case Op::Revert:return client.system(uint8_t(SystemOp::Revert));
            case Op::Reboot:return client.system(uint8_t(SystemOp::Reboot));
            case Op::SaveAndReboot:
                client.system(uint8_t(SystemOp::Save));
                c->savedBeforeReboot=true;
                return client.system(uint8_t(SystemOp::Reboot));
            case Op::FactoryReset:return client.system(uint8_t(SystemOp::FactoryReset));
            case Op::ExitSafeMode:return client.system(uint8_t(SystemOp::ExitSafeMode));
            }
            throw Error(ErrorKind::Usage,"unknown configuration action");
        },[self,c,op,reboots,id,oldName,name](std::expected<SystemResult,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_configAction.clear();
            if(!result) {
                const auto &e=result.error();
                // A refused or unsent request did not reboot; an unanswered one
                // may have, so the reconnect follow-up stays armed. A failed
                // SAVE before Save and reboot never sent REBOOT.
                const bool rebootSent=reboots && e.outcome_unknown &&
                    !(op==Op::SaveAndReboot && e.op==uint8_t(SystemOp::Save));
                // SAVE succeeded when only the REBOOT of Save and reboot failed.
                if(op==Op::SaveAndReboot && e.op==uint8_t(SystemOp::Reboot)) {self->forgetChanges(id);self->m_savedBeforeReboot=true;}
                if(reboots && !rebootSent) {self->clearRebootExpectation();self->m_linkExpectation.reset();}
                if(rebootSent) {self->m_rebootElapsed.start();}
                self->report(e);
                if(rebootSent) {
                    emit self->notification(tr("No reply to the reboot request; it is not repeated. Waiting for the device."),0);
                }
                emit self->changed();return;
            }
            const auto model=c->getModel();
            switch(op) {
            case Op::Rename:
                if(name!=oldName) {
                    self->m_changes[id]<<tr("Device name: \"%1\" to \"%2\"").arg(oldName.isEmpty()?id:oldName,name.isEmpty()?id:name);
                }
                if(model) {model->reloadAsync();}
                emit self->notification(tr("Device renamed"),1);
                break;
            case Op::Save:
                self->forgetChanges(id);emit self->notification(tr("Configuration saved"),1);break;
            case Op::Revert:
                // REVERT restores live params only; staged layout changes stay
                // pending and would still be lost at a reboot without SAVE.
                self->m_changedParams.remove(id);
                if(const auto staged=self->m_stagedLines.value(id);staged.isEmpty()) {self->m_changes.remove(id);}
                else {self->m_changes[id]=staged;}
                if(model) {model->reloadAsync();}
                emit self->notification(tr("Unsaved changes reverted"),1);break;
            case Op::SaveAndReboot:
                self->forgetChanges(id);self->m_savedBeforeReboot=true;[[fallthrough]];
            default:
                self->m_rebootElapsed.start();
                if(self->m_linkExpectation) {self->m_linkWaitElapsed.restart();}
                emit self->notification(tr("The device is rebooting"),0);
                // Direct serial baud change: the open session is still on the
                // old rate, so close it after a short grace and probe the new one.
                if(self->m_linkExpectation && !self->m_agent && self->m_endpoint.startsWith(QLatin1String("serial://")) &&
                   self->m_linkExpectation->trial.baud!=self->m_linkExpectation->confirmed.baud) {
                    const auto serial=self->m_connectionSerial;
                    QTimer::singleShot(250,self,[self,serial] {
                        if(self && self->m_connectionSerial==serial && self->m_expectReboot && self->m_linkExpectation) {
                            self->finishClose(true);
                        }
                    });
                }
                break;
            }
            // SAVE and REVERT clear the Unsaved marks of the param rows.
            self->updateParams();
            emit self->changed();
        });
    } catch(const Error &e) {
        m_configAction.clear();
        if(reboots) {clearRebootExpectation();m_linkExpectation.reset();}
        report(e);
    }
}
bool LocalConnection::linkTrialActive() const {
    return ready() && m_snapshot && m_snapshot->info.link_trial_ms!=0 &&
        m_snapshot->generation!=m_linkSettledGen;
}
bool LocalConnection::linkChangeStaged() const {
    if(!ready() || !m_snapshot) {return false;}
    const auto ls=snapshotLink(*m_snapshot);
    return ls && ls->changed();
}
void LocalConnection::clearLinkTrialState() {
    m_linkExpectation.reset();m_linkOutcome.clear();m_linkConfirmBusy=false;m_linkSettlePending=false;
    m_linkNotReachable=false;m_linkNotReachableOld.clear();m_linkNotReachableNew.clear();m_linkNotReachablePins=false;
    m_linkWaitMs=0;m_linkWaitElapsed.invalidate();m_linkTrialElapsed.invalidate();m_linkTrialBaseMs=0;m_linkTrialGen=0;m_linkSettledGen=0;
}
void LocalConnection::dismissLinkOutcome() {
    if(m_linkOutcome.isEmpty()) {return;}
    m_linkOutcome.clear();emit changed();
}
QVariantMap LocalConnection::linkTrial() const {
    QVariantMap out;
    const auto ls=ready() && m_snapshot?snapshotLink(*m_snapshot):std::nullopt;
    const bool waiting=m_linkExpectation.has_value();
    const bool active=linkTrialActive() && !waiting;
    const bool staged=!waiting && ls && ls->changed();
    out["staged"]=staged;
    out["waiting"]=waiting;
    out["active"]=active;
    out["confirmBusy"]=m_linkConfirmBusy;
    out["throughAgent"]=m_agent.has_value();
    out["agentAddress"]=m_agent?m_agent->address:QString();
    out["notReachable"]=m_linkNotReachable;
    out["notReachableOld"]=m_linkNotReachableOld;
    out["notReachableNew"]=m_linkNotReachableNew;
    out["notReachablePins"]=m_linkNotReachablePins;
    if(!m_linkOutcome.isEmpty()) {
        out["outcome"]=m_linkOutcome.value(QStringLiteral("outcome"));
        out["outcomeText"]=m_linkOutcome.value(QStringLiteral("text"));
        out["outcomeDetail"]=m_linkOutcome.value(QStringLiteral("detail"));
    }
    if(staged) {
        const bool pins=!ls->next.samePins(ls->running);
        out["pinsChanged"]=pins;
        out["trialSeconds"]=pins?120:30;
        out["runningText"]=formatLink(ls->running);
        out["nextText"]=formatLink(ls->next);
        out["runningBaud"]=int(ls->running.baud);
        out["nextBaud"]=int(ls->next.baud);
        out["runningTx"]=int(ls->running.tx_pin);
        out["runningRx"]=int(ls->running.rx_pin);
        out["nextTx"]=int(ls->next.tx_pin);
        out["nextRx"]=int(ls->next.rx_pin);
    } else if(waiting) {
        const bool pins=!m_linkExpectation->trial.samePins(m_linkExpectation->confirmed);
        const int window=m_linkWaitOverrideMs>0?m_linkWaitOverrideMs:m_linkWaitMs;
        const int elapsed=m_linkWaitElapsed.isValid()?int(m_linkWaitElapsed.elapsed()):0;
        const int remaining=std::max(0,window-elapsed);
        out["pinsChanged"]=pins;
        out["trialSeconds"]=pins?120:30;
        out["windowMs"]=window;
        out["remainingMs"]=remaining;
        out["progress"]=window>0?std::clamp(double(window-remaining)/double(window),0.0,1.0):0.0;
        out["oldText"]=formatLink(m_linkExpectation->confirmed);
        out["newText"]=formatLink(m_linkExpectation->trial);
        out["runningTx"]=int(m_linkExpectation->confirmed.tx_pin);
        out["runningRx"]=int(m_linkExpectation->confirmed.rx_pin);
        out["nextTx"]=int(m_linkExpectation->trial.tx_pin);
        out["nextRx"]=int(m_linkExpectation->trial.rx_pin);
    } else if(active && ls) {
        const bool pins=!ls->running.samePins(ls->next);
        const int elapsed=m_linkTrialElapsed.isValid()?int(m_linkTrialElapsed.elapsed()):0;
        const int remaining=std::max(0,int(m_linkTrialBaseMs)-elapsed);
        out["pinsChanged"]=pins;
        out["remainingMs"]=remaining;
        out["runningText"]=formatLink(ls->running);
        out["confirmedText"]=formatLink(ls->next);
        out["runningBaud"]=int(ls->running.baud);
        out["confirmBlocked"]=m_linkConfirmBusy?tr("Confirming link…"):exclusiveBlocked();
    }
    return out;
}
void LocalConnection::settlePendingLinkTrial() {
    if(!m_linkExpectation || m_linkSettlePending || !ready()) {return;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {return;}
    const auto expected=*m_linkExpectation;
    // Locally or when we hold (or nobody holds) the lease, settle confirms automatically.
    const bool confirm=!m_agent || m_lease.mine() || m_lease.holder().id==0;
    m_linkSettlePending=true;emit changed();
    const QPointer<LocalConnection> self(this);
    const auto serial=m_connectionSerial;
    try {
        Client(*session).async([expected,confirm](Client &client) {
            return settleLinkTrial(client,expected,confirm);
        },[self,c,expected,confirm,serial](std::expected<LinkTrialResult,Error> result) {
            if(!self || !self->current(c) || self->m_connectionSerial!=serial) {return;}
            self->m_linkSettlePending=false;
            if(!self->m_linkExpectation) {emit self->changed();return;}
            const int window=self->m_linkWaitOverrideMs>0?self->m_linkWaitOverrideMs:self->m_linkWaitMs;
            const bool withinWindow=!self->m_linkWaitElapsed.isValid() || self->m_linkWaitElapsed.elapsed()<window;
            auto reload=[&] {
                if(auto model=c->getModel()) {try {model->reloadAsync();} catch(const Error &) {}}
            };
            if(!result) {
                const auto &e=result.error();
                if((e.kind==ErrorKind::Timeout || e.kind==ErrorKind::Connection || e.boot_changed) && withinWindow) {
                    QTimer::singleShot(500,self,[self,serial] {
                        if(self && self->m_connectionSerial==serial && self->m_linkExpectation) {self->settlePendingLinkTrial();}
                    });
                    emit self->changed();return;
                }
                self->m_linkExpectation.reset();
                self->report(e);
                reload();emit self->changed();return;
            }
            const auto &r=*result;
            if(r.baud>0) {self->m_baud=int(r.baud);self->m_lastBaud=int(r.baud);}
            switch(r.outcome) {
            case LinkOutcome::Confirmed: {
                const auto running=formatLink(r.state?r.state->running:expected.trial);
                self->m_linkExpectation.reset();self->m_notice.clear();
                if(self->m_snapshot) {self->m_linkSettledGen=self->m_snapshot->generation;}
                self->m_linkOutcome={{"outcome",QStringLiteral("confirmed")},
                    {"text",tr("Serial link confirmed")},
                    {"detail",tr("Now running %1. Previous link was %2.").arg(running,formatLink(expected.confirmed))}};
                emit self->notification(tr("Serial link confirmed (%1)").arg(running),1);
                reload();
                break;
            }
            case LinkOutcome::Reverted: {
                const auto running=formatLink(r.state?r.state->running:expected.confirmed);
                self->m_linkExpectation.reset();self->m_notice.clear();
                self->m_linkOutcome={{"outcome",QStringLiteral("reverted")},
                    {"text",tr("Serial link reverted")},
                    {"detail",tr("Restored %1 (the trial of %2 was not confirmed in time).").arg(running,formatLink(expected.trial))}};
                emit self->notification(tr("Serial link reverted to %1").arg(running),2);
                reload();
                break;
            }
            case LinkOutcome::Deferred:
                self->m_linkExpectation.reset();self->m_notice.clear();
                self->m_linkOutcome={{"outcome",QStringLiteral("deferred")},
                    {"text",tr("Link change deferred (safe mode)")},
                    {"detail",QString::fromStdString(linkOutcomeText(LinkOutcome::Deferred))}};
                reload();
                break;
            case LinkOutcome::WrongDevice:
                self->m_linkExpectation.reset();
                self->m_linkOutcome={{"outcome",QStringLiteral("wrong_device")},
                    {"text",tr("Different device answered")},
                    {"detail",QString::fromStdString(linkOutcomeText(LinkOutcome::WrongDevice))}};
                break;
            case LinkOutcome::OtherLink:
                self->m_linkExpectation.reset();
                self->m_linkOutcome={{"outcome",QStringLiteral("other_link")},
                    {"text",tr("Link settings changed")},
                    {"detail",QString::fromStdString(linkOutcomeText(LinkOutcome::OtherLink))}};
                reload();
                break;
            case LinkOutcome::NotRebooted:
                if(self->m_linkWaitElapsed.isValid() && self->m_linkWaitElapsed.elapsed()<10000) {
                    QTimer::singleShot(500,self,[self,serial] {
                        if(self && self->m_connectionSerial==serial && self->m_linkExpectation) {self->settlePendingLinkTrial();}
                    });
                } else {
                    self->m_linkExpectation.reset();
                    self->m_linkOutcome={{"outcome",QStringLiteral("not_rebooted")},
                        {"text",tr("Device did not reboot into the new link")},
                        {"detail",QString::fromStdString(linkOutcomeText(LinkOutcome::NotRebooted))}};
                }
                break;
            case LinkOutcome::Unconfirmed:
                if(!confirm) {
                    self->m_linkExpectation.reset();
                    reload();
                } else if(withinWindow) {
                    // Lost confirm request, or expired trial waiting ~0.5 s to reboot and revert.
                    QTimer::singleShot(500,self,[self,serial] {
                        if(self && self->m_connectionSerial==serial && self->m_linkExpectation) {self->settlePendingLinkTrial();}
                    });
                } else {
                    self->m_linkExpectation.reset();
                    reload();
                }
                break;
            case LinkOutcome::NotReachable:
                self->m_linkExpectation.reset();
                break;
            }
            emit self->changed();
        });
    } catch(const Error &e) {
        m_linkSettlePending=false;m_linkExpectation.reset();report(e);emit changed();
    }
}
void LocalConnection::confirmLink() {
    if(!linkTrialActive() || m_linkConfirmBusy) {return;}
    if(const auto why=exclusiveBlocked();!why.isEmpty()) {
        report(Error(ErrorKind::Usage,why.toStdString()));return;
    }
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session || !m_snapshot) {return;}
    const auto id=m_snapshot->info.device_id;
    const auto before=snapshotLink(*m_snapshot);
    m_linkConfirmBusy=true;emit changed();
    const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([id](Client &client) {
            return confirmLinkTrial(client,id);
        },[self,c,before](std::expected<LinkConfirmation,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_linkConfirmBusy=false;
            auto reload=[&] {
                if(auto model=c->getModel()) {try {model->reloadAsync();} catch(const Error &) {}}
            };
            if(!result) {
                self->report(result.error());
                if(result.error().outcome_unknown) {reload();}
                emit self->changed();return;
            }
            switch(*result) {
            case LinkConfirmation::Confirmed:
                if(self->m_snapshot) {self->m_linkSettledGen=self->m_snapshot->generation;}
                self->m_linkOutcome={{"outcome",QStringLiteral("confirmed")},
                    {"text",tr("Serial link confirmed")},
                    {"detail",before?tr("Confirmed %1.").arg(formatLink(before->running)):tr("The running link settings are now saved.")}};
                emit self->notification(tr("Serial link confirmed"),1);
                reload();
                break;
            case LinkConfirmation::Reverted:
                self->m_linkOutcome={{"outcome",QStringLiteral("reverted")},
                    {"text",tr("Serial link reverted")},
                    {"detail",before?tr("Restored %1.").arg(formatLink(before->next)):tr("The trial ended and reverted to the previous link.")}};
                emit self->notification(tr("Serial link reverted"),2);
                reload();
                break;
            case LinkConfirmation::NoTrial:
                emit self->notification(tr("No active link trial to confirm; refreshing state"),0);
                reload();
                break;
            case LinkConfirmation::Unconfirmed:
                emit self->notification(tr("No reply confirmed the link; press Confirm link again"),2);
                break;
            case LinkConfirmation::WrongDevice:
                self->report(Error(ErrorKind::Protocol,linkOutcomeText(LinkOutcome::WrongDevice)));
                break;
            }
            emit self->changed();
        });
    } catch(const Error &e) {
        m_linkConfirmBusy=false;report(e);emit changed();
    }
}
QVariantList LocalConnection::serialPorts() {
    QString active;
    if(m_context && m_endpoint.startsWith("serial://")) {active=m_endpoint;}
    return portRows(listSerialPorts(),m_recent,active);
}
void LocalConnection::forgetRecent(const QString &id) {m_recent.forget(id);emit recentDevicesChanged();}
void LocalConnection::selectRows(DescriptorRows &rows,uint8_t type,uint8_t instance,bool pending) const {
    rows.reset(m_snapshot,type,instance,pending);
}
} // namespace drivelink::gui
