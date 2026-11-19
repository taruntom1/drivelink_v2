#include "app_settings.hpp"
#include "connection.hpp"
#include "register.hpp"
#include "safety_keys.hpp"
#include "style.hpp"
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

// The static DriveLink.Shell module's C++ types (PlotItem).
void qml_register_types_DriveLink_Shell();

#ifdef _WIN32
#include <windows.h>
// A Windows GUI program (the installer's build) has no console. Started from
// one, print there; output already redirected (a pipe or file) is kept.
static void attachParentConsole() {
    const auto valid=[](DWORD id) {const HANDLE h=GetStdHandle(id);return h!=nullptr && h!=INVALID_HANDLE_VALUE;};
    if(valid(STD_OUTPUT_HANDLE) || valid(STD_ERROR_HANDLE) || !AttachConsole(ATTACH_PARENT_PROCESS)) {return;}
    std::FILE *ignored=nullptr;
    freopen_s(&ignored,"CONOUT$","w",stdout);
    freopen_s(&ignored,"CONOUT$","w",stderr);
}
#endif

int main(int argc,char **argv) {
#ifdef _WIN32
    attachParentConsole();
#endif
    Q_INIT_RESOURCE(qmake_DriveLink_Shell);
    Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);
    qml_register_types_DriveLink_Shell();
    drivelink::gui::registerModulePanels();
    bool render=false;
    for(int i=1;i<argc;++i) {if(qstrcmp(argv[i],"--out")==0 || qstrncmp(argv[i],"--out=",6)==0) {render=true;}}
    if(render) {
        if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {qputenv("QT_QPA_PLATFORM","offscreen");}
        if(qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {qputenv("QT_QUICK_BACKEND","software");}
    }
    QGuiApplication app(argc,argv);app.setApplicationName("DriveLink");
    QCommandLineParser parser;parser.addHelpOption();
    parser.addOption({"endpoint","Connect to a serial or TCP endpoint.","url"});
    parser.addOption({"baud","Serial baud rate (default 921600).","rate","921600"});
    // Remote mode (H3b-1): connect through drivelink-agent as an observer.
    parser.addOption({"agent","Connect through drivelink-agent at host[:port] (port 47620 by default).","address"});
    parser.addOption({"token","The agent's token (default: DRIVELINK_AGENT_TOKEN).","token"});
    parser.addOption({"device","The agent's device: an alias or a 12-hex device id.","device"});
    parser.addOption({"client-name","The name other agent clients see (default drivelink-gui@<host>).","name"});
    parser.addOption({"take-lease","Screenshots through an agent: take the control lease once ready (prints GUI LEASE MINE when it is this window's)."});
    parser.addOption({"live-edit","live-edit screen: change this live param (KEY=VALUE, KEY as type:instance:item) and capture the held confirmation.","edit"});
    parser.addOption({"settings","Keep recent devices in this INI file instead of the user's.","file"});
    parser.addOption({"out","Render the shell in both sizes and themes, then disconnect.","directory"});
    parser.addOption({"screen","connect, settings (both also without --endpoint), device, layout, tune, monitor, drawer, dialog, rename, reset, unsaved, notice, update, "
                      "update-writing, update-confirmed, update-rolledback (unsaved, notice and update-* change a tcp:// vdev), "
                      "layout-review, layout-add-instance, layout-add-loop, monitor-paused, monitor-budget, tune-running, tune-disarm, tune-ab or tune-identify (a vdev; --identify does the same on tune). "
                      "Remote screens (through --agent): agent-connect (the Agent tab's listing; --agent/--token optional, no --device), agent-failure (the failed connect), "
                      "lease-take, lease-release (needs --take-lease), live-edit (needs --live-edit; on Tune with --tune, else Layout) and revoked "
                      "(needs --take-lease; prints GUI LEASE MINE, then waits for another client to take the lease); "
                      "the lease screens sit on Device, or on Tune with --tune.","name","device"});
    parser.addOption({"plot","Monitor screens: plot these signals in order (comma-separated item keys, type:instance:item).","keys"});
    parser.addOption({"rate","Monitor screens: plot rates before plotting (comma-separated loop:divider).","rates"});
    parser.addOption({"window","Monitor screens: the plot window in seconds.","seconds","10"});
    parser.addOption({"command","Monitor screens: put these inputs on the Manual command card (comma-separated "
                      "KEY[@LIMIT][=VALUE[:latch]]); with a value the device is armed first and the slider is held there, "
                      "or latched (a vdev only).","inputs"});
    parser.addOption({"tune","Tune screens: the controller (type:instance).","key"});
    parser.addOption({"tune-set","Tune screens: set these live params first (comma-separated KEY=VALUE; an enum by its name).","params"});
    parser.addOption({"signal","Tune screens: test signal fields (comma-separated FIELD=VALUE: target (an item key), waveform "
                      "(0-5), offset, amplitude, f0, f1, duration, repeat).","fields"});
    parser.addOption({"tune-runs","Tune screens: arm, then one step run per value of this gain (KEY=V1:V2:...; a vdev only). "
                      "tune-running starts one more and captures it running; tune-disarm cuts one more with DISARM.","runs"});
    parser.addOption({"identify","Tune screens: arm, show Plant identification, confirm, run the step and wait for the fit (a vdev only; the tune-identify screen implies it)."});
    parser.addOption({"identify-response","Plant identification screens: show the response whose name starts with this text.","text"});
    parser.addOption({"select","Layout screens: select this node (type:instance, e.g. 5:0).","key"});
    parser.addOption({"image","Firmware image for the update screens (vdev fixtures).","file"});
    parser.addOption({"expect-state","Capture when ready, fault, safe or lost.","state","ready"});
    parser.addOption({"settle","Wait this long (ms) after the screen is ready before capturing.","ms","0"});
    parser.addOption({"action","Layout screens: run this action of the selected instance once (an item key, no arguments).","key"});
    parser.addOption({"panel","Layout screens: show the selected instance's custom panel tab (after its first live sample)."});
    parser.process(app);
    const QString screen=parser.value("screen");
    const QString expected=parser.value("expect-state");
    if(!QStringList{"ready","fault","safe","lost"}.contains(expected)) {return 2;}
    // Pages first (their index is the nav index plus one; Settings is page 4,
    // the rail's footer item), then shell states.
    const QStringList pages={"connect","device","layout","tune","monitor","settings"};
    const QStringList states={"drawer","dialog","rename","reset","unsaved","notice","update","update-writing","update-confirmed","update-rolledback",
                              "layout-review","layout-add-instance","layout-add-loop","monitor-paused","monitor-budget",
                              "tune-running","tune-disarm","tune-ab","tune-identify",
                              "agent-connect","agent-failure","lease-take","lease-release","live-edit","revoked"};
    // Remote screens: the live edit renders on Tune with --tune, else on Layout.
    const bool liveEditScreen=screen=="live-edit";
    const bool lobbyScreen=screen=="agent-connect";
    const bool agentScreen=lobbyScreen || screen=="agent-failure";
    const bool remoteScreen=agentScreen || screen=="lease-take" || screen=="lease-release" || liveEditScreen || screen=="revoked";
    const bool layoutScreen=screen.startsWith("layout") || (liveEditScreen && !parser.isSet("tune"));
    const bool monitorScreen=screen.startsWith("monitor");
    // The lease dialogs render over the Device page, or over Tune with --tune.
    const bool leaseDialogScreen=screen=="lease-take" || screen=="lease-release" || screen=="revoked";
    const bool tuneScreen=screen.startsWith("tune") || ((liveEditScreen || leaseDialogScreen) && parser.isSet("tune"));
    const bool update=screen.startsWith("update");
    if(update && !parser.isSet("image")) {std::fprintf(stderr,"--screen %s needs --image\n",qPrintable(screen));return 2;}
    const QString liveEditText=parser.value("live-edit");
    const bool takeLease=parser.isSet("take-lease");
    if(remoteScreen && !lobbyScreen && !parser.isSet("agent")) {std::fprintf(stderr,"--screen %s needs --agent\n",qPrintable(screen));return 2;}
    if((screen=="lease-release" || screen=="revoked") && !takeLease) {std::fprintf(stderr,"--screen %s needs --take-lease\n",qPrintable(screen));return 2;}
    if(liveEditScreen && !liveEditText.contains(QLatin1Char('='))) {std::fprintf(stderr,"--screen live-edit needs --live-edit KEY=VALUE\n");return 2;}
    if(!pages.contains(screen) && !states.contains(screen)) {std::fprintf(stderr,"Unknown screen\n");return 2;}
    bool numeric=false;const int baud=parser.value("baud").toInt(&numeric);
    const int settle=parser.value("settle").toInt();
    if(!numeric || baud<=0 || settle<0) {std::fprintf(stderr,"Invalid --baud or --settle\n");return 2;}
    drivelink::gui::LocalConnection connection;
    if(parser.isSet("settings")) {connection.setSettingsPath(parser.value("settings"));}
    drivelink::gui::AppSettings appSettings(connection.settingsPath());
    // Screenshots render both themes without saving the user's choice.
    appSettings.setPersistent(!render);
    drivelink::gui::SafetyKeys keys;
    QObject::connect(&keys,&drivelink::gui::SafetyKeys::escapeRequested,&connection,&drivelink::gui::Connection::disarm);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("connection",&connection);
    engine.rootContext()->setContextProperty("safetyKeys",&keys);
    engine.rootContext()->setContextProperty("appSettings",&appSettings);
    if(!drivelink::gui::installStyle(engine)) {return 2;}
    engine.loadFromModule("DriveLink.Shell","Shell");
    if(engine.rootObjects().isEmpty()) {return 2;}
    // Theme.dark follows the Settings page's choice (dark, light or the OS).
    auto *theme=engine.singletonInstance<QObject *>("DriveLink.Style","Theme");
    if(!theme) {return 2;}
    auto applyTheme=[&] {theme->setProperty("dark",appSettings.dark());};
    applyTheme();
    QObject::connect(&appSettings,&drivelink::gui::AppSettings::themeChanged,theme,applyTheme);
    auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    if(!window) {return 2;}
    const bool agentLink=parser.isSet("agent");
    const QString agentAddress=parser.value("agent");
    QString agentToken;
    if(agentLink) {
        agentToken=parser.isSet("token")?parser.value("token"):qEnvironmentVariable("DRIVELINK_AGENT_TOKEN");
        // The Agent tab's own screens may show an empty or wrong token (the listing refuses it); every other one needs a device.
        if(parser.isSet("endpoint") || (agentToken.isEmpty() && !agentScreen) || (!parser.isSet("device") && !lobbyScreen)) {
            std::fprintf(stderr,"--agent needs --device and --token (or DRIVELINK_AGENT_TOKEN), and no --endpoint\n");return 2;
        }
        if(parser.isSet("client-name")) {connection.setClientName(parser.value("client-name"));}
        // agent-connect lists the agent's devices from the Agent tab instead of connecting.
        if(!lobbyScreen) {connection.connectAgent(agentAddress,agentToken,parser.value("device"),false);}
    }
    if(parser.isSet("endpoint")) {connection.connectTo(parser.value("endpoint"),baud);}
    if(!render) {return app.exec();}
    const bool standalone=screen=="connect" || screen=="settings" || lobbyScreen;
    const bool linked=parser.isSet("endpoint") || (agentLink && !lobbyScreen);
    if(!standalone && !linked) {
        std::fprintf(stderr,"This screen needs --endpoint or --agent; use host/gui/tests/scenarios.py for isolated vdev fixtures\n");return 2;
    }
    // These screens rename (and reboot) the device: vdev only, never a board.
    if((screen=="unsaved" || screen=="notice" || (update && screen!="update")) && (agentLink || !parser.value("endpoint").startsWith("tcp://"))) {
        std::fprintf(stderr,"--screen %s changes the device; use a vdev tcp:// endpoint\n",qPrintable(screen));return 2;
    }
    QDir directory(parser.value("out"));if(!directory.mkpath(".")) {return 2;}
    QTimer timeout;timeout.setSingleShot(true);timeout.setInterval(30000);
    QObject::connect(&timeout,&QTimer::timeout,&app,[&] {std::fprintf(stderr,"Screenshot timed out: %s (arm: %s; command: %s)\n",qPrintable(connection.lastError().value("message").toString()),
                     qPrintable(connection.armBlockedReason()),qPrintable(connection.commandBlocked()));
        app.exit(2);});
    timeout.start();
    auto capture=[&] {
        for(const QSize size:{QSize(2560,1440),QSize(1920,1080)}) {
            window->resize(size);
            for(const bool dark:{true,false}) {
                appSettings.setTheme(dark?QStringLiteral("dark"):QStringLiteral("light"));
                QCoreApplication::processEvents();
                const auto image=window->grabWindow();
                const auto name=QString("%1-%2x%3-%4.png").arg(screen).arg(size.width()).arg(size.height()).arg(dark?"dark":"light");
                if(image.isNull() || image.size()!=size || !image.save(directory.filePath(name))) {return false;}
                std::printf("%s %dx%d\n",qPrintable(name),image.width(),image.height());
            }
        }
        return true;
    };
    // Each stage polls until its condition holds, then runs its action.
    struct Stage { std::function<bool()> ready; std::function<void()> action; };
    std::vector<Stage> stages;
    auto openDialog=[&](const char *name) {
        if(auto *dialog=window->findChild<QObject *>(name)) {QMetaObject::invokeMethod(dialog,"open");}
    };
    bool announced=false;
    stages.push_back({[&] {
        if(connection.ready() && connection.snapshot()->status && !announced) {
            announced=true;std::printf("GUI READY\n");std::fflush(stdout);
        }
        if(standalone && !linked) {return true;}
        if(screen=="connect") {return true;}
        if(screen=="agent-failure") {return !connection.agentFailure().isEmpty();}
        if(expected=="lost") {return connection.linkLost();}
        if(!connection.ready() || !connection.snapshot()->status) {return false;}
        if(expected=="fault" && connection.armState()!=2) {return false;}
        if(expected=="safe" && !connection.safeMode()) {return false;}
        return true;
    },[&] {
        if(agentScreen) {
            // The Agent tab as a user leaves it: address and token typed, the listing asked for.
            if(auto *connectPage=window->findChild<QObject *>("connectPage")) {connectPage->setProperty("tab",2);}
            if(agentLink) {
                if(auto *field=window->findChild<QObject *>("agentAddress")) {field->setProperty("text",agentAddress);}
                if(auto *field=window->findChild<QObject *>("agentToken")) {field->setProperty("text",agentToken);}
            }
            if(lobbyScreen && agentLink) {connection.listAgent(agentAddress,agentToken);}
            return;
        }
        if(agentLink && takeLease) {connection.takeLease();}
        window->setProperty("page",layoutScreen?1:monitorScreen?3:tuneScreen?2:std::max(0,int(pages.indexOf(screen))-1));
        if(monitorScreen) {
            // Like the user's clicks: rates first, then each signal (a
            // refused one shows its refusal).
            auto &monitor=connection.monitorView();
            monitor.setWindowSeconds(parser.value("window").toInt());
            for(const auto &rate:parser.value("rate").split(',',Qt::SkipEmptyParts)) {
                const auto parts=rate.split(':');
                if(parts.size()==2) {monitor.setRate(parts[0].toInt(),parts[1].toInt());}
            }
            for(const auto &key:parser.value("plot").split(',',Qt::SkipEmptyParts)) {monitor.toggleSignal(key);}
        }
        if(layoutScreen && parser.isSet("select")) {connection.selectNode(parser.value("select"));}
        if(screen=="layout-review") {window->setProperty("reviewOpen",true);}
        if(screen=="layout-add-instance" || screen=="layout-add-loop") {
            auto *dialogs=window->property("layoutDialogs").value<QObject *>();
            QMetaObject::invokeMethod(dialogs,screen=="layout-add-loop"?"openAddLoop":"openAddInstance");
        }
        if(screen=="drawer") {window->setProperty("drawerOpen",true);}
        if(screen=="dialog") {openDialog("shellDialog");}
        if(screen=="rename") {openDialog("renameDialog");}
        if(screen=="reset") {openDialog("resetDialog");}
        if(screen=="unsaved" || screen=="notice") {connection.rename(QStringLiteral("bench-left"));}
        if(update) {
            const auto choice=connection.inspectImage(parser.value("image"));
            if(screen=="update") {window->property("otaDialog").value<QObject *>()->setProperty("choice",choice);}
            else {
                if(screen=="update-writing") {connection.setOtaPauseAt(62);}
                connection.startUpdate(screen!="update-rolledback");
            }
        }
    }});
    // Remote screens. The lobby listing ends ok or failed; a failed connect is
    // captured by the first stage; the others follow the lease: the agent's
    // Lease events are the truth, so each waits for what it shows.
    const auto lease=[&] {return connection.lease();};
    if(lobbyScreen) {
        stages.push_back({[&] {const auto state=connection.agentListing().value("state").toString();return state=="ok" || state=="failed";},[] {}});
    }
    if(takeLease && agentLink && !agentScreen) {
        stages.push_back({[&] {return lease().value("mine").toBool();},[] {}});
    }
    if(screen=="lease-take") {
        stages.push_back({[&] {return lease().value("holderId").toInt()!=0 && !lease().value("mine").toBool();},[&] {openDialog("takeLeaseDialog");}});
    }
    if(screen=="lease-release") {
        stages.push_back({[&] {return lease().value("mine").toBool();},[&] {openDialog("releaseLeaseDialog");}});
    }
    if(screen=="revoked") {
        // The script makes another client take the lease; the dialog opens by itself.
        stages.push_back({[&] {return lease().value("revoked").toBool();},[] {}});
    }
    // The Device page's Clients card refreshes after the first ClientList.
    if(agentLink && screen=="device") {
        stages.push_back({[&] {return !connection.clients().isEmpty();},[] {}});
    }
    // The update screens wait for the firmware state, then for their phase.
    if(update) {
        stages.insert(stages.begin(),{[&] {return connection.ready() && connection.firmware().value("known").toBool();},[] {}});
    }
    if(screen=="update-writing") {
        stages.push_back({[&] {return connection.ota().value("percent").toInt()>=62;},[] {}});
    }
    if(screen=="update-confirmed") {
        stages.push_back({[&] {return connection.ota().value("phase").toString()=="confirmed";},[] {}});
    }
    if(screen=="update-rolledback") {
        stages.push_back({[&] {return connection.ota().value("phase").toString()=="unconfirmed";},[&] {connection.rollBackFirmware();}});
        stages.push_back({[&] {return connection.ota().value("phase").toString()=="rolledBack";},[] {}});
    }
    if(screen=="unsaved" || screen=="notice") {
        stages.push_back({[&] {return connection.unsaved() && connection.deviceName()=="bench-left" && !connection.configBusy();},
                          [&] {if(screen=="notice") {connection.reboot();}}});
    }
    if(screen=="notice") {
        stages.push_back({[&] {return connection.ready() && connection.rebootNotice().value("visible").toBool();},[] {}});
    }
    // The custom panel tab of the selected instance, once it has live data.
    if(layoutScreen && parser.isSet("panel")) {
        stages.push_back({[&] {return !connection.layout().value("panel").toString().isEmpty() &&
                                      connection.live().value("telemetry").toBool();},
                          [&] {if(auto *page=window->findChild<QObject *>("layoutPage")) {page->setProperty("tab",1);}}});
    }
    // An action with its outcome under the row (sent once, like a click).
    const QString action=parser.value("action");
    if(layoutScreen && !action.isEmpty()) {
        const auto row=[&] {return connection.actionTable().row(action);};
        stages.push_back({[&] {return !row().isEmpty() && row().value("blocked").toString().isEmpty() &&
                                      connection.live().value("telemetry").toBool();},
                          [&] {connection.runAction(action,{},{});}});
        stages.push_back({[&] {const auto state=row().value("result").toMap().value("state").toString();
                                return !state.isEmpty() && state!="running";},[] {}});
    }
    // Manual command: the inputs on the card; with values, ARM, then each
    // slider held at its value or latched there, as a drag would.
    struct Commanded { QString key,limit; std::optional<double> value; bool latch=false; };
    std::vector<Commanded> commanded;
    for(const auto &entry:parser.value("command").split(',',Qt::SkipEmptyParts)) {
        Commanded c;const auto parts=entry.split(QLatin1Char('='));c.key=parts[0].section(QLatin1Char('@'),0,0);c.limit=parts[0].section(QLatin1Char('@'),1);
        if(parts.size()>1) {
            auto value=parts[1];
            if(value.endsWith(":latch")) {c.latch=true;value.chop(6);}
            bool ok=false;const double v=value.toDouble(&ok);
            if(!ok) {std::fprintf(stderr,"Invalid --command value\n");return 2;}
            c.value=v;
        }
        commanded.push_back(c);
    }
    const bool armForCommand=std::any_of(commanded.begin(),commanded.end(),[](const Commanded &c){return c.value.has_value();});
    // Commanding arms and drives inputs: a vdev only, never a board.
    if(armForCommand && !parser.value("endpoint").startsWith("tcp://")) {
        std::fprintf(stderr,"--command with a value arms the device; use a vdev tcp:// endpoint\n");return 2;
    }
    if(monitorScreen && !commanded.empty()) {
        auto &command=connection.commandView();
        stages.push_back({[&] {return connection.armEnabled() || connection.armState()==1;},[&] {
            for(const auto &c:commanded) {command.add(c.key);if(!c.limit.isEmpty()) {command.setLimitText(c.key,c.limit);}}
            if(armForCommand && connection.armState()!=1) {connection.arm();}
        }});
        if(armForCommand) {
            stages.push_back({[&] {return connection.armState()==1 && command.enabled();},[&] {
                for(const auto &c:commanded) {
                    if(!c.value) {continue;}
                    command.press(c.key);command.drag(c.key,*c.value);
                    if(c.latch) {command.setLatched(c.key,true);command.release(c.key);}
                }
            }});
        }
    }
    // Tune: the controller, its params and the test signal as typed; then,
    // armed (a vdev only), one step run per gain value as a user would.
    auto &tune=connection.tuneView();
    const auto runs=parser.value("tune-runs");
    const auto runKey=runs.section(QLatin1Char('='),0,0);
    const auto runValues=runs.section(QLatin1Char('='),1).split(QLatin1Char(':'),Qt::SkipEmptyParts);
    const bool extraRun=screen=="tune-running" || screen=="tune-disarm";
    const bool identify=parser.isSet("identify") || screen=="tune-identify";
    if((!runValues.isEmpty() || extraRun || identify) && !parser.value("endpoint").startsWith("tcp://")) {
        std::fprintf(stderr,"--tune-runs, --identify and the tune-running/tune-disarm/tune-identify screens arm the device; use a vdev tcp:// endpoint\n");return 2;
    }
    if(tuneScreen) {
        stages.push_back({[&] {return connection.ready() && !tune.controllers().isEmpty();},[&] {
            if(parser.isSet("tune")) {tune.selectController(parser.value("tune"));}
            for(const auto &entry:parser.value("tune-set").split(',',Qt::SkipEmptyParts)) {
                const auto key=entry.section(QLatin1Char('='),0,0),value=entry.section(QLatin1Char('='),1);
                bool isEnum=false;
                for(const auto &e:tune.enums()) {
                    const auto m=e.toMap();
                    if(m["key"].toString()==key) {isEnum=true;tune.setEnum(key,int(m["options"].toStringList().indexOf(value)));}
                }
                if(!isEnum) {tune.gainText(key,value);}
            }
            for(const auto &entry:parser.value("signal").split(',',Qt::SkipEmptyParts)) {
                const auto field=entry.section(QLatin1Char('='),0,0),value=entry.section(QLatin1Char('='),1);
                if(field=="target") {tune.setTarget(value);}
                else if(field=="waveform") {tune.setWaveform(value.toInt());}
                else {tune.setSignalField(field,value);}
            }
        }});
        if(!runValues.isEmpty() || extraRun || identify) {
            stages.push_back({[&] {return !tune.gainBusy() && connection.armEnabled();},[&] {connection.arm();}});
            stages.push_back({[&] {return connection.armState()==1 && tune.run().value("canRun").toBool();},[] {}});
        }
        for(qsizetype i=0;i<runValues.size();++i) {
            stages.push_back({[&] {return !connection.tuneRunning() && !tune.gainBusy();},[&,i] {tune.gainText(runKey,runValues[i]);}});
            stages.push_back({[&] {return !tune.gainBusy() && tune.run().value("canRun").toBool();},[&] {tune.startRun();}});
            stages.push_back({[&,i] {return !connection.tuneRunning() && tune.runCount()==int(i)+1;},[] {}});
        }
        if(extraRun) {
            stages.push_back({[&] {return tune.run().value("canRun").toBool();},[&] {tune.startRun();}});
            stages.push_back({[&] {return tune.phase()==drivelink::gui::TuneView::Phase::Running && tune.run().value("progress").toDouble()>0.3;},
                [&] {if(screen=="tune-disarm") {connection.disarm();}}});
            if(screen=="tune-disarm") {
                stages.push_back({[&] {return !connection.tuneRunning() && tune.runCount()==int(runValues.size())+1;},[] {}});
            }
        }
        // H2e-3 Plant identification: after the runs (if any), show the tab,
        // tick the confirmation, run the step and wait (bounded) for the fit.
        if(identify) {
            auto &id=tune.identifyView();
            auto started=std::make_shared<QElapsedTimer>();
            stages.push_back({[&] {return connection.armState()==1 && id.availability().value("available").toBool();},[&] {
                id.setShown(true);id.setConfirmed(true);
            }});
            stages.push_back({[&] {return !tune.gainBusy() && id.run().value("canRun").toBool();},[&,started] {id.startStep();started->start();}});
            stages.push_back({[&,started] {
                if(started->isValid() && started->elapsed()>20000) {
                    std::fprintf(stderr,"The identification step did not finish in 20 s: %s\n",
                        qPrintable(id.run().value("text").toString()));
                    app.exit(2);return false;
                }
                return !id.run().value("active").toBool() && id.result().value("available").toBool() && !id.result().value("fitting").toBool();
            },[&] {
                const auto responses=id.result().value("responses").toStringList();
                for(qsizetype i=0;i<responses.size();++i) {
                    if(parser.isSet("identify-response") && responses[i].startsWith(parser.value("identify-response"))) {id.setResponse(int(i));break;}
                }
            }});
        }
        // H2e-2 A/B: the oldest and the newest gain set with a run, compared.
        if(screen=="tune-ab") {
            stages.push_back({[&] {return !connection.tuneRunning() && tune.runCount()>=2;},[&] {
                std::vector<int> ids;
                for(const auto &e:tune.historyList()) {if(!e.runs.empty()) {ids.push_back(e.id);}}
                if(ids.size()>=2) {tune.pickCompare(ids.front());tune.pickCompare(ids.back());}
            }});
        }
    }
    // live-edit: another client holds the lease, so one live param edit is
    // held unsent for the operator's confirmation (the dialog opens by itself).
    if(liveEditScreen) {
        const auto key=liveEditText.section(QLatin1Char('='),0,0),value=liveEditText.section(QLatin1Char('='),1);
        stages.push_back({[&] {return connection.ready() && lease().value("holderId").toInt()!=0 && !lease().value("mine").toBool() &&
                                      (!layoutScreen || !parser.isSet("select") || !connection.layout().value("selected").toString().isEmpty());},
                          [&,key,value] {connection.setParamText(key,value);}});
        stages.push_back({[&] {return !connection.liveEdit().isEmpty();},[] {}});
    }
    // Monitor: data across most of the window, then (paused) freeze it and
    // place the cursors.
    if(monitorScreen) {
        auto filled=std::make_shared<QElapsedTimer>();
        stages.push_back({[&connection,filled,&parser] {
            if(connection.monitorView().nowUs()<=0) {return false;}
            if(!filled->isValid()) {filled->start();}
            return filled->elapsed()>=qint64(parser.value("window").toInt())*900;
        },[&connection,&screen] {
            if(screen=="monitor-paused") {connection.monitorView().setPaused(true);connection.monitorView().setCursors(true);}
        }});
    }
    QElapsedTimer settled;
    stages.push_back({[&] {
        if(!settled.isValid()) {settled.start();}
        return settled.elapsed()>=settle;
    },[] {}});
    size_t stage=0;bool captured=false;
    QTimer poll;poll.setInterval(50);
    QObject::connect(&connection,&drivelink::gui::Connection::disconnected,&app,[&] {if(captured) {app.quit();}});
    bool mineAnnounced=false;
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        // Scripts sequence on this line (the revoked screen waits for it before another client takes the lease).
        if(agentLink && takeLease && !agentScreen && !mineAnnounced && connection.lease().value("mine").toBool()) {
            mineAnnounced=true;std::printf("GUI LEASE MINE\n");std::fflush(stdout);
        }
        while(stage<stages.size() && stages[stage].ready()) {stages[stage].action();++stage;}
        if(stage<stages.size()) {return;}
        poll.stop();
        if(!capture()) {app.exit(2);return;}
        captured=true;
        if(connection.linked()) {connection.disconnectDevice();} else {app.quit();}
    });
    poll.start();return app.exec();
}
