// The Layout screen's operations on LocalConnection: param SETs and layout
// ops. Each mutation is sent once; its completion is followed by a
// configuration refresh (DeviceModel::refreshConfigAsync), which also settles
// an unanswered request. Nothing here is retried, touches command groups or
// arms.
#include "connection_internal.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <QFile>
#include <QPointer>
#include <QUrl>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
const ModelItem *findItem(const DeviceSnapshot &s,const Ref &r) {
    const auto t=s.types.find(r.type);if(t==s.types.end()) {return nullptr;}
    const auto i=t->second.items.find(r.item);return i==t->second.items.end()?nullptr:&i->second;
}
bool flag(const ItemDescriptor &d,defs::ItemFlag f) {return (d.flags&uint16_t(f))!=0;}
QVariantList candidateList(const std::vector<Candidate> &items) {
    QVariantList result;
    for(const auto &c:items) {
        result<<QVariantMap{{"value",QVariant::fromValue(c.value)},{"text",c.text},{"detail",c.detail},
                            {"enabled",c.enabled},{"reason",c.reason}};
    }
    return result;
}
}

QString LocalConnection::layoutBlocked() const {
    if(const auto common=commonBlock();!common.isEmpty()) {return common;}
    if(m_expectReboot) {return tr("The device is rebooting");}
    if(armState()==1 || m_arming) {return tr("The layout is locked while armed; disarm first");}
    return {};
}

QString LocalConnection::rebootBlocked() const {
    const auto blocked=layoutBlocked();
    return blocked.isEmpty()?exclusiveBlocked():blocked;
}
QString LocalConnection::liveParamBlocked() const {
    auto blocked=commonBlock();
    if(blocked.isEmpty() && m_expectReboot) {blocked=tr("The device is rebooting");}
    return blocked;
}

void LocalConnection::refreshConfig() {
    auto c=m_context;if(!c) {return;}
    if(auto model=c->getModel()) {
        // A Boot in between fails the refresh; the Boot reload replaces it.
        try {model->refreshConfigAsync();} catch(const Error &) {}
    }
}

void LocalConnection::rebuildLayout(bool metadata) {
    const bool armed=armState()==1 || m_arming;
    // Readiness can return (a Boot reload, a finished load) without a further
    // metadata event, so a change of readiness rebuilds too.
    if(metadata || ready()!=m_layoutReady) {
        metadata=true;m_layoutReady=ready();
        m_layoutTree.reset(m_layoutReady?m_snapshot:nullptr);
        // Keep the selection while it exists; else select the first loop.
        const auto keys=m_layoutTree.keys();
        if(!keys.isEmpty() && !keys.contains(m_selected)) {m_selected=keys.front();}
    }
    // The settings rows also follow the status flags (safe mode, pending reboot).
    if(metadata || armed!=m_paramsArmed || safeMode()!=m_paramsSafe || pendingReboot()!=m_paramsReboot) {
        m_paramsArmed=armed;m_paramsSafe=safeMode();m_paramsReboot=pendingReboot();updateParams();
    }
}
void LocalConnection::updateParams() {
    m_params.reset(ready()?m_snapshot:nullptr,parseNodeKey(m_selected),m_paramsArmed,m_edits,
                   m_changedParams.value(deviceId()),m_paramFilter,m_paramFilterText);
    m_systemParams.resetSystem(ready()?m_snapshot:nullptr,m_paramsArmed,m_edits,m_changedParams.value(deviceId()),
                               m_paramsSafe,m_paramsReboot);
}
void LocalConnection::selectNode(const QString &key) {
    if(key==m_selected || !m_layoutTree.keys().contains(key)) {return;}
    m_selected=key;
    // Kept refusals belong to the rows they were typed in; the settings card
    // (system params, instance 0 of type 0) keeps its own.
    const auto system=QString::number(defs::kSystemTypeId)+":0:";
    for(auto it=m_edits.begin();it!=m_edits.end();) {
        if(it->state!="sending" && !it.key().startsWith(system)) {it=m_edits.erase(it);} else {++it;}
    }
    // So do action outcomes; the one in flight keeps its row.
    for(auto it=m_actionResults.begin();it!=m_actionResults.end();) {
        if(it.key()!=m_actionRunning) {it=m_actionResults.erase(it);} else {++it;}
    }
    updateParams();syncLive();emit changed();emit liveChanged();
}
void LocalConnection::setParamFilter(int filter,const QString &text) {
    if(filter==m_paramFilter && text==m_paramFilterText) {return;}
    m_paramFilter=std::clamp(filter,0,2);m_paramFilterText=text;updateParams();emit changed();
}
void LocalConnection::clearParamEdit(const QString &key) {
    if(const auto it=m_edits.find(key);it!=m_edits.end() && it->state!="sending") {
        m_edits.erase(it);updateParams();emit changed();
    }
}

