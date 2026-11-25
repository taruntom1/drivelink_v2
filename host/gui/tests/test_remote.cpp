// Remote mode (H3b-1): the pure rules in remote_model (both OSes) and a
// LocalConnection through drivelink-agent and vdev (Linux, where the agent
// builds; skipped elsewhere): observer first, lease handoff and revocation,
// DISARM from an observer, the exclusive-operation rule, live-edit
// confirmation, history, client list, refusals and device loss; and the
// Agent tab's "Found on the network" (H3b-2) against a fake backend.
#include "app_settings.hpp"
#include "safety_keys.hpp"
#include "test_support.hpp"
#include "remote_model.hpp"
#include "style.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/link.hpp>
#include <QElapsedTimer>
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

// The backend surface AgentTab uses (H3b-2): the listing, the recent agents and
// the browse state, with every call recorded.
class AgentBackend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap agentListing MEMBER agentListing NOTIFY changed)
    Q_PROPERTY(QVariantMap agentFailure MEMBER agentFailure NOTIFY changed)
    Q_PROPERTY(QVariantMap agentBrowse MEMBER agentBrowse NOTIFY changed)
    Q_PROPERTY(QVariantList recentAgents MEMBER recentAgents NOTIFY changed)
    Q_PROPERTY(bool closing MEMBER closing NOTIFY changed)
    Q_PROPERTY(bool linked MEMBER linked NOTIFY changed)
    Q_PROPERTY(bool loading MEMBER loading NOTIFY changed)
public:
    QVariantMap agentListing,agentFailure,agentBrowse;
    QVariantList recentAgents;
    bool closing=false,linked=false,loading=false;
    int starts=0,stops=0,rounds=0;
    QList<QStringList> listed;
    static QVariantMap agent(const QString &instance,const QString &address,const QString &version,int proto) {
        return {{"instance",instance},{"host",instance+".local"},{"address",address},{"version",version},{"proto",proto},{"compatible",proto==3}};
    }
    void browse(const QString &state,const QVariantList &agents) {
        agentBrowse={{"state",state},{"protocol",3},{"agents",agents}};emit changed();
    }
    Q_INVOKABLE void startAgentBrowse() {++starts;}
    Q_INVOKABLE void stopAgentBrowse() {++stops;}
    Q_INVOKABLE void browseAgentsNow() {++rounds;}
    Q_INVOKABLE void listAgent(const QString &address,const QString &token) {listed.append({address,token});}
    Q_INVOKABLE void connectAgent(const QString &,const QString &,const QString &,bool) {}
    Q_INVOKABLE void refreshAgentList() {}
    Q_INVOKABLE void disconnectDevice() {}
    Q_INVOKABLE void forgetAgent(const QString &) {}
signals:
    void changed();
};

