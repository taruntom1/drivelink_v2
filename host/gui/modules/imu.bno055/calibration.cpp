#include "calibration.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace drivelink::gui::imu_calibration {
namespace {
bool i16(int v) {return v>=-32768 && v<=32767;}
// A JSON number that is an integer inside int's range.
std::optional<int> integer(const QJsonValue &v) {
    if(!v.isDouble()) {return std::nullopt;}
    const double d=v.toDouble();
    if(std::floor(d)!=d || d<-2147483648.0 || d>2147483647.0) {return std::nullopt;}
    return int(d);
}
QString rangeText(int index) {
    return QString::fromLatin1(kValueNames[size_t(index)])+
           (isRadius(index)?QStringLiteral(" must be an integer 0 to 32767"):QStringLiteral(" must be an integer -32768 to 32767"));
}
// The groups of the file and their value indices.
struct Group { const char *name; std::vector<std::pair<const char *,int>> fields; };
const std::array<Group,3> &groups() {
    static const std::array<Group,3> g{{
        {"accel",{{"x",0},{"y",1},{"z",2},{"radius",3}}},
        {"gyro",{{"x",4},{"y",5},{"z",6}}},
        {"mag",{{"x",7},{"y",8},{"z",9},{"radius",10}}}}};
    return g;
}
QString str(const QJsonObject &o,const char *key) {return o.value(QLatin1String(key)).toString();}
}

bool isRadius(int index) {return index==3 || index==10;}

QString problem(const Calibration &c) {
    for(int i=0;i<kValues;++i) {
        const int v=c.values[size_t(i)];
        if(isRadius(i)?(v<0 || v>32767):!i16(v)) {return rangeText(i);}
    }
    return {};
}

QString encodeBlob(const Calibration &c) {
    QStringList bytes{QStringLiteral("%1").arg(kBlobVersion,2,16,QLatin1Char('0')),c.has_sic?QStringLiteral("01"):QStringLiteral("00")};
    for(const int v:c.values) {
        const auto u=uint16_t(int16_t(v));
        bytes<<QStringLiteral("%1").arg(u&0xFF,2,16,QLatin1Char('0'))<<QStringLiteral("%1").arg(u>>8,2,16,QLatin1Char('0'));
    }
    return bytes.join(QLatin1Char(' '));
}

std::expected<Calibration,QString> decodeBlob(const QString &hex) {
    QString text=hex;text.remove(QRegularExpression(QStringLiteral("\\s+")));
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9a-fA-F]*$"));
    if(!hexOnly.match(text).hasMatch() || text.size()%2) {return std::unexpected(QStringLiteral("The blob is not hex"));}
    if(text.size()!=2*kBlobBytes) {
        return std::unexpected(QStringLiteral("The blob has %1 bytes, not %2").arg(text.size()/2).arg(kBlobBytes));
    }
    std::array<int,kBlobBytes> b{};
    for(int i=0;i<kBlobBytes;++i) {b[size_t(i)]=text.mid(2*i,2).toInt(nullptr,16);}
    if(b[0]!=kBlobVersion) {return std::unexpected(QStringLiteral("Blob version %1 is not supported").arg(b[0]));}
    if(b[1]>1) {return std::unexpected(QStringLiteral("The blob's has_sic flag is %1").arg(b[1]));}
    Calibration c;c.has_sic=b[1]==1;
    for(int k=0;k<kValues;++k) {c.values[size_t(k)]=int16_t(uint16_t(b[size_t(2+2*k)]|(b[size_t(3+2*k)]<<8)));}
    if(const auto why=problem(c);!why.isEmpty()) {return std::unexpected(why);}
    return c;
}

// ---- The PC file ------------------------------------------------------------------

QString utcText(const QDateTime &t) {return t.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));}