QVariantMap LocalConnection::layout() const {
    QVariantMap result;
    if(!ready() || !m_snapshot) {return result;}
    const auto &s=*m_snapshot;
    const auto counts=layoutCounts(s);
    // Layout ops are exclusive through an agent (its lease rule); reboot
    // params are shown with the same block, although the agent forwards them.
    const auto blocked=rebootBlocked();
    result["blocked"]=blocked;
    // Live params: only the common blocks (reboot params use `blocked`).
    result["paramBlocked"]=liveParamBlocked();
    result["counts"]=QVariantMap{{"loops",counts.loops},{"loopLimit",counts.loopLimit},
                                 {"instances",counts.instances},{"instanceLimit",counts.instanceLimit}};
    result["addLoopBlocked"]=!blocked.isEmpty()?blocked:
        counts.loopLimit>0 && counts.loops>=counts.loopLimit?tr("%1 of %2 loops: remove a loop first").arg(counts.loops).arg(counts.loopLimit):QString();
    result["addInstanceBlocked"]=!blocked.isEmpty()?blocked:
        counts.instanceLimit>0 && counts.instances>=counts.instanceLimit
            ?tr("%1 of %2 instances: remove an instance first").arg(counts.instances).arg(counts.instanceLimit):QString();
    QVariantList changes;
    for(const auto &c:pendingChanges(s,safeMode(),pendingReboot())) {
        changes<<QVariantMap{{"kind",c.kind},{"key",c.key},{"subject",c.subject},{"from",c.from},{"to",c.to},{"note",c.note}};
    }
    result["pendingChanges"]=changes;
    result["selected"]=m_selected;
    result["paramCounts"]=QVariantMap{{"total",m_params.total()},{"changed",m_params.changedCount()},
                                      {"pending",m_params.pendingCount()}};
    result["paramFilter"]=m_paramFilter;
    result["paramFilterText"]=m_paramFilterText;
    const auto node=parseNodeKey(m_selected);
    if(!node) {return result;}
    const auto *entry=anyEntry(s,*node);
    if(!entry) {return result;}
    const auto *configured=configuredEntry(s,*node);
    const bool isLoop=node->first==defs::kLoopTypeId;
    const auto pending=pendingState(s,*node);
    const auto blocker=removeBlocker(s,*node);
    QVariantMap n{{"key",m_selected},{"isLoop",isLoop},{"label",QString::fromStdString(entry->instance.label)},
                  {"typeName",typeName(s,node->first)},{"pending",pending},
                  {"health",configured?configured->instance.state:0xFF},
                  {"healthText",healthText(configured?configured->instance.state:0xFF)}};
    if(isLoop) {
        n["loopId"]=node->second;
        const auto rt=entry->params.find(defs::loop::kRealtime);
        n["realtime"]=rt!=entry->params.end() && valueBits(rt->second)!=0;
        const auto rate=entry->params.find(defs::loop::kRateHz);
        n["rateHz"]=rate!=entry->params.end()?valueBits(rate->second):0;
    } else {
        n["loopId"]=entry->instance.loop;n["loopName"]=loopName(s,entry->instance.loop);
    }
    n["removeBlocked"]=!blocked.isEmpty()?blocked:pending=="remove"?tr("Already removed; Discard all restores it"):blocker;
    n["renameBlocked"]=!blocked.isEmpty()?blocked:pending=="remove"?tr("Removed instances keep their label"):QString();
    result["node"]=n;
    // Wiring and source rows of the selected instance.
    QVariantList wiring,sources,members;
    const auto *c=configured;const auto *p=pendingEntry(s,*node);
    if(const auto t=s.types.find(node->first);t!=s.types.end()) {
        for(const auto &[id,item]:t->second.items) {
            const auto &d=item.descriptor;
            const bool wire=isWiring(d),source=d.isSource();
            if(d.kind!=ItemKind::Param || (!wire && !source)) {continue;}
            const auto key=itemKey(node->first,node->second,id);
            std::optional<uint32_t> now,staged;
            if(c) {if(auto it=c->params.find(id);it!=c->params.end()) {now=valueBits(it->second);}}
            if(p) {if(auto it=p->params.find(id);it!=p->params.end()) {staged=valueBits(it->second);}}
            const auto text=[&](uint32_t v) {return source?sourceText(s,v):wiringText(s,v);};
            // The chip: the ref category, or the source's value type.
            QString chip=QString::fromStdString(d.ref);
            if(source) {
                const auto valueName=d.name+".value";
                for(const auto &[sid,signal]:t->second.items) {
                    if(signal.descriptor.kind==ItemKind::Signal && signal.descriptor.name==valueName) {
                        static const char *names[]={"bool","u8","u16","u32","i32","f32","enum"};
                        const auto vt=size_t(signal.descriptor.value_type);
                        chip=vt<std::size(names)?names[vt]:QString();
                    }
                }
            }
            const auto shown=staged.value_or(now.value_or(0));
            QVariantMap row{{"key",key},{"name",QString::fromStdString(d.name)},{"value",QVariant::fromValue(shown)},
                            {"valueText",text(shown)},{"currentText",now?text(*now):QString()},
                            {"isPending",!c || (staged && now && *staged!=*now)},{"unset",shown==0},
                            {"chip",chip},
                            {"blocked",!blocked.isEmpty()?blocked:!p?tr("Removed instances keep their wiring"):QString()}};
            if(const auto e=m_edits.find(key);e!=m_edits.end()) {
                row["editState"]=e->state;row["editNote"]=e->note;row["editText"]=e->text;
            }
            (source?sources:wiring)<<row;
        }
    }
    if(isLoop) {
        for(const auto &[k,e]:s.pending) {
            if(k.first==defs::kLoopTypeId || e.instance.loop!=node->second) {continue;}
            const auto *ce=configuredEntry(s,k);
            members<<QVariantMap{{"key",nodeKey(k.first,k.second)},{"label",QString::fromStdString(e.instance.label)},
                                 {"typeName",typeName(s,k.first)},{"health",ce?int(ce->instance.state):0xFF},
                                 {"healthText",healthText(ce?ce->instance.state:0xFF)},
                                 {"pending",pendingState(s,k)}};
        }
    }
    result["wiring"]=wiring;result["sources"]=sources;result["members"]=members;
    // A custom panel for the module type (H2f ships them), next to Params.
    if(!isLoop) {
        // The type name comes from the device: never a path.
        const auto type=typeName(s,node->first);
        const auto path=m_panelRoot+"/"+type+"/Panel.qml";
        if(!type.isEmpty() && !type.contains('/') && !type.contains('\\') && !type.contains("..") && QFile::exists(path)) {
            result["panel"]=path.startsWith(":/")?QString("qrc"+path):QUrl::fromLocalFile(path).toString();
        }
    }
    return result;
}