using namespace drivelink;
using namespace drivelink::gui;
using drivelink::gui::test::waitFor;
using drivelink::gui::test::visualChild;
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
    explicit AgentFixture(const QStringList &extra={},bool pty=false,int linkTrialMs=0)
        : vdev(false,pty,false,linkTrialMs) {
        agent.start(QString::fromUtf8(AGENT_PATH),QStringList{"--port","0","--token",token,"--device","base="+vdev.endpoint}+extra);
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
    // A system param, running or pending (a device setting).
    uint32_t system(uint16_t id,bool pending=false) {
        const auto &map=pending?c.snapshot()->pending_system_params:c.snapshot()->system_params;
        return valueBits(map.at(id));
    }
    QVariant role(const QString &key,int role) {
        auto *model=c.systemParams();
        for(int i=0;i<model->rowCount();++i) {if(model->index(i,0).data(ParamTable::Key).toString()==key) {return model->index(i,0).data(role);}}
        return {};
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
    // "Found on the network" (H3b-2) on the Agent tab, against a fake backend:
    // the rows, the incompatible one, what a click does, Search again and the
    // browse start/stop that follows the tab and the Settings switch.
    void foundAgentsTab() {
        initShellResources();
        QTemporaryDir dir;QVERIFY(dir.isValid());
        AppSettings settings(dir.filePath("gui.ini"));
        AgentBackend backend;
        backend.recentAgents={QVariantMap{{"address","192.168.1.20:47620"},{"token","remembered"},{"rememberToken",false},{"lastSeen",QDateTime::currentDateTime()}}};
        backend.browse("idle",{AgentBackend::agent("robot-pi","192.168.1.20:47620","0.1.0",3),
                               AgentBackend::agent("bench-wsl","192.168.1.42:47620","",3),
                               AgentBackend::agent("old-agent","192.168.1.50:47620","0.0.9",2)});
        QQmlApplicationEngine engine;QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("agentBackend",&backend);
        engine.rootContext()->setContextProperty("agentSettings",&settings);
        engine.loadData(R"(import QtQuick
import DriveLink.Shell
Window { width: 800; height: 1200; visible: true
    AgentTab { objectName: "tab"; width: 760; backend: agentBackend; settings: agentSettings } })");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());QVERIFY(window);
        window->requestActivate();
        const auto item=[&](const QString &name) {return visualChild(window->contentItem(),name);};
        const auto click=[&](const QString &name) {
            auto *i=item(name);if(!i || !i->isVisible()) {return false;}
            QTest::qWait(50); // positioners lay out on polish
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,i->mapToScene({i->width()/2,i->height()/2}).toPoint());
            return true;
        };
        const auto texts=[&](const QString &name) {
            QStringList all;
            std::function<void(QQuickItem *)> walk=[&](QQuickItem *i) {
                if(i->metaObject()->indexOfProperty("text")>=0 && i->isVisible()) {all<<i->property("text").toString();}
                for(auto *child:i->childItems()) {walk(child);}
            };
            if(auto *i=item(name)) {walk(i);}
            return all;
        };
        auto *tab=item("tab");QVERIFY(tab);
        // Started when shown.
        QVERIFY(waitFor([&]{return backend.starts==1;}));QCOMPARE(backend.stops,0);
        QVERIFY(item("foundAgents") && item("foundAgents")->isVisible());
        QVERIFY(!item("foundAgentsOff")->isVisible());QVERIFY(!item("foundAgentsEmpty")->isVisible());
        QVERIFY(!item("foundAgentsSearching")->isVisible());
        // One row per agent: instance, address and version.
        QVERIFY(waitFor([&]{return item("foundAgent_robot-pi") && item("foundAgent_bench-wsl") && item("foundAgent_old-agent");}));
        QCOMPARE(item("foundAgent_robot-pi")->height(),56.0);
        QVERIFY(texts("foundAgent_robot-pi").contains("robot-pi"));
        QVERIFY(texts("foundAgent_robot-pi").contains("192.168.1.20:47620  ·  DriveLink 0.1.0"));
        QVERIFY(texts("foundAgent_bench-wsl").contains("192.168.1.42:47620")); // no version: no suffix
        // The incompatible row: badge, reason, and not clickable.
        QVERIFY(item("foundIncompatible_old-agent")->isVisible());
        QVERIFY(!item("foundIncompatible_robot-pi")->isVisible());QVERIFY(!item("foundIncompatible_bench-wsl")->isVisible());
        QCOMPARE(item("foundReason_old-agent")->property("reason").toString(),QString("Agent protocol 2; this DriveLink speaks 3"));
        QVERIFY(!item("foundReason_robot-pi")->isVisible());
        QVERIFY(!item("useFoundAgent_old-agent")->property("enabled").toBool());QVERIFY(item("useFoundAgent_robot-pi")->property("enabled").toBool());
        auto *address=item("agentAddress");auto *token=item("agentToken");
        QVERIFY(address && token);
        // The tab filled the address from the recent agent when it opened; clear it.
        address->setProperty("text","");token->setProperty("text","");
        QVERIFY(click("useFoundAgent_old-agent"));
        QCOMPARE(address->property("text").toString(),QString());QVERIFY(backend.listed.isEmpty());
        // A row whose address has a remembered token: fills both and lists.
        QVERIFY(click("useFoundAgent_robot-pi"));
        QCOMPARE(address->property("text").toString(),QString("192.168.1.20:47620"));
        QCOMPARE(token->property("text").toString(),QString("remembered"));
        QCOMPARE(tab->property("rememberToken").toBool(),false);
        QCOMPARE(backend.listed.size(),1);
        QCOMPARE(backend.listed[0],(QStringList{"192.168.1.20:47620","remembered"}));
        // Another address with no remembered token: the old token is not carried over, the token field gets focus.
        QVERIFY(click("useFoundAgent_bench-wsl"));
        QCOMPARE(address->property("text").toString(),QString("192.168.1.42:47620"));
        QCOMPARE(token->property("text").toString(),QString());
        QCOMPARE(backend.listed.size(),1);
        QVERIFY(waitFor([&]{return token->property("focus").toBool();}));
        // Once a token is typed for it, picking the same row again keeps it and lists.
        token->setProperty("text","typed");
        QVERIFY(click("useFoundAgent_bench-wsl"));
        QCOMPARE(token->property("text").toString(),QString("typed"));
        QCOMPARE(backend.listed.size(),2);QCOMPARE(backend.listed[1],(QStringList{"192.168.1.42:47620","typed"}));
        // No row can be used while a listing runs.
        backend.agentListing={{"state","listing"}};emit backend.changed();
        QVERIFY(waitFor([&]{return !item("useFoundAgent_robot-pi")->property("enabled").toBool();}));
        backend.agentListing={};emit backend.changed();
        QVERIFY(waitFor([&]{return item("useFoundAgent_robot-pi")->property("enabled").toBool();}));
        // Search again.
        QVERIFY(click("foundAgentsSearch"));QCOMPARE(backend.rounds,1);
        // A round shows "Searching..." beside the title (and in an empty list).
        backend.browse("searching",backend.agentBrowse["agents"].toList());
        QVERIFY(waitFor([&]{return item("foundAgentsSearching")->isVisible();}));QVERIFY(!item("foundAgentsEmpty")->isVisible());
        backend.browse("searching",{});
        QVERIFY(waitFor([&]{return item("foundAgentsEmpty")->isVisible();}));
        QCOMPARE(item("foundAgentsEmpty")->property("text").toString(),QString("Searching…"));
        backend.browse("idle",{});
        QVERIFY(waitFor([&]{return item("foundAgentsEmpty")->property("text").toString().startsWith("No agents found");}));
        QVERIFY(!item("foundAgentsSearching")->isVisible());QVERIFY(!item("foundAgent_robot-pi"));
        // Hidden: stop; shown again: start.
        tab->setVisible(false);QVERIFY(waitFor([&]{return backend.stops==1;}));QCOMPARE(backend.starts,1);
        tab->setVisible(true);QVERIFY(waitFor([&]{return backend.starts==2;}));
        // Discovery off in Settings: stop, the one-line notice, no list; on again: start.
        settings.setDiscoverAgents(false);
        QVERIFY(waitFor([&]{return backend.stops==2;}));
        QVERIFY(item("foundAgentsOff")->isVisible());QVERIFY(!item("foundAgents")->isVisible());
        QCOMPARE(item("foundAgentsOff")->property("text").toString(),QString("Finding agents on the network is off (Settings)."));
        tab->setVisible(false);tab->setVisible(true);QTest::qWait(50);QCOMPARE(backend.starts,2);
        settings.setDiscoverAgents(true);
        QVERIFY(waitFor([&]{return backend.starts==3;}));QVERIFY(!item("foundAgentsOff")->isVisible());
    }
    void foundAgentsOffNeverStarts() {
        initShellResources();
        QTemporaryDir dir;QVERIFY(dir.isValid());
        AppSettings settings(dir.filePath("gui.ini"));settings.setDiscoverAgents(false);
        AgentBackend backend;
        QQmlApplicationEngine engine;QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("agentBackend",&backend);
        engine.rootContext()->setContextProperty("agentSettings",&settings);
        engine.loadData(R"(import QtQuick
import DriveLink.Shell
Window { width: 800; height: 1200; visible: true
    Loader { objectName: "loader"; width: 760
        sourceComponent: AgentTab { objectName: "tab"; width: 760; backend: agentBackend; settings: agentSettings } } })");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());QVERIFY(window);
        QTest::qWait(100);
        QVERIFY(visualChild(window->contentItem(),"foundAgentsOff")->isVisible());
        QCOMPARE(backend.starts,0);QCOMPARE(backend.stops,0);
        // On, then the tab is destroyed: browsing stops.
        settings.setDiscoverAgents(true);
        QVERIFY(waitFor([&]{return backend.starts==1;}));
        auto *loader=visualChild(window->contentItem(),"loader");QVERIFY(loader);
        loader->setProperty("active",false);
        QVERIFY(waitFor([&]{return backend.stops==1;}));
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
    // The PC hardware check (H3b-2): an agent advertising on the LAN (in WSL
    // with mirrored networking, or on another host) is found by this GUI's
    // browse; its address lists the devices and a connection through it
    // observes the device. Nothing is written to the device.
    void benchDiscovery() {
        const auto instance=qEnvironmentVariable("DRIVELINK_GUI_BENCH_DISCOVER");
        if(instance.isEmpty()) {QSKIP("set DRIVELINK_GUI_BENCH_DISCOVER (the agent's instance name) and _TOKEN to find an agent");}
        const auto token=qEnvironmentVariable("DRIVELINK_GUI_BENCH_TOKEN");
        const auto device=qEnvironmentVariable("DRIVELINK_GUI_BENCH_DEVICE","bench");
        QVERIFY2(!token.isEmpty(),"DRIVELINK_GUI_BENCH_TOKEN is empty");
        QTemporaryDir dir;LocalConnection c;
        c.setSettingsPath(dir.filePath("bench.ini"));c.setClientName("bench-gui-discovery");
        QElapsedTimer clock;clock.start();
        c.startAgentBrowse();
        QVariantMap row;
        const bool found=waitFor([&]{
            for(const auto &r : c.agentBrowse()["agents"].toList()) {
                if(r.toMap()["instance"].toString()==instance) {row=r.toMap();return true;}
            }
            return false;
        },30000);
        QStringList seen;
        for(const auto &r : c.agentBrowse()["agents"].toList()) {seen<<r.toMap()["instance"].toString()+"@"+r.toMap()["address"].toString();}
        QVERIFY2(found,qPrintable("not found; seen: "+seen.join(", ")));
        qInfo("bench: found %s at %s (DriveLink %s, protocol %d) after %lld ms; all: %s",qPrintable(instance),
              qPrintable(row["address"].toString()),qPrintable(row["version"].toString()),row["proto"].toInt(),
              static_cast<long long>(clock.elapsed()),qPrintable(seen.join(", ")));
        QVERIFY(row["compatible"].toBool());
        c.listAgent(row["address"].toString(),token);
        QVERIFY2(waitFor([&]{return c.agentListing()["state"].toString()!="listing";},10000),"listing timed out");
        QVERIFY2(c.agentListing()["state"].toString()=="ok",qPrintable(c.agentListing()["failure"].toMap()["text"].toString()));
        c.stopAgentBrowse();
        c.connectAgent(row["address"].toString(),token,device,false);
        QVERIFY2(waitFor([&]{return c.ready() && c.snapshot() && c.snapshot()->status.has_value();},30000),
                 qPrintable(c.lastError()["message"].toString()+" "+c.agentFailure()["text"].toString()));
        QVERIFY(c.remote());QVERIFY(c.commandGroups().empty());
        qInfo("bench: connected to %s device %s through the found address",qPrintable(device),qPrintable(c.deviceId()));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.linked() && !c.closing();}));
    }
    void foundAgentsAndBrowseSchedule() {
        discovery::FoundAgent pi{"robot-pi","drivelink-robot-pi.local.",47620,{"192.168.1.20"},"192.168.1.20",3u,"0.1.0"};
        discovery::FoundAgent old{"old-agent","drivelink-old.local.",47620,{"192.168.1.50"},"192.168.1.50",2u,"0.0.9"};
        discovery::FoundAgent bare{"bare","drivelink-bare.local.",47621,{"192.168.1.60"},"",std::nullopt,""};
        FoundAgents found;
        found.round({pi,old,bare});
        auto rows=found.rows(3);
        QCOMPARE(rows.size(),3);
        QCOMPARE(rows[0].toMap()["instance"].toString(),QString("bare")); // sorted
        QCOMPARE(rows[0].toMap()["proto"].toInt(),-1);
        QVERIFY(!rows[0].toMap()["compatible"].toBool());
        QCOMPARE(rows[1].toMap()["version"].toString(),QString("0.0.9"));
        QVERIFY(!rows[1].toMap()["compatible"].toBool());
        QCOMPARE(rows[2].toMap()["address"].toString(),QString("192.168.1.20:47620"));
        QCOMPARE(rows[2].toMap()["host"].toString(),QString("drivelink-robot-pi.local."));
        QVERIFY(rows[2].toMap()["compatible"].toBool());
        // Missed once: kept; twice in a row: dropped. A new address replaces the old.
        pi.addresses={"10.0.0.5"};pi.from="10.0.0.5";
        found.round({pi});QCOMPARE(found.rows(3).size(),3);
        QCOMPARE(found.rows(3)[2].toMap()["address"].toString(),QString("10.0.0.5:47620"));
        found.round({pi,bare});
        rows=found.rows(3);QCOMPARE(rows.size(),2);
        QCOMPARE(rows[0].toMap()["instance"].toString(),QString("bare"));
        found.round({});found.round({});QVERIFY(found.rows(3).isEmpty());
        found.round({pi});found.clear();QVERIFY(found.rows(3).isEmpty());
        // Rounds at 0, 1, 3, 7, 15, 31 s, then every 60 s.
        QCOMPARE(browseDelay(1).count(),1000);QCOMPARE(browseDelay(2).count(),2000);
        QCOMPARE(browseDelay(5).count(),16000);QCOMPARE(browseDelay(6).count(),60000);
        QCOMPARE(browseDelay(100).count(),60000);
        // The screenshot hook.
        const auto fake=parseFakeAgents(R"([{"instance":"robot-pi","host":"h.local.","address":"192.168.1.20:47620","version":"0.1.0","proto":3},{"instance":"x","address":"10.0.0.1:1"}])");
        QVERIFY(fake.has_value());QCOMPARE(fake->size(),size_t(2));
        QCOMPARE(QString::fromStdString((*fake)[0].address()),QString("192.168.1.20:47620"));
        QVERIFY(!(*fake)[1].proto.has_value());
        QVERIFY(parseFakeAgents("[]").has_value());
        for(const auto *bad:{"{}","nope",R"([{"instance":"a","address":"1.2.3.4"}])",R"([{"instance":"a","address":"1.2.3.4:0"}])",R"([{"address":"1.2.3.4:5"}])"}) {
            QVERIFY2(!parseFakeAgents(bad).has_value(),bad);
        }
    }
    void browseStatesWithFakeAgents() {
        // The backend's states with the hook: off, searching, idle; stop forgets.
        qputenv("DRIVELINK_GUI_FAKE_AGENTS",R"([{"instance":"robot-pi","address":"192.168.1.20:47620","version":"0.1.0","proto":3}])");
        QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));
        qunsetenv("DRIVELINK_GUI_FAKE_AGENTS");
        QCOMPARE(c.agentBrowse()["state"].toString(),QString("off"));
        QCOMPARE(c.agentBrowse()["protocol"].toInt(),3);
        c.startAgentBrowse();
        QCOMPARE(c.agentBrowse()["state"].toString(),QString("searching"));
        QVERIFY(waitFor([&]{return c.agentBrowse()["state"].toString()=="idle";},3000));
        auto agents=c.agentBrowse()["agents"].toList();
        QCOMPARE(agents.size(),1);QVERIFY(agents[0].toMap()["compatible"].toBool());
        c.browseAgentsNow();QCOMPARE(c.agentBrowse()["state"].toString(),QString("searching"));
        c.browseAgentsNow(); // while searching: one more round after this one
        QVERIFY(waitFor([&]{return c.agentBrowse()["state"].toString()=="idle";},3000));
        c.stopAgentBrowse();
        QCOMPARE(c.agentBrowse()["state"].toString(),QString("off"));
        QVERIFY(c.agentBrowse()["agents"].toList().isEmpty());
        QTest::qWait(500); // a dropped round lands nowhere
        QCOMPARE(c.agentBrowse()["state"].toString(),QString("off"));
    }
