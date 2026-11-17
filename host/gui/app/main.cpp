#include "connection.hpp"
#include "safety_keys.hpp"
#include "style.hpp"
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <cstdio>
#include <functional>

int main(int argc,char **argv) {
    Q_INIT_RESOURCE(qmake_DriveLink_Shell);
    Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);
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
    parser.addOption({"settings","Keep recent devices in this INI file instead of the user's.","file"});
    parser.addOption({"out","Render the shell in both sizes and themes, then disconnect.","directory"});
    parser.addOption({"screen","connect, device, layout, tune, monitor, drawer, dialog, rename, reset, unsaved, notice, update, "
                      "update-writing, update-confirmed or update-rolledback (unsaved, notice and update-* change a tcp:// vdev).","name","device"});
    parser.addOption({"image","Firmware image for the update screens (vdev fixtures).","file"});
    parser.addOption({"expect-state","Capture when ready, fault, safe or lost.","state","ready"});
    parser.addOption({"settle","Wait this long (ms) after the screen is ready before capturing.","ms","0"});
    parser.process(app);
    const QString screen=parser.value("screen");
    const QString expected=parser.value("expect-state");
    if(!QStringList{"ready","fault","safe","lost"}.contains(expected)) {return 2;}
    // Pages first (their index is the nav index plus one), then shell states.
    const QStringList pages={"connect","device","layout","tune","monitor"};
    const QStringList states={"drawer","dialog","rename","reset","unsaved","notice","update","update-writing","update-confirmed","update-rolledback"};
    const bool update=screen.startsWith("update");
    if(update && !parser.isSet("image")) {std::fprintf(stderr,"--screen %s needs --image\n",qPrintable(screen));return 2;}
    if(!pages.contains(screen) && !states.contains(screen)) {std::fprintf(stderr,"Unknown screen\n");return 2;}
    bool numeric=false;const int baud=parser.value("baud").toInt(&numeric);
    const int settle=parser.value("settle").toInt();
    if(!numeric || baud<=0 || settle<0) {std::fprintf(stderr,"Invalid --baud or --settle\n");return 2;}
    drivelink::gui::LocalConnection connection;
    if(parser.isSet("settings")) {connection.setSettingsPath(parser.value("settings"));}
    drivelink::gui::SafetyKeys keys;
    QObject::connect(&keys,&drivelink::gui::SafetyKeys::escapeRequested,&connection,&drivelink::gui::Connection::disarm);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("connection",&connection);
    engine.rootContext()->setContextProperty("safetyKeys",&keys);
    if(!drivelink::gui::installStyle(engine)) {return 2;}
    engine.loadFromModule("DriveLink.Shell","Shell");
    if(engine.rootObjects().isEmpty()) {return 2;}
    auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    if(!window) {return 2;}
    if(parser.isSet("endpoint")) {connection.connectTo(parser.value("endpoint"),baud);}
    if(!render) {return app.exec();}
    if(screen!="connect" && !parser.isSet("endpoint")) {
        std::fprintf(stderr,"This screen needs --endpoint; use host/gui/tests/scenarios.py for isolated vdev fixtures\n");return 2;
    }
    // These screens rename (and reboot) the device: vdev only, never a board.
    if((screen=="unsaved" || screen=="notice" || (update && screen!="update")) && !parser.value("endpoint").startsWith("tcp://")) {
        std::fprintf(stderr,"--screen %s changes the device; use a vdev tcp:// endpoint\n",qPrintable(screen));return 2;
    }
    QDir directory(parser.value("out"));if(!directory.mkpath(".")) {return 2;}
    QTimer timeout;timeout.setSingleShot(true);timeout.setInterval(30000);
    QObject::connect(&timeout,&QTimer::timeout,&app,[&] {std::fprintf(stderr,"Screenshot timed out: %s\n",qPrintable(connection.lastError().value("message").toString()));app.exit(2);});
    timeout.start();
    auto capture=[&] {
        auto *theme=engine.singletonInstance<QObject *>("DriveLink.Style","Theme");
        for(const QSize size:{QSize(2560,1440),QSize(1920,1080)}) {
            window->resize(size);
            for(const bool dark:{true,false}) {
                theme->setProperty("dark",dark);
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
        if(screen=="connect") {return true;}
        if(expected=="lost") {return connection.linkLost();}
        if(!connection.ready() || !connection.snapshot()->status) {return false;}
        if(expected=="fault" && connection.armState()!=2) {return false;}
        if(expected=="safe" && !connection.safeMode()) {return false;}
        return true;
    },[&] {
        window->setProperty("page",std::max(0,int(pages.indexOf(screen))-1));
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
    QElapsedTimer settled;
    stages.push_back({[&] {
        if(!settled.isValid()) {settled.start();}
        return settled.elapsed()>=settle;
    },[] {}});
    size_t stage=0;bool captured=false;
    QTimer poll;poll.setInterval(50);
    QObject::connect(&connection,&drivelink::gui::Connection::disconnected,&app,[&] {if(captured) {app.quit();}});
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        while(stage<stages.size() && stages[stage].ready()) {stages[stage].action();++stage;}
        if(stage<stages.size()) {return;}
        poll.stop();
        if(!capture()) {app.exit(2);return;}
        captured=true;
        if(connection.linked()) {connection.disconnectDevice();} else {app.quit();}
    });
    poll.start();return app.exec();
}