QString buildFile(const Calibration &c,const Provenance &read,const QString &note) {
    QJsonObject file{
        {"format",QLatin1String(kFileFormat)},{"version",kFileVersion},{"type",read.type},{"created_utc",read.created_utc},
        {"source",QJsonObject{{"device_id",read.device_id},{"device_name",read.device_name},{"instance",read.instance},
                              {"firmware",read.firmware},{"gui",read.gui}}},
        {"actual_mode",read.actual_mode},{"has_sic",c.has_sic},{"note",note}};
    QJsonObject levels;
    for(size_t i=0;i<4;++i) {levels[QLatin1String(kLevelNames[i])]=read.levels[i];}
    file["levels"]=levels;
    for(const auto &g:groups()) {
        QJsonObject o;
        for(const auto &[field,index]:g.fields) {o[QLatin1String(field)]=c.values[size_t(index)];}
        file[QLatin1String(g.name)]=o;
    }
    QJsonArray sic;
    for(int i=11;i<kValues;++i) {sic.append(c.values[size_t(i)]);}
    file["sic"]=sic;
    return QString::fromUtf8(QJsonDocument(file).toJson(QJsonDocument::Indented));
}

QString suggestedName(const Provenance &read) {
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_.-]+"));
    const auto safe=[](QString s) {return s.replace(unsafe,QStringLiteral("_"));};
    QString time=read.created_utc;
    time.remove(QLatin1Char('-')).remove(QLatin1Char(':')).replace(QLatin1Char('T'),QLatin1Char('-')).remove(QLatin1Char('Z'));
    QStringList parts;
    for(const auto &p:{safe(read.device_name.isEmpty()?read.device_id:read.device_name),safe(read.instance),time}) {
        if(!p.isEmpty()) {parts<<p;}
    }
    return (parts.isEmpty()?QStringLiteral("imu-calibration"):parts.join(QLatin1Char('-')))+QLatin1String(kFileSuffix);
}

std::expected<ParsedFile,QString> parseFile(const QString &text,const Target &target) {
    QJsonParseError error;
    const auto doc=QJsonDocument::fromJson(text.toUtf8(),&error);
    if(error.error!=QJsonParseError::NoError) {return std::unexpected(QStringLiteral("The file is not JSON"));}
    const auto data=doc.object();
    if(!doc.isObject() || data.value(QLatin1String("format")).toString()!=QLatin1String(kFileFormat)) {
        return std::unexpected(QStringLiteral("Not a DriveLink IMU calibration file"));
    }
    if(integer(data.value(QLatin1String("version")))!=kFileVersion) {
        const auto v=data.value(QLatin1String("version"));
        const auto shown=v.isUndefined()?QStringLiteral("(none)"):v.toVariant().toString();
        return std::unexpected(QStringLiteral("File version %1 is not supported (this GUI reads version %2)").arg(shown).arg(kFileVersion));
    }
    const auto type=data.value(QLatin1String("type")).toString();
    if(type.isEmpty()) {return std::unexpected(QStringLiteral("The file names no module type"));}
    if(!target.type.isEmpty() && type!=target.type) {
        return std::unexpected(QStringLiteral("The file is for %1; this instance is %2").arg(type,target.type));
    }
    const auto invalid=[](const QString &why) {return std::unexpected(QStringLiteral("Incomplete or invalid calibration: ")+why);};
    ParsedFile out;
    const auto hasSic=data.value(QLatin1String("has_sic"));
    if(!hasSic.isBool()) {return invalid(QStringLiteral("has_sic must be true or false"));}
    out.calibration.has_sic=hasSic.toBool();
    for(const auto &g:groups()) {
        const auto o=data.value(QLatin1String(g.name));
        if(!o.isObject()) {return invalid(QStringLiteral("%1 is missing").arg(QLatin1String(g.name)));}
        for(const auto &[field,index]:g.fields) {
            const auto v=o.toObject().value(QLatin1String(field));
            if(v.isUndefined()) {return invalid(QStringLiteral("%1.%2 is missing").arg(QLatin1String(g.name),QLatin1String(field)));}
            const auto n=integer(v);
            if(!n || (isRadius(index)?(*n<0 || *n>32767):!i16(*n))) {return invalid(rangeText(index));}
            out.calibration.values[size_t(index)]=*n;
        }
    }
    const auto sic=data.value(QLatin1String("sic"));
    if(!sic.isArray() || sic.toArray().size()!=9) {return invalid(QStringLiteral("sic must list nine values"));}
    for(int k=0;k<9;++k) {
        const auto n=integer(sic.toArray().at(k));
        if(!n || !i16(*n)) {return invalid(rangeText(11+k));}
        out.calibration.values[size_t(11+k)]=*n;
    }
    const auto src=data.value(QLatin1String("source")).toObject();
    const auto levels=data.value(QLatin1String("levels")).toObject();
    auto &s=out.source;
    s.type=type;s.created_utc=data.value(QLatin1String("created_utc")).toString();
    s.device_id=str(src,"device_id");s.device_name=str(src,"device_name");s.instance=str(src,"instance");
    s.firmware=str(src,"firmware");s.gui=str(src,"gui");
    s.actual_mode=integer(data.value(QLatin1String("actual_mode"))).value_or(-1);
    s.note=data.value(QLatin1String("note")).toString();
    QStringList low;
    for(size_t i=0;i<4;++i) {
        const auto l=integer(levels.value(QLatin1String(kLevelNames[i])));
        s.levels[i]=l.value_or(-1);
        if(!l || *l<3) {low<<QLatin1String(kLevelTitles[i]);}
    }
    if(s.device_id.isEmpty()) {out.warnings<<QStringLiteral("The file does not say which device it was read from");}
    else if(!target.device_id.isEmpty() && s.device_id!=target.device_id) {
        out.warnings<<QStringLiteral("Read from another device (%1%2). Offsets belong to one physical sensor and its mounting.")
                      .arg(s.device_name.isEmpty()?QString():s.device_name+QStringLiteral(" · "),s.device_id);
    }
    if(!s.instance.isEmpty() && !target.instance.isEmpty() && s.instance!=target.instance) {
        out.warnings<<QStringLiteral("Read from instance %1, not %2").arg(s.instance,target.instance);
    }
    if(!low.isEmpty()) {out.warnings<<QStringLiteral("Not fully calibrated when read (%1 below 3 / 3)").arg(low.join(QStringLiteral(", ")));}
    return out;
}