// The Device settings card (system params): the same blocks as Layout, plus
// the two states a SAVE then DISCARD leaves: the device keeps its reboot flag
// with nothing different in GET_PENDING (a), and what was discarded had been
// saved (b), so the next boot still loads it.
QVariantMap LocalConnection::deviceSettings() const {
    QVariantMap result;
    if(!ready() || !m_snapshot) {return result;}
    const bool safe=safeMode();
    const bool listed=!pendingChanges(*m_snapshot,safe,pendingReboot()).empty();
    result["total"]=m_systemParams.total();
    result["pending"]=m_systemParams.pendingCount();
    result["unsavedCount"]=m_systemParams.unsavedCount();
    result["unsaved"]=m_systemParams.unsavedCount()>0 && unsaved();
    result["blocked"]=rebootBlocked();
    result["paramBlocked"]=liveParamBlocked();
    result["rebootNote"]=layoutBlocked().isEmpty() && !exclusiveBlocked().isEmpty()?tr("Needs the control lease"):QString();
    result["safeMode"]=safe;
    // Only after a SAVE: a value staged and set back, or one just staged whose
    // pending value is not read yet, was never saved.
    result["savedThenDiscarded"]=pendingReboot() && !listed && !safe && m_savedPending;
    result["discardAfterSave"]=listed && !unsaved();
    return result;
}

