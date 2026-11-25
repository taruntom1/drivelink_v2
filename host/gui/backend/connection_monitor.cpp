// Monitor's subscription groups on LocalConnection: one per plotted loop.
//
// They follow the loop-statistics and live-group rules: free slots only,
// after command setup, the statistics groups and the Layout live group;
// intent is recorded before SUB_CONFIG so close reconciles an unanswered
// mutation; a slot is reused or cleared only when LIST shows it still holds
// exactly what this GUI configured; Busy/Withdrawn is retried twice (it
// guarantees no change); a SUB_CONFIG that lands on a newer boot releases
// its slot. One group mutation at a time across live and Monitor; when one
// completes, the latest plot set is applied. Every pass ends with a LIST
// read, which the link budget and the free-slot count use.
#include "connection_internal.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QPointer>
#include <set>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
constexpr int kMonitorRetries=3;
bool withdrawn(const Error &e) {
    return e.kind==ErrorKind::Rejected && e.result==uint8_t(defs::Result::Busy) && e.detail==uint8_t(defs::Detail::Withdrawn);
}
struct GroupSetup {
    MonitorPlan plan;
    uint8_t id=0;
    std::shared_ptr<GroupBuffer> buffer;
};
struct MonitorSetup {
    std::vector<GroupSetup> groups;
    GroupList list;
    std::string note;
    std::optional<Error> error;
};
QString signature(uint64_t generation,const std::vector<MonitorPlan> &plans) {
    QString text=QString::number(generation);
    for(const auto &p:plans) {
        text+=QString("|%1/%2").arg(p.loop).arg(p.divider);
        for(const auto &r:p.refs) {text+=QString(":%1.%2.%3").arg(r.type).arg(r.instance).arg(r.item);}
    }
    return text;
}
// Retries Busy/Withdrawn twice; anything else is the caller's.
template<class C,class F> auto withRetry(const C &c,F &&f) {
    for(int attempt=1;;++attempt) {
        try {return f();}
        catch(const Error &e) {if(!withdrawn(e) || attempt>=3 || c->retired) {throw;}}
    }
}
}

void LocalConnection::setPage(const QString &page) {
    if(page==m_page) {return;}
    m_page=page;
    // A slider still pressed when Monitor is left counts as released
    // (dead-man); latched values keep running (user decision 2026-11-18).
    m_command.releaseHeld();
    // The Layout live group follows the page; Monitor re-reads the slots.
    if(page==QLatin1String("monitor")) {m_monitorListStale=true;}
    syncLive();syncMonitor();
}

void LocalConnection::resetMonitorGroups() {
    m_monitorGroups.clear();m_monitorAttempt.clear();m_monitorListStale=true;m_monitorRetries=0;
}