// ---- Live sensor state ------------------------------------------------------------

SensorState sensorState(const QVariantMap &live) {
    SensorState s;
    s.note=live.value(QStringLiteral("note")).toString();
    QHash<QString,double> by;
    for(const auto &row:live.value(QStringLiteral("signals")).toList()) {
        const auto m=row.toMap();const auto n=m.value(QStringLiteral("number"));
        if(n.isValid() && !n.isNull()) {by.insert(m.value(QStringLiteral("name")).toString(),n.toDouble());}
    }
    if(!by.contains(QStringLiteral("sample_count"))) {
        s.state=s.note.startsWith(QLatin1String("Waiting"))?SensorKind::Waiting:SensorKind::Unavailable;
        return s;
    }
    s.sample_count=by.value(QStringLiteral("sample_count"));
    s.age_us=by.value(QStringLiteral("sample_age_us"),-1);
    s.mode=int(by.value(QStringLiteral("actual_mode"),-1));
    s.available=int(by.value(QStringLiteral("available"),0));
    s.valid=by.value(QStringLiteral("valid"),0)==1;
    if(s.age_us==kNoSample) {s.state=SensorKind::Waiting;return s;}
    const auto deg=[](double r) {return r*180/std::numbers::pi;};
    if(by.contains(QStringLiteral("roll")) && by.contains(QStringLiteral("pitch")) && by.contains(QStringLiteral("yaw"))) {
        s.euler=Euler{deg(by.value(QStringLiteral("roll"))),deg(by.value(QStringLiteral("pitch"))),deg(by.value(QStringLiteral("yaw")))};
    }
    const QStringList q{QStringLiteral("quaternion.w"),QStringLiteral("quaternion.x"),QStringLiteral("quaternion.y"),QStringLiteral("quaternion.z")};
    if(std::all_of(q.begin(),q.end(),[&](const QString &n){return by.contains(n);})) {
        s.quat=Quaternion{by.value(q[0]),by.value(q[1]),by.value(q[2]),by.value(q[3])};
    }
    s.state=live.value(QStringLiteral("telemetry")).toBool() && s.valid?SensorKind::Live:SensorKind::Stale;
    if(s.state==SensorKind::Live && (s.available&0x100)) {
        for(size_t i=0;i<4;++i) {
            const auto name=QStringLiteral("calibration_status.")+QLatin1String(kLevelNames[i]);
            s.levels[i]=by.contains(name)?int(by.value(name)):-1;
        }
    }
    return s;
}

