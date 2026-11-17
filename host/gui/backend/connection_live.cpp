// The Layout detail's live half on LocalConnection: one subscription group
// for the selected running node, and its actions.
//
// The group follows the loop-statistics rules: free slots only, after command
// setup and the statistics groups; intent is recorded before SUB_CONFIG so
// close reconciles an unanswered mutation; a slot is cleared or reused only
// when LIST shows it still holds exactly what this GUI configured. One
// mutation at a time; when it completes, the latest selection is applied.
//
// Actions are mutations: RUN is sent once, one at a time, and an unanswered
// one is reported as uncertain, never repeated. A configuration refresh
// follows (paused actions can stage params and set Unsaved).
#include "connection_internal.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QPointer>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
const ModelItem *findItem(const DeviceSnapshot &s,const Ref &r) {
    const auto t=s.types.find(r.type);if(t==s.types.end()) {return nullptr;}
    const auto i=t->second.items.find(r.item);return i==t->second.items.end()?nullptr:&i->second;
}
size_t entryLimit(const DeviceSnapshot &s) {
    const auto it=s.system_params.find(defs::system::kLimitsGroupEntries);
    const auto limit=it==s.system_params.end()?0:valueBits(it->second);
    return limit?std::min<size_t>(limit,64):64;
}
bool withdrawn(const Error &e) {
    return e.kind==ErrorKind::Rejected && e.result==uint8_t(defs::Result::Busy) && e.detail==uint8_t(defs::Detail::Withdrawn);
}
// What the selection's group was set to on the service executor.
struct LiveSetup {
    uint8_t id=0;
    std::shared_ptr<GroupBuffer> buffer;
    std::string note;
};
QString signature(const QString &key,const std::optional<LivePlan> &plan,uint64_t generation) {
    QString text=QString::number(generation)+"|"+key;
    if(!plan) {return text+"|none";}
    text+=QString("|%1|%2").arg(plan->loop).arg(plan->divider);
    for(const auto &r:plan->refs) {text+=QString("|%1:%2:%3").arg(r.type).arg(r.instance).arg(r.item);}
    return text;
}
}

void LocalConnection::resetLive() {
    m_live.reset();m_liveValues={};m_liveCounter.reset();m_liveSeen.invalidate();
    m_liveAttempt.clear();m_liveNote.clear();
}

