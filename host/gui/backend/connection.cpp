#include "connection.hpp"
#include <drivelink/command.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QCoreApplication>
#include <QPointer>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace drivelink::gui {
struct GuiDispatch {
    QObject *receiver=new QObject(qApp);
    std::atomic<size_t> pending{0};
    size_t running=0; // GUI thread only, including nested close-time delivery.
};
namespace {
namespace defs=protocol::defs;
// Each queued functor retains its target until consumed, including after close.
// Session bounds outstanding requests and event deliveries before this queue.
void post(const std::shared_ptr<GuiDispatch> &queue,Task task) {
    ++queue->pending;
    QMetaObject::invokeMethod(queue->receiver,[queue,task=std::move(task)]() mutable {
        struct Delivery {
            std::shared_ptr<GuiDispatch> queue;
            ~Delivery() {--queue->running;--queue->pending;}
        } delivery{queue};
        ++queue->running;task();
    },Qt::QueuedConnection);
}
struct Owned {
    CommandPlan plan;uint8_t epoch;uint64_t generation;
    std::shared_ptr<CommandGroup> keeper;
};
bool matches(const Owned &o,const Group &g) {
    return g.kind==GroupKind::Command && g.id==o.plan.id && g.configured &&
        g.loop==o.plan.loop && g.epoch==o.epoch && g.refs==o.plan.refs;
}
}
struct LocalConnection::Context {
    std::mutex mutex;
    std::shared_ptr<Session> session;
    std::shared_ptr<DeviceModel> model;
    std::vector<Owned> owned;
    std::atomic<bool> retired{false};
    std::atomic<uint64_t> safety{0};
    std::atomic<uint64_t> armedSeen{0};
    std::shared_ptr<Session> getSession() {std::lock_guard lock(mutex);return session;}
    std::shared_ptr<DeviceModel> getModel() {std::lock_guard lock(mutex);return model;}
    bool owns() {std::lock_guard lock(mutex);return !owned.empty();}
    void stopKeepers() {std::lock_guard lock(mutex);for(auto &o:owned) {if(o.keeper) {o.keeper->stop();}}}
    // Called only on the serial service executor. Never clears a slot without
    // matching its generation, epoch and complete configuration after a read.
    void clearOwned(Client &client) {
        std::vector<Owned> copy;
        {std::lock_guard lock(mutex);copy=owned;}
        stopKeepers();
        for(const auto &o:copy) {
            if(client.session().generation()!=o.generation) {continue;}
            const auto list=client.groups();
            const auto it=std::find_if(list.groups.begin(),list.groups.end(),[&](const auto &g){return matches(o,g);});
            if(it!=list.groups.end()) {client.groupClear(o.plan.id,true);}
        }
        std::lock_guard lock(mutex);owned.clear();
    }
};

