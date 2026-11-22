#include "tune_session.hpp"
#include "command_model.hpp"
#include <drivelink/protocol/defs.hpp>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <set>

namespace drivelink::gui {
namespace {
namespace defs=protocol::defs;
QString tr(const char *text) { return QCoreApplication::translate("DriveLink", text); }
QString str(const std::string &s) { return QString::fromStdString(s); }
float f32(double v) { return float(v); }

// ---- writing --------------------------------------------------------------
QJsonObject refJson(const Ref &r) { return {{"type",r.type},{"instance",r.instance},{"item",r.item}}; }
QJsonObject controllerJson(InstanceKey k, const QString &type, const QString &label) {
    return {{"type",k.first},{"instance",k.second},{"typeName",type},{"label",label}};
}
QJsonArray gainsJson(const std::map<uint16_t,double> &gains, const std::map<uint16_t,QString> &names) {
    QJsonArray a;
    for(const auto &[id,value]:gains) {
        const auto n=names.find(id);
        a.append(QJsonObject{{"id",id},{"name",n==names.end()?QString():n->second},{"value",value}});
    }
    return a;
}
QJsonObject signalJson(const TestSignal &s) {
    return {{"target",refJson(s.target)},{"waveform",int(s.waveform)},{"offset",s.offset},{"amplitude",s.amplitude},
        {"f0",s.f0},{"f1",s.f1},{"duration",s.duration_s},{"repeat",double(s.repeat)}};
}
QJsonObject captureJson(const CaptureSetup &c) {
    QJsonArray refs;
    for(const auto &r:c.refs) {refs.append(refJson(r));}
    return {{"refs",refs},{"divider",c.divider},{"samples",double(c.samples)},{"pre",double(c.pre)}};
}
QString column(const SampleRing &ring, bool times) {
    QByteArray bytes(qsizetype(ring.size()*8),Qt::Uninitialized);
    for(size_t i=0;i<ring.size();++i) {
        if(times) {qToLittleEndian<qint64>(ring.time(i),bytes.data()+i*8);}
        else {qToLittleEndian<double>(ring.value(i),bytes.data()+i*8);}
    }
    return QString::fromLatin1(bytes.toBase64());
}

// ---- reading ---------------------------------------------------------------
struct Bad { QString why; };
[[noreturn]] void bad(const QString &why) { throw Bad{why}; }
QJsonValue field(const QJsonObject &o, const char *name, const QString &where) {
    const auto v=o.value(QLatin1String(name));
    if(v.isUndefined()) {bad(tr("%1: \"%2\" is missing").arg(where,QLatin1String(name)));}
    return v;
}
QJsonObject object(const QJsonObject &o, const char *name, const QString &where) {
    const auto v=field(o,name,where);
    if(!v.isObject()) {bad(tr("%1: \"%2\" is not an object").arg(where,QLatin1String(name)));}
    return v.toObject();
}
QJsonArray array(const QJsonObject &o, const char *name, const QString &where) {
    const auto v=field(o,name,where);
    if(!v.isArray()) {bad(tr("%1: \"%2\" is not a list").arg(where,QLatin1String(name)));}
    return v.toArray();
}
QString text(const QJsonObject &o, const char *name, const QString &where) {
    const auto v=field(o,name,where);
    if(!v.isString()) {bad(tr("%1: \"%2\" is not text").arg(where,QLatin1String(name)));}
    return v.toString();
}
double number(const QJsonObject &o, const char *name, const QString &where) {
    const auto v=field(o,name,where);
    if(!v.isDouble() || !std::isfinite(v.toDouble())) {bad(tr("%1: \"%2\" is not a number").arg(where,QLatin1String(name)));}
    return v.toDouble();
}
// A whole number in [lo, hi].
int64_t whole(const QJsonObject &o, const char *name, const QString &where, double lo, double hi) {
    const double v=number(o,name,where);
    if(std::floor(v)!=v || v<lo || v>hi) {bad(tr("%1: \"%2\" is out of range").arg(where,QLatin1String(name)));}
    return int64_t(v);
}
bool flag(const QJsonObject &o, const char *name, const QString &where) {
    const auto v=field(o,name,where);
    if(!v.isBool()) {bad(tr("%1: \"%2\" is not true or false").arg(where,QLatin1String(name)));}
    return v.toBool();
}
Ref readRef(const QJsonObject &o, const QString &where) {
    return {uint8_t(whole(o,"type",where,0,255)),uint8_t(whole(o,"instance",where,0,255)),uint16_t(whole(o,"item",where,0,65535))};
}
void readController(const QJsonObject &o, const QString &where, InstanceKey &key, QString &type, QString &label) {
    key={uint8_t(whole(o,"type",where,0,255)),uint8_t(whole(o,"instance",where,0,255))};
    type=text(o,"typeName",where);label=text(o,"label",where);
}
void readGains(const QJsonArray &a, const QString &where, std::map<uint16_t,double> &gains, std::map<uint16_t,QString> &names) {
    for(const auto &v:a) {
        if(!v.isObject()) {bad(tr("%1: a gain is not an object").arg(where));}
        const auto g=v.toObject();
        const auto id=uint16_t(whole(g,"id",where,0,65535));
        if(gains.contains(id)) {bad(tr("%1: gain %2 is listed twice").arg(where).arg(id));}
        gains[id]=number(g,"value",where);
        names[id]=text(g,"name",where);
    }
}
TestSignal readSignal(const QJsonObject &o, const QString &where) {
    TestSignal s;
    s.target=readRef(object(o,"target",where),where);
    s.waveform=Waveform(whole(o,"waveform",where,0,5));
    s.offset=number(o,"offset",where);s.amplitude=number(o,"amplitude",where);
    s.f0=number(o,"f0",where);s.f1=number(o,"f1",where);
    s.duration_s=number(o,"duration",where);
    s.repeat=uint32_t(whole(o,"repeat",where,1,1000));
    return s;
}
CaptureSetup readCapture(const QJsonObject &o, const QString &where) {
    CaptureSetup c;
    for(const auto &v:array(o,"refs",where)) {
        if(!v.isObject()) {bad(tr("%1: a capture signal is not an object").arg(where));}
        c.refs.push_back(readRef(v.toObject(),where));
    }
    c.divider=uint16_t(whole(o,"divider",where,1,65535));
    c.samples=uint32_t(whole(o,"samples",where,0,4294967295.0));
    c.pre=uint32_t(whole(o,"pre",where,0,4294967295.0));
    return c;
}
QByteArray decodeColumn(const QJsonObject &o, const char *name, const QString &where) {
    const auto encoded=text(o,name,where).toLatin1();
    auto decoded=QByteArray::fromBase64Encoding(encoded,QByteArray::AbortOnBase64DecodingErrors);
    if(!decoded) {bad(tr("%1: \"%2\" is not base64").arg(where,QLatin1String(name)));}
    if(decoded.decoded.size()%8!=0) {bad(tr("%1: \"%2\" is not whole 8-byte values").arg(where,QLatin1String(name)));}
    if(size_t(decoded.decoded.size()/8)>kMaxSessionSamples) {bad(tr("%1: more than %2 samples").arg(where).arg(kMaxSessionSamples));}
    return std::move(decoded.decoded);
}
SampleRing readSeries(const QJsonObject &o, const QString &where) {
    const auto t=decodeColumn(o,"t",where),v=decodeColumn(o,"v",where);
    if(t.size()!=v.size()) {bad(tr("%1: times and values differ in length").arg(where));}
    const size_t n=size_t(t.size()/8);
    SampleRing ring(std::max<size_t>(16,n));
    int64_t last=INT64_MIN;
    for(size_t i=0;i<n;++i) {
        const auto us=qFromLittleEndian<qint64>(t.constData()+i*8);
        if(us<last) {bad(tr("%1: times go back").arg(where));}
        last=us;
        ring.push(us,qFromLittleEndian<double>(v.constData()+i*8));
    }
    return ring;
}
} // namespace

bool sameGains(const std::map<uint16_t,double> &a, const std::map<uint16_t,double> &b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const auto &x,const auto &y) {
        return x.first==y.first && f32(x.second)==f32(y.second);
    });
}
QString gainChanges(const std::map<uint16_t,QString> &names, const std::map<uint16_t,double> &from,
                    const std::map<uint16_t,double> &to) {
    QStringList parts;
    for(const auto &[id,value]:to) {
        const auto it=from.find(id);
        if(it!=from.end() && f32(it->second)==f32(value)) {continue;}
        const auto n=names.find(id);
        const auto name=n==names.end() || n->second.isEmpty()?QString::number(id):n->second;
        parts<<(it==from.end()?QStringLiteral("%1 %2").arg(name,gainText(value))
                              :QStringLiteral("%1 %2 → %3").arg(name,gainText(it->second),gainText(value)));
    }
    return parts.join(QStringLiteral(", "));
}
QString equalGains(const std::map<uint16_t,QString> &names, const std::map<uint16_t,double> &a,
                   const std::map<uint16_t,double> &b) {
    QStringList same;
    for(const auto &[id,value]:b) {
        const auto it=a.find(id);
        if(it==a.end() || f32(it->second)!=f32(value)) {continue;}
        const auto n=names.find(id);
        same<<(n==names.end() || n->second.isEmpty()?QString::number(id):n->second);
    }
    return same.isEmpty()?QString():tr("%1 equal").arg(same.join(QStringLiteral(", ")));
}
bool sameController(const QString &typeA, const QString &labelA, const QString &typeB, const QString &labelB) {
    return typeA==typeB && labelA==labelB;
}
int lastEntry(const std::vector<HistoryEntry> &h, const QString &type, const QString &label, const QString &device) {
    for(size_t i=h.size();i-->0;) {
        if(sameController(h[i].controllerType,h[i].controllerLabel,type,label) && h[i].device==device) {return int(i);}
    }
    return -1;
}
int recordEntry(std::vector<HistoryEntry> &h, HistoryEntry e) {
    const int last=lastEntry(h,e.controllerType,e.controllerLabel,e.device);
    if(last>=0 && sameGains(h[size_t(last)].gains,e.gains)) {return last;}
    const auto type=e.controllerType,label=e.controllerLabel;
    h.push_back(std::move(e));
    // The controller's oldest entries beyond the limit go.
    size_t count=size_t(std::count_if(h.begin(),h.end(),[&](const HistoryEntry &x){return sameController(x.controllerType,x.controllerLabel,type,label);}));
    while(count>kMaxHistory) {
        const auto it=std::find_if(h.begin(),h.end(),[&](const HistoryEntry &x){return sameController(x.controllerType,x.controllerLabel,type,label);});
        h.erase(it);--count;
    }
    return int(h.size())-1;
}
size_t sessionSamples(const std::vector<RunRecord> &runs) {
    size_t n=0;
    for(const auto &r:runs) {for(const auto &s:r.series) {n+=s.size();}}
    return n;
}
std::optional<int> entryRun(const HistoryEntry &e, const std::vector<RunRecord> &runs) {
    for(auto it=e.runs.rbegin();it!=e.runs.rend();++it) {
        if(std::any_of(runs.begin(),runs.end(),[&](const RunRecord &r){return r.number==*it;})) {return *it;}
    }
    return std::nullopt;
}