void LocalConnection::syncLive() {
    auto c=m_context;
    if(!c || m_closing || m_livePending || !m_snapshot || !ready() || !m_snapshot->status) {return;}
    const auto generation=m_snapshot->generation;
    // After command setup and the statistics groups, so group mutations
    // never interleave and the Device page keeps its run-time columns.
    if(!m_subsAttempted || m_subsPending || m_subsGeneration!=generation || m_setupPending) {return;}
    const auto node=parseNodeKey(m_selected);
    std::optional<LivePlan> want;
    if(node) {want=livePlan(*m_snapshot,*node,entryLimit(*m_snapshot));}
    const auto attempt=signature(m_selected,want,generation);
    if(attempt==m_liveAttempt) {return;}
    m_liveAttempt=attempt;
    if(m_live && m_live->generation==generation && want && m_live->plan.loop==want->loop &&
       m_live->plan.divider==want->divider && m_live->plan.refs==want->refs) {
        m_live->key=m_selected;m_live->plan.hidden=want->hidden;emit liveChanged();return;
    }
    if(!want && !m_live && c->liveSubs(generation).empty()) {m_liveNote.clear();emit liveChanged();return;}
    auto session=c->getSession();if(!session) {return;}
    m_livePending=true;
    const QPointer<LocalConnection> self(this);const auto key=m_selected;
    try {
        session->async<LiveSetup>([c,session,generation,want] {
            if(c->retired || session->generation()!=generation) {throw Error(ErrorKind::Cancelled,"Connection changed during live setup");}
            Client client(*session);
            auto groups=client.groups();
            const auto slotOf=[&](uint8_t id) {
                return std::find_if(groups.groups.begin(),groups.groups.end(),[&](const Group &g){return g.kind==GroupKind::Subscription && g.id==id;});
            };
            const auto forget=[&](const OwnedSub &o) {
                std::lock_guard lock(c->mutex);
                std::erase_if(c->subs,[&](const OwnedSub &s){return s.live && s.generation==o.generation && s.id==o.id && s.epoch==o.epoch && s.refs==o.refs;});
            };
            // Our previous groups: keep one exact match to reconfigure, clear
            // the rest; a record the device no longer holds is not ours.
            std::optional<OwnedSub> reuse;
            for(const auto &o:c->liveSubs(generation)) {
                const auto slot=slotOf(o.id);
                if(slot==groups.groups.end() || !matchesSub(o,*slot)) {forget(o);continue;}
                if(want && !reuse) {reuse=o;continue;}
                for(int attempt=1;;++attempt) {
                    try {client.groupClear(o.id,false);forget(o);break;}
                    catch(const Error &e) {
                        if(e.kind==ErrorKind::Cancelled || e.kind==ErrorKind::Connection) {throw;}
                        // Busy/Withdrawn guarantees no change; anything else
                        // keeps the record so close reconciles it.
                        if(withdrawn(e) && attempt<3 && !c->retired) {continue;}
                        throw;
                    }
                }
            }
            if(!want) {return LiveSetup{};}
            uint8_t id=0;
            if(reuse) {id=reuse->id;}
            else {
                // Only free slots: another client's subscriptions are never reused.
                const auto free=std::find_if(groups.groups.begin(),groups.groups.end(),
                    [](const Group &g){return g.kind==GroupKind::Subscription && !g.configured;});
                if(free==groups.groups.end()) {return LiveSetup{0,{},"No free subscription slot"};}
                id=free->id;
            }
            const OwnedSub intent{id,want->loop,uint8_t(slotOf(id)->epoch+1),generation,want->refs,{},true};
            {std::lock_guard lock(c->mutex);c->subs.push_back(intent);}
            GroupMutation configured;
            for(int attempt=1;;++attempt) {
                try {configured=client.subConfigure(id,want->loop,want->divider,want->refs);break;}
                catch(const Error &e) {
                    if(withdrawn(e) && attempt<3 && !c->retired) {continue;}
                    // A refusal changed nothing: the slot keeps what it held.
                    if(!e.outcome_unknown) {forget(intent);}
                    throw;
                }
            }
            if(configured.generation!=generation) {throw Error(ErrorKind::Cancelled,"Device rebooted during live setup");}
            auto buffer=session->subscribeBuffer(id,configured.epoch,want->refs.size(),8,configured.generation);
            std::lock_guard lock(c->mutex);
            if(c->retired) {throw Error(ErrorKind::Cancelled,"Live ownership was invalidated");}
            const auto owned=std::find_if(c->subs.begin(),c->subs.end(),[&](const OwnedSub &s) {
                return s.live && s.generation==generation && s.id==id && s.epoch==intent.epoch && s.refs==intent.refs;
            });
            if(owned==c->subs.end()) {throw Error(ErrorKind::Cancelled,"Live ownership was invalidated");}
            owned->epoch=configured.epoch;owned->buffer=buffer;
            // The reconfigured slot no longer holds the previous configuration.
            std::erase_if(c->subs,[&](const OwnedSub &s) {
                return s.live && s.id==id && !(s.epoch==configured.epoch && s.refs==intent.refs && s.buffer);
            });
            return LiveSetup{id,std::move(buffer),{}};
        },[self,c,generation,key,want](std::expected<LiveSetup,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_livePending=false;
            if(!self->m_snapshot || self->m_snapshot->generation!=generation) {self->resetLive();self->syncLive();return;}
            self->m_liveValues={};self->m_liveCounter.reset();self->m_liveSeen.invalidate();
            if(!result) {
                self->m_live.reset();self->m_liveNote=errorText(result.error());
            } else if(result->buffer) {
                self->m_live=LiveGroup{key,*want,generation,result->id,std::move(result->buffer)};
                self->m_liveNote.clear();self->m_liveSeen.start();
            } else {
                self->m_live.reset();self->m_liveNote=QString::fromStdString(result->note);
            }
            self->syncLive();
            emit self->liveChanged();
        });
    } catch(const Error &e) {m_livePending=false;m_liveNote=errorText(e);emit liveChanged();}
}

