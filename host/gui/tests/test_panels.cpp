// Custom panels (H2f): the generic backend surface a panel uses (numeric live
// values, action outcomes, small text files) and the imu.bno055 calibration
// panel in the shell on vdev's imu.mock (the same F6 contract): import,
// review, write → apply, a new valid sample, read, export. benchCalibration
// runs the same path on a device (a real BNO055: the deferred HW check).
#include "connection.hpp"
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

using namespace drivelink;
using namespace drivelink::gui;
namespace {
using namespace drivelink::gui::test;

constexpr uint16_t kImuMock=114;

// A version-1 calibration file (calibration.js buildFile's shape).
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
        QVERIFY(QFile::exists(":/qt/qml/DriveLink/Modules/imu.bno055/calibration.js"));
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
