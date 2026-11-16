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
    parser.addOption({"out","Render the shell in both sizes and themes, then disconnect.","directory"});
    parser.addOption({"screen","connect, device, layout, tune, monitor, drawer or dialog.","name","device"});
    parser.addOption({"expect-state","Capture when ready, fault, safe or lost.","state","ready"});
    parser.process(app);
    const QString screen=parser.value("screen");
    const QString expected=parser.value("expect-state");
    if(!QStringList{"ready","fault","safe","lost"}.contains(expected)) {return 2;}
    const QStringList names={"connect","device","layout","tune","monitor","drawer","dialog"};
    if(!names.contains(screen)) {std::fprintf(stderr,"Unknown screen\n");return 2;}
    drivelink::gui::LocalConnection connection;
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
    if(parser.isSet("endpoint")) {connection.connectTo(parser.value("endpoint"));}
    if(!render) {return app.exec();}
    if(screen!="connect" && !parser.isSet("endpoint")) {
        std::fprintf(stderr,"This screen needs --endpoint; use host/gui/tests/scenarios.py for isolated vdev fixtures\n");return 2;
    }
    QDir directory(parser.value("out"));if(!directory.mkpath(".")) {return 2;}
    QTimer timeout;timeout.setSingleShot(true);timeout.setInterval(30000);
    QObject::connect(&timeout,&QTimer::timeout,&app,[&] {std::fprintf(stderr,"Screenshot timed out: %s\n",qPrintable(connection.lastError().value("message").toString()));app.exit(2);});
    timeout.start();
    QTimer poll;poll.setInterval(50);bool captured=false,announced=false;
    QObject::connect(&connection,&drivelink::gui::Connection::disconnected,&app,[&] {if(captured) {app.quit();}});
    QObject::connect(&poll,&QTimer::timeout,&app,[&] {
        if(connection.ready() && connection.snapshot()->status && !announced) {
            announced=true;std::printf("GUI READY\n");std::fflush(stdout);
        }
        if(screen!="connect") {
            if(expected=="lost") {if(!connection.linkLost()) {return;}}
            else {
                if(!connection.ready() || !connection.snapshot()->status) {return;}
                if(expected=="fault" && connection.armState()!=2) {return;}
                if(expected=="safe" && !connection.safeMode()) {return;}
            }
        }
        poll.stop();
        window->setProperty("page",std::max(0,int(names.indexOf(screen))-1));
        if(screen=="drawer") {window->setProperty("page",0);window->setProperty("drawerOpen",true);}
        if(screen=="dialog") {
            window->setProperty("page",0);
            auto *dialog=window->findChild<QObject *>("shellDialog");
            if(dialog) {QMetaObject::invokeMethod(dialog,"open");}
        }
        auto *theme=engine.singletonInstance<QObject *>("DriveLink.Style","Theme");
        for(const QSize size:{QSize(2560,1440),QSize(1920,1080)}) {
            window->resize(size);
            for(const bool dark:{true,false}) {
                theme->setProperty("dark",dark);
                QCoreApplication::processEvents();
                const auto image=window->grabWindow();
                const auto name=QString("%1-%2x%3-%4.png").arg(screen).arg(size.width()).arg(size.height()).arg(dark?"dark":"light");
                if(image.isNull() || image.size()!=size || !image.save(directory.filePath(name))) {app.exit(2);return;}
                std::printf("%s %dx%d\n",qPrintable(name),image.width(),image.height());
            }
        }
        captured=true;
        if(connection.linked()) {connection.disconnectDevice();} else {app.quit();}
    });
    poll.start();return app.exec();
}