void LocalConnection::pollLive() {
    if(!m_live || !m_live->buffer || !m_snapshot) {return;}
    bool changed=false;
    GroupSample sample;
    if(m_live->buffer->latest(sample) && sample.generation==m_live->generation &&
       (!m_liveCounter || *m_liveCounter!=sample.counter)) {
        m_liveCounter=sample.counter;m_liveSeen.restart();changed=true;
        m_liveValues.values.assign(sample.values.begin(),sample.values.begin()+std::min<size_t>(sample.count,sample.values.size()));
    }
    // Fresh while samples keep arriving: about a second, or three sample
    // periods for a slow loop.
    qint64 window=1000;
    if(const auto loop=std::find_if(m_snapshot->info.loops.begin(),m_snapshot->info.loops.end(),[&](const LoopInfo &l){return l.id==m_live->plan.loop;});
       loop!=m_snapshot->info.loops.end() && loop->rate_hz) {
        window=std::max<qint64>(window,qint64(3000)*m_live->plan.divider/loop->rate_hz);
    }
    const bool fresh=!m_liveValues.values.empty() && m_liveSeen.isValid() && m_liveSeen.elapsed()<=window;
    if(fresh!=m_liveValues.fresh) {m_liveValues.fresh=fresh;changed=true;}
    if(changed) {emit liveChanged();}
}

QVariantMap LocalConnection::live() const {
    QVariantMap result;
    if(!ready() || !m_snapshot) {return result;}
    const auto node=parseNodeKey(m_selected);
    if(!node || !anyEntry(*m_snapshot,*node)) {return result;}
    const bool mine=m_live && m_live->key==m_selected && m_live->generation==m_snapshot->generation;
    const LivePlan *plan=mine?&m_live->plan:nullptr;
    const LiveValues *values=mine?&m_liveValues:nullptr;
    const bool telemetry=mine && m_liveValues.fresh;
    QString note;
    if(const auto why=notRunning(*m_snapshot,*node);!why.isEmpty()) {note=why;}
    else if(!mine) {note=m_liveNote.isEmpty()?tr("Waiting for telemetry"):tr("Telemetry unavailable: %1").arg(m_liveNote);}
    else if(!telemetry) {note=m_liveValues.values.empty()?tr("Waiting for the first sample"):tr("Telemetry unavailable: no new sample for over a second");}
    result["note"]=note;result["telemetry"]=telemetry;result["hidden"]=mine?m_live->plan.hidden:0;
    result["entries"]=int(entryLimit(*m_snapshot));
    if(node->first==defs::kLoopTypeId) {
        result["statistics"]=loopStatistics(*m_snapshot,node->second,plan,values);
    } else {
        result["signals"]=signalRows(*m_snapshot,*node,plan,values);
        result["sources"]=sourceStates(*m_snapshot,*node,plan,values);
    }
    return result;
}

void LocalConnection::updateActions() {
    QVariantList rows;
    const auto node=parseNodeKey(m_selected);
    if(ready() && m_snapshot && node && anyEntry(*m_snapshot,*node)) {
        // Each row with its last outcome; one action runs at a time.
        for(auto row:actionRows(*m_snapshot,*node,armState()==1 || m_arming,actionCommon(),!m_actionRunning.isEmpty())) {
            auto map=row.toMap();const auto key=map["key"].toString();
            map["running"]=key==m_actionRunning;map["result"]=m_actionResults.value(key);
            rows<<map;
        }
    }
    m_actions.update(rows);
}

QString LocalConnection::actionCommon() const {
    if(const auto common=commonBlock();!common.isEmpty()) {return common;}
    if(m_expectReboot) {return tr("The device is rebooting");}
    return {};
}

