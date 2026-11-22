// Qt Quick Test runner for the DriveLink style: every tst_*.qml in this
// directory, with the bundled fonts and icon provider installed.
#include "style.hpp"

#include <QQmlContext>
#include <QQmlEngine>
#include <QtQuickTest>

class Setup : public QObject
{
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        const bool fonts = drivelink::gui::installStyle(*engine);
        engine->rootContext()->setContextProperty("styleInstalled", fonts);
        engine->rootContext()->setContextProperty("bundledIcons", drivelink::gui::bundledIconNames());
    }
};

QUICK_TEST_MAIN_WITH_SETUP(gui_quick, Setup)

#include "tst_main.moc"
