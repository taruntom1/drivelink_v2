// Remote mode (H3b-1): the pure rules in remote_model (both OSes) and a
// LocalConnection through drivelink-agent and vdev (Linux, where the agent
// builds; skipped elsewhere): observer first, lease handoff and revocation,
// DISARM from an observer, the exclusive-operation rule, live-edit
// confirmation, history, client list, refusals and device loss.
#include "test_support.hpp"
#include "remote_model.hpp"
#include "style.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>

// Q_INIT_RESOURCE declares at global scope.
static void initShellResources() {
    Q_INIT_RESOURCE(qmake_DriveLink_Shell);
    Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);drivelink::gui::test::registerShellTypes();
}

// The backend surface LeaseDialogs uses. A held live edit can be re-held by
// the Apply itself, as Tune's gain writes are when a write is refused.
class LeaseBackend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap lease MEMBER lease NOTIFY changed)
    Q_PROPERTY(QVariantMap liveEdit MEMBER liveEdit NOTIFY changed)
public:
    QVariantMap lease{{"holderId",3},{"holderName","ros_bridge"},{"holderKindText","ROS"},{"ros",true},{"mine",false}};
    QVariantMap liveEdit;
    int applied=0,cancelled=0,takes=0,serial=0;
    bool reholdOnApply=false;
    void hold() {liveEdit={{"key","5:0:1"},{"serial",QString::number(++serial)},{"what","rate = 20"},{"holder","ros_bridge"},{"ros",true}};emit changed();}
    Q_INVOKABLE void applyLiveEdit(bool) {
        ++applied;liveEdit.clear();emit changed();
        if(reholdOnApply) {reholdOnApply=false;hold();}
    }
    Q_INVOKABLE void cancelLiveEdit() {++cancelled;liveEdit.clear();emit changed();}
    Q_INVOKABLE void takeLease() {++takes;}
    Q_INVOKABLE void releaseLease() {}
    Q_INVOKABLE void acknowledgeRevoked() {}
signals:
    void changed();
    void leaseRevoked(const QString &by);
};

using namespace drivelink;
using namespace drivelink::gui;
using drivelink::gui::test::waitFor;
namespace defs=drivelink::protocol::defs;

namespace {
constexpr uint8_t kGui=uint8_t(defs::AgentClientKind::Gui),kRos=uint8_t(defs::AgentClientKind::Ros);
agent::LeaseEvent leaseEvent(uint16_t holder,const char *name,uint8_t kind,uint16_t previous,defs::LeaseChange reason) {
    return {holder,name,kind,previous,uint8_t(reason)};
}
Error refusal(defs::Result result,defs::Detail detail) {
    Error e(ErrorKind::Rejected,"refused");e.result=uint8_t(result);e.detail=uint8_t(detail);return e;
}
#ifdef AGENT_PATH
// One vdev ("base") behind a drivelink-agent on a free port.
struct AgentFixture {
    test::Fixture vdev;
    QProcess agent;
    QString address;
    const QString token=QStringLiteral("gui-remote-token");
    AgentFixture() {
        agent.start(QString::fromUtf8(AGENT_PATH),{"--port","0","--token",token,"--device","base="+vdev.endpoint});
        if(!agent.waitForStarted(10000) || !agent.waitForReadyRead(10000)) {throw std::runtime_error("agent did not start");}
        const auto line=QString::fromUtf8(agent.readLine()).trimmed();
        if(!line.startsWith("READY tcp://")) {throw std::runtime_error("bad agent READY line");}
        address=line.mid(12);
        // The agent opens the device (GET_INFO, DISARM, clear) before it accepts clients for it.
        const bool online=waitFor([&] {
            try {
                const auto devices=listAgentDevices("tcp://"+address.toStdString(),token.toStdString());
                return !devices.empty() && devices.front().online;
            } catch(const Error &) {return false;}
        },15000);
        if(!online) {throw std::runtime_error("the agent's device did not come online");}
    }
    ~AgentFixture() {agent.kill();agent.waitForFinished(10000);}
};
struct Window {
    QTemporaryDir dir;
    LocalConnection c;
    explicit Window(const QString &name) {c.setSettingsPath(dir.filePath("gui.ini"));c.setClientName(name);}
    bool open(AgentFixture &f,int timeout=15000) {
        c.connectAgent(f.address,f.token,"base",true);
        return waitFor([&]{return c.ready() && c.snapshot() && c.snapshot()->status;},timeout);
    }
    uint32_t param(const Ref &ref) {
        const auto *entry=anyEntry(*c.snapshot(),{ref.type,ref.instance});
        return entry?valueBits(entry->params.at(ref.item)):0;
    }
};
#endif
} // namespace

