#include "layout_model.hpp"
#include "device_models.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QCoreApplication>
#include <algorithm>
#include <bit>
#include <charconv>
#include <map>
#include <set>

namespace drivelink::gui {
namespace defs=protocol::defs;
namespace loopdefs=protocol::defs::loop;
namespace {
QString tr(const char *text) { return QCoreApplication::translate("DriveLink", text); }
QString str(const std::string &s) { return QString::fromStdString(s); }
bool flag(const ItemDescriptor &d, defs::ItemFlag f) { return (d.flags & uint8_t(f))!=0; }
std::optional<uint32_t> paramBits(const ModelInstance *e, uint16_t id) {
    if(!e) {return std::nullopt;}
    const auto it=e->params.find(id);
    if(it==e->params.end()) {return std::nullopt;}
    return valueBits(it->second);
}
const ModelType *typeOf(const DeviceSnapshot &s, uint8_t type) {
    const auto it=s.types.find(type);return it==s.types.end()?nullptr:&it->second;
}
const ModelItem *itemOf(const DeviceSnapshot &s, uint8_t type, uint16_t id) {
    const auto *t=typeOf(s,type);if(!t) {return nullptr;}
    const auto it=t->items.find(id);return it==t->items.end()?nullptr:&it->second;
}
QString label(const ModelInstance &e) { return str(e.instance.label); }
QString category(const std::string &name) {
    const auto dot=name.find('.');return dot==std::string::npos?QString():str(name.substr(0,dot));
}
// The generated `<name>.value` signal of a source param.
const ModelItem *sourceValue(const ModelType &type, const ItemDescriptor &param) {
    const auto name=param.name+".value";
    for(const auto &[id,item]:type.items) {
        if(item.descriptor.kind==ItemKind::Signal && item.descriptor.name==name) {return &item;}
    }
    return nullptr;
}
// Wiring/source values render as their target; everything else as a value.
QString displayValue(const DeviceSnapshot &s, const ModelItem &item, uint32_t bits) {
    if(item.descriptor.isSource()) {return sourceText(s,bits);}
    if(isWiring(item.descriptor)) {return wiringText(s,bits);}
    return formatValue(item,bits);
}
uint32_t systemParam(const DeviceSnapshot &s, uint16_t id) {
    const auto it=s.system_params.find(id);return it==s.system_params.end()?0:valueBits(it->second);
}
bool loopRealtime(const ModelInstance &loop) {
    const auto bits=paramBits(&loop,loopdefs::kRealtime);return bits && *bits;
}
uint32_t loopRate(const ModelInstance &loop) {
    return paramBits(&loop,loopdefs::kRateHz).value_or(0);
}
} // namespace

QString nodeKey(uint8_t t,uint8_t i) { return QString::number(t)+":"+QString::number(i); }
QString itemKey(uint8_t t,uint8_t i,uint16_t id) { return nodeKey(t,i)+":"+QString::number(id); }
std::optional<InstanceKey> parseNodeKey(const QString &key) {
    const auto parts=key.split(':');if(parts.size()!=2) {return std::nullopt;}
    bool a=false,b=false;const auto t=parts[0].toUInt(&a),i=parts[1].toUInt(&b);
    if(!a || !b || t>255 || i>255) {return std::nullopt;}
    return InstanceKey{uint8_t(t),uint8_t(i)};
}
std::optional<Ref> parseItemKey(const QString &key) {
    const auto parts=key.split(':');if(parts.size()!=3) {return std::nullopt;}
    bool a=false,b=false,c=false;const auto t=parts[0].toUInt(&a),i=parts[1].toUInt(&b),id=parts[2].toUInt(&c);
    if(!a || !b || !c || t>255 || i>255 || id>65535) {return std::nullopt;}
    return Ref{uint8_t(t),uint8_t(i),uint16_t(id)};
}

bool isWiring(const ItemDescriptor &d) { return d.kind==ItemKind::Param && !d.ref.empty(); }
bool isGpio(const ItemDescriptor &d) { return d.kind==ItemKind::Param && flag(d,defs::ItemFlag::Gpio); }

QString formatValue(const ModelItem &item,uint32_t bits) {
    const auto &d=item.descriptor;
    if(isGpio(d) && bits==defs::kNoGpio) {return tr("No pin");}
    switch(d.value_type) {
    case ValueType::Bool:return bits?tr("On"):tr("Off");
    case ValueType::Enum: {
        for(const auto &e:item.enums) {if(e.value==uint8_t(bits)) {return str(e.name);}}
        return QString::number(bits&0xFF);
    }
    case ValueType::F32: {
        const float f=std::bit_cast<float>(bits);char text[32];
        const auto r=std::to_chars(text,text+sizeof text,f);
        return QString::fromLatin1(text,int(r.ptr-text));
    }
    case ValueType::I32:return QString::number(std::bit_cast<int32_t>(bits));
    case ValueType::U8:return QString::number(bits&0xFF);
    case ValueType::U16:return QString::number(bits&0xFFFF);
    default:return QString::number(bits);
    }
}
ParsedValue parseValueText(const ModelItem &item,const QString &input) {
    const auto text=input.trimmed();
    const auto &d=item.descriptor;
    if(isGpio(d) && (text.isEmpty() || text.compare(tr("No pin"),Qt::CaseInsensitive)==0 || text=="-")) {
        return {defs::kNoGpio,{}};
    }
    if(text.isEmpty()) {return {std::nullopt,tr("Enter a value")};}
    if(d.value_type==ValueType::Enum) {
        for(const auto &e:item.enums) {if(str(e.name)==text) {return {e.value,{}};}}
    }
    if(d.value_type==ValueType::Bool) {
        if(text.compare(tr("On"),Qt::CaseInsensitive)==0) {return {1,{}};}
        if(text.compare(tr("Off"),Qt::CaseInsensitive)==0) {return {0,{}};}
    }
    try {
        return {parseValue(d,text.toStdString(),item.enums),{}};
    } catch(const Error &) {
        switch(d.value_type) {
        case ValueType::F32:return {std::nullopt,tr("Enter a finite number")};
        case ValueType::I32:return {std::nullopt,tr("Enter a whole number")};
        case ValueType::Enum:return {std::nullopt,tr("Choose one of the listed values")};
        case ValueType::Bool:return {std::nullopt,tr("Enter on or off")};
        default:return {std::nullopt,tr("Enter a whole number from 0 to %1").arg(
            d.value_type==ValueType::U8?255:d.value_type==ValueType::U16?65535:4294967295.0,0,'f',0)};
        }
    }
}
QString rangeText(const ModelItem &item) {
    const auto &d=item.descriptor;
    if(d.value_type==ValueType::Bool || d.value_type==ValueType::Enum || isWiring(d) || d.isSource()) {return {};}
    if(!d.has_min && !d.has_max) {return {};}
    const auto unit=d.unit.empty()?QString():" "+str(d.unit);
    const auto lo=d.has_min?formatValue(item,d.min_bits):QString::fromUtf8("−∞");
    const auto hi=d.has_max?formatValue(item,d.max_bits):QString::fromUtf8("∞");
    return lo+QString::fromUtf8(" … ")+hi+unit;
}

const ModelInstance *pendingEntry(const DeviceSnapshot &s,InstanceKey k) {
    const auto it=s.pending.find(k);return it==s.pending.end()?nullptr:&it->second;
}
const ModelInstance *configuredEntry(const DeviceSnapshot &s,InstanceKey k) {
    const auto it=s.configured.find(k);return it==s.configured.end()?nullptr:&it->second;
}
const ModelInstance *anyEntry(const DeviceSnapshot &s,InstanceKey k) {
    if(const auto *p=pendingEntry(s,k)) {return p;}
    return configuredEntry(s,k);
}
QString typeName(const DeviceSnapshot &s,uint8_t type) {
    const auto *t=typeOf(s,type);return t?str(t->descriptor.name):tr("type %1").arg(type);
}
QString loopName(const DeviceSnapshot &s,uint8_t loop) {
    if(const auto *e=anyEntry(s,{defs::kLoopTypeId,loop})) {return label(*e);}
    return tr("loop %1").arg(loop);
}
QString healthText(uint8_t state) {
    switch(state) {
    case 0:return tr("running");case 1:return tr("failed");case 2:return tr("invalid");
    case 3:return tr("dependency missing");case 4:return tr("offline");case 0xFF:return tr("pending");
    default:return tr("state %1").arg(state);
    }
}
QString pendingState(const DeviceSnapshot &s,InstanceKey k) {
    const auto *c=configuredEntry(s,k);const auto *p=pendingEntry(s,k);
    if(p && !c) {return "add";}
    if(c && !p) {return "remove";}
    if(!c || !p) {return {};}
    if(c->instance.label!=p->instance.label) {return "label";}
    for(const auto &[id,v]:p->params) {
        const auto it=c->params.find(id);
        if(it==c->params.end() || valueBits(it->second)!=valueBits(v)) {return "change";}
    }
    return {};
}

QString wiringText(const DeviceSnapshot &s,uint32_t value) {
    if(value==defs::kUnwired) {return tr("Not wired");}
    if(!defs::isWiringRef(value)) {return QString("0x%1").arg(value,8,16,QChar('0'));}
    const InstanceKey k{defs::wiringRefType(value),defs::wiringRefInstance(value)};
    if(const auto *e=anyEntry(s,k)) {return label(*e);}
    return tr("%1 #%2 (not in the layout)").arg(typeName(s,k.first)).arg(k.second);
}
QString sourceText(const DeviceSnapshot &s,uint32_t value) {
    if(value==0) {return tr("Not connected");}
    const InstanceKey k{defs::sourceRefType(value),defs::sourceRefInstance(value)};
    const auto id=defs::sourceRefId(value);
    const auto *item=itemOf(s,k.first,id);
    const auto itemName=item?str(item->descriptor.name):QString("0x%1").arg(id,4,16,QChar('0'));
    if(const auto *e=anyEntry(s,k)) {return label(*e)+"."+itemName;}
    return tr("%1 #%2 (not in the layout)").arg(typeName(s,k.first)).arg(k.second)+"."+itemName;
}

std::vector<LoopChoice> loopChoices(const DeviceSnapshot &s,uint8_t type) {
    const auto *t=typeOf(s,type);const uint8_t mask=t?t->descriptor.loop_mask:0;
    std::vector<LoopChoice> result;
    for(const auto &[k,e]:s.pending) {
        if(k.first!=defs::kLoopTypeId) {continue;}
        LoopChoice c{k.second,label(e),loopRate(e),loopRealtime(e),false};
        c.compatible=(c.realtime && (mask&uint8_t(defs::LoopClass::Realtime))) ||
                     (!c.realtime && (mask&uint8_t(defs::LoopClass::NonRealtime)));
        result.push_back(c);
    }
    return result; // std::map order: ascending loop id
}
std::optional<uint8_t> defaultLoop(const DeviceSnapshot &s,uint8_t type) {
    for(const auto &c:loopChoices(s,type)) {if(c.compatible) {return c.id;}}
    return std::nullopt;
}
QString typeNeed(uint8_t mask) {
    const bool rt=mask&uint8_t(defs::LoopClass::Realtime),other=mask&uint8_t(defs::LoopClass::NonRealtime);
    if(rt && other) {return tr("any loop");}
    if(rt) {return tr("needs a real-time loop");}
    if(other) {return tr("not in a real-time loop");}
    return tr("runs in no loop");
}

QString removeBlocker(const DeviceSnapshot &s,InstanceKey target) {
    QStringList users;
    if(target.first==defs::kLoopTypeId) {
        for(const auto &[k,e]:s.pending) {
            if(k.first!=defs::kLoopTypeId && e.instance.loop==target.second) {users<<label(e);}
        }
        if(users.isEmpty()) {return {};}
        return (users.size()==1?tr("%1 still runs in this loop; move or remove it first")
                               :tr("%1 still run in this loop; move or remove them first")).arg(users.join(", "));
    }
    const auto wired=defs::wiringRef(target.first,target.second);
    for(const auto &[k,e]:s.pending) {
        if(k==target) {continue;}
        const auto *t=typeOf(s,k.first);if(!t) {continue;}
        for(const auto &[id,v]:e.params) {
            const auto it=t->items.find(id);if(it==t->items.end()) {continue;}
            const auto &d=it->second.descriptor;const auto bits=valueBits(v);
            if(isWiring(d) && bits==wired) {
                users<<tr("%1 is wired to it (%2)").arg(label(e),str(d.name));
            } else if(d.isSource() && bits && defs::sourceRefType(bits)==target.first &&
                      defs::sourceRefInstance(bits)==target.second) {
                users<<tr("%1 reads it as a source (%2)").arg(label(e),str(d.name));
            }
        }
    }
    if(users.isEmpty()) {return {};}
    return users.join("; ")+tr("; rewire or disconnect first");
}

std::vector<Candidate> wiringCandidates(const DeviceSnapshot &s,InstanceKey consumer,const ItemDescriptor &param) {
    std::vector<Candidate> result{{defs::kUnwired,tr("Not wired"),{},{},true}};
    const auto *self=anyEntry(s,consumer);
    const auto want=str(param.ref);
    for(const auto &[k,e]:s.pending) {
        if(k==consumer || k.first==defs::kLoopTypeId) {continue;}
        const auto *t=typeOf(s,k.first);if(!t || category(t->descriptor.name)!=want) {continue;}
        Candidate c{defs::wiringRef(k.first,k.second),label(e),
                    str(t->descriptor.name)+QString::fromUtf8(" · ")+loopName(s,e.instance.loop),{},true};
        if(self && e.instance.loop!=self->instance.loop) {
            c.enabled=false;
            c.reason=tr("%1 loop; %2 runs in %3").arg(loopName(s,e.instance.loop),label(*self),loopName(s,self->instance.loop));
        }
        result.push_back(c);
    }
    return result;
}
std::vector<Candidate> sourceCandidates(const DeviceSnapshot &s,InstanceKey consumer,const ItemDescriptor &param) {
    std::vector<Candidate> result{{0,tr("Disconnect"),{},{},true}};
    const auto *ct=typeOf(s,consumer.first);if(!ct) {return result;}
    const auto *value=sourceValue(*ct,param);if(!value) {return result;}
    const auto want=value->descriptor.value_type;
    for(const auto &[k,e]:s.pending) {
        if(k==consumer) {continue;}
        const auto *t=typeOf(s,k.first);if(!t) {continue;}
        const auto *loop=pendingEntry(s,{defs::kLoopTypeId,e.instance.loop});
        const auto where=k.first==defs::kLoopTypeId?str(t->descriptor.name)
            :loopName(s,e.instance.loop)+(loop?QString::fromUtf8(" · %1 Hz").arg(loopRate(*loop)):QString());
        for(const auto &[id,item]:t->items) {
            const auto &d=item.descriptor;
            if((d.kind!=ItemKind::Signal && d.kind!=ItemKind::Input) || d.value_type!=want) {continue;}
            result.push_back({defs::sourceRef(k.first,k.second,id),label(e)+"."+str(d.name),
                              str(t->descriptor.name)+QString::fromUtf8(" · ")+where,{},true});
        }
    }
    return result;
}

std::vector<PendingChange> pendingChanges(const DeviceSnapshot &s) {
    std::vector<PendingChange> result;
    std::set<InstanceKey> keys;
    for(const auto &[k,e]:s.configured) {keys.insert(k);}
    for(const auto &[k,e]:s.pending) {keys.insert(k);}
    for(const auto &k:keys) {
        const auto *c=configuredEntry(s,k);const auto *p=pendingEntry(s,k);
        const auto *t=typeOf(s,k.first);
        const bool loop=k.first==defs::kLoopTypeId;
        const auto key=nodeKey(k.first,k.second);
        if(p && !c) {
            result.push_back({"add",key,label(*p),{},loop?tr("new loop"):
                tr("%1 in %2").arg(typeName(s,k.first),loopName(s,p->instance.loop))});
        } else if(c && !p) {
            result.push_back({"remove",key,label(*c),loop?tr("loop"):typeName(s,k.first),tr("removed")});
            continue;
        } else if(c->instance.label!=p->instance.label) {
            result.push_back({"label",key,label(*p),label(*c),label(*p)});
        }
        if(!t) {continue;}
        for(const auto &[id,v]:p->params) {
            const auto it=t->items.find(id);if(it==t->items.end()) {continue;}
            const auto bits=valueBits(v);
            std::optional<uint32_t> before;
            if(c) {before=paramBits(c,id);}
            else {before=it->second.descriptor.default_bits;}
            if(before && *before==bits) {continue;}
            result.push_back({"param",itemKey(k.first,k.second,id),label(*p)+"."+str(it->second.descriptor.name),
                              before?(c?displayValue(s,it->second,*before):tr("default")):QString(),
                              displayValue(s,it->second,bits)});
        }
    }
    return result;
}

LayoutCounts layoutCounts(const DeviceSnapshot &s) {
    LayoutCounts n;
    for(const auto &[k,e]:s.pending) {if(k.first==defs::kLoopTypeId) {++n.loops;} else {++n.instances;}}
    n.loopLimit=int(systemParam(s,defs::system::kLimitsLoops));
    n.instanceLimit=int(systemParam(s,defs::system::kLimitsInstances));
    return n;
}

QString gpioOwner(const DeviceSnapshot &s,Ref self,uint32_t pin) {
    if(pin==defs::kNoGpio) {return {};}
    for(const auto &[k,e]:s.pending) {
        const auto *t=typeOf(s,k.first);if(!t) {continue;}
        for(const auto &[id,v]:e.params) {
            if(k.first==self.type && k.second==self.instance && id==self.item) {continue;}
            const auto it=t->items.find(id);
            if(it==t->items.end() || !isGpio(it->second.descriptor) || valueBits(v)!=pin) {continue;}
            return tr("%1 (%2)").arg(label(e),str(it->second.descriptor.name));
        }
    }
    return {};
}
QString layoutRefusal(const DeviceSnapshot &s,const Error &e,std::optional<Ref> item,
                      std::optional<uint32_t> value,std::optional<InstanceKey> removed) {
    if(e.kind==ErrorKind::Rejected) {
        using D=defs::Detail;
        if(D(e.detail)==D::GpioConflict && item && value) {
            const auto owner=gpioOwner(s,*item,*value);
            if(!owner.isEmpty()) {return tr("GPIO %1 is already used by %2").arg(*value).arg(owner);}
        }
        if(D(e.detail)==D::Referenced && removed) {
            const auto why=removeBlocker(s,*removed);
            if(!why.isEmpty()) {return why;}
        }
    }
    return errorText(e);
}

QHash<int,QByteArray> LayoutTree::roleNames() const {
    return {{Key,"key"},{IsLoop,"isLoop"},{Depth,"depth"},{Label,"label"},{TypeName,"typeName"},
            {LoopId,"loopId"},{Health,"health"},{HealthText,"healthText"},{Pending,"pending"},
            {RateHz,"rateHz"},{Realtime,"realtime"},{Budget,"budget"},{HasBudget,"hasBudget"}};
}
QVariant LayoutTree::data(const QModelIndex &i,int role) const {
    if(!i.isValid() || i.row()<0 || size_t(i.row())>=m_rows.size()) {return {};}
    const auto &r=m_rows[i.row()];
    switch(role) {
    case Key:return r.key;case IsLoop:return r.isLoop;case Depth:return r.depth;
    case Label:case Qt::DisplayRole:return r.label;case TypeName:return r.type;case LoopId:return r.loop;
    case Health:return r.state;case HealthText:return r.health;case Pending:return r.pending;
    case RateHz:return r.rate;case Realtime:return r.realtime;
    case Budget:return m_budgets.value(r.loop,0.0);
    case HasBudget:return r.isLoop && m_budgets.contains(r.loop);
    default:return {};
    }
}
QStringList LayoutTree::keys() const {
    QStringList result;for(const auto &r:m_rows) {result<<r.key;}return result;
}
void LayoutTree::setBudgets(const QHash<int,double> &budgets) {
    if(budgets==m_budgets) {return;}
    m_budgets=budgets;
    if(!m_rows.empty()) {emit dataChanged(index(0),index(int(m_rows.size())-1),{Budget,HasBudget});}
}
void LayoutTree::reset(const std::shared_ptr<const DeviceSnapshot> &snapshot) {
    beginResetModel();m_rows.clear();
    if(snapshot) {
        const auto &s=*snapshot;
        std::set<InstanceKey> keys;
        for(const auto &[k,e]:s.configured) {keys.insert(k);}
        for(const auto &[k,e]:s.pending) {keys.insert(k);}
        auto row=[&](InstanceKey k,int depth) {
            const auto *e=anyEntry(s,k);const auto *c=configuredEntry(s,k);
            Row r;r.key=nodeKey(k.first,k.second);r.label=label(*e);r.type=typeName(s,k.first);
            r.loop=k.first==defs::kLoopTypeId?k.second:e->instance.loop;
            r.state=c?c->instance.state:0xFF;r.health=healthText(r.state);r.pending=pendingState(s,k);
            r.depth=depth;r.isLoop=k.first==defs::kLoopTypeId;
            if(r.isLoop) {r.rate=loopRate(*e);r.realtime=loopRealtime(c?*c:*e);}
            m_rows.push_back(r);
        };
        // Parent of a module: an instance in its loop wired to it that is not
        // itself wired to by another instance (one level, never a cycle).
        std::map<InstanceKey,std::vector<InstanceKey>> wiredBy;
        for(const auto &k:keys) {
            if(k.first==defs::kLoopTypeId) {continue;}
            const auto *e=anyEntry(s,k);const auto *t=typeOf(s,k.first);if(!t) {continue;}
            for(const auto &[id,v]:e->params) {
                const auto it=t->items.find(id);if(it==t->items.end() || !isWiring(it->second.descriptor)) {continue;}
                const auto bits=valueBits(v);if(!bits || !defs::isWiringRef(bits)) {continue;}
                const InstanceKey target{defs::wiringRefType(bits),defs::wiringRefInstance(bits)};
                const auto *te=anyEntry(s,target);
                if(target!=k && te && te->instance.loop==e->instance.loop) {wiredBy[target].push_back(k);}
            }
        }
        std::map<InstanceKey,InstanceKey> parent;
        for(const auto &[target,users]:wiredBy) {
            for(const auto &u:users) {
                if(!wiredBy.contains(u)) {parent.emplace(target,u);break;}
            }
        }
        std::set<InstanceKey> placed;
        auto modules=[&](uint8_t loop) {
            for(const auto &k:keys) {
                if(k.first==defs::kLoopTypeId || parent.contains(k) || anyEntry(s,k)->instance.loop!=loop) {continue;}
                row(k,1);placed.insert(k);
                for(const auto &[child,p]:parent) {if(p==k) {row(child,2);placed.insert(child);}}
            }
        };
        for(const auto &k:keys) {
            if(k.first!=defs::kLoopTypeId) {continue;}
            row(k,0);modules(k.second);
        }
        // Instances whose loop is not in the layout (or whose parent was not
        // placed) still appear, unindented.
        for(const auto &k:keys) {
            if(k.first!=defs::kLoopTypeId && !placed.contains(k)) {row(k,0);}
        }
    }
    endResetModel();
}

QHash<int,QByteArray> ParamTable::roleNames() const {
    return {{Key,"key"},{Name,"name"},{Group,"group"},{Unit,"unit"},{Description,"description"},
            {Editor,"editor"},{ValueText,"valueText"},{PendingText,"pendingText"},{IsPending,"isPending"},
            {Unsaved,"unsaved"},{Locked,"locked"},{ReadOnly,"readOnly"},{RebootRequired,"rebootRequired"},
            {RangeText,"rangeText"},{Enums,"enums"},{ValueBits,"valueBits"},{EditState,"editState"},
            {EditNote,"editNote"},{EditText,"editText"},{Minimum,"minimum"},{Maximum,"maximum"},
            {ValueType,"valueType"}};
}
QVariant ParamTable::data(const QModelIndex &i,int role) const {
    if(!i.isValid() || i.row()<0 || size_t(i.row())>=m_rows.size()) {return {};}
    const auto &r=m_rows[i.row()];
    switch(role) {
    case Key:return r.key;case Name:case Qt::DisplayRole:return r.name;case Group:return r.group;
    case Unit:return r.unit;case Description:return r.description;case Editor:return r.editor;
    case ValueText:return r.value;case PendingText:return r.pendingValue;case IsPending:return r.pending;
    case Unsaved:return r.unsaved;case Locked:return r.locked;case ReadOnly:return r.readOnly;
    case RebootRequired:return r.reboot;case RangeText:return r.range;case Enums:return r.enums;
    case ValueBits:return QVariant::fromValue(r.bits);case EditState:return r.editState;
    case EditNote:return r.editNote;case EditText:return r.editText;
    case Minimum:return r.minimum;case Maximum:return r.maximum;case ValueType:return r.valueType;
    default:return {};
    }
}
void ParamTable::reset(const std::shared_ptr<const DeviceSnapshot> &snapshot,std::optional<InstanceKey> node,bool armed,
                       const QHash<QString,ParamEdit> &edits,const QSet<QString> &changed,int filter,const QString &text) {
    std::vector<Row> next;m_total=m_changed=m_pending=0;
    if(snapshot && node) {
        const auto &s=*snapshot;
        const auto *c=configuredEntry(s,*node);const auto *p=pendingEntry(s,*node);
        const auto *t=typeOf(s,node->first);
        if(t && (c || p)) {
            std::vector<Row> rows;QStringList groups;
            for(const auto &[id,item]:t->items) {
                const auto &d=item.descriptor;
                if(d.kind!=ItemKind::Param || isWiring(d) || d.isSource()) {continue;}
                Row r;r.key=itemKey(node->first,node->second,id);r.name=str(d.name);
                const auto dot=d.name.find('.');r.group=dot==std::string::npos?QString():str(d.name.substr(0,dot));
                r.unit=str(d.unit);r.description=str(d.description);r.range=rangeText(item);
                r.valueType=int(d.value_type);
                r.editor=d.value_type==ValueType::Bool?"bool":d.value_type==ValueType::Enum?"enum":isGpio(d)?"gpio":"number";
                for(const auto &e:item.enums) {r.enums<<QVariantMap{{"value",e.value},{"name",str(e.name)}};}
                if(d.has_min) {r.minimum=formatValue(item,d.min_bits);}
                if(d.has_max) {r.maximum=formatValue(item,d.max_bits);}
                r.reboot=flag(d,defs::ItemFlag::RebootRequired);
                r.readOnly=flag(d,defs::ItemFlag::ReadOnly) || !p; // a removed instance is not edited
                r.locked=armed && flag(d,defs::ItemFlag::LockedWhileArmed);
                const auto current=paramBits(c,id),staged=paramBits(p,id);
                if(c) {
                    r.bits=current.value_or(0);r.value=current?formatValue(item,*current):QString();
                    r.pending=staged && current && *staged!=*current;
                    if(r.pending) {r.pendingValue=formatValue(item,*staged);r.bits=*staged;}
                } else {
                    // A pending-only instance: every param is staged.
                    r.bits=staged.value_or(d.default_bits);r.value=formatValue(item,r.bits);r.pending=true;
                    r.pendingValue=r.value;
                }
                r.unsaved=changed.contains(r.key);
                if(const auto it=edits.find(r.key);it!=edits.end()) {
                    r.editState=it->state;r.editNote=it->note;r.editText=it->text;
                }
                ++m_total;
                if(r.pending) {++m_pending;}
                if(r.pending || r.unsaved) {++m_changed;}
                if(filter==1 && !(r.pending || r.unsaved)) {continue;}
                if(filter==2 && !r.pending) {continue;}
                if(!text.isEmpty() && !r.name.contains(text,Qt::CaseInsensitive)) {continue;}
                if(!groups.contains(r.group)) {groups<<r.group;}
                rows.push_back(std::move(r));
            }
            // Rows grouped by name prefix: ungrouped names first (G4 "General"),
            // then the groups in order of first appearance.
            const auto rank=[&](const QString &g) {return g.isEmpty()?-1:int(groups.indexOf(g));};
            std::stable_sort(rows.begin(),rows.end(),[&](const Row &a,const Row &b) {return rank(a.group)<rank(b.group);});
            next=std::move(rows);
        }
    }
    // The same rows update in place, so an editor keeps its focus and text
    // while a value is sent and read back; other changes reset the model.
    const bool same=next.size()==m_rows.size() &&
        std::equal(next.begin(),next.end(),m_rows.begin(),[](const Row &a,const Row &b) {return a.key==b.key;});
    if(same) {
        m_rows=std::move(next);
        if(!m_rows.empty()) {emit dataChanged(index(0),index(int(m_rows.size())-1));}
        return;
    }
    beginResetModel();m_rows=std::move(next);endResetModel();
}
} // namespace drivelink::gui