std::map<uint16_t,QString> gainNamesOf(const DeviceSnapshot &s, InstanceKey k) {
    std::map<uint16_t,QString> names;
    const auto t=s.types.find(k.first);
    if(t==s.types.end()) {return names;}
    for(const auto &[id,item]:t->second.items) {
        if(item.descriptor.kind==ItemKind::Param && tuneFlagged(item.descriptor)) {names[id]=str(item.descriptor.name);}
    }
    return names;
}
std::expected<ResolvedGains,QString> resolveGains(const DeviceSnapshot &s, const QString &type, const QString &label,
                                                  const std::map<uint16_t,double> &gains,
                                                  const std::map<uint16_t,QString> &names) {
    if(gains.empty()) {return std::unexpected(tr("No gains recorded"));}
    std::optional<InstanceKey> key;
    for(const auto &c:tuneControllers(s)) {
        if(c.typeName==type && c.label==label) {key=c.key;break;}
    }
    if(!key) {return std::unexpected(tr("No running %1 named %2 on this device").arg(type,label));}
    const auto &items=s.types.at(key->first).items;
    ResolvedGains out{*key,{}};
    for(const auto &[id,value]:gains) {
        const auto n=names.find(id);
        if(n==names.end() || n->second.isEmpty()) {return std::unexpected(tr("Gain %1 has no name").arg(id));}
        const auto it=std::find_if(items.begin(),items.end(),[&](const auto &x) {return str(x.second.descriptor.name)==n->second;});
        if(it==items.end()) {return std::unexpected(tr("%1 has no gain %2").arg(type,n->second));}
        const auto &d=it->second.descriptor;
        const bool numericType=d.value_type==ValueType::U8 || d.value_type==ValueType::U16 || d.value_type==ValueType::U32 ||
                               d.value_type==ValueType::I32 || d.value_type==ValueType::F32;
        if(d.kind!=ItemKind::Param || !tuneFlagged(d) || !numericType ||
           (d.flags&(uint16_t(defs::ItemFlag::RebootRequired)|uint16_t(defs::ItemFlag::ReadOnly)))) {
            return std::unexpected(tr("%1 is not a live gain here").arg(n->second));
        }
        // What the type holds (a file may carry anything): never an infinite
        // f32 or an integer conversion out of range.
        double lo=0,hi=0;
        switch(d.value_type) {
        case ValueType::U8:hi=255;break;
        case ValueType::U16:hi=65535;break;
        case ValueType::U32:hi=4294967295.0;break;
        case ValueType::I32:lo=double(INT32_MIN);hi=double(INT32_MAX);break;
        default:lo=-double(std::numeric_limits<float>::max());hi=double(std::numeric_limits<float>::max());break;
        }
        if(d.has_min) {lo=std::max(lo,bitsToNumber(d.value_type,d.min_bits));}
        if(d.has_max) {hi=std::min(hi,bitsToNumber(d.value_type,d.max_bits));}
        if(!std::isfinite(value) || value<lo || value>hi) {
            return std::unexpected(tr("%1 %2 is outside its range here").arg(n->second,gainText(value)));
        }
        if(d.value_type!=ValueType::F32 && std::floor(value)!=value) {
            return std::unexpected(tr("%1 %2 is not a whole number").arg(n->second,gainText(value)));
        }
        out.values.emplace_back(it->first,value);
    }
    return out;
}