bool fullyCalibrated(const SensorState &s) {return std::all_of(s.levels.begin(),s.levels.end(),[](int l){return l==3;});}

QString readBlock(const SensorState &s,const QString &blocked) {
    if(!blocked.isEmpty()) {return blocked;}
    if(s.state!=SensorKind::Live) {return QStringLiteral("Read needs live IMU data");}
    if(!fullyCalibrated(s)) {return QStringLiteral("Read requires all four calibration levels at 3 / 3");}
    return {};
}

QString levelText(int level) {
    if(level<0) {return QStringLiteral("Unavailable");}
    return level>=3?QStringLiteral("Calibrated"):QStringLiteral("Calibrating");
}

std::vector<GuidanceRow> guidance(const SensorState &s) {
    struct Text { size_t level; const char *text; };
    static const std::array<Text,4> order{{
        {1,"Keep the sensor still on a stable surface for a few seconds."},
        {2,"Hold the sensor steady in about six different orientations, a few seconds each, including along each axis."},
        {3,"Move the sensor through varied orientations (a figure of eight), away from magnetic interference."},
        {0,"Rises to 3 once the sensors are calibrated and fusion has settled."}}};
    std::vector<GuidanceRow> rows;
    for(const auto &t:order) {
        const int l=s.levels[t.level];
        rows.push_back({QLatin1String(kLevelNames[t.level]),QLatin1String(kLevelTitles[t.level]),QString::fromUtf8(t.text),l,l==3});
    }
    return rows;
}

QString degText(std::optional<double> v) {
    return v && std::isfinite(*v)?QString::number(*v,'f',1)+QStringLiteral("°"):QStringLiteral("—");
}
QString quatText(std::optional<double> v) {return v && std::isfinite(*v)?QString::number(*v,'f',4):QStringLiteral("—");}
QString ageText(double us) {
    if(us<0 || us==kNoSample) {return QStringLiteral("—");}
    if(us<1000) {return QString::number(us,'f',0)+QStringLiteral(" µs");}
    if(us<1e6) {return QString::number(us/1000,'f',0)+QStringLiteral(" ms");}
    return QString::number(us/1e6,'f',1)+QStringLiteral(" s");
}

// ---- Orientation preview ----------------------------------------------------------

std::optional<std::array<double,3>> rotate(const Quaternion &q,const std::array<double,3> &v) {
    const double n=std::sqrt(q.w*q.w+q.x*q.x+q.y*q.y+q.z*q.z);
    if(!(n>1e-9)) {return std::nullopt;}
    const double w=q.w/n,x=q.x/n,y=q.y/n,z=q.z/n;
    // v' = v + 2w(u × v) + 2u × (u × v), u = (x, y, z)
    const double cx=y*v[2]-z*v[1],cy=z*v[0]-x*v[2],cz=x*v[1]-y*v[0];
    const double dx=y*cz-z*cy,dy=z*cx-x*cz,dz=x*cy-y*cx;
    return std::array<double,3>{v[0]+2*(w*cx+dx),v[1]+2*(w*cy+dy),v[2]+2*(w*cz+dz)};
}