class RemoteTests final : public QObject {
    Q_OBJECT
private slots:
    void leaseTrackerHandoff() {
        LeaseTracker t;t.reset(7,{});
        QCOMPARE(t.chip(),LeaseChip::Available);QVERIFY(!t.mine());QVERIFY(!t.otherHolds());
        // A Take granted by event.
        t.takeSent();QCOMPARE(t.chip(),LeaseChip::Requesting);
        auto change=t.apply(leaseEvent(7,"me",kGui,0,defs::LeaseChange::Taken));
        QVERIFY(change.gained);QVERIFY(!change.lost);QVERIFY(t.mine());QCOMPARE(t.chip(),LeaseChip::Mine);
        QVERIFY(!t.requesting());
        // The answer after the event changes nothing.
        QVERIFY(!t.takeAnswered(true,false).gained);
        // Revoked by ROS: first the revocation, then the grant naming the taker.
        change=t.apply(leaseEvent(0,"",0,7,defs::LeaseChange::Taken));
        QVERIFY(change.lost);QVERIFY(change.revoked);QCOMPARE(t.chip(),LeaseChip::Revoked);QVERIFY(!t.revokedBy());
        change=t.apply(leaseEvent(3,"ros_bridge",kRos,0,defs::LeaseChange::Taken));
        QVERIFY(!change.lost);QVERIFY(t.revokedBy());QCOMPARE(t.revokedBy()->name,QString("ros_bridge"));
        QCOMPARE(t.chip(),LeaseChip::Revoked);QVERIFY(t.otherHolds());
        t.acknowledgeRevoked();QCOMPARE(t.chip(),LeaseChip::Ros);QCOMPARE(t.holderText(),QString("ros_bridge"));
        // Another GUI.
        t.apply(leaseEvent(0,"",0,3,defs::LeaseChange::Released));QCOMPARE(t.chip(),LeaseChip::Available);
        t.apply(leaseEvent(9,"bench-laptop",kGui,0,defs::LeaseChange::Taken));QCOMPARE(t.chip(),LeaseChip::Other);
        // A release of our own is not a revocation.
        t.apply(leaseEvent(0,"",0,9,defs::LeaseChange::Disconnected));
        t.takeSent();QVERIFY(t.takeAnswered(true,false).gained);QVERIFY(t.mine()); // answer before the event
        change=t.apply(leaseEvent(0,"",0,7,defs::LeaseChange::Released));
        QVERIFY(change.lost);QVERIFY(!change.revoked);QCOMPARE(t.chip(),LeaseChip::Available);
        // released() after the event changes nothing; before it, it ends the lease.
        QVERIFY(!t.released().lost);
        t.apply(leaseEvent(7,"me",kGui,0,defs::LeaseChange::Taken));QVERIFY(t.released().lost);QVERIFY(!t.mine());
    }
    void leaseTrackerUncertainTake() {
        LeaseTracker t;t.reset(4,{2,"other",kGui});
        QCOMPARE(t.chip(),LeaseChip::Other);
        t.takeSent();QVERIFY(!t.takeAnswered(false,true).gained);
        QVERIFY(t.requesting());QCOMPARE(t.chip(),LeaseChip::Requesting); // a Lease event decides
        // Another client's grant decides it too: ours did not win.
        t.apply(leaseEvent(0,"",0,2,defs::LeaseChange::Taken));QVERIFY(t.requesting());
        t.apply(leaseEvent(5,"third",kGui,0,defs::LeaseChange::Taken));
        QVERIFY(!t.requesting());QCOMPARE(t.chip(),LeaseChip::Other);
        // A refused Take ends the request.
        t.takeSent();t.takeAnswered(false,false);QVERIFY(!t.requesting());
        // Events naming us as previous when we did not hold it are no loss.
        QVERIFY(!t.apply(leaseEvent(0,"",0,4,defs::LeaseChange::Taken)).lost);QVERIFY(!t.revoked());
    }
    void leaseTrackerLateTakeAnswer() {
        // Granted, then revoked, and only then the Take's Ok: the events are
        // newer, so the late answer must not make this window the holder.
        LeaseTracker t;t.reset(7,{});
        t.takeSent();
        QVERIFY(t.apply(leaseEvent(7,"me",kGui,0,defs::LeaseChange::Taken)).gained);
        QVERIFY(t.apply(leaseEvent(0,"",0,7,defs::LeaseChange::Taken)).lost);
        t.apply(leaseEvent(3,"ros_bridge",kRos,0,defs::LeaseChange::Taken));
        const auto change=t.takeAnswered(true,false);
        QVERIFY(!change.gained);QVERIFY(!t.mine());QVERIFY(t.otherHolds());QCOMPARE(t.chip(),LeaseChip::Revoked);
    }
    void leaseTrackerReconcile() {
        using drivelink::agent::ClientRecord;
        LeaseTracker t;t.reset(7,{});
        // Agreeing list: no change.
        QVERIFY(!t.reconcile({{7,"me",kGui,false},{3,"ros",kRos,false}},t.events()).gained);
        QCOMPARE(t.chip(),LeaseChip::Available);
        // A missed grant to us.
        auto change=t.reconcile({{7,"me",kGui,true},{3,"ros",kRos,false}},t.events());
        QVERIFY(change.gained);QVERIFY(t.mine());
        // A missed revocation by ROS: lost, revoked, taker known.
        change=t.reconcile({{7,"me",kGui,false},{3,"ros",kRos,true}},t.events());
        QVERIFY(change.lost);QVERIFY(change.revoked);QVERIFY(!t.mine());
        QCOMPARE(t.chip(),LeaseChip::Revoked);QVERIFY(t.revokedBy());QCOMPARE(t.revokedBy()->kind,uint8_t(kRos));
        t.acknowledgeRevoked();QCOMPARE(t.chip(),LeaseChip::Ros);
        // A missed release by the holder: nobody holds it; no loss of ours.
        change=t.reconcile({{7,"me",kGui,false}},t.events());
        QVERIFY(!change.lost);QCOMPARE(t.chip(),LeaseChip::Available);
        // Our lease ended with nobody else holding it: a loss, not a revocation.
        t.reconcile({{7,"me",kGui,true}},t.events());QVERIFY(t.mine());
        change=t.reconcile({{7,"me",kGui,false}},t.events());
        QVERIFY(change.lost);QVERIFY(!change.revoked);QCOMPARE(t.chip(),LeaseChip::Available);
        // A list requested before a Lease event that was applied since is
        // older than this view: it changes nothing (reply, events, then the
        // completion: the revocation must not be undone).
        t.reconcile({{7,"me",kGui,true}},t.events());QVERIFY(t.mine());
        const auto sentAt=t.events();
        t.apply(leaseEvent(0,"",0,7,defs::LeaseChange::Taken));
        t.apply(leaseEvent(3,"ros",kRos,0,defs::LeaseChange::Taken));
        change=t.reconcile({{7,"me",kGui,true},{3,"ros",kRos,false}},sentAt);
        QVERIFY(!change.gained);QVERIFY(!t.mine());QCOMPARE(t.chip(),LeaseChip::Revoked);
    }
    void leaseDialogsLiveEdit() {
        initShellResources();
        LeaseBackend backend;QQmlApplicationEngine engine;
        QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("leaseBackend",&backend);
        engine.loadData(R"(import QtQuick
import DriveLink.Shell
Window { width: 1280; height: 800; visible: true
    LeaseDialogs { objectName: "dialogs"; anchors.fill: parent; backend: leaseBackend } })");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());QVERIFY(window);
        auto *dialogs=window->findChild<QObject *>("dialogs");QVERIFY(dialogs);
        auto *dialog=dialogs->findChild<QObject *>("liveEditDialog");QVERIFY(dialog);
        const auto shown=[&]{return dialog->property("visible").toBool();};
        // Apply sends, and an edit held again by that Apply is asked about,
        // not cancelled by the close that ends the Apply.
        backend.hold();QVERIFY(waitFor(shown));
        backend.reholdOnApply=true;
        QMetaObject::invokeMethod(dialog,"accept");
        QVERIFY(waitFor([&]{return shown() && dialog->property("shownSerial").toString()=="2";}));
        QCOMPARE(backend.applied,1);QCOMPARE(backend.cancelled,0);QVERIFY(!backend.liveEdit.isEmpty());
        // Cancel drops it unsent.
        QMetaObject::invokeMethod(dialog,"reject");
        QVERIFY(waitFor([&]{return !shown();}));QCOMPARE(backend.cancelled,1);QCOMPARE(backend.applied,1);
        // Esc does too.
        backend.hold();QVERIFY(waitFor(shown));
        window->requestActivate();QTest::keyClick(window,Qt::Key_Escape);
        QVERIFY(waitFor([&]{return !shown();}));QCOMPARE(backend.cancelled,2);
        // An edit that ends elsewhere (the connection closed) only closes it.
        backend.hold();QVERIFY(waitFor(shown));
        backend.liveEdit.clear();emit backend.changed();
        QVERIFY(waitFor([&]{return !shown();}));QCOMPARE(backend.cancelled,2);QCOMPARE(backend.applied,1);
        // The take question closes unanswered when the lease changes hands.
        auto *take=dialogs->findChild<QObject *>("takeLeaseDialog");QVERIFY(take);
        QMetaObject::invokeMethod(dialogs,"requestTake");QVERIFY(waitFor([&]{return take->property("visible").toBool();}));
        backend.lease["holderId"]=9;backend.lease["holderName"]="bench-laptop";backend.lease["ros"]=false;emit backend.changed();
        QVERIFY(waitFor([&]{return !take->property("visible").toBool();}));QCOMPARE(backend.takes,0);
    }
    // The PC hardware check (H3b-1): drivelink-agent in WSL owns the bench
    // board (tools/wsl_serial.py run), and two GUI connections on Windows go
    // through it. Observer first, Take, ARM; a CLI client takes the lease
    // (A revoked and the board disarmed); DISARM from an observer while the
    // other holds and arms; REBOOT through the agent on serial (the agent's
    // Boot path: the link stays, both see the reboot, the holder sets up its
    // command groups again and can ARM). Nothing is written to the board.
    void benchRemote() {
        const auto address=qEnvironmentVariable("DRIVELINK_GUI_BENCH_AGENT");
        if(address.isEmpty()) {QSKIP("set DRIVELINK_GUI_BENCH_AGENT (host:port) and _TOKEN to run through an agent");}
        const auto token=qEnvironmentVariable("DRIVELINK_GUI_BENCH_TOKEN");
        const auto device=qEnvironmentVariable("DRIVELINK_GUI_BENCH_DEVICE","bench");
        const auto cli=qEnvironmentVariable("DRIVELINK_GUI_BENCH_CLI");
        QVERIFY2(!token.isEmpty(),"DRIVELINK_GUI_BENCH_TOKEN is empty");
        QTemporaryDir dir;
        auto open=[&](LocalConnection &c,const QString &name) {
            c.setSettingsPath(dir.filePath(name+".ini"));c.setClientName(name);
            c.connectAgent(address,token,device,false);
            return waitFor([&]{return c.ready() && c.snapshot() && c.snapshot()->status.has_value();},30000);
        };
        LocalConnection a,b;
        QVERIFY2(open(a,"bench-gui-a"),qPrintable(a.lastError()["message"].toString()+" "+a.agentFailure()["text"].toString()));
        QVERIFY2(open(b,"bench-gui-b"),qPrintable(b.lastError()["message"].toString()));
        QVERIFY2(!a.unsaved() && !a.pendingReboot(),"refusing to run with unsaved or pending changes");
        QVERIFY2(a.lease()["chip"].toInt()==int(LeaseChip::Available),"another client holds the lease");
        qInfo("bench: device %s firmware %s through %s",qPrintable(a.deviceId()),qPrintable(a.info()["firmware"].toString()),qPrintable(address));
        QSignalSpy revoked(&a,&Connection::leaseRevoked);
        // Observer first.
        QTest::qWait(500);
        QVERIFY(a.commandGroups().empty());QVERIFY(!a.armEnabled());QVERIFY(a.armBlockedReason().contains("lease"));
        QVERIFY(waitFor([&]{return !a.agentHistory().isEmpty() && b.clients().size()==2;},10000));
        // Take, ARM with prefix-only keepalives through the agent, hold.
        a.takeLease();
        QVERIFY2(waitFor([&]{return a.lease()["mine"].toBool() && a.armEnabled();},20000),qPrintable(a.armBlockedReason()));
        QCOMPARE(a.armState(),0);
        a.arm();QVERIFY2(waitFor([&]{return a.armState()==1 && b.armState()==1;}),qPrintable(a.lastError()["message"].toString()));
        QTest::qWait(1500);QCOMPARE(a.armState(),1);
        QVERIFY(b.exclusiveBlocked().contains("bench-gui-a"));
        // Another client takes: A is revoked and the board disarmed.
        if(!cli.isEmpty()) {
            QProcess p;
            p.start(cli,{"--endpoint","tcp://"+address,"--device",device,"--token",token,"--client-name","bench-cli","--lease","info"});
            QVERIFY(p.waitForFinished(30000));QCOMPARE(p.exitCode(),0);
            QVERIFY(waitFor([&]{return a.lease()["revoked"].toBool() && a.armState()==0 && b.armState()==0;},10000));
            QVERIFY(waitFor([&]{return revoked.count()==1;}));QCOMPARE(revoked.first().at(0).toString(),QString("bench-cli"));
            QVERIFY(a.commandGroups().empty());QVERIFY(!a.armEnabled());
            a.acknowledgeRevoked();
            // The CLI's connection ended: nobody holds the lease.
            QVERIFY(waitFor([&]{return a.lease()["chip"].toInt()==int(LeaseChip::Available);},10000));
        } else {
            qInfo("bench: DRIVELINK_GUI_BENCH_CLI not set; B takes the lease from A instead");
        }
        // B takes and arms; A, an observer, disarms the board.
        b.takeLease();
        QVERIFY2(waitFor([&]{return b.lease()["mine"].toBool() && b.armEnabled();},20000),qPrintable(b.armBlockedReason()));
        if(cli.isEmpty()) {QVERIFY(waitFor([&]{return a.lease()["revoked"].toBool() && a.armState()==0;}));a.acknowledgeRevoked();}
        b.arm();QVERIFY2(waitFor([&]{return b.armState()==1 && a.armState()==1;}),qPrintable(b.lastError()["message"].toString()));
        a.disarm();
        QVERIFY(waitFor([&]{return b.armState()==0 && a.armState()==0;}));
        // REBOOT through the agent (B holds the lease, the board is disarmed).
        const auto generation=b.snapshot()->generation;
        b.reboot();
        QVERIFY2(waitFor([&]{return b.ready() && b.snapshot()->generation!=generation && b.rebootNotice()["visible"].toBool();},40000),
                 qPrintable(b.lastError()["message"].toString()));
        qInfo("bench: reboot notice reason '%s' expected %d",qPrintable(b.rebootNotice()["reason"].toString()),b.rebootNotice()["expected"].toBool());
        QVERIFY(b.rebootNotice()["expected"].toBool());b.dismissRebootNotice();
        QVERIFY(waitFor([&]{return a.ready() && a.snapshot()->generation!=generation;},20000));
        QVERIFY(!a.linkLost());QVERIFY(!b.linkLost());QVERIFY(b.lease()["mine"].toBool());
        // The holder's command groups again after the Boot, then ARM and DISARM.
        QVERIFY2(waitFor([&]{return b.armEnabled();},20000),qPrintable(b.armBlockedReason()));
        b.arm();QVERIFY(waitFor([&]{return b.armState()==1;}));
        b.disarm();QVERIFY(waitFor([&]{return b.armState()==0 && a.armState()==0;}));
        b.releaseLease();QVERIFY(waitFor([&]{return a.lease()["chip"].toInt()==int(LeaseChip::Available);},15000));
        a.disconnectDevice();b.disconnectDevice();
        QVERIFY(waitFor([&]{return !a.linked() && !a.closing() && !b.linked() && !b.closing();},15000));
    }
    void exclusiveRule() {
        LeaseTracker t;t.reset(1,{});
        QVERIFY(leaseBlock(false,t,1,false).isEmpty());  // direct connection: never
        QVERIFY(leaseBlock(true,t,0,false).isEmpty());   // nobody holds it, disarmed
        QVERIFY(!leaseBlock(true,t,1,false).isEmpty());  // armed with nobody holding it
        QVERIFY(!leaseBlock(true,t,0,true).isEmpty());   // arming
        t.apply(leaseEvent(2,"ros_bridge",kRos,0,defs::LeaseChange::Taken));
        QVERIFY(leaseBlock(true,t,0,false).contains("ROS"));
        t.apply(leaseEvent(0,"",0,2,defs::LeaseChange::Released));
        t.takeSent();QVERIFY(!leaseBlock(true,t,0,false).isEmpty()); // requesting
        t.apply(leaseEvent(1,"me",kGui,0,defs::LeaseChange::Taken));
        QVERIFY(leaseBlock(true,t,1,false).isEmpty());   // the holder, armed
    }
    void rows() {
        const auto clients=clientRows({{2,"ros_bridge",kRos,true},{5,"me",kGui,false},{7,"",uint8_t(defs::AgentClientKind::Cli),false}},5);
        QCOMPARE(clients.size(),3);
        QCOMPARE(clients[0].toMap()["name"].toString(),QString("me"));QVERIFY(clients[0].toMap()["self"].toBool());
        QCOMPARE(clients[1].toMap()["kindText"].toString(),QString("ROS"));QVERIFY(clients[1].toMap()["holder"].toBool());
        QCOMPARE(clients[2].toMap()["kindText"].toString(),QString("CLI"));
        agent::DeviceRecord d;d.alias="base";d.online=true;d.device_id={0x02,0x44,0x4c,0,0,1};d.name="bench";
        d.endpoint="serial:///dev/ttyACM0";d.clients=2;d.holder_id=2;d.holder_name="ros_bridge";d.holder_kind=kRos;
        agent::DeviceRecord offline;offline.alias="arm";
        const auto devices=agentDeviceRows({d,offline});
        QCOMPARE(devices.size(),2);
        const auto first=devices[0].toMap();
        QCOMPARE(first["id"].toString(),QString("02444c000001"));QCOMPARE(first["holderKindText"].toString(),QString("ROS"));
        QCOMPARE(first["device"].toString(),QString("base"));QVERIFY(first["online"].toBool());
        QVERIFY(devices[1].toMap()["id"].toString().isEmpty());QVERIFY(!devices[1].toMap()["online"].toBool());
        QVERIFY(devices[1].toMap()["holderKindText"].toString().isEmpty());
        // History: newest first; lease, arm and online entries are named.
        std::vector<agent::HistoryEntry> entries{
            {uint8_t(defs::HistoryKind::Online),1000,{1,2,3,4,5,6}},
            {uint8_t(defs::HistoryKind::Lease),2000,agent::encodeLease({2,"ros_bridge",kRos,0,uint8_t(defs::LeaseChange::Taken)})},
            {uint8_t(defs::HistoryKind::Arm),3000,{1,0}},
            {uint8_t(defs::HistoryKind::Lease),4000,agent::encodeLease({0,"",0,2,uint8_t(defs::LeaseChange::Disconnected)})},
            {uint8_t(defs::HistoryKind::Offline),5000,{}}};
        const auto history=historyRows(entries,nullptr,5);
        QCOMPARE(history.size(),5);
        QCOMPARE(history[0].toMap()["kind"].toString(),QString("offline"));
        // A lease end names the holder from the grant recorded before it.
        QVERIFY(history[1].toMap()["text"].toString().contains("ros_bridge disconnected"));
        QCOMPARE(history[2].toMap()["text"].toString(),QString("Armed"));
        QVERIFY(history[3].toMap()["text"].toString().contains("ros_bridge"));
        QVERIFY(history[3].toMap()["text"].toString().contains("ROS"));
        QCOMPARE(history[4].toMap()["kind"].toString(),QString("online"));
        QCOMPARE(history[4].toMap()["time"].toDateTime().toMSecsSinceEpoch(),qint64(1000));
    }
    void failures() {
        QCOMPARE(agentFailure(Error(ErrorKind::Connection,"refused"))["state"].toString(),QString("unreachable"));
        QCOMPARE(agentFailure(Error(ErrorKind::Timeout,"slow"))["state"].toString(),QString("unreachable"));
        QCOMPARE(agentFailure(refusal(defs::Result::InvalidArgument,defs::Detail::BadToken))["state"].toString(),QString("token"));
        QCOMPARE(agentFailure(refusal(defs::Result::NotFound,defs::Detail::UnknownDevice))["state"].toString(),QString("unknownDevice"));
        QCOMPARE(agentFailure(refusal(defs::Result::Busy,defs::Detail::DeviceOffline))["state"].toString(),QString("offline"));
        QVERIFY(agentFailure(refusal(defs::Result::Busy,defs::Detail::DeviceOffline))["text"].toString().contains("offline"));
        QCOMPARE(agentFailure(refusal(defs::Result::NoMemory,defs::Detail::Capacity))["state"].toString(),QString("capacity"));
        auto version=refusal(defs::Result::InvalidArgument,defs::Detail::AgentVersion);version.peer_version=2;
        QCOMPARE(agentFailure(version)["state"].toString(),QString("version"));QCOMPARE(agentFailure(version)["agentVersion"].toInt(),2);
        Error protocol(ErrorKind::Protocol,"incompatible agent protocol version 4");protocol.peer_version=4;
        QCOMPARE(agentFailure(protocol)["state"].toString(),QString("version"));
    }
    void addressesAndRecentAgents() {
        QCOMPARE(agentEndpoint("pi5.local"),QString("tcp://pi5.local:47620"));
        QCOMPARE(agentEndpoint(" 10.0.0.2:7300 "),QString("tcp://10.0.0.2:7300"));
        QCOMPARE(agentEndpoint("tcp://host:1"),QString("tcp://host:1"));
        QCOMPARE(agentEndpoint("[::1]:9"),QString("tcp://[::1]:9"));
        for(const auto *bad:{"","host:x","host:0","host:70000","a/b","tcp://","a b"}) {QVERIFY2(agentEndpoint(bad).isEmpty(),bad);}
        QTemporaryDir dir;const auto path=dir.filePath("gui.ini");
        RecentAgents recent(path);
        recent.remember("pi5.local","base","02444c000001","bench","secret",false);
        recent.remember("10.0.0.2","arm","","","kept",true);
        QCOMPARE(recent.list().size(),2);
        QCOMPARE(recent.list()[0].toMap()["address"].toString(),QString("10.0.0.2"));
        QCOMPARE(RecentAgents(path).find("10.0.0.2")["token"].toString(),QString("kept"));
        QVERIFY(RecentAgents(path).find("pi5.local")["token"].toString().isEmpty());
        // A token not asked to be remembered never reaches the file.
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));QVERIFY(!file.readAll().contains("secret"));file.close();
        recent.remember("10.0.0.2","arm","","","kept",false);
        QFile again(path);QVERIFY(again.open(QIODevice::ReadOnly));QVERIFY(!again.readAll().contains("kept"));again.close();
        recent.forget("pi5.local");QCOMPARE(RecentAgents(path).list().size(),1);
    }
