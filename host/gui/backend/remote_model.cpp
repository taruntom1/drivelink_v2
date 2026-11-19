#include "remote_model.hpp"
#include "device_models.hpp"
#include <drivelink/client.hpp>
#include <drivelink/protocol/gen/registry.hpp>
#include <QTimeZone>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <map>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
QString tr(const char *text) {return QCoreApplication::translate("RemoteModel",text);}
QString fromStd(const std::string &s) {return QString::fromUtf8(s.data(),qsizetype(s.size()));}
QString kindOf(defs::HistoryKind k) {
    switch(k) {
    case defs::HistoryKind::DiagEvent:return QStringLiteral("diag");
    case defs::HistoryKind::Arm:return QStringLiteral("arm");
    case defs::HistoryKind::Lease:return QStringLiteral("lease");
    case defs::HistoryKind::Online:return QStringLiteral("online");
    case defs::HistoryKind::Offline:return QStringLiteral("offline");
    }
    return QStringLiteral("other");
}
QString codeName(const DeviceSnapshot *s,uint8_t source,uint16_t code) {
    if(s) {
        const auto it=std::find_if(s->codes.begin(),s->codes.end(),[&](const auto &c){return c.source==source && c.code==code;});
        if(it!=s->codes.end()) {return diagSourceName(*s,source)+QLatin1Char('.')+fromStd(it->name);}
    }
    return QStringLiteral("source %1 code 0x%2").arg(source).arg(code,4,16,QLatin1Char('0'));
}
QString holderLabel(uint16_t id,const QString &name,uint16_t self) {
    if(id!=0 && id==self) {return tr("this GUI");}
    return name.isEmpty()?tr("client %1").arg(id):name;
}
} // namespace

void LeaseTracker::reset(uint16_t self,const LeaseHolder &holder) {
    *this={};m_self=self;m_holder=holder;
}
LeaseTracker::Change LeaseTracker::apply(const agent::LeaseEvent &event) {
    Change change;
    const bool before=mine();
    ++m_events;
    m_holder=LeaseHolder{event.holder_id,fromStd(event.holder_name),event.holder_kind};
    if(m_self!=0 && event.previous_id==m_self && before) {
        change.lost=true;
        change.revoked=event.reason==uint8_t(defs::LeaseChange::Taken);
        if(change.revoked) {m_revoked=true;m_revokedBy.reset();}
    }
    // The grant after a revocation names who took it.
    if(m_revoked && event.holder_id!=0 && !mine() && !m_revokedBy) {m_revokedBy=m_holder;}
    // Any grant decides a request: ours, or another client's (ours was refused or never reached).
    if(event.holder_id!=0) {m_requesting=false;}
    if(mine() && !before) {change.gained=true;m_revoked=false;m_revokedBy.reset();}
    return change;
}
LeaseTracker::Change LeaseTracker::takeAnswered(bool ok,bool uncertain) {
    Change change;
    if(uncertain) {return change;} // still requesting: a Lease event decides
    // A Lease event already decided this request; what followed it is newer.
    if(!m_requesting) {return change;}
    m_requesting=false;
    if(ok && !mine()) {
        // The answer came before the grant event: the agent says we hold it.
        m_holder=LeaseHolder{m_self,{},uint8_t(defs::AgentClientKind::Gui)};
        change.gained=true;m_revoked=false;m_revokedBy.reset();
    }
    return change;
}
LeaseTracker::Change LeaseTracker::released() {
    Change change;
    m_requesting=false;
    if(mine()) {m_holder={};change.lost=true;}
    return change;
}
LeaseTracker::Change LeaseTracker::reconcile(const std::vector<agent::ClientRecord> &clients,uint64_t sentAt) {
    if(sentAt!=m_events) {return {};}
    const auto holder=std::find_if(clients.begin(),clients.end(),[](const auto &c){return c.holder;});
    const uint16_t id=holder!=clients.end()?holder->client_id:0;
    if(id==m_holder.id) {return {};}
    agent::LeaseEvent event;
    event.holder_id=id;
    if(holder!=clients.end()) {event.holder_name=holder->name;event.holder_kind=holder->kind;}
    event.previous_id=m_holder.id;
    event.reason=uint8_t(id!=0?defs::LeaseChange::Taken:defs::LeaseChange::Released);
    if(mine() && id!=0) {
        // Lost to another client: the revocation, then the grant.
        auto change=apply({0,{},0,m_self,uint8_t(defs::LeaseChange::Taken)});
        const auto grant=apply({id,event.holder_name,event.holder_kind,0,uint8_t(defs::LeaseChange::Taken)});
        change.gained=change.gained || grant.gained;
        return change;
    }
    return apply(event);
}
LeaseChip LeaseTracker::chip() const {
    if(m_revoked) {return LeaseChip::Revoked;}
    if(m_requesting) {return LeaseChip::Requesting;}
    if(mine()) {return LeaseChip::Mine;}
    if(m_holder.id==0) {return LeaseChip::Available;}
    return m_holder.kind==uint8_t(defs::AgentClientKind::Ros)?LeaseChip::Ros:LeaseChip::Other;
}
QString LeaseTracker::holderText() const {
    if(m_holder.id==0) {return {};}
    return holderLabel(m_holder.id,m_holder.name,m_self);
}

