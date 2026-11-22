#include "command_view.hpp"
#include "layout_model.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <QGuiApplication>
#include <QLocale>
#include <QSettings>
#include <cmath>

namespace drivelink::gui {
namespace defs=protocol::defs;
namespace {
std::optional<double> parseNumber(QString text) {
    text=text.trimmed();
    if(text.startsWith('+')) {text.remove(0,1);}
    bool ok=false;const double value=QLocale::c().toDouble(text,&ok);
    if(!ok || !std::isfinite(value)) {return std::nullopt;}
    return value;
}
}

int CommandRows::rowCount(const QModelIndex &p) const {return p.isValid()?0:int(m_view.m_visible.size());}
QHash<int,QByteArray> CommandRows::roleNames() const {
    return {{Key,"key"},{Name,"name"},{Owner,"owner"},{Unit,"unit"},{From,"from"},{To,"to"},{Limit,"limit"},
        {Bound,"bound"},{Value,"value"},{Step,"step"},{Decimals,"decimals"},{Latched,"latched"},{Held,"held"},
        {Ramping,"ramping"},{Writing,"writing"}};
}
QVariant CommandRows::data(const QModelIndex &i,int role) const {
    const auto *in=i.isValid()?m_view.row(i.row()):nullptr;
    if(!in) {return {};}
    switch(role) {
    case Key:return itemKey(in->ref.type,in->ref.instance,in->ref.item);
    case Name:return QString::fromStdString(in->descriptor.name);
    case Owner:return m_view.owner(in->ref);
    case Unit:return QString::fromStdString(in->descriptor.unit);
    case From:return in->from();
    case To:return in->to();
    case Limit:return in->limit;
    case Bound:return in->bound;
    case Value:return in->value;
    case Step:return in->step;
    case Decimals:return in->decimals;
    case Latched:return in->latched;
    case Held:return in->held;
    case Ramping:return in->ramping;
    case Writing:return in->writes>0;
    default:return {};
    }
}

CommandView::CommandView(QObject *parent):QObject(parent),m_rows(*this) {
    m_clock.start();
    m_timer.setInterval(int(CommandModel::kStepMs));
    connect(&m_timer,&QTimer::timeout,this,[this] {pump();});
    // A slider held when the window loses focus counts as released.
    if(qGuiApp) {
        connect(qGuiApp,&QGuiApplication::applicationStateChanged,this,[this](Qt::ApplicationState state) {
            if(state!=Qt::ApplicationActive) {releaseHeld();}
        });
    }
}
int CommandView::index(const QString &key) const {
    const auto ref=parseItemKey(key);
    return ref?m_model.find(*ref):-1;
}
const CommandModel::Input *CommandView::row(int r) const {
    if(r<0 || size_t(r)>=m_visible.size()) {return nullptr;}
    const auto i=m_model.find(m_visible[size_t(r)]);
    return i<0?nullptr:&m_model.inputs()[size_t(i)];
}
QString CommandView::owner(const Ref &ref) const {
    if(!m_snapshot) {return {};}
    const auto it=m_snapshot->configured.find({ref.type,ref.instance});
    return it==m_snapshot->configured.end()?QString():QString::fromStdString(it->second.instance.label);
}
QString CommandView::limitKey(const Ref &ref) const {
    return QStringLiteral("commandLimits/%1/%2-%3-%4").arg(m_deviceId).arg(ref.type).arg(ref.instance).arg(ref.item);
}
bool CommandView::commanding() const {
    for(const auto &in:m_model.inputs()) {if(in.value!=0) {return true;}}
    return false;
}
QVariantList CommandView::candidates() const {
    QVariantList result;
    if(!m_snapshot) {return result;}
    const bool configured=!m_model.inputs().empty();
    for(const auto &[k,v]:m_snapshot->configured) {
        if(k.first==defs::kLoopTypeId || v.instance.state!=0) {continue;}
        const auto type=m_snapshot->types.find(k.first);
        if(type==m_snapshot->types.end()) {continue;}
        for(const auto &[id,item]:type->second.items) {
            const auto &d=item.descriptor;
            if(d.kind!=ItemKind::Input) {continue;}
            const Ref ref{k.first,k.second,id};
            const int i=m_model.find(ref);
            QString reason=commandRefusal(d);
            if(reason.isEmpty() && i<0) {reason=configured?tr("Not in a command group of this window"):tr("Command groups are not set up");}
            if(reason.isEmpty() && m_model.inputs()[size_t(i)].shown) {reason=tr("Already on the card");}
            result.push_back(QVariantMap{{"key",itemKey(ref.type,ref.instance,ref.item)},{"name",QString::fromStdString(d.name)},
                {"owner",QString::fromStdString(v.instance.label)},{"unit",QString::fromStdString(d.unit)},
                {"enabled",reason.isEmpty()},{"reason",reason}});
        }
    }
    return result;
}
void CommandView::setSnapshot(std::shared_ptr<const DeviceSnapshot> snapshot,const QString &deviceId) {
    if(deviceId!=m_deviceId) {reset();m_deviceId=deviceId;}
    m_snapshot=std::move(snapshot);
    emit changed();
}
void CommandView::configure(const std::vector<CommandGroupSpec> &groups) {
    m_model.configure(groups);
    QSettings settings(m_settingsPath,QSettings::IniFormat);
    for(size_t i=0;i<m_model.inputs().size();++i) {
        const auto saved=settings.value(limitKey(m_model.inputs()[i].ref));
        bool ok=false;const double limit=saved.toDouble(&ok);
        if(saved.isValid() && ok) {m_model.setLimit(int(i),limit);}
    }
    // Inputs still commandable stay on the card, in their order.
    std::erase_if(m_order,[&](const Ref &ref){return m_model.find(ref)<0;});
    for(const auto &ref:m_order) {m_model.setShown(m_model.find(ref),true);}

    rebuildRows();
}
void CommandView::clearGroups() {
    m_timer.stop();
    m_model.clear();
    rebuildRows();
}
void CommandView::reset() {
    m_order.clear();clearGroups();
}
void CommandView::setBlocked(const QString &reason) {
    const bool wasEnabled=m_model.enabled();
    m_blocked=reason;
    m_model.setEnabled(reason.isEmpty());
    if(!m_model.enabled()) {m_timer.stop();}
    if(wasEnabled!=m_model.enabled()) {updateValues();emit changed();}
    else if(!reason.isEmpty()) {emit changed();}
}
void CommandView::releaseHeld() {m_model.releaseHeld(m_clock.elapsed());pump();}
bool CommandView::commands(uint8_t type,uint8_t instance) const {
    for(const auto &in:m_model.inputs()) {if(in.ref.type==type && in.ref.instance==instance) {return true;}}
    return false;
}
void CommandView::beginWrite(uint8_t type,uint8_t instance) {m_model.beginWrite(type,instance);pump();}
void CommandView::endWrite(uint8_t type,uint8_t instance) {m_model.endWrite(type,instance);pump();}
void CommandView::zeroInstance(uint8_t type,uint8_t instance) {m_model.zeroInstance(type,instance);pump();}
void CommandView::add(const QString &key) {
    const int i=index(key);
    if(i<0 || m_model.inputs()[size_t(i)].shown) {return;}
    const auto ref=m_model.inputs()[size_t(i)].ref;
    m_rows.beginInsertRows({},int(m_visible.size()),int(m_visible.size()));
    m_order.push_back(ref);m_visible.push_back(ref);m_model.setShown(i,true);
    m_rows.endInsertRows();
    emit changed();
}
void CommandView::remove(const QString &key) {
    const auto ref=parseItemKey(key);
    if(!ref) {return;}
    std::erase(m_order,*ref);
    const auto it=std::find(m_visible.begin(),m_visible.end(),*ref);
    if(it==m_visible.end()) {return;}
    const int r=int(it-m_visible.begin());
    m_model.remove(m_model.find(*ref));
    m_rows.beginRemoveRows({},r,r);m_visible.erase(it);m_rows.endRemoveRows();
    pump();emit changed();
}
void CommandView::press(const QString &key) {if(m_model.press(index(key))) {updateValues();}}
void CommandView::drag(const QString &key,double value) {if(m_model.drag(index(key),value)) {pump();}}
void CommandView::release(const QString &key) {m_model.release(index(key),m_clock.elapsed());pump();}
void CommandView::setLatched(const QString &key,bool on) {m_model.setLatched(index(key),on,m_clock.elapsed());pump();}
QString CommandView::typeValue(const QString &key,const QString &text) {
    const int i=index(key);
    if(i<0) {return tr("Unknown input");}
    if(!m_model.enabled()) {return m_blocked;}
    if(!m_model.inputs()[size_t(i)].latched) {return tr("Turn on Latch to type a value");}
    const auto value=parseNumber(text);
    if(!value) {updateValues();return tr("Not a number");}
    m_model.type(i,*value);
    pump();
    return {};
}
QString CommandView::setLimitText(const QString &key,const QString &text) {
    const int i=index(key);
    if(i<0) {return tr("Unknown input");}
    const auto value=parseNumber(text);
    if(!value || *value<=0) {updateValues();return tr("Enter a limit above 0");}
    m_model.setLimit(i,std::abs(*value));
    QSettings(m_settingsPath,QSettings::IniFormat).setValue(limitKey(m_model.inputs()[size_t(i)].ref),m_model.inputs()[size_t(i)].limit);
    pump();
    return {};
}
void CommandView::holdAll() {m_model.holdAll();pump();}
void CommandView::pump() {
    for(const auto &frame:m_model.tick(m_clock.elapsed())) {
        // A full queue (or a group gone) keeps the latest values for the next tick.
        if(!m_writer || !m_writer(frame.group,frame.bits)) {m_model.retry(frame.group);}
    }
    if(m_model.active()) {if(!m_timer.isActive()) {m_timer.start();}}
    else {m_timer.stop();}
    updateValues();
}
void CommandView::updateValues() {
    if(!m_visible.empty()) {emit m_rows.dataChanged(m_rows.index(0),m_rows.index(int(m_visible.size())-1));}
    emit valuesChanged();
}
void CommandView::rebuildRows() {
    m_rows.beginResetModel();
    m_visible.clear();
    for(const auto &ref:m_order) {if(m_model.find(ref)>=0) {m_visible.push_back(ref);}}
    m_rows.endResetModel();
    emit changed();emit valuesChanged();
}
} // namespace drivelink::gui