LocalConnection::LocalConnection(QObject *parent):Connection(parent),
    // Application ownership matters at Qt shutdown: deleting a receiver from
    // the destruction of one of its own posted events re-enters Qt's queue
    // mutex. The app deletes this receiver after its posted-event cleanup.
    m_dispatch(std::make_shared<GuiDispatch>()),m_instances(this) {
    m_timer.setInterval(100);
    connect(&m_timer,&QTimer::timeout,this,[this] {
        if(m_closing) {
            if(m_closeElapsed.elapsed()>3500) {finishClose();}
            return;
        }
        if(m_context && m_snapshot && !m_loading && m_seen.isValid() && m_seen.elapsed()>3000) {
            report(Error(ErrorKind::Connection,"Device stopped sending status"));finishClose(true);
        }
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
bool LocalConnection::pendingReboot() const {return m_snapshot && (m_snapshot->info.flags&1);}
QString LocalConnection::deviceName() const {return m_snapshot?QString::fromStdString(m_snapshot->info.name):QString();}
QString LocalConnection::deviceId() const {return m_snapshot?QString::fromStdString(hex(m_snapshot->info.device_id)):QString();}
QString LocalConnection::reason() const {
    if(m_linkLost) {return tr("Link lost");}
    if(m_loading) {return tr("Loading device");}
    if(m_snapshot && m_snapshot->status) {
        static const char *reasons[]={"Boot","Explicit disarm","Command watchdog","Fault","Loop stalled","Shutdown"};
        const auto r=m_snapshot->status->last_disarm_reason;
        return r<std::size(reasons)?tr(reasons[r]):tr("Unknown disarm reason (%1)").arg(r);
    }
    return {};
}
bool LocalConnection::armEnabled() const {
    return ready() && m_groupsReady && !m_arming && !m_armPending && !m_disarmPending && !m_waitingDisarm &&
        (!m_observingArmed || m_explicitDisarmAcknowledged) &&
        !safeMode() && m_snapshot->status && armState()==0 && !(m_snapshot->status->flags&2);
}
QString LocalConnection::armBlockedReason() const {
    if(!ready()) {return tr("Wait for current device metadata and status");}
    if(safeMode()) {return tr("Device is in safe mode");}
    if(m_observingArmed && !m_explicitDisarmAcknowledged) {return tr("Press DISARM before taking command ownership");}
    if(m_arming || m_armPending || m_disarmPending || m_waitingDisarm) {return tr("Waiting for arm/disarm confirmation");}
    if(!m_groupsReady) {return m_blocked.isEmpty()?tr("Preparing command keepalives"):m_blocked;}
    if(!m_snapshot->status) {return tr("Waiting for device status");}
    if(m_snapshot->status->flags&2) {return tr("Device reports an ARM blocking condition");}
    return {};
}
void LocalConnection::report(const Error &e) {m_error=errorFields(e);emit notification(m_error["message"].toString(),2);emit changed();}
void LocalConnection::connectTo(const QString &endpoint) {
    if(m_context || m_closing || endpoint.trimmed().isEmpty()) {return;}
    auto c=std::make_shared<Context>();m_context=c;m_endpoint=endpoint.trimmed();m_loading=true;m_linkLost=false;
    m_error.clear();m_blocked.clear();m_snapshot.reset();m_statusCounter.reset();m_setupAttempted=false;
    m_observingArmed=false;m_explicitDisarmAcknowledged=false;
    m_disarmWhenOpened=false;
    emit changed();
    const auto queue=m_dispatch;const QPointer<LocalConnection> self(this);const auto address=m_endpoint.toStdString();
    // Start after this GUI turn. Safety requests during opening are retained
    // until publication, including an Esc immediately after Connect.
    QTimer::singleShot(0,this,[this,c,queue,self,address] {
    if(!current(c)) {return;}
    m_opener=std::thread([c,queue,self,address] {
        try {
            SessionOptions options;options.dispatcher=[queue](Task task){post(queue,std::move(task));};
            auto session=std::make_shared<Session>(address,options);
            // Discovery can outlast an already-armed owner's watchdog. Keep
            // the early status history independently of DeviceModel, which
            // installs its observer only after its initial metadata load.
            auto noteArmed=[weak=std::weak_ptr<Context>(c)](const Frame &frame) {
                if(auto state=weak.lock();state && !state->retired) {
                    try {
                        const auto event=Client::decodeEvent(frame);
                        if(const auto *status=std::get_if<StatusFrame>(&event);status && status->arm_state==1) {state->armedSeen=frame.generation;}
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
                if(self->m_disarmWhenOpened) {self->disarm();}
                emit self->changed();
            });
            Client client(*session);
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
            post(queue,[self,c,error] {if(self && self->current(c)) {self->report(error);self->finishClose();}});
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
    if(!m_snapshot || m_snapshot->generation!=snapshot->generation) {
        c->stopKeepers();{std::lock_guard lock(c->mutex);c->owned.clear();}
        m_groupsReady=false;m_setupAttempted=false;m_statusCounter.reset();++c->safety;
        m_arming=false;m_waitingDisarm=false;
        if(m_snapshot) {m_explicitDisarmAcknowledged=false;}
    }
    m_snapshot=std::move(snapshot);
    if(!m_snapshot->connected) {finishClose(true);return;}
    if(metadata) {m_instances.reset(m_snapshot);}
    if(m_snapshot->status && (!m_statusCounter || *m_statusCounter!=m_snapshot->status->counter)) {
        m_statusCounter=m_snapshot->status->counter;m_seen.restart();
        if(armState()==1) {m_arming=false;}
        if(armState()==0) {m_waitingDisarm=false;}
    }
    if(!m_groupsReady && !m_setupPending && !c->owns() &&
       (c->armedSeen==m_snapshot->generation || (m_snapshot->status && armState()==1))) {
        m_observingArmed=true;
    }
    if(ready() && m_snapshot->status && armState()==0 && !safeMode() && !m_setupPending &&
       (!m_observingArmed || m_explicitDisarmAcknowledged) &&
       (!m_setupAttempted || m_setupGeneration!=m_snapshot->generation)) {setupGroups();}
    emit changed();
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
            if(!result) {self->m_blocked=QString::fromUtf8(result.error().what());self->report(result.error());}
            emit self->changed();
        });
    } catch(const Error &e) {m_setupPending=false;m_blocked=QString::fromUtf8(e.what());report(e);}
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
            else if(!result) {self->m_arming=false;self->report(result.error());}
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
                if(c->owns() || self->m_setupPending || self->m_armPending) {self->clearAndClose();}
                else {self->finishClose();}
            }
            else {self->refresh(c);}
            emit self->changed();
        });
    } catch(const Error &e) {m_disarmPending=false;report(e);if(m_closing) {finishClose();}}
}
void LocalConnection::acknowledge() {
    if(!ready() || armState()!=2 || m_ackPending) {return;}
    auto c=m_context;auto session=c->getSession();m_ackPending=true;const QPointer<LocalConnection> self(this);
    try {
        Client(*session).async([](Client &client){return client.acknowledgeFault();},[self,c](std::expected<SystemResult,Error> result) {
            if(!self || !self->current(c)) {return;}self->m_ackPending=false;
            if(!result) {self->report(result.error());}emit self->changed();
        });
    } catch(const Error &e) {m_ackPending=false;report(e);}
}
void LocalConnection::disconnectDevice() {
    if(!m_context) {emit disconnected();return;}
    if(m_closing) {return;}
    m_closing=true;m_closeElapsed.start();emit changed();
    // An observer may have explicitly requested DISARM while opening. Await
    // that admitted request; closing must not turn it into a silent cancel.
    if(m_disarmPending) {return;}
    if(m_context->owns() || m_setupPending || m_armPending) {disarm();}
    else {finishClose();}
}
void LocalConnection::clearAndClose() {
    auto c=m_context;auto session=c->getSession();const QPointer<LocalConnection> self(this);
    try {
        session->async<OperationResult>([c,session] {Client client(*session);c->clearOwned(client);return OperationResult{};},
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
    m_disarmWhenOpened=false;
    m_linkLost=lost;
    if(!lost) {m_snapshot.reset();m_instances.reset({});}
    if(unsentDisarm) {report(Error(ErrorKind::Cancelled,"DISARM could not be sent before the connection closed"));}
    if(unconfirmedDisarm) {
        Error error(ErrorKind::Connection,"Connection closed before DISARM was confirmed");
        error.family=1;error.op=3;error.has_request_context=true;error.outcome_unknown=true;report(error);
    }
    emit changed();if(c) {emit disconnected();}
}
void LocalConnection::selectRows(DescriptorRows &rows,uint8_t type,uint8_t instance,bool pending) const {
    rows.reset(m_snapshot,type,instance,pending);
}
} // namespace drivelink::gui