QVariantList LocalConnection::layoutCandidates(const QString &key) const {
    const auto ref=parseItemKey(key);
    if(!ready() || !m_snapshot || !ref) {return {};}
    const auto *item=findItem(*m_snapshot,*ref);
    if(!item) {return {};}
    const InstanceKey consumer{ref->type,ref->instance};
    if(item->descriptor.isSource()) {return candidateList(sourceCandidates(*m_snapshot,consumer,item->descriptor));}
    if(isWiring(item->descriptor)) {return candidateList(wiringCandidates(*m_snapshot,consumer,item->descriptor));}
    return {};
}

QVariantList LocalConnection::addInstanceTypes() const {
    QVariantList result;
    if(!ready() || !m_snapshot) {return result;}
    for(const auto &[id,type]:m_snapshot->types) {
        // System and loop run in no loop (mask zero): not module types.
        if(!type.descriptor.loop_mask) {continue;}
        QVariantList loops;
        for(const auto &l:loopChoices(*m_snapshot,id)) {
            loops<<QVariantMap{{"id",l.id},{"name",l.name},{"rate",l.rate},{"realtime",l.realtime},{"compatible",l.compatible}};
        }
        const auto def=defaultLoop(*m_snapshot,id);
        result<<QVariantMap{{"type",id},{"name",QString::fromStdString(type.descriptor.name)},
                            {"need",typeNeed(type.descriptor.loop_mask)},{"loops",loops},
                            {"defaultLoop",def?int(*def):-1}};
    }
    return result;
}
QVariantList LocalConnection::addLoopFields() const {
    QVariantList result;
    if(!ready() || !m_snapshot) {return result;}
    const auto t=m_snapshot->types.find(defs::kLoopTypeId);
    if(t==m_snapshot->types.end()) {return result;}
    for(const auto &[id,item]:t->second.items) {
        const auto &d=item.descriptor;
        if(d.kind!=ItemKind::Param || flag(d,defs::ItemFlag::ReadOnly)) {continue;}
        QVariantList enums;
        for(const auto &e:item.enums) {enums<<QVariantMap{{"value",e.value},{"name",QString::fromStdString(e.name)}};}
        result<<QVariantMap{{"id",id},{"name",QString::fromStdString(d.name)},{"unit",QString::fromStdString(d.unit)},
            {"editor",d.value_type==ValueType::Bool?"bool":d.value_type==ValueType::Enum?"enum":"number"},
            {"value",formatValue(item,d.default_bits)},{"bits",QVariant::fromValue(d.default_bits)},
            {"range",rangeText(item)},{"enums",enums},{"description",QString::fromStdString(d.description)}};
    }
    return result;
}

