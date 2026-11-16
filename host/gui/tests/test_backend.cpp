#include "connection.hpp"
#include "safety_keys.hpp"
#include "style.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QAbstractItemModelTester>
#include <QFile>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <stdexcept>

using namespace drivelink;
using namespace drivelink::gui;
namespace defs=drivelink::protocol::defs;
namespace {
bool waitFor(const std::function<bool()> &condition,int timeout=8000) {
    QElapsedTimer elapsed;elapsed.start();
    while(!condition() && elapsed.elapsed()<timeout) {QCoreApplication::processEvents();QTest::qWait(10);}
    return condition();
}
QQuickItem *visualChild(QQuickItem *parent,const QString &name) {
    if(parent->objectName()==name) {return parent;}
    for(auto *child:parent->childItems()) {if(auto *found=visualChild(child,name)) {return found;}}
    return nullptr;
}
struct Fixture {
    QTemporaryDir directory;
    QProcess process;
    QString endpoint;
    QByteArray output;
    uint64_t sequence=0;
    explicit Fixture(bool safe=false,bool pty=false) {
        QStringList args={"--port","0","--state",directory.path()+"/state","--scenario-file",directory.path()+"/control"};
        if(safe) {args<<"--safe-mode";}
        if(pty) {args<<"--pty";}
        process.start(QString::fromUtf8(VDEV_PATH),args);
        if(!process.waitForStarted(10000) || !process.waitForReadyRead(10000)) {throw std::runtime_error("vdev did not start");}
        endpoint=QString::fromUtf8(process.readLine()).trimmed().mid(6);
        if(!endpoint.startsWith("tcp://") && !endpoint.startsWith("serial://")) {throw std::runtime_error("bad vdev endpoint");}
        if(!safe) {
            Session s(endpoint.toStdString());Client c(s);auto info=c.info();
            std::vector<Ref> refs;std::vector<uint32_t> values;
            for(const auto &loop:info.loops) {refs.push_back({5,loop.id,1});values.push_back(10);}
            refs.push_back({0,0,defs::system::kWatchdogMs});values.push_back(1000);c.set(refs,values);
            // A reboot must retain the desktop scheduling allowance too.
            // Otherwise the reconnect case silently returns to a 1 kHz loop
            // with an 8 ms group-swap deadline and a 200 ms watchdog.
            c.system(5);
        }
    }
    ~Fixture() {process.kill();process.waitForFinished(10000);}
    void command(const QString &action) {
        const auto temp=directory.path()+"/next";QFile file(temp);
        if(!file.open(QIODevice::WriteOnly)) {throw std::runtime_error("cannot write control");}
        file.write((QString::number(++sequence)+" "+action+"\n").toUtf8());file.close();
        const auto path=directory.path()+"/control";
        if(QFile::exists(path) && !QFile::remove(path)) {throw std::runtime_error("cannot replace control");}
        if(!QFile::rename(temp,path)) {throw std::runtime_error("cannot publish control");}
        const auto acknowledgement=(QString("SCENARIO %1 %2\n").arg(sequence).arg(action.section(' ',0,0))).toUtf8();
        // The 100 ms poll is not a scheduling guarantee. Wait for this command's
        // acknowledgement, emitted after its effect and stats-file close.
        const auto acknowledged=waitFor([&]{
            output+=process.readAllStandardOutput();output.replace("\r\n","\n");
            return output.contains(acknowledgement) || process.state()==QProcess::NotRunning;
        });
        if(!acknowledged || !output.contains(acknowledgement)) {
            throw std::runtime_error((QString("vdev did not acknowledge %1: %2 %3")
                .arg(sequence).arg(QString::fromUtf8(output),QString::fromUtf8(process.readAllStandardError()))).toStdString());
        }
        output.clear();
    }
    QByteArray stats() {
        command("stats");QFile file(directory.path()+"/control.stats");
        if(!file.open(QIODevice::ReadOnly)) {throw std::runtime_error("acknowledged stats file missing");}
        return file.readAll().replace("\r\n","\n");
    }
    void multiLoop() {
        {Session s(endpoint.toStdString());Client c(s);c.info();const auto extra=c.add(5,"extra").instance;
         const std::array<Ref,2> refs{{{5,extra,1},{5,extra,2}}};const std::array<uint32_t,2> values{{10,1}};
         c.set(refs,values);c.add(112,"first",0);c.add(112,"second",extra);c.system(5);c.system(8);}
        QTest::qWait(150);
    }
};
DeviceSnapshot sample() {
    DeviceSnapshot s;s.info.max_payload=1024;
    s.system_params[defs::system::kLimitsGroupEntries]=uint32_t(2);
    s.configured[{5,3}]={{5,3,255,0,"loop"},{}};
    s.configured[{5,9}]={{5,9,255,0,"other"},{}};
    s.configured[{112,7}]={{112,7,3,0,"motor"},{{1,float(2)}}};
    s.configured[{112,11}]={{112,11,9,0,"second"},{{1,float(4)}}};
    auto &type=s.types[112];type.descriptor={112,1,"discovered.mock"};
    ModelItem param;param.descriptor.id=1;param.descriptor.name="gain";param.descriptor.value_type=ValueType::F32;
    type.items[1]=param;
    for(uint16_t id=10;id<13;++id) {
        ModelItem input;input.descriptor.id=id;input.descriptor.name="input";input.descriptor.kind=ItemKind::Input;
        input.descriptor.value_type=ValueType::F32;type.items[id]=input;
    }
    s.pending=s.configured;s.pending.at({112,7}).instance.label="pending";s.pending.erase({112,11});
    return s;
}
GroupList available() {
    GroupList list;list.entry_capacity=64;list.command_capacity=5;
    for(uint8_t id=0;id<5;++id) {Group g;g.kind=GroupKind::Command;g.id=id;list.groups.push_back(g);}
    return list;
}
}
class BackendTests final : public QObject {
    Q_OBJECT
private slots:
    void scenarioFailureUnwinds() {
        Fixture fixture;
        QVERIFY(fixture.stats().contains("disarms 0\n"));
        fixture.process.kill();QVERIFY(fixture.process.waitForFinished(10000));
        QVERIFY_EXCEPTION_THROWN(fixture.stats(),std::runtime_error);
    }
    void planning() {
        auto s=sample();auto groups=available();
        auto plan=planCommands(s,groups);QCOMPARE(plan.size(),size_t(4));
        QCOMPARE(plan[0].loop,3);QCOMPARE(plan[2].loop,9);QCOMPARE(plan[0].refs.size(),size_t(2));
        groups.groups[0].configured=true;groups.groups[0].refs={{112,7,10}};
        QVERIFY_EXCEPTION_THROWN(planCommands(s,groups),Error);
        groups=available();groups.groups.resize(3);QVERIFY_EXCEPTION_THROWN(planCommands(s,groups),Error);
        s.info.max_payload=11;groups=available();Group extra;extra.kind=GroupKind::Command;extra.id=5;groups.groups.push_back(extra);
        plan=planCommands(s,groups);QCOMPARE(plan.size(),size_t(6));
    }
    void emptyAndUnavailable() {
        auto s=sample();s.configured.erase({112,7});s.configured.erase({112,11});
        auto result=planCommands(s,available());QCOMPARE(result.size(),size_t(1));QVERIFY(result[0].refs.empty());
        s.info.flags=8;QVERIFY_EXCEPTION_THROWN(planCommands(s,available()),Error);
        s.info.flags=0;s.configured.clear();QVERIFY_EXCEPTION_THROWN(planCommands(s,available()),Error);
    }
    void modelsAndErrors() {
        auto s=std::make_shared<DeviceSnapshot>(sample());InstanceTree tree;
        QAbstractItemModelTester tester(&tree,QAbstractItemModelTester::FailureReportingMode::QtTest);
        tree.reset(s);QCOMPARE(tree.rowCount(),2);QCOMPARE(tree.rowCount(tree.index(0,0)),1);
        auto child=tree.index(0,0,tree.index(0,0));QCOMPARE(tree.data(child,InstanceTree::Key).toString(),"112:7");
        QCOMPARE(tree.parent(child),tree.index(0,0));
        tree.reset(s,true);QCOMPARE(tree.data(tree.index(0,0,tree.index(0,0)),InstanceTree::Label).toString(),"pending");
        QCOMPARE(tree.rowCount(tree.index(1,0)),0);
        DescriptorRows rows;QAbstractItemModelTester rowTester(&rows,QAbstractItemModelTester::FailureReportingMode::QtTest);
        rows.reset(s,112,7);QCOMPARE(rows.rowCount(),4);
        QCOMPARE(rows.data(rows.index(0),DescriptorRows::ValueBits).toUInt(),valueBits(Value(float(2))));
        QVERIFY(!rows.data(rows.index(1),DescriptorRows::ValueBits).isValid());
        rows.reset(s,112,11,true);QCOMPARE(rows.rowCount(),0);
        Error error(ErrorKind::Timeout,"uncertain",3,4);error.family=1;error.op=2;error.has_request_context=true;error.outcome_unknown=true;
        const auto fields=errorFields(error);QVERIFY(fields["outcomeUnknown"].toBool());QCOMPARE(fields["op"].toInt(),2);
    }
    void connectArmFaultAndCleanup() {
        Fixture fixture;LocalConnection connection;
        bool wrongThread=false;
        connect(&connection,&Connection::changed,&connection,[&]{wrongThread|=QThread::currentThread()!=connection.thread();});
        connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.armEnabled();}));
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        QTest::qWait(1300);QCOMPARE(connection.armState(),1); // automatic keepalives
        const auto stats=fixture.stats();QVERIFY(stats.contains("values 0\n"));QVERIFY(!stats.contains("prefix 0\n"));
        fixture.command("fault");QVERIFY(waitFor([&]{return connection.armState()==2;}));
        connection.acknowledge();QVERIFY(waitFor([&]{return !connection.lastError().isEmpty();}));
        QVERIFY(connection.lastError()["hasContext"].toBool());
        fixture.command("fault-clear");connection.acknowledge();
        QVERIFY(waitFor([&]{return connection.armState()==0;}));
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing() && !connection.linked();}));
        QVERIFY(!wrongThread);
        Session s(fixture.endpoint.toStdString());Client c(s);c.info();
        const auto groups=c.groups();for(const auto &g:groups.groups) {QVERIFY(!g.configured);}
    }
    void disarmRacesDelayedArm() {
        Fixture fixture;LocalConnection connection;connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.armEnabled();}));
        fixture.command("slow 400");connection.arm();QTest::qWait(70);connection.disarm();
        QVERIFY(waitFor([&]{return connection.ready() && !connection.arming() && connection.armEnabled();},10000));
        QTest::qWait(600);QCOMPARE(connection.armState(),0);
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
    }
    void laterDisarmSurvivesFirstTimeout() {
        Fixture fixture;LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.armEnabled();}));c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        fixture.command("drop-disarm 1");c.disarm();QTest::qWait(50);c.disarm();
        QVERIFY(waitFor([&]{return c.armState()==0 && c.armEnabled();},6000));
        QCOMPARE(c.lastError()["kind"].toInt(),int(ErrorKind::Timeout));
        QVERIFY(fixture.stats().contains("disarms 2\n"));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void disarmBeforeSessionPublication() {
        Fixture fixture;
        {Session s(fixture.endpoint.toStdString());Client client(s);client.info();
         const auto g=client.cmdConfigure(0,0,{});client.arm();client.command(0,g.epoch,0);}
        LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(!c.linked()); // opening is deferred to the next GUI turn
        c.disarm();QVERIFY(waitFor([&]{return c.armEnabled();}));
        QCOMPARE(c.armState(),0);QCOMPARE(c.reason(),"Explicit disarm");
        QVERIFY(fixture.stats().contains("disarms 1\n"));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void closeAfterUncertainArm() {
        Fixture fixture;LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.armEnabled();}));fixture.command("drop-arm-reply 1");
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();},8000));
        QVERIFY(!fixture.stats().contains("group "));
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        QCOMPARE(c.armState(),0);c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void observerCloseWaitsForExplicitDisarm() {
        Fixture fixture;
        {Session s(fixture.endpoint.toStdString());Client client(s);client.info();
         const auto g=client.cmdConfigure(0,0,{});client.arm();client.command(0,g.epoch,0);}
        fixture.command("slow 100");fixture.command("drop-disarm 1");
        LocalConnection c;c.connectTo(fixture.endpoint);c.disarm();
        QVERIFY(waitFor([&]{return c.linked();}));
        QVERIFY(fixture.stats().contains("disarms 1\n"));
        QVERIFY(c.loading());c.disconnectDevice();QVERIFY(c.closing());QVERIFY(c.linked());
        QVERIFY(waitFor([&]{return !c.closing() && !c.linked();},6000));
        QCOMPARE(c.lastError()["kind"].toInt(),int(ErrorKind::Timeout));
        const auto stats=fixture.stats();QVERIFY(stats.contains("disarms 1\n"));
        QVERIFY(stats.contains("group 0 0 0\n"));QVERIFY(!stats.contains("group 1 "));
    }
    void multipleLoopsAndNoValueWrites() {
        Fixture fixture;fixture.multiLoop();LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.armEnabled();}));c.arm();QVERIFY(waitFor([&]{return c.armState()==1;}));
        QTest::qWait(1200);const auto stats=fixture.stats();QVERIFY(stats.contains("values 0\n"));
        QVERIFY(stats.contains("group 0 0 "));QVERIFY(stats.contains("group 1 2 "));
        QCOMPARE(c.armState(),1);c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        QVERIFY(!fixture.stats().contains("group "));
    }
    void existingInputAndUncertainPartialSetup() {
        {Fixture fixture;fixture.multiLoop();
         {Session s(fixture.endpoint.toStdString());Client c(s);c.info();
          const auto items=c.items(112);const auto input=std::find_if(items.begin(),items.end(),[](const auto &v){return v.kind==ItemKind::Input;});
          QVERIFY(input!=items.end());const Ref ref{112,0,input->id};c.cmdConfigure(0,0,{&ref,1});}
         LocalConnection c;c.connectTo(fixture.endpoint);
         QVERIFY(waitFor([&]{return c.ready() && !c.lastError().isEmpty();}));QVERIFY(!c.armEnabled());
         c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
         const auto stats=fixture.stats();QVERIFY(stats.contains("group 0 0 1\n"));QVERIFY(!stats.contains("group 1 "));}
        Fixture fixture;fixture.multiLoop();fixture.command("drop-config-reply 2");
        LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.ready() && c.lastError()["outcomeUnknown"].toBool();}));QVERIFY(!c.armEnabled());
        QVERIFY(!fixture.stats().contains("group "));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void observeAlreadyArmed() {
        Fixture fixture;
        {Session s(fixture.endpoint.toStdString());Client c(s);c.info();auto g=c.cmdConfigure(0,0,{});
         c.command(0,g.epoch,0);c.arm();c.command(0,g.epoch,1);}
        LocalConnection connection;connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.ready() && connection.snapshot()->status.has_value();}));
        QCOMPARE(connection.armState(),1);QVERIFY(!connection.armEnabled());
        // Observing must not send keepalives for the old owner's group.
        QVERIFY(waitFor([&]{return connection.armState()==0 && connection.reason()=="Command watchdog";},3000));
        QVERIFY(!connection.armEnabled());
        const auto stats=fixture.stats();QVERIFY(!stats.contains("group 1 "));
        connection.disarm();QVERIFY(waitFor([&]{return connection.armEnabled();}));
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
        Session s(fixture.endpoint.toStdString());Client c(s);c.info();const auto groups=c.groups();
        QVERIFY(groups.groups.at(groups.subscription_capacity).configured);
        for(const auto &g:groups.groups) {if(g.kind==GroupKind::Command && g.id!=0) {QVERIFY(!g.configured);}}
    }
    void safeModeAndLostLink() {
        {Fixture fixture(true);LocalConnection c;c.connectTo(fixture.endpoint);
         QVERIFY(waitFor([&]{return c.ready();}));QVERIFY(c.safeMode());QVERIFY(!c.armEnabled());
         c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));}
        Fixture fixture;LocalConnection c;c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        fixture.command("stall");QVERIFY(waitFor([&]{return !c.linked();},6000));QVERIFY(!c.armEnabled());
        fixture.command("resume");c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        fixture.command("reboot");QVERIFY(waitFor([&]{return !c.linked();}));
        c.connectTo(fixture.endpoint);
        QVERIFY2(waitFor([&]{return c.armEnabled();}),qPrintable(c.armBlockedReason()+": "+c.lastError()["message"].toString()+
            QString(" (%1/%2)").arg(c.lastError()["result"].toInt()).arg(c.lastError()["detail"].toInt())));
        QCOMPARE(c.armState(),0);
        for(const auto &loop:c.snapshot()->info.loops) {QCOMPARE(loop.rate_hz,uint32_t(10));}
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void observeDuringSlowDiscovery() {
        Fixture fixture;
        {Session s(fixture.endpoint.toStdString());Client client(s);client.info();
         const auto g=client.cmdConfigure(0,0,{});client.arm();client.command(0,g.epoch,0);}
        fixture.command("slow 50");LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.ready() && c.snapshot()->status.has_value();},15000));
        QVERIFY(waitFor([&]{return c.armState()==0 && c.reason()=="Command watchdog";}));
        QVERIFY(!c.armEnabled());QVERIFY(c.armBlockedReason().contains("DISARM"));
        QVERIFY(!fixture.stats().contains("group 1 "));
        c.disarm();QVERIFY(waitFor([&]{return c.armEnabled();}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
    void disconnectDuringDiscovery() {
        Fixture fixture;fixture.command("slow 500");LocalConnection c;
        c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.linked();}));
        c.disarm();c.disconnectDevice();QVERIFY(waitFor([&]{return !c.linked() && !c.closing();}));
        QTest::qWait(600);QVERIFY(!c.ready());
        fixture.command("slow 0");c.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return c.armEnabled();}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