#ifdef AGENT_PATH
    void discoveryFindsLanAgent() {
        // H3b-2: an agent on the LAN address is found by the backend's browse
        // and its address lists the devices.
        const auto interfaces=discovery::listIpv4Interfaces();
        if(interfaces.empty()) {QSKIP("no non-loopback IPv4 interface");}
        const auto ip=QString::fromStdString(discovery::formatIpv4(interfaces.front().address));
        const auto name=QStringLiteral("gui-itest-%1").arg(QCoreApplication::applicationPid());
        AgentFixture f({"--bind",ip,"--name",name});
        QVERIFY(f.address.startsWith(ip+":"));
        Window a("gui-a");
        a.c.startAgentBrowse();
        QVariantMap row;
        QVERIFY(waitFor([&]{
            for(const auto &r : a.c.agentBrowse()["agents"].toList()) {
                if(r.toMap()["instance"].toString()==name) {row=r.toMap();return true;}
            }
            return false;
        },20000));
        QCOMPARE(row["address"].toString(),f.address);
        QVERIFY(row["compatible"].toBool());
        QCOMPARE(row["proto"].toInt(),3);
        a.c.listAgent(row["address"].toString(),f.token);
        QVERIFY(waitFor([&]{return a.c.agentListing()["state"].toString()=="ok";}));
        a.c.stopAgentBrowse();
        QCOMPARE(a.c.agentBrowse()["state"].toString(),QString("off"));
    }
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
    void disconnectFromAgent() {
        initShellResources();
        AgentFixture f;Window driver("gui-driver"),viewer("gui-viewer");
        QVERIFY(driver.open(f));QVERIFY(viewer.open(f));
        AppSettings settings(viewer.dir.filePath("app.ini"));
        SafetyKeys keys;
        QQmlApplicationEngine engine;QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("connection",&viewer.c);
        engine.rootContext()->setContextProperty("safetyKeys",&keys);
        engine.rootContext()->setContextProperty("appSettings",&settings);
        engine.loadFromModule("DriveLink.Shell","Shell");QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto click=[&](QQuickItem *item) {
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene({item->width()/2,item->height()/2}).toPoint());
        };
        auto *action=visualChild(window->contentItem(),"navDisconnect");QVERIFY(action);
        auto *status=visualChild(window->contentItem(),"statusBar");QVERIFY(status);
        QCOMPARE(status->property("connection").toString(),QString("%1 · base").arg(f.address));
        auto *dialog=window->findChild<QObject *>("disconnectDialog");QVERIFY(dialog);

        driver.c.takeLease();QVERIFY(waitFor([&]{return driver.c.armEnabled();},15000));
        driver.c.arm();QVERIFY(waitFor([&]{return viewer.c.armState()==1 && viewer.c.lease()["holderName"]=="gui-driver";}));
        QVERIFY(!viewer.c.unsaved() && !viewer.c.controllingMotion());
        QVERIFY(action->isEnabled() && action->isVisible());click(action);
        QVERIFY(waitFor([&]{return !viewer.c.linked() && !viewer.c.closing();}));
        QVERIFY(!dialog->property("opened").toBool());
        QCOMPARE(driver.c.armState(),1);QVERIFY(driver.c.lease()["mine"].toBool());

        QVERIFY(viewer.open(f));
        viewer.c.takeLease();QVERIFY(waitFor([&]{return viewer.c.armEnabled();},15000));
        viewer.c.arm();QVERIFY(waitFor([&]{return viewer.c.armState()==1 && driver.c.armState()==1;}));
        QVERIFY(viewer.c.controllingMotion());
        click(action);QVERIFY(waitFor([&]{return dialog->property("opened").toBool();}));
        QVERIFY(dialog->property("text").toString().contains("agent disarms the device"));
        const auto popup=dialog->property("contentItem").value<QQuickItem *>();
        auto *confirm=visualChild(popup,"acceptButton");QVERIFY(confirm);
        click(confirm);
        QVERIFY(waitFor([&]{return !viewer.c.linked() && !viewer.c.closing();}));
        QVERIFY(waitFor([&]{return driver.c.armState()==0 && driver.c.lease()["holderId"].toInt()==0;}));
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
    // Device settings through the agent (issue 105 step 4): with nobody
    // holding the lease both kinds go through; an observer's reboot rows are
    // blocked with the lease reason and send nothing, and its live rows wait
    // for the operator's confirmation, then go.
    void deviceSettingsThroughAgent() {
        AgentFixture f;Window a("gui-a"),b("gui-b");
        QVERIFY(a.open(f));QVERIFY(b.open(f));
        QCOMPARE(a.c.systemParams()->rowCount(),16);QVERIFY(a.c.deviceSettings()["blocked"].toString().isEmpty());
        // No lease held: a live edit and a staged limit are sent at once.
        a.c.setParamText("0:0:3","20");
        QVERIFY(waitFor([&]{return a.role("0:0:3",ParamTable::EditState)=="applied" && a.system(defs::system::kStatusRateHz)==20u;}));
        QVERIFY(a.c.liveEdit().isEmpty());QVERIFY(a.role("0:0:3",ParamTable::Unsaved).toBool());
        a.c.setParamText("0:0:16","6");
        QVERIFY(waitFor([&]{return a.role("0:0:16",ParamTable::IsPending).toBool();}));
        QCOMPARE(a.system(defs::system::kLimitsLoops,true),6u);QCOMPARE(a.system(defs::system::kLimitsLoops),4u);
        QCOMPARE(a.c.layout()["counts"].toMap()["loopLimit"].toInt(),6);
        // The holder (b) sees the same pending value.
        QVERIFY(waitFor([&]{return b.c.pendingReboot();}));
        QVERIFY(waitFor([&]{return b.role("0:0:16",ParamTable::IsPending).toBool();}));
        a.c.discardPending();QVERIFY(waitFor([&]{return !a.c.pendingReboot() && !a.role("0:0:16",ParamTable::IsPending).toBool();}));
        // b takes the lease: a observes. Reboot rows are blocked, nothing is sent.
        b.c.takeLease();QVERIFY(waitFor([&]{return a.c.lease()["chip"].toInt()==int(LeaseChip::Other);},15000));
        const auto reason=a.c.deviceSettings()["blocked"].toString();
        QVERIFY2(reason.startsWith("Needs the control lease:") && reason.contains("gui-b"),qPrintable(reason));
        QCOMPARE(a.c.deviceSettings()["rebootNote"].toString(),QString("Needs the control lease"));
        QVERIFY(a.c.deviceSettings()["paramBlocked"].toString().isEmpty());
        a.c.setParamText("0:0:17","30");
        QCOMPARE(a.role("0:0:17",ParamTable::EditState).toString(),QString("rejected"));
        QCOMPARE(a.role("0:0:17",ParamTable::EditNote).toString(),reason);
        QVERIFY(a.c.liveEdit().isEmpty());
        QTest::qWait(300);QCOMPARE(a.system(defs::system::kLimitsInstances,true),16u);QVERIFY(!b.c.pendingReboot());
        // A live row waits for the confirmation; cancelling sends nothing.
        a.c.setParamText("0:0:3","25");
        QVERIFY(!a.c.liveEdit().isEmpty());QVERIFY(a.c.liveEdit()["holder"].toString().contains("gui-b"));
        QTest::qWait(300);QCOMPARE(a.system(defs::system::kStatusRateHz),20u);
        a.c.cancelLiveEdit();QVERIFY(a.c.liveEdit().isEmpty());QTest::qWait(300);QCOMPARE(a.system(defs::system::kStatusRateHz),20u);
        a.c.setParamText("0:0:3","25");QVERIFY(!a.c.liveEdit().isEmpty());a.c.applyLiveEdit(false);
        QVERIFY(waitFor([&]{return a.system(defs::system::kStatusRateHz)==25u;}));
        QVERIFY(waitFor([&]{return a.role("0:0:3",ParamTable::EditState)=="applied";}));
        // The holder is never asked and stages its limit; the observer sees it pending.
        b.c.setParamText("0:0:17","30");
        QVERIFY(waitFor([&]{return b.role("0:0:17",ParamTable::IsPending).toBool();}));
        QVERIFY(b.c.liveEdit().isEmpty());
        QVERIFY(waitFor([&]{return a.c.pendingReboot();}));
        QVERIFY(waitFor([&]{return a.role("0:0:17",ParamTable::IsPending).toBool();}));
        QCOMPARE(a.system(defs::system::kLimitsInstances,true),30u);
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
    // Step 5b: serial link trial through drivelink-agent on a real PTY:
    // auto-baud confirm, unmoved-wire pin revert, and first-connect active
    // trial where only the lease holder can confirm.
    void linkTrialThroughAgent() {
        AgentFixture f({"--retry-ms","400"},true,6000);
        Window a("gui-a");
        QVERIFY(a.open(f));
        // Baud change through agent: agent reconnects with auto-baud and confirms.
        a.c.setParamValue("0:0:48",*protocol::linkBaudValue(115200));
        QVERIFY(waitFor([&]{return a.c.linkTrial()["staged"].toBool();}));
        a.c.saveAndReboot();
        QVERIFY(waitFor([&]{return a.c.linkTrial()["waiting"].toBool() && a.c.linkTrial()["throughAgent"].toBool();},10000));
        QVERIFY(waitFor([&]{return a.c.ready() && a.c.linkTrial()["outcome"].toString()=="confirmed" && !a.c.linkTrial()["active"].toBool();},25000));
        QCOMPARE(protocol::linkBaudRate(a.system(defs::system::kLinkBaud)),115200u);
        // Pin change without moving PTY wires: reverts after 4 s and GUI reports Reverted.
        a.c.dismissLinkOutcome();
        a.c.setParamText("0:0:49","17");
        a.c.setParamText("0:0:50","18");
        QVERIFY(waitFor([&]{return a.c.linkTrial()["pinsChanged"].toBool();}));
        a.c.saveAndReboot();
        QVERIFY(waitFor([&]{return a.c.ready() && a.c.linkTrial()["outcome"].toString()=="reverted";},30000));
        QCOMPARE(a.system(defs::system::kLinkTxPin),43u);
        QCOMPARE(a.system(defs::system::kLinkRxPin),44u);
        a.c.disconnectDevice();QVERIFY(waitFor([&]{return !a.c.linked() && !a.c.closing();}));
        // Active trial on agent's first connect: agent does not auto-confirm;
        // observer cannot confirm while another client holds the lease, holder can.
        test::Fixture ptyVdev(false,true,false,10000);
        {
            SessionOptions opts;opts.baud=kBaudAuto;
            Session s(ptyVdev.endpoint.toStdString(),opts);Client c(s);c.info();
            const std::array<Ref,1> refs{{{0,0,defs::system::kLinkBaud}}};
            const std::array<uint32_t,1> vals{{*protocol::linkBaudValue(230400)}};
            c.set(refs,vals);c.system(5);c.system(8);
        }
        QTest::qWait(250);
        QProcess agent;
        const QString token=QStringLiteral("gui-trial-token");
        agent.start(QString::fromUtf8(AGENT_PATH),QStringList{"--port","0","--token",token,"--device","base="+ptyVdev.endpoint});
        QVERIFY(agent.waitForStarted(10000) && agent.waitForReadyRead(10000));
        const auto line=QString::fromUtf8(agent.readLine()).trimmed();
        QVERIFY(line.startsWith("READY tcp://"));
        const auto addr=line.mid(12);
        QVERIFY(waitFor([&]{
            try {const auto d=listAgentDevices("tcp://"+addr.toStdString(),token.toStdString());return !d.empty() && d.front().online;}
            catch(const Error &) {return false;}
        },15000));
        Window obs("gui-obs"),holder("gui-holder");
        obs.c.connectAgent(addr,token,"base",false);
        holder.c.connectAgent(addr,token,"base",false);
        QVERIFY(waitFor([&]{return obs.c.ready() && holder.c.ready();},15000));
        QVERIFY(obs.c.linkTrial()["active"].toBool());
        QVERIFY(holder.c.linkTrial()["active"].toBool());
        QVERIFY(!holder.c.armEnabled());
        holder.c.takeLease();
        QVERIFY(waitFor([&]{return holder.c.lease()["mine"].toBool() && obs.c.lease()["holderName"]=="gui-holder";},15000));
        QVERIFY(obs.c.linkTrial()["confirmBlocked"].toString().contains("gui-holder"));
        QVERIFY(holder.c.linkTrial()["confirmBlocked"].toString().isEmpty());
        holder.c.confirmLink();
        QVERIFY(waitFor([&]{return !holder.c.linkTrial()["active"].toBool() && holder.c.linkTrial()["outcome"].toString()=="confirmed";},15000));
        QCOMPARE(protocol::linkBaudRate(holder.system(defs::system::kLinkBaud)),230400u);
        obs.c.disconnectDevice();holder.c.disconnectDevice();
        QVERIFY(waitFor([&]{return !obs.c.linked() && !holder.c.linked();},10000));
        agent.kill();agent.waitForFinished(10000);
    }
#endif
};

QTEST_MAIN(RemoteTests)
#include "test_remote.moc"