std::vector<Axis> axes(const std::optional<Quaternion> &q) {
    if(!q) {return {};}
    std::vector<Axis> out;
    const double c=std::cos(std::numbers::pi/6),s=std::sin(std::numbers::pi/6);
    const std::array<std::pair<const char *,std::array<double,3>>,3> units{{{"X",{1,0,0}},{"Y",{0,1,0}},{"Z",{0,0,1}}}};
    for(const auto &[name,unit]:units) {
        const auto p=rotate(*q,unit);
        if(!p) {return {};}
        const auto &r=*p;
        out.push_back({QLatin1String(name),(r[0]-r[1])*c*0.85,((r[0]+r[1])*s-r[2])*0.85,r[0]+r[1]+r[2]});
    }
    std::stable_sort(out.begin(),out.end(),[](const Axis &a,const Axis &b){return a.depth<b.depth;});
    return out;
}

// ---- The sequence -----------------------------------------------------------------

Outcome stepOutcome(const QString &key,const QVariantMap &results,const QString &running,bool linked) {
    if(running==key) {return {StepState::Running,QStringLiteral("Running…"),{}};}
    if(!results.contains(key)) {
        return {StepState::Uncertain,linked?QStringLiteral("No outcome was recorded; it may have run. It was not retried.")
                                            :QStringLiteral("The link closed before the reply; it may have run. It was not retried."),{}};
    }
    const auto r=results.value(key).toMap();
    const auto state=r.value(QStringLiteral("state")).toString();
    const auto text=r.value(QStringLiteral("text")).toString();
    if(state==QLatin1String("ok")) {
        if(r.value(QStringLiteral("rebooted")).toBool()) {
            return {StepState::Rebooted,text.isEmpty()?QStringLiteral("Ran; the device has since rebooted"):text,{}};
        }
        return {StepState::Ok,text.isEmpty()?QStringLiteral("Done"):text,r.value(QStringLiteral("blobHex")).toString()};
    }
    if(state==QLatin1String("uncertain")) {return {StepState::Uncertain,text.isEmpty()?QStringLiteral("No reply in time; it may have run"):text,{}};}
    if(state==QLatin1String("running")) {return {StepState::Running,QStringLiteral("Running…"),{}};}
    return {StepState::Refused,text.isEmpty()?QStringLiteral("Refused"):text,{}};
}

Outcome sentOutcome(const QString &key,const QVariantMap &results,const QString &running) {
    if(running==key) {return {StepState::Running,QStringLiteral("Running…"),{}};}
    const auto r=results.value(key).toMap();
    const auto state=r.value(QStringLiteral("state")).toString();
    if(state==QLatin1String("refused") || state==QLatin1String("invalid")) {
        const auto text=r.value(QStringLiteral("text")).toString();
        return {StepState::Refused,text.isEmpty()?QStringLiteral("Refused"):text,{}};
    }
    return {StepState::Refused,QStringLiteral("Not sent"),{}};
}

Sequence newApply(const QString &nodeKey,const QString &writeKey,const QString &applyKey,const QString &blobHex,const QString &label) {
    Sequence s;
    s.node_key=nodeKey;s.write_key=writeKey;s.apply_key=applyKey;s.blob_hex=blobHex;s.label=label;
    s.steps={Step{QStringLiteral("Write to the device (staged in RAM)"),StepState::Pending,{}},
             Step{QStringLiteral("Apply to the sensor"),StepState::Pending,QStringLiteral("Not sent")}};
    return s;
}

bool busy(const Sequence &s) {return s.phase==Phase::Write || s.phase==Phase::Apply || s.phase==Phase::Confirming;}

Sequence markSent(Sequence s,const Outcome &o) {
    auto &step=s.steps[s.phase==Phase::Write?0:1];
    step.state=o.state;step.text=o.text;
    if(o.state!=StepState::Running) {s.phase=Phase::Failed;s.note=step.title+QStringLiteral(": ")+o.text;}
    return s;
}

