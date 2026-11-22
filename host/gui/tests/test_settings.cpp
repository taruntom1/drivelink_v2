// Settings and About (H2g): AppSettings' rules, and the Settings page in the
// shell.
#include "app_settings.hpp"
#include "connection.hpp"
#include "safety_keys.hpp"
#include "style.hpp"
#include "test_support.hpp"
#include <QClipboard>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using drivelink::gui::AppSettings;
using namespace drivelink::gui;
using namespace drivelink::gui::test;

// Q_INIT_RESOURCE declares at global scope.
static void initShellResources() {
    Q_INIT_RESOURCE(qmake_DriveLink_Shell);
    Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);registerShellTypes();
}

namespace {
// The shell as main.cpp builds it: the connection, the safety keys and the
// settings, with Theme.dark following the settings.
struct ShellHarness {
    QTemporaryDir dir;
    LocalConnection connection;
    SafetyKeys keys;
    AppSettings settings;
    QQmlApplicationEngine engine;
    QQuickWindow *window=nullptr;
    QObject *theme=nullptr;
    ShellHarness():settings(dir.filePath("app.ini")) {}
    bool open() {
        initShellResources();
        connection.setSettingsPath(dir.filePath("gui.ini"));
        if(!installStyle(engine)) {return false;}
        engine.rootContext()->setContextProperty("connection",&connection);
        engine.rootContext()->setContextProperty("safetyKeys",&keys);
        engine.rootContext()->setContextProperty("appSettings",&settings);
        engine.loadFromModule("DriveLink.Shell","Shell");
        if(engine.rootObjects().isEmpty()) {return false;}
        theme=engine.singletonInstance<QObject *>("DriveLink.Style","Theme");
        window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        if(!theme || !window) {return false;}
        auto apply=[this] {theme->setProperty("dark",settings.dark());};
        apply();QObject::connect(&settings,&AppSettings::themeChanged,theme,apply);
        window->resize(1920,1080);
        return true;
    }
    QQuickItem *item(const char *name) {return visualChild(window->contentItem(),name);}
    int page() {return window->property("page").toInt();}
    bool shown(const char *name) {auto *i=item(name);return i && i->isVisible();}
    bool click(const char *name) {
        auto *i=item(name);
        if(!i || !i->isVisible() || !i->isEnabled()) {return false;}
        QTest::qWait(50); // positioners lay out on polish
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,i->mapToScene({i->width()/2,i->height()/2}).toPoint());
        return true;
    }
};
}