void LocalConnection::setParamText(const QString &key,const QString &text) {
    const auto ref=parseItemKey(key);
    if(!ready() || !m_snapshot || !ref) {return;}
    const auto *item=findItem(*m_snapshot,*ref);
    if(!item || item->descriptor.kind!=ItemKind::Param) {return;}
    const auto parsed=parseValueText(*item,text);
    if(!parsed.bits) {
        m_edits[key]={"rejected",parsed.problem,text};updateParams();emit changed();return;
    }
    sendParam(*ref,*parsed.bits,text);
}
void LocalConnection::setParamValue(const QString &key,quint32 bits) {
    const auto ref=parseItemKey(key);
    if(!ready() || !m_snapshot || !ref) {return;}
    const auto *item=findItem(*m_snapshot,*ref);
    if(!item || item->descriptor.kind!=ItemKind::Param) {return;}
    const auto &d=item->descriptor;
    sendParam(*ref,bits,d.isSource()?sourceText(*m_snapshot,bits):isWiring(d)?wiringText(*m_snapshot,bits):formatValue(*item,bits));
}
void LocalConnection::sendParam(const Ref &ref,uint32_t bits,const QString &text) {
    const auto key=itemKey(ref.type,ref.instance,ref.item);
    const auto *item=findItem(*m_snapshot,ref);
    const auto &d=item->descriptor;
    // A system param has no instance: only its reboot flag stages it.
    const bool system=ref.type==defs::kSystemTypeId;
    const bool staged=flag(d,defs::ItemFlag::RebootRequired) || (!system && !configuredEntry(*m_snapshot,{ref.type,ref.instance}));
    auto blocked=liveParamBlocked();
    // Reboot params are layout configuration: locked while armed like the
    // layout ops (G7b), although the device would stage them, and held back
    // for an observer of another client's lease (the card shows the reason).
    if(blocked.isEmpty() && staged) {blocked=rebootBlocked();}
    if(blocked.isEmpty() && (armState()==1 || m_arming) && flag(d,defs::ItemFlag::LockedWhileArmed)) {
        blocked=tr("Locked while armed; disarm first");
    }
    if(blocked.isEmpty() && flag(d,defs::ItemFlag::ReadOnly)) {blocked=tr("This value is read-only");}
    if(const auto it=m_edits.find(key);blocked.isEmpty() && it!=m_edits.end() && it->state=="sending") {
        blocked=tr("The previous value is still being sent");
    }
    if(!blocked.isEmpty()) {m_edits[key]={"rejected",blocked,text};updateParams();emit changed();return;}
    // The values the edit starts from: the pending ones for a reboot param.
    const auto *entry=system?nullptr:anyEntry(*m_snapshot,{ref.type,ref.instance});
    const auto &values=system?(staged && m_snapshot->pending_system_params.contains(ref.item)?m_snapshot->pending_system_params
                                                                                             :m_snapshot->system_params)
                             :entry->params;
    const auto subject=(system?QString("system"):QString::fromStdString(entry->instance.label))+"."+QString::fromStdString(d.name);
    QString before;
    if(const auto it=values.find(ref.item);it!=values.end()) {
        before=d.isSource()?sourceText(*m_snapshot,valueBits(it->second)):isWiring(d)?wiringText(*m_snapshot,valueBits(it->second))
                          :formatValue(*item,valueBits(it->second));
    }
    const auto after=d.isSource()?sourceText(*m_snapshot,bits):isWiring(d)?wiringText(*m_snapshot,bits):formatValue(*item,bits);
    // Another client holds the lease: the operator confirms first (G6);
    // nothing is sent until then.
    if(holdLiveEdit(key,tr("%1: %2 to %3").arg(subject,before.isEmpty()?tr("(unset)"):before,after),
                    [this,ref,bits,text,generation=m_snapshot->generation] {
                        if(m_snapshot && m_snapshot->generation==generation && findItem(*m_snapshot,ref)) {sendParam(ref,bits,text);}
                    },
                    [this,key] {m_edits.remove(key);updateParams();emit changed();})) {
        return;
    }
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session) {return;}
    const QPointer<LocalConnection> self(this);
    const auto id=deviceId();const auto generation=m_snapshot->generation;
    const auto snapshot=m_snapshot;const auto modelItem=*item;
    m_edits[key]={"sending",{},text};updateParams();emit changed();
    // Manual command (hold rule, user decisions 2026-11-18): an instance
    // this GUI commands goes to 0 and takes no command while the write is in
    // flight, and its queued values are discarded first; when the write ends,
    // whatever the outcome, its group sends a fresh frame with 0 for it.
    const bool commanded=!system && m_command.commands(ref.type,ref.instance);
    if(commanded) {discardCommands(ref.type,ref.instance);m_command.beginWrite(ref.type,ref.instance);}
    try {
        Client(*session).async([ref,bits](Client &client) {
            const std::array refs{ref};const std::array values{bits};
            return client.set(refs,values);
        },[self,c,key,ref,bits,text,id,generation,snapshot,modelItem,subject,before,after,commanded](std::expected<ParamResult,Error> result) {
            if(!self || !self->current(c)) {return;}
            const bool sameBoot=self->m_snapshot && self->m_snapshot->generation==generation;
            // After a Boot the card was set up again: this write began on the
            // old groups and must not end a newer write's hold.
            if(commanded && sameBoot) {self->m_command.endWrite(ref.type,ref.instance);}
            if(!result) {
                const auto &e=result.error();
                if(sameBoot) {
                    self->m_edits[key]={e.outcome_unknown?"uncertain":"rejected",
                        e.outcome_unknown?tr("No reply in time; the device may have applied it. Shown as read back.")
                                         :layoutRefusal(*snapshot,e,ref,bits),text};
                }
                if(e.outcome_unknown) {self->refreshConfig();}
            } else {
                const auto applied=result->values.empty()?bits:result->values.front();
                if(sameBoot) {
                    const auto line=tr("%1: %2 to %3").arg(subject,before,applied!=bits?formatValue(modelItem,applied):after);
                    // A staged value shows as Pending; only live changes get
                    // the Unsaved marker.
                    if(result->pending_reboot) {self->m_edits.remove(key);self->noteStaged(id,line);}
                    else {
                        if(applied!=bits) {
                            self->m_edits[key]={"clamped",tr("Clamped to %1").arg(formatValue(modelItem,applied)),{}};
                        } else {self->m_edits[key]={"applied",{},{}};}
                        self->m_changedParams[id].insert(key);self->m_changes[id]<<line;
                    }
                }
                self->refreshConfig();
            }
            self->updateParams();emit self->changed();
        });
    } catch(const Error &e) {
        // Not sent: the write ends here (a fresh frame with 0 for the instance).
        if(commanded) {m_command.endWrite(ref.type,ref.instance);}
        m_edits[key]={"rejected",errorText(e),text};updateParams();emit changed();
    }
}