Advance advance(Sequence s,const View &view) {
    if(busy(s) && view.node_key!=s.node_key) {
        // A running step keeps its own outcome; nothing further is sent.
        s.phase=Phase::Abandoned;s.note=QStringLiteral("Another instance was selected; nothing further was sent.");
        return {s,false};
    }
    const auto observe=[&](size_t i,const QString &key) {
        const auto o=stepOutcome(key,view.results,view.running,view.linked);
        s.steps[i].state=o.state;s.steps[i].text=o.text;
        return o;
    };
    if(s.phase==Phase::Write && s.steps[0].state==StepState::Running) {
        const auto w=observe(0,s.write_key);
        switch(w.state) {
        case StepState::Ok:
            if(!view.linked) {
                s.phase=Phase::Uncertain;s.note=QStringLiteral("The link closed before Apply was sent; the staged calibration was not applied.");
                return {s,false};
            }
            s.phase=Phase::Apply;s.steps[1].state=StepState::Pending;s.steps[1].text.clear();
            return {s,true};
        case StepState::Refused:
            s.phase=Phase::Failed;s.note=QStringLiteral("Write refused: %1. Nothing was applied.").arg(w.text);break;
        case StepState::Uncertain:
            s.phase=Phase::Uncertain;s.note=QStringLiteral("The write may have run; Apply was not sent.");break;
        case StepState::Rebooted:
            s.phase=Phase::Ended;s.note=QStringLiteral("The device rebooted; Apply was not sent and the RAM stage is gone.");break;
        default:break;
        }
        return {s,false};
    }
    if(s.phase==Phase::Apply && s.steps[1].state==StepState::Running) {
        const auto a=observe(1,s.apply_key);
        switch(a.state) {
        case StepState::Ok:
            s.phase=Phase::Confirming;s.confirm_count=view.sensor.sample_count;s.confirm_seen=0;s.confirm_since=view.now;
            s.note=QStringLiteral("Applied; waiting for a new valid sample.");break;
        case StepState::Refused:
            s.phase=Phase::Failed;s.note=QStringLiteral("Write succeeded (staged, Unsaved); Apply failed: ")+a.text;break;
        case StepState::Uncertain:
            s.phase=Phase::Uncertain;s.note=QStringLiteral("Write succeeded; Apply may have run. Check the levels before retrying.");break;
        case StepState::Rebooted:
            s.phase=Phase::Ended;s.note=QStringLiteral("The device rebooted after Apply; the saved calibration is what runs now.");break;
        default:break;
        }
        return {s,false};
    }
    if(s.phase==Phase::Confirming || s.phase==Phase::NoSample) {
        if(!view.linked) {
            s.phase=Phase::NoSample;s.note=QStringLiteral("Applied; the link closed before a new sample arrived.");
            return {s,false};
        }
        if(view.sensor.state==SensorKind::Live && view.sensor.sample_count!=s.confirm_count) {
            s.confirm_count=view.sensor.sample_count;++s.confirm_seen;
        }
        if(s.confirm_seen>=2) {
            s.phase=Phase::Applied;s.note=QStringLiteral("Applied, and the sensor is sampling again. Save keeps it after a reboot.");
        } else if(s.phase==Phase::Confirming && view.now-s.confirm_since>kConfirmMs) {
            s.phase=Phase::NoSample;s.note=QStringLiteral("Applied, but no new valid sample has arrived yet.");
        }
    }
    return {s,false};
}

namespace {
constexpr std::array<const char *,6> kStepNames{"pending","running","ok","rebooted","refused","uncertain"};
constexpr std::array<const char *,9> kPhaseNames{"write","apply","confirming","applied","noSample","failed","uncertain","ended","abandoned"};
template<typename E,size_t N> E fromName(const std::array<const char *,N> &names,const QString &name,E fallback) {
    for(size_t i=0;i<N;++i) {if(name==QLatin1String(names[i])) {return E(i);}}
    return fallback;
}
}
QString stateName(StepState s) {return QLatin1String(kStepNames[size_t(s)]);}
StepState stepStateFrom(const QString &n) {return fromName(kStepNames,n,StepState::Pending);}
QString phaseName(Phase p) {return QLatin1String(kPhaseNames[size_t(p)]);}
Phase phaseFrom(const QString &n) {return fromName(kPhaseNames,n,Phase::Failed);}

} // namespace drivelink::gui::imu_calibration