QString clientKindText(uint8_t kind) {
    switch(defs::AgentClientKind(kind)) {
    case defs::AgentClientKind::Gui:return QStringLiteral("GUI");
    case defs::AgentClientKind::Cli:return QStringLiteral("CLI");
    case defs::AgentClientKind::Ros:return QStringLiteral("ROS");
    default:return tr("Other");
    }
}

QString leaseBlock(bool remote,const LeaseTracker &lease,int armState,bool arming) {
    if(!remote || lease.mine()) {return {};}
    if(lease.otherHolds()) {
        return lease.holder().kind==uint8_t(defs::AgentClientKind::Ros)
            ?tr("Needs the control lease: ROS (%1) holds it").arg(lease.holderText())
            :tr("Needs the control lease: %1 holds it").arg(lease.holderText());
    }
    if(lease.requesting()) {return tr("Waiting for the control lease");}
    if(armState==1 || arming) {return tr("Needs the control lease while the device is armed");}
    return {};
}

QVariantList agentDeviceRows(const std::vector<agent::DeviceRecord> &devices) {
    QVariantList rows;
    for(const auto &d:devices) {
        const bool hasId=std::any_of(d.device_id.begin(),d.device_id.end(),[](uint8_t b){return b!=0;});
        rows.push_back(QVariantMap{{"alias",fromStd(d.alias)},{"id",hasId?fromStd(agent::formatDeviceId(d.device_id)):QString()},
            {"name",fromStd(d.name)},{"endpoint",fromStd(d.endpoint)},{"online",d.online},{"clients",int(d.clients)},
            {"holderId",int(d.holder_id)},{"holderName",fromStd(d.holder_name)},{"holderKind",int(d.holder_kind)},
            {"holderKindText",d.holder_id?clientKindText(d.holder_kind):QString()},{"device",fromStd(d.alias)}});
    }
    return rows;
}

QVariantList clientRows(const std::vector<agent::ClientRecord> &clients,uint16_t self) {
    QVariantList rows,others;
    for(const auto &c:clients) {
        QVariantMap row{{"id",int(c.client_id)},{"name",fromStd(c.name)},{"kind",int(c.kind)},
            {"kindText",clientKindText(c.kind)},{"holder",c.holder},{"self",c.client_id==self}};
        (c.client_id==self?rows:others).push_back(row);
    }
    rows.append(others);
    return rows;
}

