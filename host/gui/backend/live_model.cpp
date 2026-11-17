#include "live_model.hpp"
#include "device_models.hpp"
#include "layout_model.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <QCoreApplication>
#include <algorithm>
#include <cctype>
#include <set>

namespace drivelink::gui {
namespace defs=protocol::defs;
namespace loopdefs=protocol::defs::loop;
namespace {
QString tr(const char *text) { return QCoreApplication::translate("DriveLink", text); }
QString str(const std::string &s) { return QString::fromStdString(s); }
const ModelType *typeOf(const DeviceSnapshot &s, uint8_t type) {
    const auto it=s.types.find(type);return it==s.types.end()?nullptr:&it->second;
}
const LoopInfo *runningLoop(const DeviceSnapshot &s, uint8_t id) {
    const auto it=std::find_if(s.info.loops.begin(),s.info.loops.end(),[&](const LoopInfo &l){return l.id==id;});
    return it==s.info.loops.end()?nullptr:&*it;
}
// The loop an instance runs in; a loop instance's statistics belong to its own id.
std::optional<uint8_t> loopOf(const DeviceSnapshot &s, InstanceKey k) {
    if(k.first==defs::kLoopTypeId) {return k.second;}
    if(const auto *c=configuredEntry(s,k)) {return c->instance.loop;}
    return std::nullopt;
}
const ModelItem *signalNamed(const ModelType &type, const std::string &name) {
    for(const auto &[id,item]:type.items) {
        if(item.descriptor.kind==ItemKind::Signal && item.descriptor.name==name) {return &item;}
    }
    return nullptr;
}
// The generated `<source>.value` / `<source>.age_us` signals of a type.
std::set<uint16_t> generatedSignals(const ModelType &type) {
    std::set<uint16_t> ids;
    for(const auto &[id,item]:type.items) {
        if(item.descriptor.kind!=ItemKind::Param || !item.descriptor.isSource()) {continue;}
        for(const char *suffix:{".value",".age_us"}) {
            if(const auto *signal=signalNamed(type,item.descriptor.name+suffix)) {ids.insert(signal->descriptor.id);}
        }
    }
    return ids;
}
ModelItem scalarItem(const ScalarDescriptor &d) {
    ModelItem item;item.descriptor.kind=ItemKind::Param;item.descriptor.value_type=d.value_type;
    item.descriptor.name=d.name;item.descriptor.unit=d.unit;return item;
}
QString typeText(ValueType t) {
    static const char *names[]={"bool","u8","u16","u32","i32","f32","enum"};
    const auto i=size_t(t);return i<std::size(names)?names[i]:QString();
}
QString withUnit(const QString &value, const std::string &unit) {
    return unit.empty()?value:value+" "+str(unit);
}
std::optional<size_t> indexOf(const LivePlan *plan, Ref ref) {
    if(!plan) {return std::nullopt;}
    const auto it=std::find(plan->refs.begin(),plan->refs.end(),ref);
    if(it==plan->refs.end()) {return std::nullopt;}
    return size_t(it-plan->refs.begin());
}
std::optional<uint32_t> sampled(const LivePlan *plan, const LiveValues *live, Ref ref) {
    const auto i=indexOf(plan,ref);
    if(!i || !live || !live->fresh || *i>=live->values.size()) {return std::nullopt;}
    return live->values[*i];
}
uint32_t loopRate(const DeviceSnapshot &s, uint8_t loop) {
    if(const auto *l=runningLoop(s,loop)) {return l->rate_hz;}
    return 0;
}
QString rateText(uint32_t hz) {
    return hz>=1000 && hz%100==0?QString::number(hz/1000.0)+" kHz":QString::number(hz)+" Hz";
}
QString usText(uint32_t us) { return QString::number(us)+QString::fromUtf8(" µs"); }
}

QString notRunning(const DeviceSnapshot &s, InstanceKey k) {
    if(s.safeMode()) {return tr("The device is in safe mode");}
    const auto *c=configuredEntry(s,k);
    if(!c) {return pendingEntry(s,k)?tr("Starts after Save and reboot"):tr("Not in the layout");}
    if(k.first==defs::kLoopTypeId) {
        return runningLoop(s,k.second)?QString():tr("%1 is not running").arg(str(c->instance.label));
    }
    if(c->instance.state!=0) {return tr("%1 is %2").arg(str(c->instance.label),healthText(c->instance.state));}
    if(!runningLoop(s,c->instance.loop)) {return tr("%1 is not running: its loop is not running").arg(str(c->instance.label));}
    return {};
}

std::optional<LivePlan> livePlan(const DeviceSnapshot &s, InstanceKey k, size_t limit) {
    if(!notRunning(s,k).isEmpty()) {return std::nullopt;}
    const auto *type=typeOf(s,k.first);const auto loop=loopOf(s,k);
    if(!type || !loop) {return std::nullopt;}
    LivePlan plan;plan.loop=*loop;
    plan.divider=uint16_t(std::clamp<uint32_t>(loopRate(s,*loop)/kLiveRateHz,1,65535));
    if(k.first==defs::kLoopTypeId) {
        for(const auto id:{loopdefs::kTicks,loopdefs::kOverruns,loopdefs::kLastRunUs,loopdefs::kMaxRunUs}) {
            if(type->items.contains(id) && plan.refs.size()<limit) {plan.refs.push_back({k.first,k.second,id});}
        }
        if(plan.refs.empty()) {return std::nullopt;}
        return plan;
    }
    for(const auto &[id,item]:type->items) {
        if(item.descriptor.kind!=ItemKind::Param || !item.descriptor.isSource()) {continue;}
        const auto *value=signalNamed(*type,item.descriptor.name+".value");
        const auto *age=signalNamed(*type,item.descriptor.name+".age_us");
        if(!value || !age || plan.refs.size()+2>limit) {continue;}
        plan.refs.push_back({k.first,k.second,value->descriptor.id});
        plan.refs.push_back({k.first,k.second,age->descriptor.id});
    }
    const auto generated=generatedSignals(*type);
    for(const auto &[id,item]:type->items) {
        if(item.descriptor.kind!=ItemKind::Signal || generated.contains(id)) {continue;}
        if(plan.refs.size()<limit) {plan.refs.push_back({k.first,k.second,id});}
        else {++plan.hidden;}
    }
    if(plan.refs.empty()) {return std::nullopt;}
    return plan;
}

QVariantList signalRows(const DeviceSnapshot &s, InstanceKey k, const LivePlan *plan, const LiveValues *live) {
    QVariantList rows;
    const auto *type=typeOf(s,k.first);
    if(!type || k.first==defs::kLoopTypeId) {return rows;}
    const auto generated=generatedSignals(*type);
    for(const auto &[id,item]:type->items) {
        if(item.descriptor.kind!=ItemKind::Signal || generated.contains(id)) {continue;}
        const Ref ref{k.first,k.second,id};
        const auto value=sampled(plan,live,ref);
        rows<<QVariantMap{{"key",itemKey(k.first,k.second,id)},{"name",str(item.descriptor.name)},
                          {"unit",str(item.descriptor.unit)},{"subscribed",indexOf(plan,ref).has_value()},
                          {"value",value?formatValue(item,*value):QString()}};
    }
    return rows;
}

QVariantMap loopStatistics(const DeviceSnapshot &s, uint8_t loop, const LivePlan *plan, const LiveValues *live) {
    const auto at=[&](uint16_t id) {return sampled(plan,live,{defs::kLoopTypeId,loop,id});};
    const auto ticks=at(loopdefs::kTicks),overruns=at(loopdefs::kOverruns);
    const auto last=at(loopdefs::kLastRunUs),longest=at(loopdefs::kMaxRunUs);
    const auto rate=loopRate(s,loop);
    QVariantMap result{{"has",ticks || overruns || last || longest}};
    if(ticks) {result["ticks"]=QString::number(*ticks);}
    if(overruns) {result["overruns"]=QString::number(*overruns);}
    if(last) {result["lastRun"]=usText(*last);}
    if(longest) {result["longestRun"]=usText(*longest);}
    if(last && rate) {
        const auto budget=LoopTable::budget(*last,rate);
        result["budgetValue"]=budget;
        result["budget"]=tr("%1% of %2").arg(budget*100,0,'f',1).arg(usText(uint32_t(1000000/rate)));
    }
    return result;
}

uint32_t staleThresholdUs(const DeviceSnapshot &s, uint32_t value) {
    const InstanceKey producer{defs::sourceRefType(value),defs::sourceRefInstance(value)};
    const auto loop=loopOf(s,producer);
    const auto rate=loop?loopRate(s,*loop):0;
    return rate?uint32_t(uint64_t(kStalePeriods)*1000000/rate):0;
}
QString ageText(uint32_t us) {
    if(us<1000) {return usText(us);}
    if(us<10000) {return QString::number(us/1000.0,'f',1)+" ms";}
    if(us<1000000) {return QString::number(us/1000)+" ms";}
    return QString::number(us/1000000.0,'f',1)+" s";
}

SourceLive sourceLive(const DeviceSnapshot &s, InstanceKey consumer, const ItemDescriptor &param,
                      std::optional<uint32_t> value, std::optional<uint32_t> age, bool telemetry) {
    const auto *c=configuredEntry(s,consumer);const auto *p=pendingEntry(s,consumer);
    const auto bits=[&](const ModelInstance *e)->std::optional<uint32_t> {
        if(!e) {return std::nullopt;}
        const auto it=e->params.find(param.id);
        return it==e->params.end()?std::nullopt:std::optional<uint32_t>(valueBits(it->second));
    };
    const auto now=bits(c),staged=bits(p);
    if(!c || !now) {return {"pending",{},{},tr("Not live until Save and reboot")};}
    // The running target still feeds the consumer; a staged one is never shown live.
    if(staged && *staged!=*now) {return {"pending",{},{},tr("Not live: the new target applies after Save and reboot")};}
    if(*now==0) {return {"notConnected",{},{},{}};}
    const InstanceKey producer{defs::sourceRefType(*now),defs::sourceRefInstance(*now)};
    if(!notRunning(s,producer).isEmpty()) {
        const auto *e=anyEntry(s,producer);
        const auto name=e?str(e->instance.label):sourceText(s,*now);
        return {"missing",{},{},tr("Missing · %1 is not running").arg(name)};
    }
    if(!telemetry || !value || !age) {return {"unavailable",{},{},tr("Telemetry unavailable")};}
    if(*age==kNoSample) {return {"waiting",{},{},tr("No valid sample")};}
    QString valueText;
    if(const auto *type=typeOf(s,consumer.first)) {
        if(const auto *signal=signalNamed(*type,param.name+".value")) {
            valueText=withUnit(formatValue(*signal,*value),signal->descriptor.unit.empty()?param.unit:signal->descriptor.unit);
        }
    }
    const auto threshold=staleThresholdUs(s,*now);
    if(threshold && *age>threshold) {
        const auto loop=loopOf(s,producer);
        return {"stale",valueText,ageText(*age),tr("Stale · No recent sample · threshold %1 (%2 × %3 periods)")
                    .arg(ageText(threshold)).arg(kStalePeriods).arg(rateText(loop?loopRate(s,*loop):0))};
    }
    return {"valid",valueText,ageText(*age),{}};
}

QVariantMap sourceStates(const DeviceSnapshot &s, InstanceKey k, const LivePlan *plan, const LiveValues *live) {
    QVariantMap result;
    const auto *type=typeOf(s,k.first);
    if(!type) {return result;}
    const bool telemetry=plan && live && live->fresh;
    for(const auto &[id,item]:type->items) {
        const auto &d=item.descriptor;
        if(d.kind!=ItemKind::Param || !d.isSource()) {continue;}
        const auto *value=signalNamed(*type,d.name+".value");const auto *age=signalNamed(*type,d.name+".age_us");
        const auto v=value?sampled(plan,live,{k.first,k.second,value->descriptor.id}):std::nullopt;
        const auto a=age?sampled(plan,live,{k.first,k.second,age->descriptor.id}):std::nullopt;
        const auto state=sourceLive(s,k,d,v,a,telemetry);
        result[itemKey(k.first,k.second,id)]=QVariantMap{{"state",state.state},{"valueText",state.valueText},
                                                        {"ageText",state.ageText},{"note",state.note}};
    }
    return result;
}

ParsedArgs parseActionArgs(const ItemDescriptor &d, const QStringList &texts) {
    ParsedArgs result;
    if(d.args.size()>result.bits.size()) {result.problem=tr("The action describes more than four arguments");return result;}
    for(size_t i=0;i<d.args.size();++i) {
        const auto parsed=parseValueText(scalarItem(d.args[i]),texts.value(int(i)));
        if(!parsed.bits) {
            result.problem=tr("%1: %2").arg(str(d.args[i].name),parsed.problem);result.field=int(i);return result;
        }
        result.bits[i]=*parsed.bits;
    }
    return result;
}
ParsedBlob parseHexBlob(const QString &text) {
    QByteArray digits;
    for(const auto ch:text) {
        if(ch.isSpace()) {continue;}
        if(ch.unicode()>127 || !std::isxdigit(ch.unicode())) {return {std::nullopt,tr("Use hex digits 0-9 and a-f")};}
        digits.append(char(ch.unicode()));
    }
    if(digits.size()%2) {return {std::nullopt,tr("Enter whole bytes: an even number of hex digits")};}
    const auto bytes=QByteArray::fromHex(digits);
    if(bytes.size()>512) {return {std::nullopt,tr("At most 512 bytes (%1 now)").arg(bytes.size())};}
    return {bytes,{}};
}
QString hexText(const QByteArray &bytes) { return QString::fromLatin1(bytes.toHex(' ')); }
QVariantList actionResultRows(const ItemDescriptor &d, const std::array<uint32_t,4> &values) {
    QVariantList rows;
    for(size_t i=0;i<d.results.size() && i<values.size();++i) {
        rows<<QVariantMap{{"name",str(d.results[i].name)},{"unit",str(d.results[i].unit)},
                          {"text",formatValue(scalarItem(d.results[i]),values[i])}};
    }
    return rows;
}

QString actionBlock(const DeviceSnapshot &s, InstanceKey k, const ItemDescriptor &d, bool armed,
                    const QString &common, bool anotherRunning) {
    if(!common.isEmpty()) {return common;}
    const auto pending=pendingState(s,k);
    if(pending=="add") {return tr("Starts after Save and reboot; actions need a running instance");}
    if(pending=="remove") {return tr("Removed at the next reboot; its actions are off");}
    if(const auto why=notRunning(s,k);!why.isEmpty()) {return why;}
    if(d.runs_in==ActionPlacement::Paused && armed) {
        const auto loop=loopOf(s,k);const auto *l=loop?runningLoop(s,*loop):nullptr;
        // The device refuses paused actions on a real-time loop while armed.
        if(l && l->realtime()) {return tr("Paused actions on a real-time loop are refused while armed; disarm first");}
    }
    if(anotherRunning) {return tr("Another action is running");}
    return {};
}
QVariantList actionRows(const DeviceSnapshot &s, InstanceKey k, bool armed, const QString &common, bool anotherRunning) {
    QVariantList rows;
    const auto *type=typeOf(s,k.first);
    if(!type) {return rows;}
    for(const auto &[id,item]:type->items) {
        const auto &d=item.descriptor;
        if(d.kind!=ItemKind::Action) {continue;}
        QVariantList args,results;
        for(const auto &a:d.args) {args<<QVariantMap{{"name",str(a.name)},{"unit",str(a.unit)},{"type",typeText(a.value_type)}};}
        for(const auto &r:d.results) {results<<QVariantMap{{"name",str(r.name)},{"unit",str(r.unit)}};}
        static const char *blobs[]={"none","in","out","both"};
        rows<<QVariantMap{{"key",itemKey(k.first,k.second,id)},{"name",str(d.name)},{"description",str(d.description)},
                          {"placement",d.runs_in==ActionPlacement::Paused?"paused":"loop"},{"args",args},{"results",results},
                          {"blob",blobs[size_t(d.blob_direction)&3]},
                          {"blocked",actionBlock(s,k,d,armed,common,anotherRunning)}};
    }
    return rows;
}

QHash<int,QByteArray> ActionTable::roleNames() const {
    return {{Key,"key"},{Name,"name"},{Description,"description"},{Placement,"placement"},{Args,"args"},
            {Results,"results"},{Blob,"blob"},{Blocked,"blocked"},{Running,"running"},{Result,"result"}};
}
QVariant ActionTable::data(const QModelIndex &i,int role) const {
    if(!i.isValid() || i.row()>=int(m_rows.size())) {return {};}
    const auto name=roleNames().value(role);
    return name.isEmpty()?QVariant():m_rows[size_t(i.row())].value(QString::fromLatin1(name));
}
void ActionTable::update(const QVariantList &rows) {
    bool same=rows.size()==qsizetype(m_rows.size());
    for(qsizetype i=0;same && i<rows.size();++i) {same=rows[i].toMap()["key"]==m_rows[size_t(i)]["key"];}
    if(!same) {
        beginResetModel();m_rows.clear();
        for(const auto &r:rows) {m_rows.push_back(r.toMap());}
        endResetModel();return;
    }
    for(qsizetype i=0;i<rows.size();++i) {
        auto map=rows[i].toMap();
        if(map!=m_rows[size_t(i)]) {m_rows[size_t(i)]=std::move(map);emit dataChanged(index(int(i)),index(int(i)));}
    }
}
QVariantMap ActionTable::row(const QString &key) const {
    for(const auto &r:m_rows) {if(r["key"].toString()==key) {return r;}}
    return {};
}
} // namespace drivelink::gui
