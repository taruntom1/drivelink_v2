#include "app_settings.hpp"
#include "device_models.hpp"
#include "drivelink_version.hpp"
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QStyleHints>

namespace drivelink::gui {
namespace {
const QString kThemeKey=QStringLiteral("appearance/theme");

QString resourceText(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly)?QString::fromUtf8(file.readAll()):QString();
}
}

AppSettings::AppSettings(QString path, QObject *parent)
    :QObject(parent),m_path(path.isEmpty()?RecentDevices().path():std::move(path)) {
    const auto saved=QSettings(m_path,QSettings::IniFormat).value(kThemeKey).toString();
    m_theme=validTheme(saved)?saved:QStringLiteral("dark");
    if(auto *hints=QGuiApplication::styleHints()) {
        connect(hints,&QStyleHints::colorSchemeChanged,this,[this] {if(m_theme==QLatin1String("system")) {emit themeChanged();}});
    }
}

bool AppSettings::validTheme(const QString &theme) {
    return theme==QLatin1String("dark") || theme==QLatin1String("light") || theme==QLatin1String("system");
}

void AppSettings::setTheme(const QString &theme) {
    if(!validTheme(theme) || theme==m_theme) {return;}
    m_theme=theme;
    if(m_persistent) {QSettings(m_path,QSettings::IniFormat).setValue(kThemeKey,m_theme);}
    emit themeChanged();
}

bool AppSettings::dark() const {
    if(m_theme==QLatin1String("light")) {return false;}
    if(m_theme!=QLatin1String("system")) {return true;}
    if(m_systemOverride>=0) {return m_systemOverride!=0;}
    const auto *hints=QGuiApplication::styleHints();
    return !hints || hints->colorScheme()!=Qt::ColorScheme::Light;
}

QString AppSettings::version() const {return QStringLiteral(DRIVELINK_VERSION_FULL);}
QString AppSettings::qtVersion() const {return QString::fromLatin1(qVersion());}

QString AppSettings::compiler() const {
#if defined(__clang__)
    return QStringLiteral("Clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
#elif defined(__GNUC__)
#if defined(__MINGW64__)
    return QStringLiteral("MinGW-w64 GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#else
    return QStringLiteral("GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#endif
#else
    return QStringLiteral("unknown compiler");
#endif
}

// Both files are compiled into the backend's resources (backend CMake).
QString AppSettings::licence() const {return resourceText(QStringLiteral(":/drivelink/about/LICENSE"));}
QString AppSettings::notices() const {return resourceText(QStringLiteral(":/drivelink/about/THIRD_PARTY_NOTICES.txt"));}

void AppSettings::copyText(const QString &text) const {
    if(auto *clipboard=QGuiApplication::clipboard()) {clipboard->setText(text);}
}

}
