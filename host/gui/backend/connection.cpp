#include "connection_internal.hpp"
#include <drivelink/buffers.hpp>
#include <drivelink/command.hpp>
#include <drivelink/protocol/core_codec.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QCoreApplication>
#include <QPointer>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
}

LocalConnection::LocalConnection(QObject *parent):Connection(parent),
    // Application ownership matters at Qt shutdown: deleting a receiver from
    // the destruction of one of its own posted events re-enters Qt's queue
    // mutex. The app deletes this receiver after its posted-event cleanup.
    m_dispatch(std::make_shared<GuiDispatch>()),m_instances(this),m_loops(this),m_faults(this) {
    m_timer.setInterval(100);
    connect(&m_timer,&QTimer::timeout,this,[this] {
        if(m_closing) {
            if(m_closeElapsed.elapsed()>3500) {finishClose();}
            return;
        }
        // A reboot that never came (refused drain, lost reply) ends the
        // expectation, so a later loss is again an unrequested one.
        if(m_expectReboot && !m_reconnecting && m_rebootElapsed.isValid() && m_rebootElapsed.elapsed()>m_reconnectWindowMs) {
            clearRebootExpectation();
            if(m_ota && (m_ota->phase==OtaJob::Phase::Rebooting || m_ota->phase==OtaJob::Phase::Reconnecting)) {
                otaStop(OtaJob::Phase::Failed,m_ota->rollback?tr("The device did not reboot; the reboot was not repeated and the build stays unconfirmed.")
                    :tr("The device did not reboot; the reboot was not repeated. The prepared image is still waiting: abort it in the Firmware card."));
            }
            emit notification(tr("The device did not reboot; it was not asked again"),2);emit changed();
        }
        // While a requested reboot is under way on a link that stays open,
        // wait for Boot instead of reopening the port (opening a tty can
        // pulse an ESP auto-reset line on Linux).
        // Only after the REBOOT reply (or its uncertain outcome) and while
        // disarmed; any other silence is a link loss after 3 s.
        const bool awaitingBoot=m_expectReboot && m_rebootElapsed.isValid() && armState()==0;
        const qint64 silence=awaitingBoot?m_reconnectWindowMs:3000;
        if(m_context && m_snapshot && !m_loading && m_seen.isValid() && m_seen.elapsed()>silence) {
            if(!m_expectReboot) {report(Error(ErrorKind::Connection,"Device stopped sending status"));}
            finishClose(true);return;
        }
        if(++m_pollTick%3==0) {pollLoopStats();}
    });
    m_timer.start();
}
LocalConnection::~LocalConnection() {finishClose();}
bool LocalConnection::current(const std::shared_ptr<Context> &c) const {return m_context==c && !c->retired;}
bool LocalConnection::linked() const {return m_context && m_context->getSession() && !m_linkLost && (!m_snapshot || m_snapshot->connected);}
bool LocalConnection::ready() const {
    return linked() && m_snapshot && m_snapshot->ready && !m_loading && !m_closing &&
        m_context->getSession()->generation()==m_snapshot->generation;
}
int LocalConnection::armState() const {return m_snapshot && m_snapshot->status?m_snapshot->status->arm_state:0;}
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
    return ready() && m_groupsReady && !m_arming && !m_armPending && !m_disarmPending && !m_waitingDisarm && !m_expectReboot &&
        !(m_ota && m_ota->active()) &&
        (!m_observingArmed || m_explicitDisarmAcknowledged) &&
        !safeMode() && m_snapshot->status && armState()==0 && !(m_snapshot->status->flags&2);
}
QString LocalConnection::armBlockedReason() const {
    if(!ready()) {return tr("Wait for current device metadata and status");}
    if(safeMode()) {return tr("Device is in safe mode");}
    if(m_expectReboot) {return tr("The device is rebooting");}
    if(m_ota && m_ota->active()) {return tr("A firmware update is in progress");}
    if(m_observingArmed && !m_explicitDisarmAcknowledged) {return tr("Press DISARM before taking command ownership");}
    if(m_arming || m_armPending || m_disarmPending || m_waitingDisarm) {return tr("Waiting for arm/disarm confirmation");}
    if(!m_groupsReady) {return m_blocked.isEmpty()?tr("Preparing command keepalives"):m_blocked;}
    if(!m_snapshot->status) {return tr("Waiting for device status");}
    if(m_snapshot->status->flags&2) {return tr("Device reports an ARM blocking condition");}
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
void LocalConnection::connectTo(const QString &endpoint,int baud) {
    if(m_context || m_closing || endpoint.trimmed().isEmpty() || baud<=0) {return;}
    // A new connection by the user ends any reboot follow-up of the last one.
    if(!m_reconnecting && !m_afterLoss) {
        clearRebootExpectation();m_previous.reset();m_notice.clear();m_staleSubs.clear();
        if(m_ota && m_ota->terminal()) {m_ota.reset();}
    }
    ++m_connectionSerial;m_otaStatus.reset();m_otaStatusPending=false;m_otaStatusFailed=false;
    auto c=std::make_shared<Context>();m_context=c;m_endpoint=endpoint.trimmed();m_baud=baud;m_loading=true;m_linkLost=false;
    m_error.clear();m_mismatch.clear();m_blocked.clear();m_subsNote.clear();m_snapshot.reset();m_statusCounter.reset();
    m_setupAttempted=false;m_subsAttempted=false;m_recorded=false;m_diag.reset();m_loadStep=0;
    m_observingArmed=false;m_explicitDisarmAcknowledged=false;
    m_disarmWhenOpened=false;
    m_loops.reset({});m_faults.reset({});
    emit changed();
    const auto queue=m_dispatch;const QPointer<LocalConnection> self(this);const auto address=m_endpoint.toStdString();
    // Start after this GUI turn. Safety requests during opening are retained
    // until publication, including an Esc immediately after Connect.
    QTimer::singleShot(0,this,[this,c,queue,self,address,baud] {
    if(!current(c)) {return;}
    m_opener=std::thread([c,queue,self,address,baud] {
        try {
            SessionOptions options;options.dispatcher=[queue](Task task){post(queue,std::move(task));};
            options.baud=unsigned(baud);
            auto session=std::make_shared<Session>(address,options);
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
            {std::lock_guard lock(c->mutex);c->session=session;}
            if(c->retired) {session->close();return;}
            post(queue,[self,c] {
                if(!self || !self->current(c)) {return;}
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
                if(error.peer_version) {
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
        c->stopKeepers();{std::lock_guard lock(c->mutex);c->owned.clear();c->subs.clear();}
        m_groupsReady=false;m_setupAttempted=false;m_subsAttempted=false;m_statusCounter.reset();++c->safety;
        m_arming=false;m_waitingDisarm=false;
        if(m_snapshot) {m_explicitDisarmAcknowledged=false;}
    }
    m_snapshot=std::move(snapshot);
    if(!m_snapshot->connected) {finishClose(true);return;}
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
        if(armState()==1) {m_arming=false;}
        if(armState()==0) {m_waitingDisarm=false;}
        m_loops.setStatus(&*m_snapshot->status);
    }
    if(ready() && (!m_recorded || m_recordedName!=deviceName())) {
        const bool first=!m_recorded;m_recorded=true;m_recordedName=deviceName();
        m_recent.remember(deviceId(),deviceName(),m_endpoint,m_baud,QString::fromStdString(m_snapshot->info.firmware_version));
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
    if(!m_groupsReady && !m_setupPending && !c->owns() &&
       (c->armedSeen==m_snapshot->generation || (m_snapshot->status && armState()==1))) {
        m_observingArmed=true;
    }
    if(ready() && m_snapshot->status && armState()==0 && !safeMode() && !m_setupPending &&
       (!m_observingArmed || m_explicitDisarmAcknowledged) &&
       (!m_setupAttempted || m_setupGeneration!=m_snapshot->generation)) {setupGroups();}
    // Loop statistics follow command setup so the two never interleave
    // group mutations; an observer (no command setup) starts them at once.
    const bool commandsSettled=!m_setupPending &&
        ((m_setupAttempted && m_setupGeneration==m_snapshot->generation) || m_observingArmed || armState()!=0);
    if(ready() && m_snapshot->status && !safeMode() && !m_subsPending && commandsSettled &&
       (!m_subsAttempted || m_subsGeneration!=m_snapshot->generation)) {setupLoopStats();}
    otaRefresh();
    emit changed();
}
void LocalConnection::showRebootNotice(std::optional<uint8_t> reason,bool dirty,const QString &id) {
    QStringList lost;
    if(dirty) {
        lost=m_changes.value(id);
        if(lost.isEmpty()) {lost<<tr("Unsaved changes made outside this window");}
    }
    // STATUS lags the SAVE reply, so a save just before the reboot wins.
    if(m_savedBeforeReboot) {lost.clear();}
    m_changes.remove(id);
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
                            std::erase_if(c->subs,[&](const auto &o){return o.generation==snapshot->generation && o.id==id;});
                        }
                        throw;
                    }
                }
                if(configured.generation!=snapshot->generation) {throw Error(ErrorKind::Cancelled,"Device rebooted during loop statistics setup");}
                auto buffer=session->subscribeBuffer(id,configured.epoch,refs.size(),8,configured.generation);
                {
                    std::lock_guard lock(c->mutex);
                    const auto owned=std::find_if(c->subs.begin(),c->subs.end(),[&](const auto &o){return o.generation==snapshot->generation && o.id==id;});
                    if(owned==c->subs.end() || c->retired) {throw Error(ErrorKind::Cancelled,"Loop statistics ownership was invalidated");}
                    owned->epoch=configured.epoch;owned->buffer=std::move(buffer);
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
            // Run time is informative: a setup failure leaves the column empty
            // with its reason and never blocks the connection.
            if(!result) {
                self->m_subsNote=errorText(result.error());
                std::vector<uint8_t> owned;
                {
                    std::lock_guard lock(c->mutex);
                    for(const auto &o:c->subs) {if(o.buffer) {owned.push_back(o.loop);}}
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
        for(const auto &o:m_context->subs) {if(o.buffer) {buffers.emplace_back(o.loop,o.buffer);}}
    }
    for(const auto &[loop,buffer]:buffers) {
        GroupSample sample;
        if(buffer->latest(sample) && sample.count>=2) {
            m_loops.setRunTime(loop,LoopRunTime{sample.values[0],sample.values[1],sample.counter});
        }
    }
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
    for(const auto &o:m_context->subs) {ids.push_back(o.id);}
    return ids;
}
void LocalConnection::setupGroups() {
    auto c=m_context;auto session=c->getSession();const auto snapshot=m_snapshot;
    m_setupPending=true;m_setupAttempted=true;m_setupGeneration=snapshot->generation;
    m_blocked.clear();const QPointer<LocalConnection> self(this);
    try {
        session->async<OperationResult>([c,session,snapshot] {
            Client client(*session);
            const auto groups=client.groups();const auto plans=planCommands(*snapshot,groups);
            const auto watchdog=valueBits(snapshot->system_params.at(defs::system::kWatchdogMs));
            if(watchdog<3 || watchdog>65535) {throw Error(ErrorKind::Usage,"Invalid command watchdog");}
            try {
                for(const auto &plan:plans) {
                    if(c->retired || session->generation()!=snapshot->generation) {throw Error(ErrorKind::Cancelled,"Connection changed during command setup");}
                    const auto before=std::find_if(groups.groups.begin(),groups.groups.end(),[&](const auto &g){return g.kind==GroupKind::Command && g.id==plan.id;});
                    Owned owned{plan,uint8_t(before->epoch+1),snapshot->generation,{}};
                    // Record intent before a sent mutation can time out. Cleanup
                    // reconciles exact configuration, even without a reply.
                    {std::lock_guard lock(c->mutex);c->owned.push_back(owned);}
                    GroupMutation configured;
                    try {configured=client.cmdConfigure(plan.id,plan.loop,plan.refs);}
                    catch(const Error &e) {
                        if(!e.outcome_unknown) {
                            std::lock_guard lock(c->mutex);
                            std::erase_if(c->owned,[&](const auto &o){return o.generation==snapshot->generation && o.plan.id==plan.id;});
                        }
                        throw;
                    }
                    if(configured.generation!=snapshot->generation) {throw Error(ErrorKind::Cancelled,"Device rebooted during command setup");}
                    // Empty descriptors permit only prefix writes on this handle;
                    // input defaults never enter the producer queue.
                    auto keeper=session->manageCommand(plan.id,configured.epoch,{}, {},uint16_t(watchdog),configured.generation);
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
        },[self,c](auto result) {
            if(!self || !self->current(c)) {return;}
            self->m_setupPending=false;
            if(self->m_snapshot && self->m_snapshot->generation!=self->m_setupGeneration) {self->refresh(c);return;}
            self->m_groupsReady=bool(result);
            if(result) {self->m_observingArmed=false;}
            if(!result) {self->m_blocked=errorText(result.error());self->report(result.error());}
            emit self->changed();
        });
    } catch(const Error &e) {m_setupPending=false;m_blocked=errorText(e);report(e);}
}
void LocalConnection::arm() {
    if(!armEnabled()) {return;}
    auto c=m_context;auto session=c->getSession();const auto fence=c->safety.load();const auto generation=m_snapshot->generation;
    m_arming=true;m_armPending=true;emit changed();const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([c,fence,generation](Client &client) {
            if(c->retired || c->safety!=fence || client.session().generation()!=generation) {throw Error(ErrorKind::Cancelled,"ARM cancelled by DISARM or connection change");}
            auto result=client.arm();
            if(c->retired || c->safety!=fence || client.session().generation()!=generation) {client.disarm();}
            return result;
        },[self,c,fence](std::expected<SystemResult,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_armPending=false;
            if(c->safety!=fence) {self->m_arming=false;self->disarm();}
            else if(!result) {
                self->m_arming=false;self->report(result.error());
                if(result.error().kind==ErrorKind::Rejected) {emit self->armRefused(errorText(result.error()));}
            }
            emit self->changed();
        });
    } catch(const Error &e) {m_armPending=false;m_arming=false;report(e);}
}
void LocalConnection::disarm() {
    auto c=m_context;if(!c) {return;}++c->safety;m_arming=false;m_waitingDisarm=true;
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
                if(c->ownsAny() || self->m_setupPending || self->m_armPending || self->m_subsPending) {self->clearAndClose();}
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
    // Disconnect also ends a lost link's overlay and any reboot follow-up.
    m_reconnecting=false;m_afterLoss=false;clearRebootExpectation();
    if(!m_context) {
        // Between reconnect attempts after an update's reboot: no link to
        // close, but the update must not wait for one any more.
        otaInterrupted(tr("Disconnected before the device came back."));
        m_linkLost=false;m_previous.reset();m_snapshot.reset();
        m_instances.reset({});m_loops.reset({});m_faults.reset({});
        emit changed();emit disconnected();return;
    }
    if(m_closing) {return;}
    m_closing=true;m_closeElapsed.start();emit changed();
    // An observer may have explicitly requested DISARM while opening. Await
    // that admitted request; closing must not turn it into a silent cancel.
    if(m_disarmPending) {return;}
    if(m_context->owns() || m_setupPending || m_armPending) {disarm();}
    // Loop statistics are released without a DISARM: they command nothing.
    else if(m_context->ownsAny() || m_subsPending) {clearAndClose();}
    else {finishClose();}
}
void LocalConnection::retry() {
    if(m_context || m_closing || m_endpoint.isEmpty()) {return;}
    m_afterLoss=true;connectTo(m_endpoint,m_baud);
}
void LocalConnection::reconnectAfterReboot() {
    if(!m_reconnecting || m_context || m_closing) {return;}
    connectTo(m_endpoint,m_baud);
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
    m_loading=false;m_closing=false;m_groupsReady=false;m_setupPending=false;m_arming=false;
    m_armPending=false;m_disarmPending=false;m_disarmAgain=false;m_ackPending=false;m_waitingDisarm=false;
    m_disarmWhenOpened=false;m_subsPending=false;m_configAction.clear();m_loadStep=0;
    if(lost && c) {
        // Remember the device for the reboot notice after a reconnect.
        std::optional<uint64_t> uptime;
        if(m_diag && m_diagElapsed.isValid()) {uptime=m_diag->uptime_us+uint64_t(m_diagElapsed.elapsed())*1000;}
        m_previous=Previous{deviceId(),deviceName(),unsaved(),uptime};m_lossElapsed.start();
        if(c->savedBeforeReboot) {m_savedBeforeReboot=true;m_changes.remove(deviceId());}
        // A link that stalled could not release this GUI's statistics groups.
        std::lock_guard lock(c->mutex);
        m_staleSubs.clear();m_staleId=deviceId();
        for(const auto &o:c->subs) {m_staleSubs.push_back({o.id,o.loop,o.epoch,o.refs});}
    }
    m_linkLost=lost;
    if(!lost) {m_snapshot.reset();m_instances.reset({});m_loops.reset({});m_faults.reset({});}
    if(c && m_reconnecting) {
        // A reconnect attempt after a requested reboot failed: retry within
        // the window. Only the link is reopened; REBOOT is never repeated.
        if(m_reconnectElapsed.elapsed()<m_reconnectWindowMs) {
            QTimer::singleShot(500,this,[this] {reconnectAfterReboot();});
        } else {
            m_reconnecting=false;clearRebootExpectation();m_linkLost=true;
            otaInterrupted(tr("The device did not reconnect after its reboot."));
            report(Error(ErrorKind::Connection,"The device did not reconnect after its reboot"));
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
    emit changed();if(c) {emit disconnected();}
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
    const auto common=commonBlock();
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
    if(reboots) {m_expectReboot=true;m_rebootCause=tr(causes[int(op)]);}
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
                if(op==Op::SaveAndReboot && e.op==uint8_t(SystemOp::Reboot)) {self->m_changes.remove(id);self->m_savedBeforeReboot=true;}
                if(reboots && !rebootSent) {self->clearRebootExpectation();}
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
                self->m_changes.remove(id);emit self->notification(tr("Configuration saved"),1);break;
            case Op::Revert:
                self->m_changes.remove(id);if(model) {model->reloadAsync();}
                emit self->notification(tr("Unsaved changes reverted"),1);break;
            case Op::SaveAndReboot:
                self->m_changes.remove(id);self->m_savedBeforeReboot=true;[[fallthrough]];
            default:
                self->m_rebootElapsed.start();
                emit self->notification(tr("The device is rebooting"),0);break;
            }
            emit self->changed();
        });
    } catch(const Error &e) {
        m_configAction.clear();
        if(reboots) {clearRebootExpectation();}
        report(e);
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