void LocalConnection::runLayoutOp(const QString &op,std::function<int(Client &)> work,std::function<QString(int)> done,
                                  std::optional<InstanceKey> removed) {
    if(const auto blocked=rebootBlocked();!blocked.isEmpty()) {
        emit layoutResult(op,false,blocked);return;
    }
    auto c=m_context;auto session=c?c->getSession():nullptr;
    if(!session || !m_snapshot) {emit layoutResult(op,false,tr("Not connected"));return;}
    const QPointer<LocalConnection> self(this);
    const auto snapshot=m_snapshot;
    m_configAction="layout";emit changed();
    try {
        Client(*session).async([work=std::move(work)](Client &client) {return work(client);},
            [self,c,op,done=std::move(done),snapshot,removed](std::expected<int,Error> result) {
            if(!self || !self->current(c)) {return;}
            self->m_configAction.clear();
            // A Boot between request and reply (a link that stays open) drops
            // the staged layout: never report it as staged.
            if(!self->m_snapshot || self->m_snapshot->generation!=snapshot->generation) {
                const auto message=tr("The device rebooted; the layout change was not kept");
                emit self->layoutResult(op,false,message);emit self->notification(message,2);emit self->changed();return;
            }
            if(!result) {
                const auto &e=result.error();
                const auto message=e.outcome_unknown?tr("No reply in time; the device may have applied it. The layout is read back.")
                                                    :layoutRefusal(*snapshot,e,{},{},removed);
                if(e.outcome_unknown) {self->refreshConfig();}
                emit self->layoutResult(op,false,message);
                if(op=="remove" || op=="discard") {emit self->notification(message,2);}
            } else {
                const auto message=done(*result);
                self->refreshConfig();
                emit self->layoutResult(op,true,message);
                emit self->notification(message,1);
            }
            emit self->changed();
        });
    } catch(const Error &e) {
        m_configAction.clear();emit layoutResult(op,false,errorText(e));emit changed();
    }
}

