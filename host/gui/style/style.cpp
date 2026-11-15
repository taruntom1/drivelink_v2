#include "style.hpp"

#include <QColor>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSvgRenderer>

#include <mutex>

namespace drivelink::gui {
namespace {

// image://dlicon/<lucide name>/<rrggbb>: the bundled SVG with its
// currentColor stroke replaced, rendered at the requested size (16 px default).
class IconProvider final : public QQuickImageProvider
{
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override
    {
        const auto slash = id.lastIndexOf(u'/');
        const QString name = slash < 0 ? id : id.left(slash);
        const QColor color(slash < 0 ? QStringLiteral("#000000") : u'#' + id.mid(slash + 1));
        QFile file(QStringLiteral(":/DriveLink/icons/%1.svg").arg(name));
        if (!color.isValid() || !bundledIconNames().contains(name) || !file.open(QIODevice::ReadOnly)) {
            qWarning("dlicon: unknown icon or colour '%s'", qPrintable(id));
            return {};
        }
        QByteArray svg = file.readAll();
        svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
        QSvgRenderer renderer(svg);
        const QSize out(requested.width() > 0 ? requested.width() : 16,
                        requested.height() > 0 ? requested.height() : 16);
        QImage image(out, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter);
        painter.end();
        if (size)
            *size = out;
        return image;
    }
};

bool registerFonts()
{
    static const char *const fonts[] = {
        "Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf",
        "JetBrainsMono-Regular.ttf", "JetBrainsMono-Medium.ttf"};
    bool ok = true;
    for (const char *font : fonts) {
        if (QFontDatabase::addApplicationFont(QStringLiteral(":/DriveLink/fonts/") + QLatin1String(font)) < 0) {
            qWarning("DriveLink.Style: cannot load bundled font %s", font);
            ok = false;
        }
    }
    return ok;
}

} // namespace

bool installStyle(QQmlEngine &engine)
{
    static std::once_flag once;
    static bool fonts = false;
    std::call_once(once, [] { fonts = registerFonts(); });
    if (!engine.imageProvider(QStringLiteral("dlicon")))
        engine.addImageProvider(QStringLiteral("dlicon"), new IconProvider);
    return fonts;
}

QStringList bundledIconNames()
{
    static const QStringList names = QString::fromLatin1(DRIVELINK_ICONS).split(u',');
    return names;
}

} // namespace drivelink::gui
