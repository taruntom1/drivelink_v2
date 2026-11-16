#include "models.hpp"
#include <drivelink/protocol/gen/type_system.hpp>
#include <drivelink/protocol/gen/registry.hpp>
#include <algorithm>
#include <set>

namespace drivelink::gui {
namespace defs=protocol::defs;
namespace {
QString key(uint8_t t,uint8_t i) { return QString::number(t)+":"+QString::number(i); }
uint32_t param(const DeviceSnapshot &s,uint16_t id) { return valueBits(s.system_params.at(id)); }
QVariantList scalars(const std::vector<ScalarDescriptor> &items) {
    QVariantList result;
    for(const auto &v:items) { result.push_back(QVariantMap{{"name",QString::fromStdString(v.name)},
        {"type",int(v.value_type)},{"unit",QString::fromStdString(v.unit)}}); }
    return result;
}
}
std::vector<CommandPlan> planCommands(const DeviceSnapshot &s,const GroupList &groups) {
    auto fail=[](const char *why)->void { throw Error(ErrorKind::Usage,why); };
    if(!s.ready || !s.connected || s.safeMode()) { fail("Device metadata is unavailable or device is in safe mode"); }
    std::vector<uint8_t> freeSlots, loops;
    std::vector<Ref> occupied;
    for(const auto &g:groups.groups) {
        if(g.kind!=GroupKind::Command) { continue; }
        if(g.configured) { occupied.insert(occupied.end(),g.refs.begin(),g.refs.end()); }
        else { freeSlots.push_back(g.id); }
    }
    const size_t width=std::min<size_t>({groups.entry_capacity,size_t(param(s,defs::system::kLimitsGroupEntries)),
        size_t(64),s.info.max_payload>7?size_t((s.info.max_payload-7)/4):0});
    if(!width) { fail("Device command group capacity is zero"); }
    std::map<uint8_t,std::vector<Ref>> by_loop;
    for(const auto &[k,v]:s.configured) {
        if(k.first==defs::kLoopTypeId && v.instance.state==0) { loops.push_back(k.second); }
    }
    if(loops.empty()) { fail("No running loop is available for command keepalives"); }
    for(const auto &[k,v]:s.configured) {
        if(v.instance.state!=0 || k.first==defs::kLoopTypeId) { continue; }
        for(const auto &[id,item]:s.types.at(k.first).items) {
            if(item.descriptor.kind!=ItemKind::Input) { continue; }
            Ref ref{k.first,k.second,id};
            if(std::find(occupied.begin(),occupied.end(),ref)!=occupied.end()) { fail("An input belongs to an existing command group"); }
            if(std::find(loops.begin(),loops.end(),v.instance.loop)==loops.end()) { fail("Input has no running owning loop"); }
            by_loop[v.instance.loop].push_back(ref);
        }
    }
    if(by_loop.empty()) { by_loop[loops.front()]={}; }
    std::vector<CommandPlan> result;
    for(const auto &[loop,refs]:by_loop) {
        size_t at=0;
        do {
            if(result.size()>=freeSlots.size()) { fail("Not enough free command group slots"); }
            const auto count=std::min(width,refs.size()-at);
            result.push_back({freeSlots[result.size()],loop,{refs.begin()+at,refs.begin()+at+count}});
            at+=count;
        } while(at<refs.size());
    }
    return result;
}
QVariantMap errorFields(const Error &e) {
    return {{"message",QString::fromUtf8(e.what())},{"kind",int(e.kind)},{"result",e.result},
        {"detail",e.detail},{"family",e.family},{"op",e.op},{"hasContext",e.has_request_context},
        {"outcomeUnknown",e.outcome_unknown},{"bootChanged",e.boot_changed}};
}
QHash<int,QByteArray> InstanceTree::roleNames() const {
    return {{Key,"key"},{Type,"typeId"},{InstanceId,"instanceId"},{Label,"label"},
        {Loop,"loopId"},{Health,"health"},{IsLoop,"isLoop"}};
}
void InstanceTree::reset(std::shared_ptr<const DeviceSnapshot> s,bool pending) {
    beginResetModel();m_nodes.clear();m_roots.clear();
    if(s) {
        const auto &layout=pending?s->pending:s->configured;
        std::map<uint8_t,int> loops;
        for(const auto &[k,v]:layout) {
            if(k.first!=defs::kLoopTypeId) { continue; }
            const int n=int(m_nodes.size());loops[k.second]=n;m_roots.push_back(n);
            m_nodes.push_back({v.instance,true,-1,{}});
        }
        for(const auto &[k,v]:layout) {
            if(k.first==defs::kLoopTypeId) { continue; }
            const int n=int(m_nodes.size());const auto it=loops.find(v.instance.loop);
            const int parent=it==loops.end()?-1:it->second;
            m_nodes.push_back({v.instance,false,parent,{}});
            if(parent<0) {m_roots.push_back(n);} else {m_nodes[parent].children.push_back(n);}
        }
    }
    endResetModel();
}
QModelIndex InstanceTree::index(int row,int column,const QModelIndex &parent) const {
    if(row<0 || column!=0 || (parent.isValid() && parent.column()!=0)) {return {};}
    const auto &items=parent.isValid()?m_nodes.at(parent.internalId()-1).children:m_roots;
    if(size_t(row)>=items.size()) {return {};}
    return createIndex(row,column,quintptr(items[row]+1));
}
QModelIndex InstanceTree::parent(const QModelIndex &i) const {
    if(!i.isValid()) {return {};}
    const int p=m_nodes.at(i.internalId()-1).parent;
    if(p<0) {return {};}
    const auto it=std::find(m_roots.begin(),m_roots.end(),p);
    return createIndex(int(it-m_roots.begin()),0,quintptr(p+1));
}
int InstanceTree::rowCount(const QModelIndex &p) const {
    if(p.isValid() && p.column()!=0) {return 0;}
    return int(p.isValid()?m_nodes.at(p.internalId()-1).children.size():m_roots.size());
}
QVariant InstanceTree::data(const QModelIndex &i,int role) const {
    if(!i.isValid()) {return {};}
    const auto &n=m_nodes.at(i.internalId()-1);const auto &v=n.instance;
    switch(role) {
    case Key:return key(v.type,v.instance);case Type:return v.type;case InstanceId:return v.instance;
    case Label:case Qt::DisplayRole:return QString::fromStdString(v.label);
    case Loop:return v.loop;case Health:return v.state;case IsLoop:return n.loop;default:return {};
    }
}
QHash<int,QByteArray> DescriptorRows::roleNames() const {
    return {{Key,"key"},{Name,"name"},{Kind,"kind"},{ValueTypeRole,"valueType"},{Unit,"unit"},
        {Description,"description"},{Flags,"flags"},{Minimum,"minimumBits"},{Maximum,"maximumBits"},
        {Enums,"enums"},{ValueBits,"valueBits"},{Pending,"pending"},{Arguments,"arguments"},{Results,"results"}};
}
void DescriptorRows::reset(std::shared_ptr<const DeviceSnapshot> s,uint8_t type,uint8_t instance,bool pending) {
    beginResetModel();m_rows.clear();m_pending=pending;
    if(s && s->types.contains(type)) {
        const auto &layout=pending?s->pending:s->configured;
        const std::map<uint16_t,Value> *values=nullptr;
        if(type==defs::kSystemTypeId) {values=&s->system_params;}
        else if(auto it=layout.find({type,instance});it!=layout.end()) {values=&it->second.params;}
        if(values) {
            for(const auto &[id,item]:s->types.at(type).items) {
                std::optional<uint32_t> bits;
                if(auto it=values->find(id);it!=values->end()) {bits=valueBits(it->second);}
                m_rows.push_back({key(type,instance)+":"+QString::number(id),item,bits});
            }
        }
    }
    endResetModel();
}
QVariant DescriptorRows::data(const QModelIndex &i,int role) const {
    if(!i.isValid() || i.row()<0 || size_t(i.row())>=m_rows.size()) {return {};}
    const auto &row=m_rows[i.row()];const auto &d=row.item.descriptor;
    switch(role) {
    case Key:return row.key;case Name:case Qt::DisplayRole:return QString::fromStdString(d.name);
    case Kind:return int(d.kind);case ValueTypeRole:return int(d.value_type);case Unit:return QString::fromStdString(d.unit);
    case Description:return QString::fromStdString(d.description);case Flags:return d.flags;
    case Minimum:return d.has_min?QVariant(d.min_bits):QVariant();
    case Maximum:return d.has_max?QVariant(d.max_bits):QVariant();case ValueBits:return row.bits?QVariant(*row.bits):QVariant();
    case Pending:return m_pending;case Arguments:return scalars(d.args);case Results:return scalars(d.results);
    case Enums: {QVariantList result;for(const auto &e:row.item.enums) {result.push_back(QVariantMap{{"value",e.value},{"name",QString::fromStdString(e.name)}});}return result;}
    default:return {};
    }
}
} // namespace drivelink::gui