class SettingsTests : public QObject {
    Q_OBJECT
private slots:
    void themeDefaultsToDarkAndPersists() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        const auto path=dir.filePath("gui.ini");
        {
            AppSettings s(path);
            QCOMPARE(s.theme(),QString("dark"));QVERIFY(s.dark());
            QSignalSpy spy(&s,&AppSettings::themeChanged);
            s.setTheme("light");QCOMPARE(spy.count(),1);QVERIFY(!s.dark());
            s.setTheme("light");QCOMPARE(spy.count(),1); // unchanged: no signal
        }
        QCOMPARE(QSettings(path,QSettings::IniFormat).value("appearance/theme").toString(),QString("light"));
        AppSettings again(path);QCOMPARE(again.theme(),QString("light"));
    }
    void invalidValuesAreIgnored() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        const auto path=dir.filePath("gui.ini");
        QSettings(path,QSettings::IniFormat).setValue("appearance/theme","purple");
        AppSettings s(path);QCOMPARE(s.theme(),QString("dark"));
        QSignalSpy spy(&s,&AppSettings::themeChanged);
        s.setTheme("");s.setTheme("Light");QCOMPARE(spy.count(),0);QCOMPARE(s.theme(),QString("dark"));
    }
    void systemFollowsTheColourScheme() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        AppSettings s(dir.filePath("gui.ini"));
        s.setTheme("system");
        s.setSystemDarkForTest(0);QVERIFY(!s.dark());
        s.setSystemDarkForTest(1);QVERIFY(s.dark());
        s.setTheme("dark");s.setSystemDarkForTest(0);QVERIFY(s.dark()); // only "system" follows
    }
    void screenshotModeDoesNotSave() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        const auto path=dir.filePath("gui.ini");
        AppSettings s(path);s.setPersistent(false);
        s.setTheme("light");QVERIFY(!s.dark());
        QVERIFY(!QSettings(path,QSettings::IniFormat).contains("appearance/theme"));
    }
    void discoverAgentsDefaultsOnAndPersists() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        const auto path=dir.filePath("gui.ini");
        {
            AppSettings s(path);
            QVERIFY(s.discoverAgents());
            QSignalSpy spy(&s,&AppSettings::discoverAgentsChanged);
            s.setDiscoverAgents(true);QCOMPARE(spy.count(),0); // unchanged: no signal
            s.setDiscoverAgents(false);QCOMPARE(spy.count(),1);QVERIFY(!s.discoverAgents());
        }
        QCOMPARE(QSettings(path,QSettings::IniFormat).value("connection/discoverAgents").toBool(),false);
        {
            AppSettings again(path);QVERIFY(!again.discoverAgents());
            again.setDiscoverAgents(true);
        }
        AppSettings third(path);QVERIFY(third.discoverAgents());
        // The theme is untouched by it, and screenshot mode does not save.
        QCOMPARE(third.theme(),QString("dark"));
        QTemporaryDir other;QVERIFY(other.isValid());
        AppSettings shot(other.filePath("gui.ini"));shot.setPersistent(false);
        shot.setDiscoverAgents(false);QVERIFY(!shot.discoverAgents());
        QVERIFY(!QSettings(other.filePath("gui.ini"),QSettings::IniFormat).contains("connection/discoverAgents"));
    }
    void aboutFacts() {
        QTemporaryDir dir;QVERIFY(dir.isValid());
        AppSettings s(dir.filePath("gui.ini"));
        // VERSION, with +g<sha>[.dirty] away from its tag (cmake/DriveLinkVersion.cmake).
        QVERIFY2(QRegularExpression("^\\d+\\.\\d+\\.\\d+(\\+g[0-9a-f]{7}(\\.dirty)?)?$").match(s.version()).hasMatch(),qPrintable(s.version()));
        QCOMPARE(s.qtVersion(),QString::fromLatin1(qVersion()));
        QVERIFY(!s.compiler().isEmpty());
        QVERIFY(s.licence().contains("All rights reserved"));
        QVERIFY(s.notices().size()>0);
        // LF only, whatever the checkout's line endings (Qt's TextArea drops CR).
        QVERIFY(!s.notices().contains(QLatin1Char('\r')));
        QVERIFY(!s.licence().contains(QLatin1Char('\r')));
    }
    // Disconnected: Settings opens from the rail's footer (the page items stay
    // disabled), replaces the Connect page, and Close or the item leads back.
    void settingsPageBeforeConnecting() {
        ShellHarness h;QVERIFY(h.open());
        QVERIFY(h.item("navSettings") && h.item("navSettings")->isEnabled());
        QVERIFY(h.item("navDevice") && !h.item("navDevice")->isEnabled());
        QVERIFY(!h.item("navMonitor")->isEnabled());
        QVERIFY(h.shown("connectPage"));QVERIFY(!h.item("settingsPage"));
        QVERIFY(h.click("navSettings"));
        QVERIFY(waitFor([&]{return h.shown("settingsPage");}));
        QCOMPARE(h.page(),4);QVERIFY(!h.shown("connectPage"));
        QVERIFY(h.item("navSettings")->property("checked").toBool());
        QVERIFY(!h.item("navDevice")->isEnabled());
        QVERIFY(h.click("settingsClose"));
        QCOMPARE(h.page(),0);QVERIFY(h.shown("connectPage"));
        QVERIFY(waitFor([&]{return !h.item("settingsPage");}));
        // The footer item toggles too.
        QVERIFY(h.click("navSettings"));QCOMPARE(h.page(),4);
        QVERIFY(h.click("navSettings"));QCOMPARE(h.page(),0);QVERIFY(h.shown("connectPage"));
    }
    void themeButtonsChangeTheSetting() {
        ShellHarness h;QVERIFY(h.open());
        QVERIFY(h.click("navSettings"));QVERIFY(waitFor([&]{return h.shown("themeLight");}));
        QVERIFY(h.item("themeDark")->property("checked").toBool());
        QVERIFY(h.theme->property("dark").toBool());
        QVERIFY(h.click("themeLight"));
        QCOMPARE(h.settings.theme(),QString("light"));QVERIFY(!h.theme->property("dark").toBool());
        QVERIFY(h.item("themeLight")->property("checked").toBool());QVERIFY(!h.item("themeDark")->property("checked").toBool());
        QCOMPARE(QSettings(h.dir.filePath("app.ini"),QSettings::IniFormat).value("appearance/theme").toString(),QString("light"));
        QVERIFY(h.click("themeSystem"));
        QCOMPARE(h.settings.theme(),QString("system"));
        h.settings.setSystemDarkForTest(0);
        QVERIFY(!h.theme->property("dark").toBool());
        QVERIFY(h.item("themeHint")->property("text").toString().contains("now: Light"));
        h.settings.setSystemDarkForTest(1);
        QVERIFY(h.theme->property("dark").toBool());
        QVERIFY(h.item("themeHint")->property("text").toString().contains("now: Dark"));
        QVERIFY(h.click("themeDark"));
        QCOMPARE(QSettings(h.dir.filePath("app.ini"),QSettings::IniFormat).value("appearance/theme").toString(),QString("dark"));
    }
    void connectionSwitchWritesTheSetting() {
        ShellHarness h;QVERIFY(h.open());
        QVERIFY(h.click("navSettings"));QVERIFY(waitFor([&]{return h.shown("discoverAgents");}));
        QVERIFY(h.shown("connectionCard"));QVERIFY(h.item("discoverAgents")->property("checked").toBool());
        QCOMPARE(h.item("discoverAgents")->property("text").toString(),QString("Find agents on the network"));
        QVERIFY(h.item("discoverAgentsHint")->property("text").toString().contains("mDNS"));
        QVERIFY(h.click("discoverAgents"));
        QVERIFY(!h.settings.discoverAgents());QVERIFY(!h.item("discoverAgents")->property("checked").toBool());
        QCOMPARE(QSettings(h.dir.filePath("app.ini"),QSettings::IniFormat).value("connection/discoverAgents").toBool(),false);
        QVERIFY(h.click("discoverAgents"));
        QVERIFY(h.settings.discoverAgents());QVERIFY(h.item("discoverAgents")->property("checked").toBool());
        // A change from elsewhere shows in the switch.
        h.settings.setDiscoverAgents(false);
        QVERIFY(waitFor([&]{return !h.item("discoverAgents")->property("checked").toBool();}));
    }
    void aboutShowsTheFacts() {
        ShellHarness h;QVERIFY(h.open());
        QVERIFY(h.click("navSettings"));QVERIFY(waitFor([&]{return h.shown("noticesView");}));
        QCOMPARE(h.item("versionText")->property("text").toString(),h.settings.version());
        auto *notices=h.item("noticesView");
        QCOMPARE(notices->property("text").toString(),h.settings.notices());
        QCOMPARE(notices->property("textFormat").toInt(),int(Qt::PlainText));
        QVERIFY(notices->property("readOnly").toBool());
        QVERIFY(h.item("noticesFrame")->height()>=360);
        QVERIFY(h.item("licenceText")->property("text").toString().contains("All rights reserved"));
        QVERIFY(h.click("copyNotices"));
        QCOMPARE(QGuiApplication::clipboard()->text(),h.settings.notices());
        QVERIFY(h.click("copyVersion"));
        QCOMPARE(QGuiApplication::clipboard()->text(),h.settings.version());
    }
    // Connected: Settings sits above the pages, DISARM in the status bar stays
    // live, the page items lead away and Close returns to the previous page.
    void settingsWhileConnected() {
        Fixture fixture;
        ShellHarness h;QVERIFY(h.open());
        h.connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return h.connection.ready() && h.connection.armEnabled();}));
        QVERIFY(h.item("navDevice")->isEnabled());QVERIFY(h.shown("devicePage"));
        QVERIFY(h.click("navLayout"));QCOMPARE(h.page(),1);QVERIFY(h.shown("layoutPage"));
        QVERIFY(h.click("navSettings"));
        QVERIFY(waitFor([&]{return h.shown("settingsPage");}));
        QCOMPARE(h.page(),4);QVERIFY(!h.shown("layoutPage"));QVERIFY(!h.shown("devicePage"));QVERIFY(!h.shown("connectPage"));
        QVERIFY(h.item("navDevice")->isEnabled());
        auto *disarm=h.item("disarmButton");QVERIFY(disarm && disarm->isVisible() && disarm->isEnabled());
        const auto before=fixture.stats();
        QVERIFY(h.click("disarmButton"));QVERIFY(waitFor([&]{return fixture.stats()!=before;}));
        QVERIFY(h.click("settingsClose"));
        QCOMPARE(h.page(),1);QVERIFY(h.shown("layoutPage"));QVERIFY(!h.item("settingsPage"));
        QVERIFY(h.click("navSettings"));QVERIFY(h.click("navSettings"));
        QCOMPARE(h.page(),1);
        QVERIFY(h.click("navSettings"));QVERIFY(h.click("navDevice"));
        QCOMPARE(h.page(),0);QVERIFY(h.shown("devicePage"));QVERIFY(!h.item("settingsPage"));
        // A closed link leaves the user on Settings.
        QVERIFY(h.click("navSettings"));QCOMPARE(h.page(),4);
        h.connection.disconnectDevice();
        QVERIFY(waitFor([&]{return !h.connection.linked() && !h.connection.closing();}));
        QCOMPARE(h.page(),4);QVERIFY(h.shown("settingsPage"));QVERIFY(!h.shown("connectPage"));
        QVERIFY(h.item("navSettings")->isEnabled());QVERIFY(!h.item("navDevice")->isEnabled());
        QVERIFY(h.click("settingsClose"));
        QCOMPARE(h.page(),0);QVERIFY(h.shown("connectPage"));
    }
};

QTEST_MAIN(SettingsTests)
#include "test_settings.moc"