void LocalConnection::addInstance(int type,const QString &label,int loop) {
    if(type<=0 || type>255 || loop<0 || loop>255) {return;}
    if(const auto problem=nameProblem(label);!problem.isEmpty() || label.isEmpty()) {
        emit layoutResult("addInstance",false,label.isEmpty()?tr("Enter a label"):problem);return;
    }
    const auto utf8=label.toStdString();const auto id=deviceId();const QPointer<LocalConnection> self(this);
    const auto name=m_snapshot?typeName(*m_snapshot,uint8_t(type)):QString();
    runLayoutOp("addInstance",[type,utf8,loop](Client &client) {
        return int(client.add(uint8_t(type),utf8,uint8_t(loop)).instance);
    },[self,type,label,id,name](int instance) {
        if(self) {
            self->m_selected=nodeKey(uint8_t(type),uint8_t(instance));
            self->noteStaged(id,tr("Added %1 (%2)").arg(label,name));
        }
        return tr("%1 added; it starts after Save and reboot").arg(label);
    });
}
void LocalConnection::addLoop(const QString &label,const QVariantMap &fields) {
    if(const auto problem=nameProblem(label);!problem.isEmpty() || label.isEmpty()) {
        emit layoutResult("addLoop",false,label.isEmpty()?tr("Enter a label"):problem);return;
    }
    if(!m_snapshot) {return;}
    const auto t=m_snapshot->types.find(defs::kLoopTypeId);
    if(t==m_snapshot->types.end()) {return;}
    // Only values that differ from the descriptor default are set.
    std::vector<std::pair<uint16_t,uint32_t>> values;
    for(auto it=fields.begin();it!=fields.end();++it) {
        bool ok=false;const auto id=it.key().toUInt(&ok);
        const auto item=ok?t->second.items.find(uint16_t(id)):t->second.items.end();
        if(item==t->second.items.end()) {continue;}
        const auto parsed=parseValueText(item->second,it.value().toString());
        if(!parsed.bits) {
            emit layoutResult("addLoop",false,tr("%1: %2").arg(QString::fromStdString(item->second.descriptor.name),parsed.problem));
            return;
        }
        if(*parsed.bits!=item->second.descriptor.default_bits) {values.emplace_back(uint16_t(id),*parsed.bits);}
    }
    const auto utf8=label.toStdString();const auto id=deviceId();const QPointer<LocalConnection> self(this);
    auto failure=std::make_shared<QString>();
    runLayoutOp("addLoop",[utf8,values,failure](Client &client) {
        const auto added=client.add(defs::kLoopTypeId,utf8,0xFF);
        if(!values.empty()) {
            std::vector<Ref> refs;std::vector<uint32_t> bits;
            for(const auto &[item,v]:values) {refs.push_back({defs::kLoopTypeId,added.instance,item});bits.push_back(v);}
            // The loop is staged already; a refused SET leaves it with the
            // defaults and says so (Discard all undoes the add).
            try {client.set(refs,bits);} catch(const Error &e) {*failure=errorText(e);}
        }
        return int(added.instance);
    },[self,label,id,failure](int instance) {
        if(self) {
            self->m_selected=nodeKey(defs::kLoopTypeId,uint8_t(instance));
            self->noteStaged(id,tr("Added loop %1").arg(label));
            if(!failure->isEmpty()) {
                emit self->notification(tr("Loop %1 was added with default settings; its settings were refused: %2").arg(label,*failure),2);
            }
        }
        return tr("Loop %1 added; it starts after Save and reboot").arg(label);
    });
}
void LocalConnection::removeNode(const QString &key) {
    const auto node=parseNodeKey(key);
    if(!node || !m_snapshot) {return;}
    const auto *e=anyEntry(*m_snapshot,*node);
    if(!e) {return;}
    // The device refuses a referenced remove; say why without sending.
    if(const auto why=removeBlocker(*m_snapshot,*node);!why.isEmpty()) {
        emit layoutResult("remove",false,why);emit notification(why,2);return;
    }
    const auto label=QString::fromStdString(e->instance.label);const auto id=deviceId();const QPointer<LocalConnection> self(this);
    runLayoutOp("remove",[n=*node](Client &client) {client.remove(n.first,n.second);return 0;},
        [self,label,id](int) {
            if(self) {self->noteStaged(id,tr("Removed %1").arg(label));}
            return tr("%1 will be removed at Save and reboot").arg(label);
        },*node);
}
void LocalConnection::setNodeLabel(const QString &key,const QString &label) {
    const auto node=parseNodeKey(key);
    if(!node || !m_snapshot) {return;}
    if(const auto problem=nameProblem(label);!problem.isEmpty() || label.isEmpty()) {
        emit layoutResult("label",false,label.isEmpty()?tr("Enter a label"):problem);return;
    }
    const auto *e=anyEntry(*m_snapshot,*node);
    if(!e) {return;}
    const auto old=QString::fromStdString(e->instance.label);
    if(old==label) {emit layoutResult("label",true,{});return;}
    const auto utf8=label.toStdString();const auto id=deviceId();const QPointer<LocalConnection> self(this);
    runLayoutOp("label",[n=*node,utf8](Client &client) {client.label(n.first,n.second,utf8);return 0;},
        [self,old,label,id](int) {
            if(self) {self->noteStaged(id,tr("Label: \"%1\" to \"%2\"").arg(old,label));}
            return tr("%1 will be named %2 after Save and reboot").arg(old,label);
        });
}
void LocalConnection::discardPending() {
    const auto id=deviceId();const QPointer<LocalConnection> self(this);
    runLayoutOp("discard",[](Client &client) {client.discard();return 0;},[self,id](int) {
        if(self) {
            // Live changes stay; only the staged layout and reboot params went.
            for(const auto &line:self->m_stagedLines.take(id)) {self->m_changes[id].removeOne(line);}
            // Kept refusals go; a send still in flight keeps its row for its
            // completion, and a live settings row has nothing staged to drop.
            for(auto it=self->m_edits.begin();it!=self->m_edits.end();) {
                const auto ref=parseItemKey(it.key());
                const auto *item=ref && self->m_snapshot?findItem(*self->m_snapshot,*ref):nullptr;
                const bool liveSetting=ref && ref->type==defs::kSystemTypeId && item && !flag(item->descriptor,defs::ItemFlag::RebootRequired);
                if(it->state!="sending" && !liveSetting) {it=self->m_edits.erase(it);} else {++it;}
            }
            self->updateParams();
        }
        return tr("Pending changes discarded");
    });
}
} // namespace drivelink::gui
