#include "calibration_qml.hpp"
#include <QJSValue>

namespace drivelink::gui::imu_calibration {
namespace {
// QML hands JS objects over as maps; a QJSValue holding one is unwrapped too.
QVariant plain(const QVariant &v) {return v.metaType()==QMetaType::fromType<QJSValue>()?v.value<QJSValue>().toVariant():v;}
bool absent(const QVariant &v) {const auto p=plain(v);return !p.isValid() || p.isNull();}
QVariantMap levelsMap(const Levels &l) {
    QVariantMap m;
    for(size_t i=0;i<4;++i) {m[QLatin1String(kLevelNames[i])]=l[i];}
    return m;
}
Levels levelsFrom(const QVariantMap &m) {
    Levels l{-1,-1,-1,-1};
    for(size_t i=0;i<4;++i) {l[i]=m.value(QLatin1String(kLevelNames[i]),-1).toInt();}
    return l;
}
}

QVariantMap toVariant(const Calibration &c) {
    const auto &v=c.values;
    QVariantList sic;
    for(int i=11;i<kValues;++i) {sic<<v[size_t(i)];}
    return {{"hasSic",c.has_sic},{"accel",QVariantMap{{"x",v[0]},{"y",v[1]},{"z",v[2]},{"radius",v[3]}}},
            {"gyro",QVariantMap{{"x",v[4]},{"y",v[5]},{"z",v[6]}}},
            {"mag",QVariantMap{{"x",v[7]},{"y",v[8]},{"z",v[9]},{"radius",v[10]}}},{"sic",sic}};
}
Calibration calibrationFrom(const QVariantMap &m) {
    Calibration c;c.has_sic=m.value(QStringLiteral("hasSic")).toBool();
    const auto a=m.value(QStringLiteral("accel")).toMap(),g=m.value(QStringLiteral("gyro")).toMap(),
               mg=m.value(QStringLiteral("mag")).toMap();
    const std::array<std::pair<const QVariantMap *,const char *>,11> fields{{
        {&a,"x"},{&a,"y"},{&a,"z"},{&a,"radius"},{&g,"x"},{&g,"y"},{&g,"z"},{&mg,"x"},{&mg,"y"},{&mg,"z"},{&mg,"radius"}}};
    for(size_t i=0;i<fields.size();++i) {c.values[i]=fields[i].first->value(QLatin1String(fields[i].second)).toInt();}
    const auto sic=m.value(QStringLiteral("sic")).toList();
    for(int i=0;i<9 && i<sic.size();++i) {c.values[size_t(11+i)]=sic[i].toInt();}
    return c;
}

QVariantMap toVariant(const Provenance &p) {
    return {{"type",p.type},{"createdUtc",p.created_utc},{"deviceId",p.device_id},{"deviceName",p.device_name},
            {"instance",p.instance},{"firmware",p.firmware},{"gui",p.gui},{"levels",levelsMap(p.levels)},
            {"actualMode",p.actual_mode},{"note",p.note}};
}
Provenance provenanceFrom(const QVariantMap &m) {
    Provenance p;
    p.type=m.value(QStringLiteral("type")).toString();p.created_utc=m.value(QStringLiteral("createdUtc")).toString();
    p.device_id=m.value(QStringLiteral("deviceId")).toString();p.device_name=m.value(QStringLiteral("deviceName")).toString();
    p.instance=m.value(QStringLiteral("instance")).toString();p.firmware=m.value(QStringLiteral("firmware")).toString();
    p.gui=m.value(QStringLiteral("gui")).toString();p.levels=levelsFrom(m.value(QStringLiteral("levels")).toMap());
    p.actual_mode=m.value(QStringLiteral("actualMode"),-1).toInt();p.note=m.value(QStringLiteral("note")).toString();
    return p;
}

QVariantMap toVariant(const SensorState &s) {
    static const std::array<const char *,4> kinds{"unavailable","waiting","stale","live"};
    QVariantMap m{{"state",QLatin1String(kinds[size_t(s.state)])},{"note",s.note},{"levels",levelsMap(s.levels)},
                  {"valid",s.valid},{"sampleCount",s.sample_count},{"ageUs",s.age_us},{"mode",s.mode},{"available",s.available}};
    m["euler"]=s.euler?QVariant(QVariantMap{{"roll",s.euler->roll},{"pitch",s.euler->pitch},{"yaw",s.euler->yaw}}):QVariant::fromValue(nullptr);
    m["quat"]=s.quat?QVariant(QVariantMap{{"w",s.quat->w},{"x",s.quat->x},{"y",s.quat->y},{"z",s.quat->z}}):QVariant::fromValue(nullptr);
    return m;
}
SensorState sensorFrom(const QVariantMap &m) {
    SensorState s;
    const auto state=m.value(QStringLiteral("state")).toString();
    s.state=state==QLatin1String("live")?SensorKind::Live:state==QLatin1String("stale")?SensorKind::Stale
           :state==QLatin1String("waiting")?SensorKind::Waiting:SensorKind::Unavailable;
    s.note=m.value(QStringLiteral("note")).toString();s.levels=levelsFrom(m.value(QStringLiteral("levels")).toMap());
    s.valid=m.value(QStringLiteral("valid")).toBool();s.sample_count=m.value(QStringLiteral("sampleCount"),-1).toDouble();
    s.age_us=m.value(QStringLiteral("ageUs"),-1).toDouble();s.mode=m.value(QStringLiteral("mode"),-1).toInt();
    s.available=m.value(QStringLiteral("available")).toInt();
    if(!absent(m.value(QStringLiteral("euler")))) {
        const auto e=plain(m.value(QStringLiteral("euler"))).toMap();
        s.euler=Euler{e.value(QStringLiteral("roll")).toDouble(),e.value(QStringLiteral("pitch")).toDouble(),e.value(QStringLiteral("yaw")).toDouble()};
    }
    if(!absent(m.value(QStringLiteral("quat")))) {
        const auto q=plain(m.value(QStringLiteral("quat"))).toMap();
        s.quat=Quaternion{q.value(QStringLiteral("w")).toDouble(),q.value(QStringLiteral("x")).toDouble(),
                          q.value(QStringLiteral("y")).toDouble(),q.value(QStringLiteral("z")).toDouble()};
    }
    return s;
}

QVariantMap toVariant(const Sequence &s) {
    QVariantList steps;
    for(const auto &t:s.steps) {steps<<QVariantMap{{"title",t.title},{"state",stateName(t.state)},{"text",t.text}};}
    return {{"nodeKey",s.node_key},{"writeKey",s.write_key},{"applyKey",s.apply_key},{"blobHex",s.blob_hex},
            {"label",s.label},{"phase",phaseName(s.phase)},{"note",s.note},{"steps",steps},
            {"confirmCount",s.confirm_count},{"confirmSeen",s.confirm_seen},{"confirmSince",s.confirm_since}};
}
Sequence sequenceFrom(const QVariantMap &m) {
    Sequence s;
    s.node_key=m.value(QStringLiteral("nodeKey")).toString();s.write_key=m.value(QStringLiteral("writeKey")).toString();
    s.apply_key=m.value(QStringLiteral("applyKey")).toString();s.blob_hex=m.value(QStringLiteral("blobHex")).toString();
    s.label=m.value(QStringLiteral("label")).toString();s.phase=phaseFrom(m.value(QStringLiteral("phase")).toString());
    s.note=m.value(QStringLiteral("note")).toString();
    const auto steps=m.value(QStringLiteral("steps")).toList();
    for(size_t i=0;i<2 && i<size_t(steps.size());++i) {
        const auto t=steps[qsizetype(i)].toMap();
        s.steps[i]={t.value(QStringLiteral("title")).toString(),stepStateFrom(t.value(QStringLiteral("state")).toString()),
                    t.value(QStringLiteral("text")).toString()};
    }
    s.confirm_count=m.value(QStringLiteral("confirmCount"),-1).toDouble();
    s.confirm_seen=m.value(QStringLiteral("confirmSeen")).toInt();
    s.confirm_since=m.value(QStringLiteral("confirmSince")).toLongLong();
    return s;
}

QVariantMap toVariant(const Outcome &o) {return {{"state",stateName(o.state)},{"text",o.text},{"blobHex",o.blob_hex}};}
Outcome outcomeFrom(const QVariantMap &m) {
    return {stepStateFrom(m.value(QStringLiteral("state")).toString()),m.value(QStringLiteral("text")).toString(),
            m.value(QStringLiteral("blobHex")).toString()};
}

QStringList ImuCalibration::levels() const {
    QStringList out;
    for(const auto *n:kLevelNames) {out<<QLatin1String(n);}
    return out;
}
QVariantMap ImuCalibration::levelTitles() const {
    QVariantMap out;
    for(size_t i=0;i<4;++i) {out[QLatin1String(kLevelNames[i])]=QLatin1String(kLevelTitles[i]);}
    return out;
}

QString ImuCalibration::encodeBlob(const QVariantMap &cal) const {return imu_calibration::encodeBlob(calibrationFrom(cal));}
QVariantMap ImuCalibration::decodeBlob(const QString &hex) const {
    const auto c=imu_calibration::decodeBlob(hex);
    if(!c) {return {{"ok",false},{"error",c.error()}};}
    return {{"ok",true},{"cal",toVariant(*c)}};
}
QString ImuCalibration::buildFile(const QVariantMap &cal,const QVariantMap &read,const QString &note) const {
    return imu_calibration::buildFile(calibrationFrom(cal),provenanceFrom(read),note);
}
QString ImuCalibration::suggestedName(const QVariantMap &read) const {return imu_calibration::suggestedName(provenanceFrom(read));}
QString ImuCalibration::nowUtcText() const {return utcText(QDateTime::currentDateTimeUtc());}
QVariantMap ImuCalibration::parseFile(const QString &text,const QVariantMap &target) const {
    const auto parsed=imu_calibration::parseFile(text,{target.value(QStringLiteral("type")).toString(),
        target.value(QStringLiteral("deviceId")).toString(),target.value(QStringLiteral("instance")).toString()});
    if(!parsed) {return {{"ok",false},{"error",parsed.error()}};}
    return {{"ok",true},{"cal",toVariant(parsed->calibration)},{"source",toVariant(parsed->source)},{"warnings",parsed->warnings}};
}

QVariantMap ImuCalibration::sensorState(const QVariant &live) const {return toVariant(imu_calibration::sensorState(plain(live).toMap()));}
bool ImuCalibration::fullyCalibrated(const QVariant &sensor) const {return imu_calibration::fullyCalibrated(sensorFrom(plain(sensor).toMap()));}
QString ImuCalibration::readBlock(const QVariant &sensor,const QString &blocked) const {
    return imu_calibration::readBlock(sensorFrom(plain(sensor).toMap()),blocked);
}
QString ImuCalibration::levelText(int level) const {return imu_calibration::levelText(level);}
QVariantList ImuCalibration::guidance(const QVariant &sensor) const {
    QVariantList rows;
    for(const auto &r:imu_calibration::guidance(sensorFrom(plain(sensor).toMap()))) {
        rows<<QVariantMap{{"name",r.name},{"title",r.title},{"level",r.level},{"done",r.done},{"text",r.text}};
    }
    return rows;
}
QString ImuCalibration::degText(const QVariant &value) const {
    return imu_calibration::degText(absent(value)?std::nullopt:std::optional<double>(plain(value).toDouble()));
}
QString ImuCalibration::quatText(const QVariant &value) const {
    return imu_calibration::quatText(absent(value)?std::nullopt:std::optional<double>(plain(value).toDouble()));
}
QString ImuCalibration::ageText(double us) const {return imu_calibration::ageText(us);}
QVariantList ImuCalibration::axes(const QVariant &quat) const {
    std::optional<Quaternion> q;
    if(!absent(quat)) {
        const auto m=plain(quat).toMap();
        q=Quaternion{m.value(QStringLiteral("w")).toDouble(),m.value(QStringLiteral("x")).toDouble(),
                     m.value(QStringLiteral("y")).toDouble(),m.value(QStringLiteral("z")).toDouble()};
    }
    QVariantList out;
    for(const auto &a:imu_calibration::axes(q)) {out<<QVariantMap{{"axis",a.axis},{"x",a.x},{"y",a.y},{"depth",a.depth}};}
    return out;
}

QVariantMap ImuCalibration::stepOutcome(const QString &key,const QVariantMap &results,const QString &running,bool linked) const {
    return toVariant(imu_calibration::stepOutcome(key,results,running,linked));
}
QVariantMap ImuCalibration::sentOutcome(const QString &key,const QVariantMap &results,const QString &running) const {
    return toVariant(imu_calibration::sentOutcome(key,results,running));
}
QVariantMap ImuCalibration::newApply(const QString &nodeKey,const QString &writeKey,const QString &applyKey,
                                     const QString &blobHex,const QString &label) const {
    return toVariant(imu_calibration::newApply(nodeKey,writeKey,applyKey,blobHex,label));
}
bool ImuCalibration::busy(const QVariant &sequence) const {
    return !absent(sequence) && imu_calibration::busy(sequenceFrom(plain(sequence).toMap()));
}
QVariantMap ImuCalibration::markSent(const QVariantMap &sequence,const QVariantMap &outcome) const {
    return toVariant(imu_calibration::markSent(sequenceFrom(sequence),outcomeFrom(outcome)));
}
QVariantMap ImuCalibration::advance(const QVariantMap &sequence,const QVariantMap &view) const {
    View v;
    v.results=view.value(QStringLiteral("results")).toMap();v.running=view.value(QStringLiteral("running")).toString();
    v.linked=view.value(QStringLiteral("linked"),true).toBool();v.node_key=view.value(QStringLiteral("nodeKey")).toString();
    v.sensor=sensorFrom(view.value(QStringLiteral("sensor")).toMap());v.now=view.value(QStringLiteral("now")).toLongLong();
    const auto a=imu_calibration::advance(sequenceFrom(sequence),v);
    return {{"seq",toVariant(a.seq)},{"send",a.send_apply?QStringLiteral("apply"):QString()}};
}

void ImuCalibration::remember(const QString &key,const QString &field,const QVariant &value) {
    auto &entry=m_store[key];
    if(absent(value)) {entry.remove(field);} else {entry[field]=plain(value);}
}

} // namespace drivelink::gui::imu_calibration