QVariantList historyRows(const std::vector<agent::HistoryEntry> &entries,const DeviceSnapshot *snapshot,uint16_t self) {
    // A lease end names only the previous holder's id: take its name from the
    // grant recorded before it.
    std::map<uint16_t,QString> names;
    for(const auto &e:entries) {
        if(e.kind!=uint8_t(defs::HistoryKind::Lease)) {continue;}
        if(const auto lease=agent::decodeLease(e.body);lease && lease->holder_id!=0) {names[lease->holder_id]=fromStd(lease->holder_name);}
    }
    QVariantList rows;
    for(auto it=entries.rbegin();it!=entries.rend();++it) {
        const auto &e=*it;
        QString text;
        const auto kind=defs::HistoryKind(e.kind);
        switch(kind) {
        case defs::HistoryKind::DiagEvent: {
            try {
                const auto event=Client::decodeEvent(Frame{defs::msg::kDiagEvent,e.body});
                if(const auto *r=std::get_if<DiagRecord>(&event)) {text=codeName(snapshot,r->source,r->code)+tr(" · occurred");}
                else if(const auto *c=std::get_if<DiagCleared>(&event)) {text=codeName(snapshot,c->source,c->code)+tr(" · cleared");}
                else if(const auto *b=std::get_if<BootEvent>(&event)) {text=tr("Device booted (%1)").arg(resetReasonText(b->reset_reason));}
                else if(std::get_if<DiagCounters>(&event)) {text=tr("Diagnostic counters");}
                else {text=tr("Diagnostic event");}
            } catch(const Error &) {text=tr("Diagnostic event (unreadable)");}
            break;
        }
        case defs::HistoryKind::Arm:
            if(e.body.size()==2) {
                text=e.body[0]==1?tr("Armed"):e.body[0]==2?tr("Fault latched · %1").arg(disarmReasonText(e.body[1]))
                                                         :tr("Disarmed · %1").arg(disarmReasonText(e.body[1]));
            } else {text=tr("Arm state changed");}
            break;
        case defs::HistoryKind::Lease:
            if(const auto lease=agent::decodeLease(e.body)) {
                const auto holder=holderLabel(lease->holder_id,fromStd(lease->holder_name),self);
                const auto known=names.find(lease->previous_id);
                const auto previous=holderLabel(lease->previous_id,known!=names.end()?known->second:QString(),self);
                if(lease->holder_id!=0) {
                    text=tr("Lease taken by %1 (%2)").arg(holder,clientKindText(lease->holder_kind));
                } else if(lease->reason==uint8_t(defs::LeaseChange::Released)) {
                    text=tr("Lease released by %1").arg(previous);
                } else if(lease->reason==uint8_t(defs::LeaseChange::Disconnected)) {
                    text=tr("Lease ended: %1 disconnected").arg(previous);
                } else {
                    text=tr("Lease revoked from %1").arg(previous);
                }
            } else {text=tr("Lease changed");}
            break;
        case defs::HistoryKind::Online:text=tr("Device online");break;
        case defs::HistoryKind::Offline:text=tr("Device offline");break;
        default:text=tr("Agent event %1").arg(e.kind);break;
        }
        rows.push_back(QVariantMap{{"time",QDateTime::fromMSecsSinceEpoch(qint64(e.unix_ms),QTimeZone::UTC)},
            {"kind",kindOf(kind)},{"text",text},{"source",QStringLiteral("agent")}});
    }
    return rows;
}

QVariantMap agentFailure(const Error &e) {
    QVariantMap m{{"text",errorText(e)},{"agentVersion",0},{"coreVersion",0}};
    // The agent's refusals read better in its own words than the generic result text.
    const auto say=[&](const char *text) {m["text"]=tr(text);};
    const auto detail=defs::Detail(e.detail);
    const bool refused=e.kind==ErrorKind::Rejected;
    if(e.kind==ErrorKind::Connection || e.kind==ErrorKind::Timeout) {
        m["state"]=QStringLiteral("unreachable");
    } else if((refused && detail==defs::Detail::AgentVersion) || (e.kind==ErrorKind::Protocol && e.peer_version)) {
        m["state"]=QStringLiteral("version");
        m["agentVersion"]=int(e.peer_version);
    } else if(refused && detail==defs::Detail::BadToken) {
        m["state"]=QStringLiteral("token");say("The agent refused the token: check it on the agent's host.");
    } else if(refused && detail==defs::Detail::UnknownDevice) {
        m["state"]=QStringLiteral("unknownDevice");say("The agent has no device by that alias or id.");
    } else if(refused && detail==defs::Detail::DeviceOffline) {
        m["state"]=QStringLiteral("offline");say("The agent is reachable, but this device is offline: check the board and its cable on the agent's host.");
    } else if(refused && detail==defs::Detail::Capacity) {
        m["state"]=QStringLiteral("capacity");say("The agent has no room for another client on this device.");
    } else {
        m["state"]=QStringLiteral("other");
    }
    return m;
}