void LocalConnection::syncMonitor() {
    auto c=m_context;
    if(!c || m_closing || m_monitorPending || m_livePending || !m_snapshot || !ready() || !m_snapshot->status) {return;}
    const auto generation=m_snapshot->generation;
    if(!m_subsAttempted || m_subsPending || m_subsGeneration!=generation || m_setupPending) {return;}
    const auto want=m_monitor.plans();
    const auto attempt=signature(generation,want);
    const bool list=m_monitorListStale && m_page==QLatin1String("monitor");
    if(attempt==m_monitorAttempt && !list) {return;}
    if(want.empty() && m_monitorGroups.empty() && c->monitorSubs(generation).empty() && !list) {
        m_monitorAttempt=attempt;m_monitor.setNote({});return;
    }
    auto session=c->getSession();if(!session) {return;}
    m_monitorAttempt=attempt;m_monitorListStale=false;m_monitorPending=true;
    const QPointer<LocalConnection> self(this);
    try {
        session->async<MonitorSetup>([c,session,generation,want] {
            if(c->retired || session->generation()!=generation) {throw Error(ErrorKind::Cancelled,"Connection changed during Monitor setup");}
            Client client(*session);
            MonitorSetup result;
            auto groups=client.groups();
            const auto slotOf=[&](uint8_t id) {
                return std::find_if(groups.groups.begin(),groups.groups.end(),[&](const Group &g){return g.kind==GroupKind::Subscription && g.id==id;});
            };
            const auto same=[](const OwnedSub &a,const OwnedSub &b) {
                return a.monitor && b.monitor && a.generation==b.generation && a.id==b.id && a.epoch==b.epoch && a.refs==b.refs;
            };
            const auto forget=[&](const OwnedSub &o) {
                std::lock_guard lock(c->mutex);std::erase_if(c->subs,[&](const OwnedSub &s){return same(s,o);});
            };
            // Our records that LIST confirms; a record the device no longer
            // holds exactly is not ours any more.
            std::vector<OwnedSub> mine;
            for(const auto &o:c->monitorSubs(generation)) {
                const auto slot=slotOf(o.id);
                if(slot==groups.groups.end() || !matchesSub(o,*slot)) {forget(o);continue;}
                mine.push_back(o);
            }
            // Each plotted loop keeps its slot; slots of loops no longer
            // plotted are cleared first, so their slots are free again.
            std::vector<std::optional<OwnedSub>> assigned(want.size());
            std::set<uint8_t> taken;
            for(size_t i=0;i<want.size();++i) {
                const auto it=std::find_if(mine.begin(),mine.end(),[&](const OwnedSub &o){return o.loop==want[i].loop && !taken.contains(o.id);});
                if(it!=mine.end()) {assigned[i]=*it;taken.insert(it->id);}
            }
            bool cleared=false;
            for(const auto &o:mine) {
                if(taken.contains(o.id)) {continue;}
                try {withRetry(c,[&]{return client.groupClear(o.id,false);});forget(o);cleared=true;}
                catch(const Error &e) {
                    // Anything but a definite refusal keeps the record so close reconciles it.
                    if(e.kind==ErrorKind::Cancelled || e.kind==ErrorKind::Connection) {throw;}
                    result.note.clear();result.error=e;
                }
            }
            if(cleared) {groups=client.groups();}
            for(size_t i=0;i<want.size();++i) {
                if(c->retired || session->generation()!=generation) {throw Error(ErrorKind::Cancelled,"Connection changed during Monitor setup");}
                const auto &plan=want[i];
                const auto &owned=assigned[i];
                // Unchanged: keep streaming into the same buffer.
                if(owned && owned->buffer && owned->refs==plan.refs && owned->divider==plan.divider &&
                   slotOf(owned->id)!=groups.groups.end() && slotOf(owned->id)->divider==plan.divider) {
                    result.groups.push_back({plan,owned->id,owned->buffer});continue;
                }
                uint8_t id=0;
                if(owned) {id=owned->id;}
                else {
                    // Only free slots: another client's subscriptions are never reused.
                    const auto free=std::find_if(groups.groups.begin(),groups.groups.end(),[&](const Group &g) {
                        return g.kind==GroupKind::Subscription && !g.configured && !taken.contains(g.id);
                    });
                    if(free==groups.groups.end()) {result.error.reset();result.note="No free subscription slot";continue;}
                    id=free->id;
                }
                taken.insert(id);
                const OwnedSub intent{id,plan.loop,uint8_t(slotOf(id)->epoch+1),generation,plan.refs,{},false,true,plan.divider};
                {std::lock_guard lock(c->mutex);c->subs.push_back(intent);}
                try {
                    GroupMutation configured;
                    try {configured=withRetry(c,[&]{return client.subConfigure(id,plan.loop,plan.divider,plan.refs);});}
                    catch(const Error &e) {
                        // A refusal changed nothing: the slot keeps what it held.
                        if(!e.outcome_unknown) {forget(intent);}
                        throw;
                    }
                    if(configured.generation!=generation) {
                        c->releaseStray(client,{id,plan.loop,configured.epoch,configured.generation,plan.refs,{},false,true,plan.divider});
                        throw Error(ErrorKind::Cancelled,"Device rebooted during Monitor setup");
                    }
                    auto buffer=session->subscribeBuffer(id,configured.epoch,plan.refs.size(),2048,configured.generation);
                    {
                        std::lock_guard lock(c->mutex);
                        if(c->retired) {throw Error(ErrorKind::Cancelled,"Monitor ownership was invalidated");}
                        const auto record=std::find_if(c->subs.begin(),c->subs.end(),[&](const OwnedSub &s){return same(s,intent);});
                        if(record==c->subs.end()) {throw Error(ErrorKind::Cancelled,"Monitor ownership was invalidated");}
                        record->epoch=configured.epoch;record->buffer=buffer;
                        // The slot now holds exactly this configuration: no earlier
                        // record for it, of any kind (a stale unanswered intent of
                        // statistics or the live group included), can match it any more.
                        std::erase_if(c->subs,[&](const OwnedSub &s) {
                            return s.id==id && s.generation==generation &&
                                !(s.monitor && s.epoch==configured.epoch && s.refs==plan.refs && s.buffer);
                        });
                    }
                    result.groups.push_back({plan,id,std::move(buffer)});
                } catch(const Error &e) {
                    // One loop's refusal leaves only that loop without plots.
                    if(e.kind==ErrorKind::Cancelled || e.kind==ErrorKind::Connection) {throw;}
                    result.note.clear();result.error=e;
                }
            }
            result.list=client.groups();
            return result;
        },[self,c,generation](std::expected<MonitorSetup,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_monitorPending=false;
            if(!self->m_snapshot || self->m_snapshot->generation!=generation) {
                self->resetMonitorGroups();self->syncLive();self->syncMonitor();return;
            }
            if(!result) {
                // Plotting stops. The same plot set is tried again a few times
                // (a timed-out read or a busy device); after that, the next
                // change of the plot set or a return to the page tries again.
                self->m_monitorGroups.clear();
                self->m_monitor.setNote(tr("Plotting stopped: %1").arg(errorText(result.error())));
                if(self->m_monitorRetries<kMonitorRetries) {
                    ++self->m_monitorRetries;
                    QTimer::singleShot(2000,self,[self,c] {
                        if(!self || !self->current(c)) {return;}
                        self->m_monitorAttempt.clear();self->syncMonitor();
                    });
                }
            } else {
                std::vector<MonitorGroup> next;
                for(auto &g:result->groups) {
                    MonitorGroup group{g.plan,generation,g.id,g.buffer,std::nullopt};
                    // A kept group keeps its counter: no loss is counted across a pass.
                    const auto old=std::find_if(self->m_monitorGroups.begin(),self->m_monitorGroups.end(),[&](const MonitorGroup &m){return m.buffer==g.buffer;});
                    if(old!=self->m_monitorGroups.end()) {group.counter=old->counter;}
                    next.push_back(std::move(group));
                }
                self->m_monitorGroups=std::move(next);
                self->m_monitor.setNote(result->error?errorText(*result->error):QString::fromStdString(result->note));
                self->m_monitorRetries=0;
                LinkContext link;link.entryLimit=groupEntryLimit(*self->m_snapshot);
                size_t used=0,free=0,total=0;
                for(const auto &g:result->list.groups) {
                    if(g.kind!=GroupKind::Subscription) {continue;}
                    ++total;
                    if(!g.configured) {++free;continue;}
                    ++used;
                    const bool mine=std::any_of(self->m_monitorGroups.begin(),self->m_monitorGroups.end(),[&](const MonitorGroup &m){return m.id==g.id;});
                    if(!mine) {link.others.push_back({g.loop,g.divider,g.refs.size()});}
                }
                link.freeSlots=free;
                self->m_monitor.setLink(std::move(link),used,total);
            }
            self->syncLive();self->syncMonitor();
        });
    } catch(const Error &e) {m_monitorPending=false;m_monitor.setNote(errorText(e));}
}

