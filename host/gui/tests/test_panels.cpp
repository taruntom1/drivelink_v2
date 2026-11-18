// Custom panels (H2f): the generic backend surface a panel uses (numeric live
// values, action outcomes, small text files) and the imu.bno055 calibration
// panel in the shell on vdev's imu.mock (the same F6 contract): import,
// review, write → apply, a new valid sample, read, export. benchCalibration
// runs the same path on a device (a real BNO055: the deferred HW check).
#include "connection.hpp"
#include "imu.bno055/calibration.hpp"
#include "imu.bno055/calibration_qml.hpp"
#include "layout_model.hpp"
#include "safety_keys.hpp"
#include "style.hpp"
#include "test_support.hpp"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <functional>
#include <map>
#include <numbers>

using namespace drivelink;
using namespace drivelink::gui;
namespace {
using namespace drivelink::gui::test;

constexpr uint16_t kImuMock=114;

// A version-1 calibration file (buildFile's shape).
QJsonObject calibrationFile(const QString &type,const QString &deviceId,const QString &instance,int sicScale=16384) {
    return QJsonObject{
        {"format","drivelink.imu-calibration"},{"version",1},{"type",type},{"created_utc","2026-11-18T09:05:03Z"},
        {"source",QJsonObject{{"device_id",deviceId},{"device_name","bench"},{"instance",instance},{"firmware",""},{"gui","test"}}},
        {"levels",QJsonObject{{"sys",3},{"gyro",3},{"accel",3},{"mag",3}}},{"actual_mode",12},{"has_sic",true},
        {"accel",QJsonObject{{"x",12},{"y",-7},{"z",25},{"radius",1000}}},
        {"gyro",QJsonObject{{"x",-2},{"y",1},{"z",0}}},
        {"mag",QJsonObject{{"x",140},{"y",-60},{"z",310},{"radius",680}}},
        {"sic",QJsonArray{sicScale,0,0,0,sicScale,0,0,0,sicScale}},{"note",""}};
}
QString writeJson(const QTemporaryDir &dir,const QString &name,const QJsonObject &o) {
    const auto path=dir.filePath(name);QFile f(path);
    if(!f.open(QIODevice::WriteOnly)) {throw std::runtime_error("cannot write a test file");}
    f.write(QJsonDocument(o).toJson());return path;
}
QVariantMap prop(QObject *o,const char *name) {return o->property(name).toMap();}
QString state(QObject *panel) {return prop(panel,"operation").value("state").toString();}

// The shell with a connection, on Layout, the panel tab of `node` open.
struct PanelShell {
    LocalConnection connection;
    SafetyKeys keys;
    QQmlApplicationEngine engine;
    QQuickWindow *window=nullptr;
    QObject *panel=nullptr;
    bool open(const QString &endpoint,const QString &node,const QString &settings,int timeout=15000) {
        registerShellTypes();
        connection.setSettingsPath(settings);
        if(!installStyle(engine)) {return false;}
        engine.rootContext()->setContextProperty("connection",&connection);
        engine.rootContext()->setContextProperty("safetyKeys",&keys);
        engine.loadFromModule("DriveLink.Shell","Shell");
        if(engine.rootObjects().isEmpty()) {return false;}
        window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        window->resize(1920,1080);
        connection.connectTo(endpoint);
        if(!waitFor([&]{return connection.ready() && connection.layoutTree()->rowCount()>0;},timeout)) {return false;}
        window->setProperty("page",1);
        connection.selectNode(node);
        if(!waitFor([&]{return !connection.layout().value("panel").toString().isEmpty();})) {return false;}
        auto *page=window->findChild<QObject *>("layoutPage");
        if(!page) {return false;}
        page->setProperty("tab",1);
        return waitFor([&]{panel=window->findChild<QObject *>("calibrationPanel");return panel!=nullptr;});
    }
    QVariant call(const char *method,const QVariant &arg=QVariant()) {
        QVariant result;
        if(arg.isValid()) {QMetaObject::invokeMethod(panel,method,Q_RETURN_ARG(QVariant,result),Q_ARG(QVariant,arg));}
        else {QMetaObject::invokeMethod(panel,method,Q_RETURN_ARG(QVariant,result));}
        return result;
    }
    QVariantMap sensor() {return prop(panel,"sensor");}
    QVariantMap levels() {return sensor().value("levels").toMap();}
    bool calibrated() {
        const auto l=levels();
        return l.value("sys").toInt()==3 && l.value("gyro").toInt()==3 && l.value("accel").toInt()==3 && l.value("mag").toInt()==3;
    }
};

// The calibration logic's fixtures: a blob in the documented layout.
constexpr const char *kSampleHex="01 01 01 00 fe ff 03 00 e8 03 ff ff 00 00 00 01 00 80 ff 7f 00 00 00 00 "
                                 "00 40 00 00 00 00 00 00 00 40 00 00 00 00 00 00 00 c0";
imu_calibration::Calibration sampleCalibration() {
    imu_calibration::Calibration c;c.has_sic=true;
    c.values={1,-2,3,1000,-1,0,256,-32768,32767,0,0,16384,0,0,0,16384,0,0,0,-16384};
    return c;
}
imu_calibration::Provenance sampleProvenance() {
    imu_calibration::Provenance p;
    p.type="imu.bno055";p.created_utc="2026-11-18T09:05:03Z";p.device_id="1cdbd446ff38";p.device_name="bench-rig";
    p.instance="imu";p.firmware="2.0.0";p.gui="drivelink-gui 0.1";p.levels={3,3,3,3};p.actual_mode=12;
    return p;
}
imu_calibration::Target sampleTarget() {return {"imu.bno055","1cdbd446ff38","imu"};}
// Connection.live with these signal numbers.
QVariantMap live(const QVariantMap &values,bool telemetry) {
    QVariantList rows;
    for(auto it=values.begin();it!=values.end();++it) {rows<<QVariantMap{{"name",it.key()},{"number",it.value()}};}
    return {{"telemetry",telemetry},{"note",QString()},{"signals",rows}};
}
QVariantMap fullSample(std::array<int,4> levels) {
    return {{"roll",std::numbers::pi/2},{"pitch",0.0},{"yaw",-std::numbers::pi/4},{"quaternion.w",1.0},{"quaternion.x",0.0},
            {"quaternion.y",0.0},{"quaternion.z",0.0},{"valid",1},{"sample_count",42},{"sample_age_us",8000},
            {"actual_mode",12},{"available",0x1FF},{"calibration_status.sys",levels[0]},{"calibration_status.gyro",levels[1]},
            {"calibration_status.accel",levels[2]},{"calibration_status.mag",levels[3]}};
}

// The device's staged calibration.* params of an instance (from the snapshot).
std::map<std::string,int64_t> stagedCalibration(const DeviceSnapshot &s,InstanceKey key) {
    std::map<std::string,int64_t> out;
    const auto &type=s.types.at(key.first);const auto &entry=s.configured.at(key);
    for(const auto &[id,value]:entry.params) {
        const auto &d=type.items.at(id).descriptor;
        if(d.name.rfind("calibration.",0)!=0) {continue;}
        out[d.name]=d.value_type==ValueType::I32?int64_t(int32_t(valueBits(value))):int64_t(valueBits(value));
    }
    return out;
}
}

class PanelTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(qmake_DriveLink_Shell);
        Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);
    }

    // The custom panels are compiled in under the device's type names.
    void panelsInResources() {
        QVERIFY(QFile::exists(":/qt/qml/DriveLink/Modules/imu.bno055/Panel.qml"));
        QVERIFY(QFile::exists(":/qt/qml/DriveLink/Modules/imu.mock/Panel.qml"));

    }

    // The calibration panel's pure logic (imu.bno055/calibration.hpp).
    void calibrationBlob() {
        using namespace imu_calibration;
        const auto cal=sampleCalibration();
        QCOMPARE(encodeBlob(cal),QString(kSampleHex));
        QCOMPARE(QString(kSampleHex).split(' ').size(),kBlobBytes);
        auto noSic=cal;noSic.has_sic=false;QCOMPARE(encodeBlob(noSic).left(5),QString("01 00"));
        const auto d=decodeBlob(kSampleHex);QVERIFY(d);QCOMPARE(*d,cal);
        QVERIFY(decodeBlob(QString(kSampleHex).remove(' ').toUpper()));
        QCOMPARE(decodeBlob("01 01").error(),QString("The blob has 2 bytes, not 42"));
        QCOMPARE(decodeBlob("zz").error(),QString("The blob is not hex"));
        QCOMPARE(decodeBlob("02"+QString(kSampleHex).mid(2)).error(),QString("Blob version 2 is not supported"));
        QCOMPARE(decodeBlob("01 02"+QString(kSampleHex).mid(5)).error(),QString("The blob's has_sic flag is 2"));
        auto bytes=QString(kSampleHex).split(' ');bytes[8]="ff";bytes[9]="ff"; // accel radius = -1
        QCOMPARE(decodeBlob(bytes.join(' ')).error(),QString("accel.radius must be an integer 0 to 32767"));
        auto bad=cal;bad.values[0]=40000;QCOMPARE(problem(bad),QString("accel.x must be an integer -32768 to 32767"));
        bad=cal;bad.values[10]=-1;QCOMPARE(problem(bad),QString("mag.radius must be an integer 0 to 32767"));
        bad=cal;bad.values[15]=-40000;QCOMPARE(problem(bad),QString("sic[4] must be an integer -32768 to 32767"));
        QCOMPARE(problem(cal),QString());
    }

    void calibrationFileFormat() {
        using namespace imu_calibration;
        const auto text=buildFile(sampleCalibration(),sampleProvenance(),"left bracket, cable down");
        const auto data=QJsonDocument::fromJson(text.toUtf8()).object();
        QCOMPARE(data["format"].toString(),QString("drivelink.imu-calibration"));QCOMPARE(data["version"].toInt(),1);
        QCOMPARE(data["type"].toString(),QString("imu.bno055"));
        QCOMPARE(data["source"].toObject()["device_id"].toString(),QString("1cdbd446ff38"));
        QCOMPARE(data["levels"].toObject()["mag"].toInt(),3);QCOMPARE(data["sic"].toArray().size(),9);
        QCOMPARE(data["mag"].toObject()["x"].toInt(),-32768);QVERIFY(!data.contains("blob")); // decoded fields only
        const auto parsed=parseFile(text,sampleTarget());
        QVERIFY2(parsed,qPrintable(parsed?QString():parsed.error()));
        QCOMPARE(parsed->calibration,sampleCalibration());QVERIFY(parsed->warnings.isEmpty());
        auto expected=sampleProvenance();expected.note="left bracket, cable down";QCOMPARE(parsed->source,expected);
        QCOMPARE(encodeBlob(parsed->calibration),QString(kSampleHex));
        // Refusals.
        const auto refuse=[&](const std::function<void(QJsonObject &)> &edit) {
            auto o=QJsonDocument::fromJson(buildFile(sampleCalibration(),sampleProvenance(),{}).toUtf8()).object();edit(o);
            const auto r=parseFile(QString::fromUtf8(QJsonDocument(o).toJson()),sampleTarget());
            return r?QString():r.error();
        };
        QCOMPARE(parseFile("{",sampleTarget()).error(),QString("The file is not JSON"));
        QCOMPARE(parseFile("[]",sampleTarget()).error(),QString("Not a DriveLink IMU calibration file"));
        QCOMPARE(refuse([](QJsonObject &o){o["version"]=2;}),QString("File version 2 is not supported (this GUI reads version 1)"));
        QCOMPARE(refuse([](QJsonObject &o){o.remove("type");}),QString("The file names no module type"));
        QCOMPARE(refuse([](QJsonObject &o){o["type"]="imu.mock";}),QString("The file is for imu.mock; this instance is imu.bno055"));
        QCOMPARE(refuse([](QJsonObject &o){o.remove("mag");}),QString("Incomplete or invalid calibration: mag is missing"));
        QCOMPARE(refuse([](QJsonObject &o){auto g=o["gyro"].toObject();g.remove("y");o["gyro"]=g;}),
                 QString("Incomplete or invalid calibration: gyro.y is missing"));
        QCOMPARE(refuse([](QJsonObject &o){auto g=o["gyro"].toObject();g["z"]=1.5;o["gyro"]=g;}),
                 QString("Incomplete or invalid calibration: gyro.z must be an integer -32768 to 32767"));
        QCOMPARE(refuse([](QJsonObject &o){auto a=o["sic"].toArray();a[4]="16384";o["sic"]=a;}),
                 QString("Incomplete or invalid calibration: sic[4] must be an integer -32768 to 32767"));
        QCOMPARE(refuse([](QJsonObject &o){auto a=o["sic"].toArray();a.removeLast();o["sic"]=a;}),
                 QString("Incomplete or invalid calibration: sic must list nine values"));
        QCOMPARE(refuse([](QJsonObject &o){o["has_sic"]=1;}),QString("Incomplete or invalid calibration: has_sic must be true or false"));
        QCOMPARE(refuse([](QJsonObject &o){auto m=o["mag"].toObject();m["radius"]=-1;o["mag"]=m;}),
                 QString("Incomplete or invalid calibration: mag.radius must be an integer 0 to 32767"));
        // Warnings; unknown keys are ignored.
        auto p=sampleProvenance();p.device_id="aabbccddeeff";p.instance="imu_left";p.levels[3]=2;
        auto w=parseFile(buildFile(sampleCalibration(),p,{}),sampleTarget());QVERIFY(w);
        QCOMPARE(w->warnings.size(),3);
        QVERIFY(w->warnings[0].startsWith(QString::fromUtf8("Read from another device (bench-rig · aabbccddeeff)")));
        QCOMPARE(w->warnings[1],QString("Read from instance imu_left, not imu"));
        QCOMPARE(w->warnings[2],QString("Not fully calibrated when read (Magnetometer below 3 / 3)"));
        p=sampleProvenance();p.device_id.clear();
        QCOMPARE(parseFile(buildFile(sampleCalibration(),p,{}),sampleTarget())->warnings,
                 QStringList{"The file does not say which device it was read from"});
        auto extra=QJsonDocument::fromJson(buildFile(sampleCalibration(),sampleProvenance(),{}).toUtf8()).object();
        auto accel=extra["accel"].toObject();accel["extra"]=5;extra["accel"]=accel;extra["future"]=true;
        QVERIFY(parseFile(QString::fromUtf8(QJsonDocument(extra).toJson()),sampleTarget()));
        // Names and times.
        QCOMPARE(suggestedName(sampleProvenance()),QString("bench-rig-imu-20261118-090503.imucal.json"));
        p=sampleProvenance();p.device_name.clear();p.instance="a/b";
        QCOMPARE(suggestedName(p),QString("1cdbd446ff38-a_b-20261118-090503.imucal.json"));
        QCOMPARE(utcText(QDateTime(QDate(2026,1,2),QTime(3,4,5),QTimeZone::UTC)),QString("2026-01-02T03:04:05Z"));
    }

    void calibrationSensor() {
        using namespace imu_calibration;
        QCOMPARE(sensorState({}).state,SensorKind::Unavailable);
        QCOMPARE(sensorState({{"note","Waiting for the first sample"},{"signals",QVariantList{}}}).state,SensorKind::Waiting);
        QCOMPARE(sensorState({{"note","Not running: failed"},{"signals",QVariantList{QVariantMap{{"name","valid"},{"number",QVariant()}}}}}).state,
                 SensorKind::Unavailable);
        auto s=sensorState(live(fullSample({3,3,2,1}),true));
        QCOMPARE(s.state,SensorKind::Live);QCOMPARE(s.levels,(Levels{3,3,2,1}));
        QVERIFY(std::abs(s.euler->roll-90)<1e-9);QVERIFY(std::abs(s.euler->yaw+45)<1e-9);
        QCOMPARE(s.quat->w,1.0);QCOMPARE(s.sample_count,42.0);QCOMPARE(s.mode,12);
        // No new samples: the pose stays, levels are not shown.
        s=sensorState(live(fullSample({3,3,3,3}),false));
        QCOMPARE(s.state,SensorKind::Stale);QCOMPARE(s.levels[0],-1);QVERIFY(s.euler);
        // The sensor reports invalid data (CONFIG, a failed read).
        auto v=fullSample({3,3,3,3});v["valid"]=0;QCOMPARE(sensorState(live(v,true)).state,SensorKind::Stale);
        // No sample yet.
        v=fullSample({0,0,0,0});v["sample_age_us"]=imu_calibration::kNoSample;
        s=sensorState(live(v,true));QCOMPARE(s.state,SensorKind::Waiting);QVERIFY(!s.euler);
        // A mode without calibration status (bit 8).
        v=fullSample({3,3,3,3});v["available"]=0x3F;QCOMPARE(sensorState(live(v,true)).levels[1],-1);

        const auto full=sensorState(live(fullSample({3,3,3,3}),true));
        QCOMPARE(readBlock(full,{}),QString());QCOMPARE(readBlock(full,"Not allowed"),QString("Not allowed"));
        QCOMPARE(readBlock(sensorState(live(fullSample({3,3,3,2}),true)),{}),QString("Read requires all four calibration levels at 3 / 3"));
        QCOMPARE(readBlock(sensorState(live(fullSample({3,3,3,3}),false)),{}),QString("Read needs live IMU data"));
        const auto rows=guidance(sensorState(live(fullSample({1,3,2,0}),true)));
        QCOMPARE(rows.size(),size_t(4));
        QCOMPARE(rows[0].name,QString("gyro"));QVERIFY(rows[0].done);QCOMPARE(rows[1].name,QString("accel"));QVERIFY(!rows[1].done);
        QCOMPARE(rows[2].name,QString("mag"));QCOMPARE(rows[3].name,QString("sys"));QCOMPARE(rows[3].level,1);
        QCOMPARE(levelText(-1),QString("Unavailable"));QCOMPARE(levelText(2),QString("Calibrating"));QCOMPARE(levelText(3),QString("Calibrated"));
        QCOMPARE(degText(2.44),QString::fromUtf8("2.4°"));QCOMPARE(degText(std::nullopt),QString::fromUtf8("—"));
        QCOMPARE(quatText(0.73123),QString("0.7312"));
        QCOMPARE(imu_calibration::ageText(8000),QString("8 ms"));QCOMPARE(imu_calibration::ageText(imu_calibration::kNoSample),QString::fromUtf8("—"));
        QCOMPARE(imu_calibration::ageText(2500000),QString("2.5 s"));
    }

    void calibrationAxes() {
        using namespace imu_calibration;
        QVERIFY(axes(std::nullopt).empty());
        QVERIFY(axes(Quaternion{0,0,0,0}).empty());
        const auto id=axes(Quaternion{1,0,0,0});
        QCOMPARE(id.size(),size_t(3));
        std::map<QString,Axis> by;for(const auto &a:id) {by[a.axis]=a;}
        // Isometric: X down-right, Y down-left, Z straight up; farthest first.
        QVERIFY(by["X"].x>0 && by["X"].y>0);QVERIFY(by["Y"].x<0 && by["Y"].y>0);
        QVERIFY(std::abs(by["Z"].x)<1e-9 && by["Z"].y<0);
        QVERIFY(id[0].depth<=id[1].depth && id[1].depth<=id[2].depth);
        // 90° about Z (not normalised): X takes Y's place.
        for(const auto &a:axes(Quaternion{2,0,0,2})) {
            if(a.axis=="X") {QVERIFY(std::abs(a.x-by["Y"].x)<1e-9 && std::abs(a.y-by["Y"].y)<1e-9);}
        }
        const auto v=rotate({std::numbers::sqrt2/2,std::numbers::sqrt2/2,0,0},{0,1,0});
        QVERIFY(v && std::abs((*v)[2]-1)<1e-9);
    }

    void calibrationSequence() {
        using namespace imu_calibration;
        const QString write="114:0:771",apply="114:0:772";
        const auto view=[&](QVariantMap results,const QString &running,double count=10,SensorKind state=SensorKind::Live,
                            qint64 now=0,bool linked=true,const QString &node="114:0") {
            View v;v.results=std::move(results);v.running=running;v.linked=linked;v.node_key=node;v.now=now;
            v.sensor.state=state;v.sensor.sample_count=count;return v;
        };
        const auto ok=QVariantMap{{"state","ok"}};
        const auto started=[&] {
            return markSent(newApply("114:0",write,apply,kSampleHex,"a.imucal.json"),
                            sentOutcome(write,{{write,QVariantMap{{"state","running"}}}},write));
        };
        // The happy path.
        auto seq=started();
        QCOMPARE(seq.phase,Phase::Write);QCOMPARE(seq.steps[0].state,StepState::Running);QVERIFY(busy(seq));
        auto step=advance(seq,view({{write,QVariantMap{{"state","running"}}}},write));
        QVERIFY(!step.send_apply);QCOMPARE(step.seq.phase,Phase::Write);
        step=advance(seq,view({{write,ok}},{}));
        QVERIFY(step.send_apply);QCOMPARE(step.seq.phase,Phase::Apply);QCOMPARE(step.seq.steps[0].state,StepState::Ok);
        seq=markSent(step.seq,sentOutcome(apply,{{apply,QVariantMap{{"state","running"}}}},apply));
        QCOMPARE(seq.steps[1].state,StepState::Running);
        const QVariantMap both{{write,ok},{apply,ok}};
        step=advance(seq,view(both,{},10,SensorKind::Live,100));
        QCOMPARE(step.seq.phase,Phase::Confirming);QCOMPARE(step.seq.confirm_count,10.0);
        seq=advance(step.seq,view(both,{},20,SensorKind::Live,200)).seq;
        QCOMPARE(seq.phase,Phase::Confirming); // one new count could predate the pause
        seq=advance(seq,view(both,{},30,SensorKind::Stale,300)).seq;
        QCOMPARE(seq.phase,Phase::Confirming); // stale samples do not count
        seq=advance(seq,view(both,{},30,SensorKind::Live,400)).seq;
        QCOMPARE(seq.phase,Phase::Applied);QVERIFY(!busy(seq));
        // No new sample in time, then two.
        seq=advance(started(),view({{write,ok}},{})).seq;seq=markSent(seq,{StepState::Running,{},{}});
        seq=advance(seq,view(both,{},10,SensorKind::Live,0)).seq;
        seq=advance(seq,view(both,{},10,SensorKind::Live,kConfirmMs+1)).seq;
        QCOMPARE(seq.phase,Phase::NoSample);QVERIFY(!busy(seq));
        seq=advance(seq,view(both,{},11)).seq;seq=advance(seq,view(both,{},12)).seq;
        QCOMPARE(seq.phase,Phase::Applied);
        // A refused, uncertain or lost write sends no apply.
        step=advance(started(),view({{write,QVariantMap{{"state","refused"},{"text","Busy"}}}},{}));
        QVERIFY(!step.send_apply);QCOMPARE(step.seq.phase,Phase::Failed);
        QCOMPARE(step.seq.note,QString("Write refused: Busy. Nothing was applied."));QCOMPARE(step.seq.steps[1].state,StepState::Pending);
        step=advance(started(),view({{write,QVariantMap{{"state","uncertain"},{"text","No reply"}}}},{}));
        QVERIFY(!step.send_apply);QCOMPARE(step.seq.phase,Phase::Uncertain);
        step=advance(started(),view({},{},10,SensorKind::Live,0,false));
        QVERIFY(!step.send_apply);QCOMPARE(step.seq.phase,Phase::Uncertain);
        QCOMPARE(step.seq.steps[0].text,QString("The link closed before the reply; it may have run. It was not retried."));
        step=advance(started(),view({{write,QVariantMap{{"state","ok"},{"rebooted",true}}}},{}));
        QVERIFY(!step.send_apply);QCOMPARE(step.seq.phase,Phase::Ended);
        step=advance(started(),view({{write,ok}},{},10,SensorKind::Live,0,false));
        QVERIFY(!step.send_apply);QCOMPARE(step.seq.phase,Phase::Uncertain);
        // Apply failures keep the write.
        seq=advance(started(),view({{write,ok}},{})).seq;seq=markSent(seq,{StepState::Running,{},{}});
        step=advance(seq,view({{write,ok},{apply,QVariantMap{{"state","refused"},{"text","Fault"}}}},{}));
        QCOMPARE(step.seq.phase,Phase::Failed);QCOMPARE(step.seq.steps[0].state,StepState::Ok);
        QCOMPARE(step.seq.steps[1].state,StepState::Refused);QVERIFY(step.seq.note.startsWith("Write succeeded"));
        step=advance(seq,view({{write,ok},{apply,QVariantMap{{"state","uncertain"}}}},{}));
        QCOMPARE(step.seq.phase,Phase::Uncertain);
        // Not sent: no running key, only an earlier outcome; a local refusal.
        seq=markSent(newApply("114:0",write,apply,kSampleHex,{}),sentOutcome(write,{{write,ok}},{}));
        QCOMPARE(seq.phase,Phase::Failed);QCOMPARE(seq.steps[0].text,QString("Not sent"));
        seq=markSent(newApply("114:0",write,apply,kSampleHex,{}),
                     sentOutcome(write,{{write,QVariantMap{{"state","refused"},{"text","Paused actions are blocked"}}}},{}));
        QCOMPARE(seq.steps[0].text,QString("Paused actions are blocked"));
        // Another instance selected mid-sequence: abandoned, nothing sent.
        step=advance(started(),view({{write,ok}},{},10,SensorKind::Live,0,true,"114:1"));
        QVERIFY(!step.send_apply);QCOMPARE(step.seq.phase,Phase::Abandoned);
    }

    // The QML singleton: maps round-trip through the typed core, and the memory copies.
    void calibrationQml() {
        using namespace imu_calibration;
        ImuCalibration q;
        const auto cal=toVariant(sampleCalibration());
        QCOMPARE(q.encodeBlob(cal),QString(kSampleHex));
        QCOMPARE(q.decodeBlob(kSampleHex)["cal"].toMap(),cal);
        QCOMPARE(sequenceFrom(toVariant(advance(newApply("k","w","a","b","l"),{}).seq)),advance(newApply("k","w","a","b","l"),{}).seq);
        const auto parsed=q.parseFile(q.buildFile(cal,toVariant(sampleProvenance()),"n"),{{"type","imu.bno055"},{"deviceId","x"}});
        QVERIFY(parsed["ok"].toBool());QCOMPARE(parsed["warnings"].toStringList().size(),1);
        QCOMPARE(q.parseFile("{",{})["error"].toString(),QString("The file is not JSON"));
        const auto sensor=q.sensorState(live(fullSample({3,3,3,3}),true));
        QVERIFY(q.fullyCalibrated(sensor));QCOMPARE(q.readBlock(sensor,{}),QString());
        QCOMPARE(q.axes(sensor["quat"]).size(),3);QVERIFY(q.axes(QVariant::fromValue(nullptr)).isEmpty());
        QVERIFY(!q.busy(QVariant::fromValue(nullptr)));
        QVERIFY(q.busy(q.newApply("k","w","a","b","l")));
        q.remember("dev|114:0","imported",QVariantMap{{"fileName","a"}});
        QCOMPARE(q.recall("dev|114:0")["imported"].toMap()["fileName"].toString(),QString("a"));
        QVERIFY(q.recall("other|114:0").isEmpty());
        q.remember("dev|114:0","imported",QVariant::fromValue(nullptr));
        QVERIFY(!q.recall("dev|114:0").contains("imported"));
    }

    void textFiles() {
        QTemporaryDir dir;LocalConnection c;
        const auto path=dir.filePath("a.imucal.json");
        auto w=c.writeTextFile(QUrl::fromLocalFile(path).toString(),QString::fromUtf8("{\"é\":1}\n"));
        QVERIFY2(w["ok"].toBool(),qPrintable(w["error"].toString()));QCOMPARE(w["path"].toString(),path);
        auto r=c.readTextFile(QUrl::fromLocalFile(path).toString());
        QVERIFY(r["ok"].toBool());QCOMPARE(r["text"].toString(),QString::fromUtf8("{\"é\":1}\n"));
        QVERIFY(c.readTextFile(path)["ok"].toBool()); // a plain path too
        QVERIFY(!c.readTextFile("http://example.com/a.json")["ok"].toBool());
        QVERIFY(!c.writeTextFile("http://example.com/a.json","x")["ok"].toBool());
        QVERIFY(!c.readTextFile(dir.filePath("missing.json"))["ok"].toBool());
        // Over the limit, both ways.
        QVERIFY(!c.writeTextFile(path,QString(Connection::kPanelFileBytes+1,'x'))["ok"].toBool());
        QCOMPARE(c.readTextFile(path)["text"].toString(),QString::fromUtf8("{\"é\":1}\n")); // unchanged
        {QFile f(dir.filePath("big"));QVERIFY(f.open(QIODevice::WriteOnly));f.write(QByteArray(Connection::kPanelFileBytes+1,'x'));}
        QVERIFY(c.readTextFile(dir.filePath("big"))["error"].toString().contains("larger"));
        {QFile f(dir.filePath("bin"));QVERIFY(f.open(QIODevice::WriteOnly));f.write(QByteArray("\xff\xfe\x00",3));}
        QVERIFY(c.readTextFile(dir.filePath("bin"))["error"].toString().contains("UTF-8"));
    }

    // imu.mock on vdev: levels are 0 until a calibration is applied, then 3.
    void calibrationPanel() {
        Fixture fixture;
        {
            Session s(fixture.endpoint.toStdString());Client cl(s);cl.info();
            cl.add(kImuMock,"imu",1);cl.system(5);cl.system(8);
        }
        QTest::qWait(150);
        QTemporaryDir dir;PanelShell shell;
        QVERIFY(shell.open(fixture.endpoint,"114:0",dir.filePath("gui.ini")));
        auto *panel=shell.panel;auto &c=shell.connection;
        // Live: numbers on the signal rows, levels 0, Read blocked.
        QVERIFY2(waitFor([&]{return shell.sensor()["state"]=="live";}),qPrintable(shell.sensor()["note"].toString()));
        const auto rows=c.live()["signals"].toList();
        QVERIFY(std::any_of(rows.begin(),rows.end(),[](const QVariant &r){
            const auto m=r.toMap();return m["name"]=="sample_count" && m["number"].toDouble()>0;}));
        QCOMPARE(shell.levels()["mag"].toInt(),0);
        QCOMPARE(panel->property("readReason").toString(),QString("Read requires all four calibration levels at 3 / 3"));
        QCOMPARE(panel->property("applyReason").toString(),QString("Import a file first"));

        // Refused imports send nothing.
        const auto before=c.actionResults();
        shell.call("importFile",QUrl::fromLocalFile(writeJson(dir,"other.json",calibrationFile("imu.bno055",c.deviceId(),"imu"))).toString());
        QCOMPARE(panel->property("importError").toString(),QString("The file is for imu.bno055; this instance is imu.mock"));
        QCOMPARE(state(panel),QString("refused"));
        auto broken=calibrationFile("imu.mock",c.deviceId(),"imu");broken.remove("gyro");
        shell.call("importFile",QUrl::fromLocalFile(writeJson(dir,"broken.json",broken)).toString());
        QVERIFY(panel->property("importError").toString().contains("gyro is missing"));
        QVERIFY(panel->property("imported").isNull() || !panel->property("imported").isValid() ||
                panel->property("imported").toMap().isEmpty());
        QCOMPARE(c.actionResults(),before);QVERIFY(c.actionRunning().isEmpty());

        // A file from another device: imported with a warning.
        shell.call("importFile",QUrl::fromLocalFile(writeJson(dir,"good.imucal.json",
            calibrationFile("imu.mock","aabbccddeeff","imu"))).toString());
        const auto imported=prop(panel,"imported");
        QCOMPARE(imported["fileName"].toString(),QString("good.imucal.json"));
        QCOMPARE(imported["warnings"].toList().size(),1);
        QVERIFY(panel->property("importError").toString().isEmpty());
        QCOMPARE(panel->property("applyReason").toString(),QString());
        QCOMPARE(c.actionResults(),before);

        // The review dialog dims the page; Cancel sends nothing.
        shell.call("requestApply");
        QVERIFY(waitFor([&]{return panel->property("dialogOpen").toBool() && shell.window->property("modalOpen").toBool();}));
        shell.call("cancelApply");
        QVERIFY(waitFor([&]{return !panel->property("dialogOpen").toBool();}));
        QCOMPARE(c.actionResults(),before);

        // Apply: write, then apply, then two new valid samples.
        shell.call("confirmApply");
        QVERIFY(panel->property("applying").toBool());
        QVERIFY2(waitFor([&]{return state(panel)=="ok";},10000),qPrintable(prop(panel,"operation")["text"].toString()));
        const auto steps=prop(panel,"operation")["steps"].toList();
        QCOMPARE(steps.size(),2);
        QCOMPARE(steps[0].toMap()["state"].toString(),QString("ok"));QCOMPARE(steps[1].toMap()["state"].toString(),QString("ok"));
        QVERIFY(prop(panel,"operation")["text"].toString().startsWith("Applied, and the sensor is sampling again"));
        QVERIFY(waitFor([&]{return shell.calibrated();}));
        QVERIFY(waitFor([&]{return c.unsaved();})); // staged by the write
        QVERIFY(waitFor([&]{
            const auto staged=stagedCalibration(*c.snapshot(),{kImuMock,0});
            return staged.at("calibration.valid")==1 && staged.at("calibration.mag_radius")==680 && staged.at("calibration.accel_y")==-7;
        }));

        // Read, then Export: the file carries what the sensor runs.
        QVERIFY(waitFor([&]{return panel->property("readReason").toString().isEmpty();}));
        shell.call("read");
        QVERIFY2(waitFor([&]{return state(panel)=="ok" && prop(panel,"operation")["title"]=="Read from IMU";},10000),
                 qPrintable(prop(panel,"operation")["text"].toString()));
        const auto lastRead=prop(panel,"lastRead");
        QCOMPARE(lastRead["cal"].toMap()["mag"].toMap()["z"].toInt(),310);
        panel->setProperty("exportNote","test rig");
        const auto exported=dir.filePath("export.imucal.json");
        shell.call("exportFile",QUrl::fromLocalFile(exported).toString());
        QCOMPARE(state(panel),QString("ok"));
        QFile f(exported);QVERIFY(f.open(QIODevice::ReadOnly));
        const auto file=QJsonDocument::fromJson(f.readAll()).object();
        const auto expected=calibrationFile("imu.mock","","imu");
        for(const auto *field:{"accel","gyro","mag","sic","has_sic"}) {QCOMPARE(file[field],expected[field]);}
        QCOMPARE(file["type"].toString(),QString("imu.mock"));QCOMPARE(file["note"].toString(),QString("test rig"));
        QCOMPARE(file["source"].toObject()["device_id"].toString(),c.deviceId());
        QCOMPARE(file["source"].toObject()["instance"].toString(),QString("imu"));
        QCOMPARE(file["levels"].toObject()["mag"].toInt(),3);
        // The export imports back without warnings.
        shell.call("importFile",QUrl::fromLocalFile(exported).toString());
        QCOMPARE(prop(panel,"imported")["warnings"].toList().size(),0);

        // The panel's memory survives another node and back.
        c.selectNode("5:1");QVERIFY(waitFor([&]{return c.layout().value("panel").toString().isEmpty();}));
        c.selectNode("114:0");
        auto *page=shell.window->findChild<QObject *>("layoutPage");page->setProperty("tab",1);
        QVERIFY(waitFor([&]{shell.panel=shell.window->findChild<QObject *>("calibrationPanel");return shell.panel && prop(shell.panel,"lastRead").size()>0;}));
        QCOMPARE(prop(shell.panel,"imported")["fileName"].toString(),QString("export.imucal.json"));

        // Another node selected mid-Apply: the panel unloads, the write runs
        // to its end, apply is never sent, and the reloaded panel says the
        // operation was interrupted.
        const auto applyKey=QString("114:0:%1").arg(0x304);
        shell.call("confirmApply");
        QVERIFY(shell.panel->property("applying").toBool());
        QCOMPARE(c.actionRunning(),QString("114:0:%1").arg(0x303));
        c.selectNode("5:1");
        QVERIFY(waitFor([&]{return shell.window->findChild<QObject *>("calibrationPanel")==nullptr && c.actionRunning().isEmpty();},10000));
        QTest::qWait(300);
        QVERIFY(c.actionRunning().isEmpty());QVERIFY(!c.actionResults().contains(applyKey));
        c.selectNode("114:0");page->setProperty("tab",1);
        QVERIFY(waitFor([&]{shell.panel=shell.window->findChild<QObject *>("calibrationPanel");return shell.panel!=nullptr;}));
        QCOMPARE(state(shell.panel),QString("uncertain"));
        QVERIFY(prop(shell.panel,"operation")["text"].toString().startsWith("Interrupted"));
        QVERIFY(!shell.panel->property("applying").toBool());

        // The link drops during Apply: the panel unloads with the layout and
        // nothing is in flight afterwards.
        shell.call("confirmApply");
        QVERIFY(shell.panel->property("applying").toBool());
        fixture.process.kill();fixture.process.waitForFinished(5000);
        QVERIFY(waitFor([&]{return !c.ready();},10000));
        QVERIFY(waitFor([&]{return shell.window->findChild<QObject *>("calibrationPanel")==nullptr;}));
        QVERIFY(c.actionRunning().isEmpty());
    }

    // The deferred real-sensor check (and its vdev dry run). Needs
    // DRIVELINK_GUI_BENCH_ENDPOINT and DRIVELINK_GUI_BENCH_IMU (type:instance);
    // DRIVELINK_GUI_BENCH_CAL_WAIT seconds for the operator to reach 3/3/3/3
    // (default 300). Refuses unsaved changes; REVERT restores the saved
    // calibration params at the end (the sensor keeps the profile it read).
    void benchCalibration() {
        const auto endpoint=qEnvironmentVariable("DRIVELINK_GUI_BENCH_ENDPOINT");
        const auto node=qEnvironmentVariable("DRIVELINK_GUI_BENCH_IMU");
        if(endpoint.isEmpty() || node.isEmpty()) {QSKIP("set DRIVELINK_GUI_BENCH_ENDPOINT and DRIVELINK_GUI_BENCH_IMU to run on a device");}
        const int wait=qEnvironmentVariableIntValue("DRIVELINK_GUI_BENCH_CAL_WAIT")>0?qEnvironmentVariableIntValue("DRIVELINK_GUI_BENCH_CAL_WAIT"):300;
        QTemporaryDir dir;PanelShell shell;
        QVERIFY2(shell.open(endpoint,node,dir.filePath("gui.ini"),30000),"the panel did not open (wrong IMU key or type?)");
        auto *panel=shell.panel;auto &c=shell.connection;
        struct Restore {std::function<void()> run;~Restore() {run();}} restore{[&] {
            if(c.linked() && c.unsaved()) {c.revert();waitFor([&]{return !c.unsaved();},5000);}
            if(c.linked()) {c.disconnectDevice();waitFor([&]{return !c.closing();},10000);}
        }};
        QVERIFY2(!c.unsaved(),"refusing to run with unsaved changes");
        qInfo("bench: device %s firmware %s, %s (%s); calibrate the sensor: up to %d s",qPrintable(c.deviceId()),
              qPrintable(c.info()["firmware"].toString()),qPrintable(node),qPrintable(c.layout()["node"].toMap()["typeName"].toString()),wait);
        QElapsedTimer t;t.start();qint64 printed=-5000;
        QVERIFY2(waitFor([&]{
            if(t.elapsed()-printed>=5000) {
                printed=t.elapsed();const auto l=shell.levels();
                qInfo("bench: %s, levels sys %d gyro %d accel %d mag %d",qPrintable(shell.sensor()["state"].toString()),
                      l["sys"].toInt(),l["gyro"].toInt(),l["accel"].toInt(),l["mag"].toInt());
            }
            return panel->property("readReason").toString().isEmpty();
        },wait*1000),qPrintable(panel->property("readReason").toString()));
        shell.call("read");
        QVERIFY2(waitFor([&]{return state(panel)!="running";},10000) && state(panel)=="ok",qPrintable(prop(panel,"operation")["text"].toString()));
        const auto exported=dir.filePath("bench.imucal.json");
        shell.call("exportFile",QUrl::fromLocalFile(exported).toString());QCOMPARE(state(panel),QString("ok"));
        shell.call("importFile",QUrl::fromLocalFile(exported).toString());
        QVERIFY2(panel->property("importError").toString().isEmpty(),qPrintable(panel->property("importError").toString()));
        QCOMPARE(prop(panel,"imported")["warnings"].toList().size(),0);
        QVERIFY2(waitFor([&]{return panel->property("applyReason").toString().isEmpty();}),qPrintable(panel->property("applyReason").toString()));
        shell.call("confirmApply");
        QVERIFY2(waitFor([&]{return !panel->property("applying").toBool();},15000) && state(panel)=="ok",
                 qPrintable(prop(panel,"operation")["text"].toString()));
        // The staged params equal the file.
        QFile f(exported);QVERIFY(f.open(QIODevice::ReadOnly));const auto file=QJsonDocument::fromJson(f.readAll()).object();
        const auto key=*parseNodeKey(node);
        QVERIFY(waitFor([&]{
            const auto s=stagedCalibration(*c.snapshot(),key);
            return s.at("calibration.valid")==1 && s.at("calibration.accel_x")==file["accel"].toObject()["x"].toInt() &&
                   s.at("calibration.mag_radius")==file["mag"].toObject()["radius"].toInt() &&
                   s.at("calibration.sic_8")==file["sic"].toArray()[8].toInt();
        }));
        QVERIFY(waitFor([&]{return shell.sensor()["state"]=="live";}));
        const auto l=shell.levels();
        qInfo("bench: applied; levels sys %d gyro %d accel %d mag %d; file %s",l["sys"].toInt(),l["gyro"].toInt(),
              l["accel"].toInt(),l["mag"].toInt(),qPrintable(QString::fromUtf8(QJsonDocument(file).toJson(QJsonDocument::Compact))));
    }
};

QTEST_MAIN(PanelTests)
#include "test_panels.moc"
