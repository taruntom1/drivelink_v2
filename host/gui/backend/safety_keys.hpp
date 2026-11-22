#pragma once
#include <QObject>
#include <QPointer>
#include <QHash>
#include <QVariant>

namespace drivelink::gui {
// Installed on the application, before QML can accept shortcut/key events.
class SafetyKeys final : public QObject {
    Q_OBJECT
public:
    explicit SafetyKeys(QObject *parent=nullptr);
    bool eventFilter(QObject *,QEvent *) override;
    // DISARM by mouse cancels an open text edit like Esc does: the focused
    // editor gets its text back, so a later focus loss sends nothing.
    Q_INVOKABLE void cancelEdit();
    // A committed edit (Enter) becomes the text Esc and DISARM restore.
    Q_INVOKABLE void editCommitted();
signals:
    void escapeRequested();
    void pageRequested(int page);
private:
    QPointer<QObject> m_editor;
    QVariant m_before;
    bool m_escapeDown=false;
    void remember(QObject *);
};
}