#ifndef Q_OS_WIN
    void bootDuringCommandSetup() {
        Fixture fixture(false,true);fixture.command("drop-config-reply 1");
        LocalConnection c;c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.ready();}));const auto generation=c.snapshot()->generation;
        QVERIFY(waitFor([&]{return fixture.stats().contains("group 0 ");}));
        fixture.command("reboot");
        QVERIFY(waitFor([&]{return c.ready() && c.snapshot()->generation!=generation && c.armEnabled();},15000));
        QCOMPARE(c.armState(),0);QVERIFY(fixture.stats().contains("values 0\n"));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
    }
#endif
    void keysAndShell() {
        Q_INIT_RESOURCE(qmake_DriveLink_Shell);
        Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);
        Fixture fixture;LocalConnection connection;SafetyKeys keys;
        connect(&keys,&SafetyKeys::escapeRequested,&connection,&Connection::disarm);
        QSignalSpy escapes(&keys,&SafetyKeys::escapeRequested);
        QQmlApplicationEngine engine;QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("connection",&connection);
        engine.rootContext()->setContextProperty("safetyKeys",&keys);
        engine.loadFromModule("DriveLink.Shell","Shell");QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());QVERIFY(window);
        connection.connectTo(fixture.endpoint);QVERIFY(waitFor([&]{return connection.armEnabled();}));
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        QQmlComponent component(&engine);component.setData("import QtQuick\nimport QtQuick.Controls.Basic\nTextField {text: 'before'; width: 300; height: 40}",QUrl());
        std::unique_ptr<QObject> field(component.create());QVERIFY2(field,qPrintable(component.errorString()));
        auto *item=qobject_cast<QQuickItem *>(field.get());item->setParentItem(window->contentItem());item->forceActiveFocus();
        QVERIFY(waitFor([&]{return item->hasActiveFocus();}));field->setProperty("text","changed");
        QTest::keyClick(window,Qt::Key_Escape);QCOMPARE(escapes.count(),1);QCOMPARE(field->property("text").toString(),"before");
        QVERIFY(waitFor([&]{return connection.armState()==0 && connection.armEnabled();}));
        QTest::keyClick(window,Qt::Key_4,Qt::ControlModifier);QCOMPARE(window->property("page").toInt(),3);
        QQmlComponent popupComponent(&engine);
        popupComponent.setData("import QtQuick\nimport QtQuick.Controls.Basic\nPopup {width: 320; height: 100; focus: true; contentItem: TextField {objectName: 'popupEdit'; text: 'original'; property int changedCommits: 0; onEditingFinished: if (text !== 'original') ++changedCommits}}",QUrl());
        std::unique_ptr<QObject> popup(popupComponent.create());QVERIFY2(popup,qPrintable(popupComponent.errorString()));
        popup->setProperty("parent",QVariant::fromValue(window->contentItem()));QMetaObject::invokeMethod(popup.get(),"open");
        QVERIFY(waitFor([&]{return popup->property("opened").toBool();}));
        auto *popupEdit=popup->findChild<QQuickItem *>("popupEdit");QVERIFY(popupEdit);popupEdit->forceActiveFocus();
        QVERIFY(waitFor([&]{return popupEdit->hasActiveFocus();}));popupEdit->setProperty("text","edited");
        QTest::keyClick(window,Qt::Key_Escape);QCOMPARE(escapes.count(),2);
        QVERIFY(waitFor([&]{return !popup->property("visible").toBool();}));
        QCOMPARE(popupEdit->property("text").toString(),"original");QCOMPARE(popupEdit->property("changedCommits").toInt(),0);
        QVERIFY(waitFor([&]{return connection.armEnabled();}));
        auto *dialog=window->findChild<QObject *>("shellDialog");QVERIFY(dialog);QMetaObject::invokeMethod(dialog,"open");
        QVERIFY(waitFor([&]{return dialog->property("opened").toBool();}));
        auto *scrim=window->findChild<QQuickItem *>("pageScrim");auto *bar=window->findChild<QQuickItem *>("statusBar");
        QVERIFY(scrim && bar);QCOMPARE(scrim->mapToScene({0,0}).y(),bar->height());
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        QTest::keyClick(window,Qt::Key_Escape);QCOMPARE(escapes.count(),3);
        QVERIFY(waitFor([&]{return !dialog->property("visible").toBool() && connection.armState()==0 && connection.armEnabled();}));
        QMetaObject::invokeMethod(dialog,"open");QVERIFY(waitFor([&]{return dialog->property("opened").toBool();}));
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1;}));
        auto *button=window->findChild<QQuickItem *>("disarmButton");QVERIFY(button);
        const auto at=button->mapToScene({button->width()/2,button->height()/2});
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,at.toPoint());
        QVERIFY(waitFor([&]{return connection.armState()==0;}));
        QTest::keyClick(window,Qt::Key_Escape);
        for(int i=0;i<6;++i) {
            QVERIFY(QMetaObject::invokeMethod(window,"notify",Q_ARG(QVariant,QString("toast %1").arg(i)),Q_ARG(QVariant,i==5?2:0)));
        }
        QCOMPARE(window->property("toastCount").toInt(),4);
        QVERIFY(waitFor([&]{return window->property("toastCount").toInt()==1;},5000));
        auto *toast=visualChild(window->contentItem(),"shellToast");QVERIFY(toast);
        auto *close=visualChild(toast,"closeButton");QVERIFY(close);
        const auto closeAt=close->mapToScene({close->width()/2,close->height()/2});
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,closeAt.toPoint());
        QVERIFY(waitFor([&]{return window->property("toastCount").toInt()==0;}));
        const auto beforeLostRelease=escapes.count();
        QTest::keyPress(window,Qt::Key_Escape);
        QTest::keyPress(window,Qt::Key_Escape); // previous release was outside the app
        QTest::keyRelease(window,Qt::Key_Escape);
        QCOMPARE(escapes.count(),beforeLostRelease+2);
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
    }
};
QTEST_MAIN(BackendTests)
#include "test_backend.moc"
