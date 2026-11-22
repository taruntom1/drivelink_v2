// drivelink-style-gallery: shows every DriveLink.Style control. With --out DIR
// it renders the gallery offscreen in Dark and Light to DIR/gallery-<theme>.png
// and exits; H2a-2 grows this into the screens screenshot tool.
#include "style.hpp"

#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickWindow>

#include <cstdio>

namespace {

bool outputRequested(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--out") == 0 || qstrncmp(argv[i], "--out=", 6) == 0)
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    // Renders without a display or GPU (CI, WSL). An explicit setting wins.
    if (outputRequested(argc, argv)) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
        if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
            qputenv("QT_QUICK_BACKEND", "software");
    }
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"out", "Render Dark and Light PNGs into <dir> and exit.", "dir"});
    parser.process(app);

    QQmlApplicationEngine engine;
    if (!drivelink::gui::installStyle(engine))
        return 2;
    engine.loadFromModule("DriveLink.Gallery", "Gallery");
    if (engine.rootObjects().isEmpty())
        return 2;
    if (!parser.isSet("out"))
        return QGuiApplication::exec();

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto *theme = engine.singletonInstance<QObject *>("DriveLink.Style", "Theme");
    const QDir dir(parser.value("out"));
    if (!window || !theme || !dir.mkpath(".")) {
        std::fprintf(stderr, "gallery: cannot set up rendering into %s\n", qPrintable(dir.path()));
        return 2;
    }
    for (const bool dark : {true, false}) {
        theme->setProperty("dark", dark);
        QCoreApplication::processEvents();
        const QImage image = window->grabWindow();
        const QString path = dir.filePath(dark ? "gallery-dark.png" : "gallery-light.png");
        if (image.isNull() || !image.save(path)) {
            std::fprintf(stderr, "gallery: cannot write %s\n", qPrintable(path));
            return 1;
        }
        std::printf("%s %dx%d\n", qPrintable(path), image.width(), image.height());
    }
    return 0;
}
