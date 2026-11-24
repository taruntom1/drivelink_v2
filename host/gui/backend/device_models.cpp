#include "device_models.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

namespace drivelink::gui {
namespace defs=protocol::defs;
namespace loopdefs=protocol::defs::loop;
namespace {
QString tr(const char *text) { return QCoreApplication::translate("DriveLink", text); }
}

QString refusalText(uint8_t result,uint8_t detail) {
    using D=defs::Detail;
    switch(D(detail)) {
    case D::LockedWhileArmed: return tr("Not allowed while armed; disarm first");
    case D::BadValue: return tr("The device rejected the value");
    case D::ReadOnly: return tr("This value is read-only");
    case D::SafeMode: return tr("Not available in safe mode");
    case D::NoSavedConfig: return tr("There is no saved configuration to revert to");
    case D::StorageFailed: return tr("The device could not write its storage");
    case D::FaultLatched: return tr("A fault is latched; acknowledge it first");
    case D::FaultActive: return tr("A fault condition is still active");
    case D::BlocksArm: return tr("An active condition blocks arming");
    case D::LayoutInvalid: return tr("The saved layout is invalid");
    case D::NoCommandGroup: return tr("No command group keeps the device alive");
    case D::DisarmRaced: return tr("A DISARM arrived during the request");
    case D::RebootPending: return tr("A reboot is pending; reboot the device first");
    case D::LinkTrialActive: return tr("The serial link settings are on trial; confirm them or wait until they revert");
    case D::NoLinkTrial: return tr("There is no link trial to confirm");
    case D::NotEnoughMemory: return tr("The device has not enough memory for this");
    case D::WrongState: return tr("The device is in the wrong state for this");
    case D::OtaActive: return tr("A firmware update is in progress");
    case D::OtaPendingConfirm: return tr("A firmware update waits for confirmation");
    case D::StagingBusy: return tr("The device is busy; try again");
    case D::Withdrawn: return tr("The loop did not take the change in time; nothing changed. Try again");
    case D::Unsupported: return tr("Not supported by this device");
    case D::Capacity: return tr("The device has no capacity left for this");
    case D::LoopMismatch: return tr("Not compatible with that loop: the type must suit the loop's real-time setting, and wiring stays within one loop");
    case D::LabelInUse: return tr("Another instance already has this label");
    case D::BadLabel: return tr("Labels are 1 to 16 bytes without control characters");
    case D::GpioConflict: return tr("The pin is already used by another instance");
    case D::GpioNotAllowed: return tr("This pin cannot be used on this device");
    case D::BadRef: return tr("The target does not exist or has the wrong type");
    case D::WiringCycle: return tr("This would make a wiring cycle within the loop");
    case D::Referenced: return tr("Something still refers to it; rewire, move or remove that first");
    case D::CrossLoopBatch: return tr("One change may touch only one loop");
    case D::BadEnum: return tr("Not one of the listed values");
    case D::NonFiniteFloat: return tr("The value must be a finite number");
    case D::UnknownRef: return tr("The device does not know this item");
    default: break;
    }
    using R=defs::Result;
    switch(R(result)) {
    case R::Armed: return tr("Not allowed while armed; disarm first");
    case R::NotArmed: return tr("The device is not armed");
    case R::Busy: return tr("The device is busy; try again");
    case R::PendingReboot: return tr("A reboot is pending; reboot the device first");
    case R::Fault: return tr("The device reported a fault while doing this");
    case R::InvalidArgument: return tr("The device rejected the request (detail %1)").arg(detail);
    case R::NotFound: return tr("The device does not have this item");
    case R::NoMemory: return tr("The device ran out of memory");
    default: return tr("The device refused (result %1, detail %2)").arg(result).arg(detail);
    }
}

QString errorText(const Error &e) {
    if(e.kind==ErrorKind::Rejected) { return refusalText(e.result,e.detail); }
    if(e.kind==ErrorKind::Timeout && e.outcome_unknown) {
        return tr("No reply in time; the device may have applied the request");
    }
    return QString::fromUtf8(e.what());
}

QString resetReasonText(uint8_t reason) {
    // ESP-IDF esp_reset_reason_t, as the firmware reports it.
    static const char *names[]={"Unknown","Power on","External reset","Software reset","Panic",
        "Interrupt watchdog","Task watchdog","Other watchdog","Deep sleep wake","Brownout","SDIO reset",
        "USB reset","JTAG reset","eFuse error","Power glitch","CPU lockup"};
    return reason<std::size(names)?tr(names[reason]):tr("Reset reason %1").arg(reason);
}

QString disarmReasonText(uint8_t reason) {
    static const char *reasons[]={"Boot","Explicit disarm","Command watchdog","Fault","Loop stalled","Shutdown"};
    return reason<std::size(reasons)?tr(reasons[reason]):tr("Unknown disarm reason (%1)").arg(reason);
}

QString diagSourceName(const DeviceSnapshot &s,uint8_t source) {
    // Module sources equal their type ids; the core's own sources are fixed.
    if(auto it=s.types.find(source);it!=s.types.end()) { return QString::fromStdString(it->second.descriptor.name); }
    switch(source) {
    case 0x01: return QStringLiteral("system");
    case 0x02: return QStringLiteral("uart_comm");
    case 0x03: return QStringLiteral("diag");
    case 0x04: return QStringLiteral("core");
    default: return QStringLiteral("0x%1").arg(source,2,16,QLatin1Char('0'));
    }
}

double LoopTable::budget(uint32_t run_us,uint32_t rate_hz) {
    if(!rate_hz) { return 0; }
    return double(run_us)*double(rate_hz)/1e6;
}
QHash<int,QByteArray> LoopTable::roleNames() const {
    return {{LoopId,"loopId"},{Name,"name"},{RateHz,"rateHz"},{Realtime,"realtime"},{Core,"core"},
        {Running,"running"},{State,"state"},{LastRunUs,"lastRunUs"},{MaxRunUs,"maxRunUs"},{Budget,"budget"},
        {Overruns,"overruns"},{HasRunTime,"hasRunTime"},{RunTimeNote,"runTimeNote"}};
}
void LoopTable::reset(const std::shared_ptr<const DeviceSnapshot> &s) {
    beginResetModel();
    std::vector<Row> rows;
    if(s) {
        const ModelType *loopType=nullptr;
        if(auto it=s->types.find(defs::kLoopTypeId);it!=s->types.end()) { loopType=&it->second; }
        for(const auto &[key,v]:s->configured) {
            if(key.first!=defs::kLoopTypeId) { continue; }
            Row row;row.id=key.second;row.state=v.instance.state;row.name=QString::fromStdString(v.instance.label);
            if(auto p=v.params.find(loopdefs::kRateHz);p!=v.params.end()) { row.rate=valueBits(p->second); }
            if(auto p=v.params.find(loopdefs::kRealtime);p!=v.params.end()) { row.realtime=valueBits(p->second)!=0; }
            if(auto p=v.params.find(loopdefs::kCore);p!=v.params.end() && loopType) {
                const auto bits=valueBits(p->second);
                if(auto item=loopType->items.find(loopdefs::kCore);item!=loopType->items.end()) {
                    for(const auto &e:item->second.enums) { if(e.value==bits) { row.core=QString::fromStdString(e.name); } }
                }
                if(row.core.isEmpty()) { row.core=QString::number(bits); }
            }
            const auto running=std::find_if(s->info.loops.begin(),s->info.loops.end(),[&](const auto &l){return l.id==row.id;});
            row.running=running!=s->info.loops.end();
            if(row.running) { row.rate=running->rate_hz;row.realtime=running->realtime(); }
            // Keep run time across a metadata refresh of the same loop.
            if(auto old=std::find_if(m_rows.begin(),m_rows.end(),[&](const Row &r){return r.id==row.id;});old!=m_rows.end()) {
                row.run=old->run;row.note=old->note;row.overruns=old->overruns;
            }
            rows.push_back(std::move(row));
        }
        setStatusRows(rows,s->status?&*s->status:nullptr);
    }
    m_rows=std::move(rows);
    endResetModel();
}
void LoopTable::setStatus(const StatusFrame *status) {
    setStatusRows(m_rows,status);
    if(!m_rows.empty()) { emit dataChanged(index(0),index(int(m_rows.size())-1),{Overruns}); }
}
void LoopTable::setStatusRows(std::vector<Row> &rows,const StatusFrame *status) {
    for(auto &row:rows) {
        row.overruns.reset();
        if(!status) { continue; }
        for(const auto &o:status->overruns) { if(o.loop_id==row.id) { row.overruns=o.overruns; } }
    }
}
void LoopTable::setRunTime(uint8_t loop,std::optional<LoopRunTime> run,const QString &note) {
    for(size_t i=0;i<m_rows.size();++i) {
        if(m_rows[i].id!=loop) { continue; }
        m_rows[i].run=run;m_rows[i].note=note;
        emit dataChanged(index(int(i)),index(int(i)),{LastRunUs,MaxRunUs,Budget,HasRunTime,RunTimeNote});
    }
}
QVariant LoopTable::data(const QModelIndex &i,int role) const {
    if(!i.isValid() || size_t(i.row())>=m_rows.size()) { return {}; }
    const auto &r=m_rows[i.row()];
    switch(role) {
    case LoopId:return r.id;case Name:case Qt::DisplayRole:return r.name;case RateHz:return r.rate;
    case Realtime:return r.realtime;case Core:return r.core;case Running:return r.running;case State:return r.state;
    case LastRunUs:return r.run?QVariant(r.run->last_us):QVariant();
    case MaxRunUs:return r.run?QVariant(r.run->max_us):QVariant();
    case Budget:return r.run && r.running?QVariant(budget(r.run->last_us,r.rate)):QVariant();
    case Overruns:return r.overruns?QVariant(*r.overruns):QVariant();
    case HasRunTime:return bool(r.run);case RunTimeNote:return r.note;
    default:return {};
    }
}

QHash<int,QByteArray> FaultList::roleNames() const {
    return {{Source,"source"},{SourceName,"sourceName"},{InstanceId,"instanceId"},{Code,"code"},{Name,"name"},
        {Severity,"severity"},{Kind,"kind"},{Disarms,"disarms"},{BlocksArm,"blocksArm"},{Active,"active"},
        {Occurred,"occurred"},{Count,"count"},{InstanceLabel,"instanceLabel"}};
}
void FaultList::reset(const std::shared_ptr<const DeviceSnapshot> &s) {
    beginResetModel();m_rows.clear();
    if(s) {
        for(const auto &d:s->diagnostics) {
            Row row{d,diagSourceName(*s,d.source),{},{}};
            const auto code=std::find_if(s->codes.begin(),s->codes.end(),[&](const auto &c){return c.source==d.source && c.code==d.code;});
            if(code!=s->codes.end()) {
                row.name=QString::fromStdString(code->name);row.severity=code->severity;row.kind=code->kind;row.flags=code->flags;
            } else {
                row.name=QStringLiteral("0x%1").arg(d.code,4,16,QLatin1Char('0'));
            }
            if(d.instance!=0xFF) {
                if(auto it=s->configured.find({d.source,d.instance});it!=s->configured.end()) {
                    row.label=QString::fromStdString(it->second.instance.label);
                }
            }
            m_rows.push_back(std::move(row));
        }
        // Active first, then most frequent; stable for equal rows.
        std::stable_sort(m_rows.begin(),m_rows.end(),[](const Row &a,const Row &b) {
            const bool aa=a.entry.state&2,ba=b.entry.state&2;
            return aa!=ba?aa:a.entry.count>b.entry.count;
        });
    }
    endResetModel();
}
int FaultList::activeCount() const {
    return int(std::count_if(m_rows.begin(),m_rows.end(),[](const Row &r){return (r.entry.state&2)!=0;}));
}
QVariant FaultList::data(const QModelIndex &i,int role) const {
    if(!i.isValid() || size_t(i.row())>=m_rows.size()) { return {}; }
    const auto &r=m_rows[i.row()];
    switch(role) {
    case Source:return r.entry.source;case SourceName:return r.source;
    case InstanceId:return r.entry.instance==0xFF?QVariant():QVariant(r.entry.instance);
    case Code:return r.entry.code;case Name:case Qt::DisplayRole:return r.name;case Severity:return r.severity;
    case Kind:return r.kind;case Disarms:return (r.flags&4)!=0;case BlocksArm:return (r.flags&2)!=0;
    case Active:return (r.entry.state&2)!=0;case Occurred:return (r.entry.state&1)!=0;
    case Count:return r.entry.count;case InstanceLabel:return r.label;
    default:return {};
    }
}

RecentDevices::RecentDevices(QString path):m_path(std::move(path)) {
    if(m_path.isEmpty()) {
        const auto dir=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        m_path=QDir(dir).filePath(QStringLiteral("drivelink-gui.ini"));
    }
    m_cache=read();
}
QVariantList RecentDevices::list() const {return m_cache;}
QVariantList RecentDevices::read() const {
    QSettings settings(m_path,QSettings::IniFormat);
    QVariantList result;
    const int n=settings.beginReadArray(QStringLiteral("recentDevices"));
    for(int i=0;i<n && result.size()<kLimit;++i) {
        settings.setArrayIndex(i);
        const auto id=settings.value("id").toString();
        if(id.isEmpty()) { continue; }
        result.push_back(QVariantMap{{"id",id},{"name",settings.value("name").toString()},
            {"endpoint",settings.value("endpoint").toString()},{"baud",settings.value("baud",921600).toInt()},
            {"firmware",settings.value("firmware").toString()},{"lastSeen",settings.value("lastSeen").toDateTime()}});
    }
    settings.endArray();
    return result;
}
void RecentDevices::write(const QVariantList &rows) {
    QSettings settings(m_path,QSettings::IniFormat);
    settings.remove(QStringLiteral("recentDevices"));
    settings.beginWriteArray(QStringLiteral("recentDevices"),int(rows.size()));
    for(int i=0;i<rows.size();++i) {
        settings.setArrayIndex(i);
        const auto row=rows[i].toMap();
        for(const auto *key:{"id","name","endpoint","baud","firmware","lastSeen"}) { settings.setValue(key,row.value(key)); }
    }
    settings.endArray();
    settings.sync();
    m_cache=read();
}
void RecentDevices::remember(const QString &id,const QString &name,const QString &endpoint,int baud,
                             const QString &firmware,const QDateTime &when) {
    if(id.isEmpty()) { return; }
    auto rows=list();
    rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const QVariant &v){return v.toMap().value("id")==id;}),rows.end());
    rows.prepend(QVariantMap{{"id",id},{"name",name},{"endpoint",endpoint},{"baud",baud},{"firmware",firmware},{"lastSeen",when}});
    while(rows.size()>kLimit) { rows.removeLast(); }
    write(rows);
}
void RecentDevices::forget(const QString &id) {
    auto rows=list();
    rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const QVariant &v){return v.toMap().value("id")==id;}),rows.end());
    write(rows);
}
QVariantMap RecentDevices::onEndpoint(const QString &endpoint) const {
    for(const auto &row:list()) { if(row.toMap().value("endpoint")==endpoint) { return row.toMap(); } }
    return {};
}
void RecentDevices::recordUpdate(const QString &id,const QString &text) {
    if(id.isEmpty()) { return; }
    QSettings settings(m_path,QSettings::IniFormat);
    settings.setValue(QStringLiteral("lastUpdate/")+id,text);settings.sync();
}
QString RecentDevices::lastUpdate(const QString &id) const {
    if(id.isEmpty()) { return {}; }
    return QSettings(m_path,QSettings::IniFormat).value(QStringLiteral("lastUpdate/")+id).toString();
}

QVariantList portRows(const std::vector<SerialPortInfo> &ports,const RecentDevices &recent,const QString &active) {
    QVariantList rows;
    const auto known=recent.list();
    for(const auto &p:ports) {
        const auto endpoint=QString::fromStdString(p.endpoint);
        QVariantMap row{{"name",QString::fromStdString(p.name)},{"endpoint",endpoint},
            {"description",QString::fromStdString(p.description)},{"inUse",endpoint==active}};
        if(p.vid && p.pid) {
            row["usbId"]=QStringLiteral("%1:%2").arg(*p.vid,4,16,QLatin1Char('0')).arg(*p.pid,4,16,QLatin1Char('0'));
        }
        for(const auto &v:known) {
            const auto m=v.toMap();
            if(m.value("endpoint")==endpoint) { row["deviceId"]=m.value("id");row["deviceName"]=m.value("name");break; }
        }
        rows.push_back(row);
    }
    return rows;
}
} // namespace drivelink::gui