std::vector<MetricComparison> compareMetrics(const RunRecord &a, const RunRecord &b) {
    const QString unit=b.response>=0?b.units[size_t(b.response)]:a.response>=0?a.units[size_t(a.response)]:QString();
    const auto signedText=[](const std::optional<double> &x,const std::optional<double> &y,const QString &u,double scale,int decimals) {
        if(!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {return QStringLiteral("—");}
        const double d=(*y-*x)*scale;
        auto t=QLocale::c().toString(std::fabs(d),'f',decimals);
        if(QLocale::c().toDouble(t)!=0) {t=(d<0?QStringLiteral("−"):QStringLiteral("+"))+t;}
        return u.isEmpty()?t:t+QChar(' ')+u;
    };
    const auto steady=[&](const RunRecord &r) {
        const double scale=r.metrics.steady_error?std::fabs(*r.metrics.steady_error):0;
        return scale>=10?1:scale>=0.1?2:3;
    };
    const int sd=std::max(steady(a),steady(b));
    return {
        {tr("Rise time"),metricText(a.metrics.rise_s,tr("ms"),1000,0),metricText(b.metrics.rise_s,tr("ms"),1000,0),
         signedText(a.metrics.rise_s,b.metrics.rise_s,tr("ms"),1000,0)},
        {tr("Overshoot"),metricText(a.metrics.overshoot_pct,QStringLiteral("%"),1,0),metricText(b.metrics.overshoot_pct,QStringLiteral("%"),1,0),
         signedText(a.metrics.overshoot_pct,b.metrics.overshoot_pct,tr("points"),1,0)},
        {tr("Settling 2 %"),metricText(a.metrics.settling_s,tr("ms"),1000,0),metricText(b.metrics.settling_s,tr("ms"),1000,0),
         signedText(a.metrics.settling_s,b.metrics.settling_s,tr("ms"),1000,0)},
        {tr("Steady error"),metricText(a.metrics.steady_error,unit,1,sd),metricText(b.metrics.steady_error,unit,1,sd),
         signedText(a.metrics.steady_error,b.metrics.steady_error,unit,1,sd)},
    };
}

QByteArray writeSession(const SessionData &d) {
    QJsonArray runs,history,windows;
    for(const auto &r:d.runs) {
        QJsonArray names,units,series;
        for(size_t i=0;i<r.series.size();++i) {
            names.append(i<r.names.size()?r.names[i]:QString());
            units.append(i<r.units.size()?r.units[i]:QString());
            series.append(QJsonObject{{"t",column(r.series[i],true)},{"v",column(r.series[i],false)}});
        }
        QJsonObject o{{"number",r.number},{"colour",r.colour},{"time",r.time},
            {"controller",controllerJson(r.controller,r.controllerType,r.controllerLabel)},
            {"signal",signalJson(r.signal)},{"capture",captureJson(r.capture)},
            {"names",names},{"units",units},{"series",series},
            {"startUs",double(r.startUs)},{"endUs",double(r.endUs)},
            {"gains",gainsJson(r.gains,r.gainNames)},{"changed",r.changed},{"end",int(r.end)},
            {"irregular",r.irregular},{"pinned",r.pinned},{"response",r.response},
            {"device",r.device.isEmpty()?d.deviceId:r.device}};
        if(r.endedUs) {o["endedUs"]=double(*r.endedUs);}
        runs.append(o);
    }
    for(const auto &e:d.history) {
        QJsonArray numbers;
        for(const int n:e.runs) {numbers.append(n);}
        history.append(QJsonObject{{"id",e.id},{"time",e.time},
            {"controller",controllerJson(e.controller,e.controllerType,e.controllerLabel)},
            {"gains",gainsJson(e.gains,e.names)},{"source",int(e.source)},{"change",e.change},{"note",e.note},{"runs",numbers},
            {"device",e.device.isEmpty()?d.deviceId:e.device}});
    }
    for(const auto &w:d.windows) {
        windows.append(QJsonObject{{"typeName",w.controllerType},{"label",w.controllerLabel},{"gain",w.gain},{"lo",w.lo},{"hi",w.hi}});
    }
    QJsonObject root{{"format",QLatin1String(kSessionFormat)},{"version",kSessionVersion},
        {"device",QJsonObject{{"id",d.deviceId},{"name",d.deviceName}}},{"name",d.name},{"saved",d.savedAt},
        {"runs",runs},{"history",history},{"windows",windows},
        {"nextNumber",d.nextNumber},{"nextColour",d.nextColour},{"nextHistory",d.nextHistory}};
    if(d.setup) {
        root["setup"]=QJsonObject{{"controller",QJsonObject{{"type",d.setup->controller.first},{"instance",d.setup->controller.second}}},
            {"signal",signalJson(d.setup->signal)},{"capture",captureJson(d.setup->capture)}};
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::expected<SessionData,QString> readSession(const QByteArray &bytes) {
    QJsonParseError error;
    const auto doc=QJsonDocument::fromJson(bytes,&error);
    if(error.error!=QJsonParseError::NoError) {return std::unexpected(tr("Not a JSON file: %1").arg(error.errorString()));}
    if(!doc.isObject()) {return std::unexpected(tr("Not a DriveLink tune session"));}
    const auto root=doc.object();
    if(root.value("format").toString()!=QLatin1String(kSessionFormat)) {return std::unexpected(tr("Not a DriveLink tune session"));}
    const auto version=root.value("version");
    if(!version.isDouble()) {return std::unexpected(tr("The session has no version"));}
    if(version.toDouble()!=kSessionVersion) {
        return std::unexpected(version.toDouble()>kSessionVersion?tr("The session was saved by a newer DriveLink (version %1)").arg(version.toDouble())
                                                                  :tr("Unknown session version %1").arg(version.toDouble()));
    }
    SessionData d;
    try {
        const QString top=tr("session");
        const auto device=object(root,"device",top);
        d.deviceId=text(device,"id",top);d.deviceName=text(device,"name",top);
        d.name=text(root,"name",top);d.savedAt=text(root,"saved",top);
        d.nextNumber=int(whole(root,"nextNumber",top,1,1e9));
        d.nextColour=int(whole(root,"nextColour",top,0,1e9));
        d.nextHistory=int(whole(root,"nextHistory",top,1,1e9));
        std::set<int> numbers;
        for(const auto &v:array(root,"runs",top)) {
            if(!v.isObject()) {bad(tr("a run is not an object"));}
            const auto o=v.toObject();
            RunRecord r;
            r.number=int(whole(o,"number",tr("run"),1,1e9));
            const QString where=tr("run %1").arg(r.number);
            if(!numbers.insert(r.number).second) {bad(tr("%1 is listed twice").arg(where));}
            if(r.number>=d.nextNumber) {bad(tr("%1: number not below nextNumber").arg(where));}
            r.colour=int(whole(o,"colour",where,0,7));
            r.time=text(o,"time",where);
            readController(object(o,"controller",where),where,r.controller,r.controllerType,r.controllerLabel);
            r.signal=readSignal(object(o,"signal",where),where);
            r.capture=readCapture(object(o,"capture",where),where);
            const auto names=array(o,"names",where),units=array(o,"units",where),series=array(o,"series",where);
            const auto n=r.capture.refs.size();
            if(n==0 || size_t(names.size())!=n || size_t(units.size())!=n || size_t(series.size())!=n) {
                bad(tr("%1: the signals, names, units and series do not match").arg(where));
            }
            for(qsizetype i=0;i<qsizetype(n);++i) {
                if(!names[i].isString() || !units[i].isString() || !series[i].isObject()) {bad(tr("%1: a series is malformed").arg(where));}
                r.names.push_back(names[i].toString());r.units.push_back(units[i].toString());
                r.series.push_back(readSeries(series[i].toObject(),where));
            }
            r.startUs=whole(o,"startUs",where,-9e15,9e15);r.endUs=whole(o,"endUs",where,-9e15,9e15);
            readGains(array(o,"gains",where),where,r.gains,r.gainNames);
            r.changed=text(o,"changed",where);
            r.end=RunEnd(whole(o,"end",where,0,2));
            if(o.contains("endedUs")) {r.endedUs=whole(o,"endedUs",where,-9e15,9e15);}
            r.irregular=flag(o,"irregular",where);r.pinned=flag(o,"pinned",where);
            r.response=int(whole(o,"response",where,-1,double(n)-1));
            r.device=o.contains("device")?text(o,"device",where):d.deviceId;
            r.metrics=runMetrics(r);
            d.runs.push_back(std::move(r));
        }
        std::set<int> ids;
        for(const auto &v:array(root,"history",top)) {
            if(!v.isObject()) {bad(tr("a gain history entry is not an object"));}
            const auto o=v.toObject();
            HistoryEntry e;
            e.id=int(whole(o,"id",tr("gain history"),1,1e9));
            const QString where=tr("gain history entry %1").arg(e.id);
            if(!ids.insert(e.id).second) {bad(tr("%1 is listed twice").arg(where));}
            if(e.id>=d.nextHistory) {bad(tr("%1: id not below nextHistory").arg(where));}
            e.time=text(o,"time",where);
            readController(object(o,"controller",where),where,e.controller,e.controllerType,e.controllerLabel);
            readGains(array(o,"gains",where),where,e.gains,e.names);
            e.source=HistorySource(whole(o,"source",where,0,3));
            e.change=text(o,"change",where);e.note=text(o,"note",where);
            e.device=o.contains("device")?text(o,"device",where):d.deviceId;
            for(const auto &n:array(o,"runs",where)) {
                if(!n.isDouble()) {bad(tr("%1: a run number is not a number").arg(where));}
                // A run that is not in the file (trimmed) is dropped.
                if(numbers.contains(n.toInt())) {e.runs.push_back(n.toInt());}
            }
            d.history.push_back(std::move(e));
        }
        for(const auto &v:array(root,"windows",top)) {
            if(!v.isObject()) {bad(tr("a gain window is not an object"));}
            const auto o=v.toObject();const QString where=tr("gain window");
            SessionWindow w{text(o,"typeName",where),text(o,"label",where),text(o,"gain",where),number(o,"lo",where),number(o,"hi",where)};
            if(!(w.lo>0) || !(w.hi>w.lo)) {bad(tr("%1 %2: low must be above 0 and below high").arg(where,w.gain));}
            d.windows.push_back(w);
        }
        if(root.contains("setup")) {
            const auto o=object(root,"setup",top);const QString where=tr("setup");
            const auto c=object(o,"controller",where);
            SessionSetup s;
            s.controller={uint8_t(whole(c,"type",where,0,255)),uint8_t(whole(c,"instance",where,0,255))};
            s.signal=readSignal(object(o,"signal",where),where);
            s.capture=readCapture(object(o,"capture",where),where);
            d.setup=s;
        }
    } catch(const Bad &b) {return std::unexpected(b.why);}
    if(sessionSamples(d.runs)>kMaxSessionTotalSamples) {return std::unexpected(tr("More than %1 samples in all").arg(kMaxSessionTotalSamples));}
    // Oldest first, as the page keeps them.
    std::sort(d.runs.begin(),d.runs.end(),[](const RunRecord &a,const RunRecord &b){return a.number<b.number;});
    trimRuns(d.runs);
    return d;
}

QString runCsv(const RunRecord &r) {
    QString out;
    const auto c=QLocale::c();
    const auto quote=[](QString s) {
        if(s.contains(',') || s.contains('"') || s.contains('\n')) {s.replace('"',QStringLiteral("\"\""));s='"'+s+'"';}
        return s;
    };
    out+=QStringLiteral("# DriveLink tune run %1, %2, %3 (%4)\n").arg(r.number).arg(r.time,r.controllerLabel,r.controllerType);
    const auto &s=r.signal;
    out+=QStringLiteral("# signal: %1, offset %2, amplitude %3").arg(waveformName(s.waveform),c.toString(s.offset,'g',7),c.toString(s.amplitude,'g',7));
    if(waveformUsesFrequency(s.waveform)) {out+=QStringLiteral(", f0 %1 Hz").arg(c.toString(s.f0,'g',7));}
    if(waveformIsChirp(s.waveform)) {out+=QStringLiteral(", f1 %1 Hz").arg(c.toString(s.f1,'g',7));}
    out+=QStringLiteral(", %1 s × %2\n").arg(c.toString(s.duration_s,'g',7)).arg(s.repeat);
    QStringList gains;
    for(const auto &[id,value]:r.gains) {
        const auto n=r.gainNames.find(id);
        gains<<QStringLiteral("%1=%2").arg(n==r.gainNames.end()?QString::number(id):n->second,c.toString(value,'g',7));
    }
    out+=QStringLiteral("# gains: %1\n").arg(gains.join(QChar(' ')));
    if(r.end!=RunEnd::Completed) {
        out+=QStringLiteral("# ended: %1").arg(r.end==RunEnd::Stopped?QStringLiteral("stopped"):QStringLiteral("disarmed"));
        if(r.endedUs) {out+=QStringLiteral(" at %1 s").arg(c.toString(double(*r.endedUs)/1e6,'f',6));}
        out+=QChar('\n');
    }
    QStringList header{QStringLiteral("time_s")};
    for(size_t i=0;i<r.series.size();++i) {
        const auto name=i<r.names.size()?r.names[i]:QStringLiteral("?");
        const auto unit=i<r.units.size()?r.units[i]:QString();
        header<<quote(unit.isEmpty()?name:QStringLiteral("%1 (%2)").arg(name,unit));
    }
    out+=header.join(',')+QChar('\n');
    // One row per sample of the first series; every series of a capture
    // shares its timestamps.
    const size_t rows=r.series.empty()?0:r.series.front().size();
    for(size_t i=0;i<rows;++i) {
        out+=c.toString(double(r.series.front().time(i))/1e6,'f',6);
        for(const auto &ring:r.series) {
            out+=QChar(',');
            if(i<ring.size()) {out+=c.toString(ring.value(i),'g',9);}
        }
        out+=QChar('\n');
    }
    return out;
}

QString sessionFilePath(QString path) {
    if(!path.endsWith(QLatin1String(".json"),Qt::CaseInsensitive)) {path+=QLatin1String(".tune.json");}
    return path;
}
QString csvFilePath(QString path) {
    if(!path.endsWith(QLatin1String(".csv"),Qt::CaseInsensitive)) {path+=QLatin1String(".csv");}
    return path;
}
} // namespace drivelink::gui
