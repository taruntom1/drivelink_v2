#pragma once
#include <QObject>
#include <QString>

namespace drivelink::gui {

// GUI-wide settings and the About facts (H2g). The theme is kept in the GUI's
// INI file (the recent devices' file, so --settings isolates it too) under
// appearance/theme. Nothing here touches a device.
class AppSettings : public QObject {
    Q_OBJECT
    // "dark", "light" or "system" (follows the OS colour scheme; dark when the
    // OS reports none).
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY themeChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    Q_PROPERTY(QString compiler READ compiler CONSTANT)
    Q_PROPERTY(QString licence READ licence CONSTANT)
    Q_PROPERTY(QString notices READ notices CONSTANT)
public:
    // An empty path is the GUI's default INI file (RecentDevices' location).
    explicit AppSettings(QString path={}, QObject *parent=nullptr);
    QString theme() const {return m_theme;}
    // Unknown values are ignored. Saved at once.
    void setTheme(const QString &theme);
    bool dark() const;
    QString version() const;
    QString qtVersion() const;
    QString compiler() const;
    QString licence() const;
    QString notices() const;
    QString path() const {return m_path;}
    // Screenshot mode switches themes without saving them.
    void setPersistent(bool persistent) {m_persistent=persistent;}
    Q_INVOKABLE void copyText(const QString &text) const;
    static bool validTheme(const QString &theme);
    // Testing seam: what "system" resolves to; unset follows the OS.
    void setSystemDarkForTest(int dark) {m_systemOverride=dark;emit themeChanged();}
signals:
    void themeChanged();
private:
    QString m_path,m_theme;
    int m_systemOverride=-1;
    bool m_persistent=true;
};

}