RecentAgents::RecentAgents(QString path):m_path(std::move(path)) {
    if(m_path.isEmpty()) {
        const auto dir=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        m_path=QDir(dir).filePath(QStringLiteral("drivelink-gui.ini"));
    }
    m_cache=read();
}
QVariantMap RecentAgents::find(const QString &address) const {
    for(const auto &row:m_cache) {if(row.toMap().value("address")==address) {return row.toMap();}}
    return {};
}
QVariantList RecentAgents::read() const {
    QSettings settings(m_path,QSettings::IniFormat);
    QVariantList result;
    const int n=settings.beginReadArray(QStringLiteral("recentAgents"));
    for(int i=0;i<n && result.size()<kLimit;++i) {
        settings.setArrayIndex(i);
        const auto address=settings.value("address").toString();
        if(address.isEmpty()) {continue;}
        const bool remember=settings.value("rememberToken",false).toBool();
        result.push_back(QVariantMap{{"address",address},{"device",settings.value("device").toString()},
            {"deviceId",settings.value("deviceId").toString()},{"deviceName",settings.value("deviceName").toString()},
            {"token",remember?settings.value("token").toString():QString()},{"rememberToken",remember},
            {"lastSeen",settings.value("lastSeen").toDateTime()}});
    }
    settings.endArray();
    return result;
}
void RecentAgents::write(const QVariantList &rows) {
    QSettings settings(m_path,QSettings::IniFormat);
    settings.remove(QStringLiteral("recentAgents"));
    settings.beginWriteArray(QStringLiteral("recentAgents"),int(rows.size()));
    for(int i=0;i<rows.size();++i) {
        settings.setArrayIndex(i);
        const auto row=rows[i].toMap();
        for(const auto *key:{"address","device","deviceId","deviceName","rememberToken","lastSeen"}) {settings.setValue(key,row.value(key));}
        // Never written unless asked for; a forgotten one is removed with its row.
        if(row.value("rememberToken").toBool()) {settings.setValue("token",row.value("token"));}
    }
    settings.endArray();
    settings.sync();
    m_cache=read();
}
void RecentAgents::remember(const QString &address,const QString &device,const QString &deviceId,const QString &deviceName,
                            const QString &token,bool rememberToken,const QDateTime &when) {
    if(address.isEmpty()) {return;}
    auto rows=m_cache;
    rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const QVariant &v){return v.toMap().value("address")==address;}),rows.end());
    rows.prepend(QVariantMap{{"address",address},{"device",device},{"deviceId",deviceId},{"deviceName",deviceName},
        {"token",rememberToken?token:QString()},{"rememberToken",rememberToken},{"lastSeen",when}});
    while(rows.size()>kLimit) {rows.removeLast();}
    write(rows);
}
void RecentAgents::forget(const QString &address) {
    auto rows=m_cache;
    rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const QVariant &v){return v.toMap().value("address")==address;}),rows.end());
    write(rows);
}

QString agentEndpoint(const QString &address) {
    QString text=address.trimmed();
    if(text.startsWith(QLatin1String("tcp://"))) {text=text.mid(6);}
    if(text.isEmpty() || text.contains(QLatin1Char('/')) || text.contains(QLatin1Char(' '))) {return {};}
    // host, host:port, [v6]:port
    QString host=text,port=QString::number(agent::kDefaultPort);
    const auto colon=text.lastIndexOf(QLatin1Char(':'));
    const bool v6=text.startsWith(QLatin1Char('['));
    if(colon>0 && (!v6 || text.at(colon-1)==QLatin1Char(']'))) {host=text.left(colon);port=text.mid(colon+1);}
    bool ok=false;const int p=port.toInt(&ok);
    if(host.isEmpty() || !ok || p<=0 || p>65535) {return {};}
    return QStringLiteral("tcp://%1:%2").arg(host).arg(p);
}

} // namespace drivelink::gui