QVariantMap LocalConnection::checkAction(const QString &key,const QStringList &args,const QString &blobHex) const {
    const auto ref=parseItemKey(key);
    const auto *item=ready() && m_snapshot && ref?findItem(*m_snapshot,*ref):nullptr;
    if(!item || item->descriptor.kind!=ItemKind::Action) {return {{"problem",tr("Unknown action")},{"field",-1}};}
    const auto &d=item->descriptor;
    const bool armed=armState()==1 || m_arming;
    const auto blocked=actionBlock(*m_snapshot,{ref->type,ref->instance},d,armed,actionCommon(),!m_actionRunning.isEmpty());
    if(!blocked.isEmpty()) {return {{"problem",blocked},{"field",-1},{"blocked",true}};}
    const auto parsed=parseActionArgs(d,args);
    if(!parsed.problem.isEmpty()) {return {{"problem",parsed.problem},{"field",parsed.field}};}
    if(d.blob_direction==BlobDir::In || d.blob_direction==BlobDir::Both) {
        const auto blob=parseHexBlob(blobHex);
        if(!blob.bytes) {return {{"problem",blob.problem},{"field",-2}};}
    }
    return {{"problem",QString()},{"field",-1}};
}

void LocalConnection::runAction(const QString &key,const QStringList &args,const QString &blobHex) {
    const auto ref=parseItemKey(key);
    const auto *item=ready() && m_snapshot && ref?findItem(*m_snapshot,*ref):nullptr;
    if(!item || item->descriptor.kind!=ItemKind::Action) {return;}
    // Refused locally (nothing sent) when blocked or the input does not parse.
    if(const auto check=checkAction(key,args,blobHex);!check["problem"].toString().isEmpty()) {
        m_actionResults[key]={{"state",check["blocked"].toBool()?"refused":"invalid"},{"text",check["problem"]},{"field",check["field"]}};
        emit changed();return;
    }
    const auto &d=item->descriptor;
    const auto parsed=parseActionArgs(d,args);
    QByteArray blob;
    if(d.blob_direction==BlobDir::In || d.blob_direction==BlobDir::Both) {blob=*parseHexBlob(blobHex).bytes;}
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {return;}
    const QPointer<LocalConnection> self(this);
    const auto generation=m_snapshot->generation;const auto descriptor=d;const auto target=*ref;
    m_actionRunning=key;m_actionResults[key]={{"state","running"},{"text",tr("Running…")}};emit changed();
    try {
        Client(*session).async([target,bits=parsed.bits,blob](Client &client) {
            const std::vector<uint8_t> bytes(blob.begin(),blob.end());
            return client.action(target,bits,bytes);
        },[self,c,key,generation,descriptor](std::expected<ActionResult,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_actionRunning.clear();
            const bool sameBoot=self->m_snapshot && self->m_snapshot->generation==generation;
            if(!result) {
                const auto &e=result.error();
                if(e.outcome_unknown) {
                    self->m_actionResults[key]={{"state","uncertain"},
                        {"text",tr("No reply in time; the action may have run. It was not retried.")}};
                    self->refreshConfig();
                } else {
                    self->m_actionResults[key]={{"state","refused"},{"text",errorText(e)}};
                }
            } else if(!sameBoot) {
                self->m_actionResults[key]={{"state","uncertain"},{"text",tr("The device rebooted after the action ran")}};
            } else {
                const auto blobOut=descriptor.blob_direction==BlobDir::Out || descriptor.blob_direction==BlobDir::Both;
                const QByteArray bytes(reinterpret_cast<const char *>(result->blob.data()),qsizetype(result->blob.size()));
                QVariantMap done{{"state","ok"},{"text",tr("Done")},{"values",actionResultRows(descriptor,result->values)}};
                if(blobOut) {done["blobHex"]=hexText(bytes);done["blobSize"]=int(bytes.size());}
                self->m_actionResults[key]=done;
                self->refreshConfig();
            }
            emit self->changed();
        });
    } catch(const Error &e) {
        m_actionRunning.clear();m_actionResults[key]={{"state","refused"},{"text",errorText(e)}};emit changed();
    }
}
} // namespace drivelink::gui