#ifdef AGENT_PATH
    void observerThenLease() {
        AgentFixture f;Window a("gui-a");
        QVERIFY(a.open(f));
        auto &c=a.c;
        QVERIFY(c.remote());
        QCOMPARE(c.lease()["chip"].toInt(),int(LeaseChip::Available));
        // An observer: no command groups, ARM needs the lease, manual commands too.
        QTest::qWait(300);
        QVERIFY(c.commandGroups().empty());QVERIFY(!c.armEnabled());
        QVERIFY(c.armBlockedReason().contains("lease"));QVERIFY(c.commandBlocked().contains("lease"));
        QVERIFY(c.tuneBlocked().contains("lease"));
        // Subscriptions are open to an observer: loop statistics start without command setup.
        QVERIFY(waitFor([&]{return !c.ownedSubscriptions().empty();}));
        // History arrived before the device frames; the client list names this window.
        QVERIFY(waitFor([&]{return !c.agentHistory().isEmpty();}));
        QVERIFY(waitFor([&]{return !c.clients().isEmpty();}));
        QVERIFY(c.clients()[0].toMap()["self"].toBool());QCOMPARE(c.clients()[0].toMap()["kindText"].toString(),QString("GUI"));
        QCOMPARE(c.agent()["device"].toString(),QString("base"));
        QVERIFY(!c.recentAgents().isEmpty());
        // Nobody holds the lease and the device is disarmed: exclusive operations are open.
        QVERIFY(c.exclusiveBlocked().isEmpty());
        // Take: groups after the grant, the device stays disarmed until ARM.
        c.takeLease();
        QVERIFY(waitFor([&]{return c.lease()["mine"].toBool() && c.armEnabled();},15000));
        QVERIFY(!c.commandGroups().empty());QCOMPARE(c.armState(),0);
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1 && c.commandBlocked().isEmpty();}));
        // Release disarms (the agent) and the groups go.
        c.releaseLease();
        QVERIFY(waitFor([&]{return !c.lease()["mine"].toBool() && c.armState()==0;},15000));
        QVERIFY(c.commandGroups().empty());QVERIFY(!c.armEnabled());
        QCOMPARE(c.lease()["chip"].toInt(),int(LeaseChip::Available));QVERIFY(!c.lease()["revoked"].toBool());
    }
    void revocationAndObserverDisarm() {
        AgentFixture f;Window a("gui-a"),b("gui-b");
        QVERIFY(a.open(f));QVERIFY(b.open(f));
        QSignalSpy revoked(&a.c,&Connection::leaseRevoked);
        a.c.takeLease();QVERIFY(waitFor([&]{return a.c.armEnabled();},15000));
        QVERIFY(waitFor([&]{return b.c.lease()["chip"].toInt()==int(LeaseChip::Other);}));
        QVERIFY(waitFor([&]{return b.c.clients().size()==2;},5000));
        // B is an observer: exclusive operations are refused with the holder's name.
        QVERIFY(b.c.exclusiveBlocked().contains("gui-a"));QVERIFY(!b.c.actionBlocked()["save"].toString().isEmpty());
        a.c.arm();QVERIFY(waitFor([&]{return a.c.armState()==1;}));
        // B takes: A is revoked, the device disarmed, A's groups gone; B never arms by itself.
        b.c.takeLease();
        QVERIFY(waitFor([&]{return b.c.lease()["mine"].toBool() && b.c.armEnabled();},15000));
        QVERIFY(waitFor([&]{return a.c.lease()["revoked"].toBool() && a.c.armState()==0;}));
        QVERIFY(waitFor([&]{return revoked.count()==1;}));QCOMPARE(revoked.first().at(0).toString(),QString("gui-b"));
        QCOMPARE(a.c.lease()["chip"].toInt(),int(LeaseChip::Revoked));QCOMPARE(a.c.lease()["revokedBy"].toString(),QString("gui-b"));
        QVERIFY(a.c.commandGroups().empty());QVERIFY(!a.c.armEnabled());QCOMPARE(b.c.armState(),0);
        a.c.acknowledgeRevoked();QCOMPARE(a.c.lease()["chip"].toInt(),int(LeaseChip::Other));
        // DISARM from the observer reaches the device.
        b.c.arm();QVERIFY(waitFor([&]{return b.c.armState()==1 && a.c.armState()==1;}));
        a.c.disarm();
        QVERIFY(waitFor([&]{return b.c.armState()==0 && a.c.armState()==0;}));
        // Released, disarmed and nobody holding it: A may save again.
        b.c.releaseLease();
        QVERIFY(waitFor([&]{return a.c.lease()["chip"].toInt()==int(LeaseChip::Available) && a.c.exclusiveBlocked().isEmpty();},15000));
    }
    void liveEditConfirmation() {
        AgentFixture f;Window a("gui-a"),b("gui-b");
        QVERIFY(a.open(f));QVERIFY(b.open(f));
        b.c.takeLease();QVERIFY(waitFor([&]{return a.c.lease()["chip"].toInt()==int(LeaseChip::Other);},15000));
        const Ref rate{5,0,1};const auto before=a.param(rate);
        a.c.selectNode("5:0");
        // Held: nothing is sent until the operator applies it.
        a.c.setParamText("5:0:1","20");
        QVERIFY(!a.c.liveEdit().isEmpty());QVERIFY(a.c.liveEdit()["holder"].toString().contains("gui-b"));
        QTest::qWait(300);QCOMPARE(a.param(rate),before);
        a.c.cancelLiveEdit();QVERIFY(a.c.liveEdit().isEmpty());QTest::qWait(300);QCOMPARE(a.param(rate),before);
        a.c.setParamText("5:0:1","20");QVERIFY(!a.c.liveEdit().isEmpty());
        a.c.applyLiveEdit(false);QVERIFY(a.c.liveEdit().isEmpty());
        QVERIFY(waitFor([&]{return a.param(rate)==20;}));
        // The same param right after: the operator just confirmed it.
        a.c.setParamText("5:0:1","21");QVERIFY(a.c.liveEdit().isEmpty());QVERIFY(waitFor([&]{return a.param(rate)==21;}));
        // Later, asked again; Don't ask again lasts this connection only.
        QTest::qWait(1600);
        a.c.setParamText("5:0:1","22");QVERIFY(!a.c.liveEdit().isEmpty());a.c.applyLiveEdit(true);
        QVERIFY(waitFor([&]{return a.param(rate)==22;}));
        QTest::qWait(1600);
        a.c.setParamText("5:0:1","23");QVERIFY(a.c.liveEdit().isEmpty());QVERIFY(waitFor([&]{return a.param(rate)==23;}));
        // The holder itself is never asked.
        b.c.selectNode("5:0");b.c.setParamText("5:0:1","24");QVERIFY(b.c.liveEdit().isEmpty());
        QVERIFY(waitFor([&]{return b.param(rate)==24;}));
        // A new connection asks again.
        a.c.disconnectDevice();QVERIFY(waitFor([&]{return !a.c.linked() && !a.c.closing();}));
        QVERIFY(a.open(f));a.c.selectNode("5:0");
        QVERIFY(waitFor([&]{return a.c.lease()["chip"].toInt()==int(LeaseChip::Other);}));
        a.c.setParamText("5:0:1","25");QVERIFY(!a.c.liveEdit().isEmpty());a.c.cancelLiveEdit();
    }
    void refusalsAndListing() {
        AgentFixture f;Window a("gui-a");
        a.c.listAgent(f.address,f.token);
        QVERIFY(waitFor([&]{return a.c.agentListing()["state"].toString()=="ok";}));
        const auto devices=a.c.agentListing()["devices"].toList();
        QCOMPARE(devices.size(),1);QCOMPARE(devices[0].toMap()["alias"].toString(),QString("base"));
        QVERIFY(devices[0].toMap()["online"].toBool());
        a.c.listAgent(f.address,"wrong-token");
        QVERIFY(waitFor([&]{return a.c.agentListing()["state"].toString()=="failed";}));
        QCOMPARE(a.c.agentListing()["failure"].toMap()["state"].toString(),QString("token"));
        a.c.connectAgent(f.address,"wrong-token","base",false);
        QVERIFY(waitFor([&]{return a.c.agentFailure()["state"].toString()=="token" && !a.c.linked() && !a.c.loading();}));
        QVERIFY(!a.c.remote());
        a.c.connectAgent(f.address,f.token,"nope",false);
        QVERIFY(waitFor([&]{return a.c.agentFailure()["state"].toString()=="unknownDevice" && !a.c.loading();}));
        // Nothing listens on the vdev's old port once the agent holds it? Use a closed port instead.
        a.c.connectAgent("127.0.0.1:1",f.token,"base",false);
        QVERIFY(waitFor([&]{return a.c.agentFailure()["state"].toString()=="unreachable" && !a.c.loading();}));
    }
    void deviceLoss() {
        AgentFixture f;Window a("gui-a");
        QVERIFY(a.open(f));
        f.vdev.process.kill();f.vdev.process.waitForFinished(10000);
        QVERIFY(waitFor([&]{return a.c.linkLost();},15000));
        QVERIFY(a.c.remote()); // Retry keeps the agent
        QVERIFY(a.c.agent()["closing"].toString().contains("lost"));
        a.c.retry();
        QVERIFY(waitFor([&]{return !a.c.loading() && a.c.agentFailure()["state"].toString()=="offline";},15000));
        a.c.disconnectDevice();QVERIFY(waitFor([&]{return !a.c.remote();}));
    }
#endif
};

QTEST_MAIN(RemoteTests)
#include "test_remote.moc"