void LocalConnection::pollMonitor() {
    for(auto &g:m_monitorGroups) {
        if(!g.buffer) {continue;}
        const auto sinks=m_monitor.sinks(g.plan.refs);
        GroupSample sample;
        size_t count=0;
        std::array<uint32_t,64> last{};
        // Every queued sample, oldest first; the counter shows what was lost
        // on the way (device skips, the link, a full host buffer).
        while(g.buffer->pop(sample)) {
            if(sample.generation!=g.generation) {continue;}
            if(g.counter) {m_monitor.addLost(counterGap(*g.counter,sample.counter));}
            g.counter=sample.counter;
            const auto n=std::min<size_t>(sample.count,sinks.size());
            for(size_t i=0;i<n;++i) {m_monitor.push(sinks[i],int64_t(sample.device_us),sample.values[i]);}
            last=sample.values;count=n;
        }
        // Only the values the newest sample carried.
        for(size_t i=0;i<count;++i) {m_monitor.setLatest(sinks[i],last[i]);}
    }
    m_monitor.endAppend();
}

std::vector<uint8_t> LocalConnection::monitorSubscriptions() const {
    std::vector<uint8_t> ids;
    for(const auto &g:m_monitorGroups) {ids.push_back(g.id);}
    return ids;
}
} // namespace drivelink::gui
