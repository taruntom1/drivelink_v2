#pragma once
// Monitor's Manual command card (H2d-2), QML `connection.command`: the
// commanded inputs as an in-place list model (a drag must survive updates),
// the inputs that can be added (the rest disabled with the reason), and the
// card's actions. The rules are in CommandModel; this object adds the clock,
// the 20 ms pump that writes frames through the connection, and the limits
// remembered per input in the app settings.
#include "command_model.hpp"
#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <memory>
#include <span>

namespace drivelink::gui {

class CommandView;
class CommandRows final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { Key=Qt::UserRole+1, Name, Owner, Unit, From, To, Limit, Bound, Value, Step, Decimals,
                Latched, Held, Ramping, Writing };
    explicit CommandRows(CommandView &view) : m_view(view) {}
    int rowCount(const QModelIndex &p={}) const override;
    QVariant data(const QModelIndex &, int) const override;
    QHash<int,QByteArray> roleNames() const override;
private:
    friend class CommandView;
    CommandView &m_view;
};

class CommandView final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* rows READ rows CONSTANT)
    // {key, name, owner, unit, enabled, reason} for every running input.
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY changed)
    // Commands are accepted (armed by this window, groups ready).
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    // Why not, when not enabled.
    Q_PROPERTY(QString blocked READ blocked NOTIFY changed)
    // Some input is away from 0 (Hold all has something to do).
    Q_PROPERTY(bool commanding READ commanding NOTIFY valuesChanged)
public:
    // Queues one value frame on a value group; false when it could not.
    using Writer=std::function<bool(uint8_t group, std::span<const uint32_t> bits)>;
    explicit CommandView(QObject *parent=nullptr);
    QAbstractItemModel *rows() { return &m_rows; }
    QVariantList candidates() const;
    bool enabled() const { return m_model.enabled(); }
    QString blocked() const { return m_blocked; }
    bool commanding() const;

    void setWriter(Writer writer) { m_writer=std::move(writer); }
    void setSettingsPath(const QString &path) { m_settingsPath=path; }
    // The device and its descriptors (candidates, owners). Another device
    // forgets the card's inputs.
    void setSnapshot(std::shared_ptr<const DeviceSnapshot>, const QString &deviceId);
    // The value groups set up at connect. The card keeps its inputs that
    // are still commandable (a Boot or reconnect of the same device).
    void configure(const std::vector<CommandGroupSpec> &);
    // The groups are gone (Boot, a lost link): no input can be commanded.
    void clearGroups();
    // An explicit close or another device: the card empties too.
    void reset();
    // Empty when commands are accepted; else the reason, and every value
    // returns to 0 locally.
    void setBlocked(const QString &reason);
    void releaseHeld();
    // The instance has inputs in a value group.
    bool commands(uint8_t type, uint8_t instance) const;
    // Around a live param write to the instance (see CommandModel).
    void beginWrite(uint8_t type, uint8_t instance);
    void endWrite(uint8_t type, uint8_t instance);
    const CommandModel &model() const { return m_model; }
    // Tests: frames are written at once instead of on the 20 ms timer.
    void pumpNow() { pump(); }

    Q_INVOKABLE void add(const QString &key);
    Q_INVOKABLE void remove(const QString &key);
    Q_INVOKABLE void press(const QString &key);
    Q_INVOKABLE void drag(const QString &key, double value);
    Q_INVOKABLE void release(const QString &key);
    Q_INVOKABLE void setLatched(const QString &key, bool on);
    // A typed value (only while latched): empty, or why it was not applied.
    Q_INVOKABLE QString typeValue(const QString &key, const QString &text);
    // A typed limit, clamped to the descriptor bound and remembered.
    Q_INVOKABLE QString setLimitText(const QString &key, const QString &text);
    Q_INVOKABLE void holdAll();
signals:
    void changed();
    void valuesChanged();
private:
    friend class CommandRows;
    CommandModel m_model;
    CommandRows m_rows;
    std::vector<Ref> m_order;   // the card's inputs, in the order added
    std::vector<Ref> m_visible; // those of them in the current value groups (the rows)
    std::shared_ptr<const DeviceSnapshot> m_snapshot;
    QString m_deviceId,m_blocked,m_settingsPath;
    Writer m_writer;
    QTimer m_timer;
    QElapsedTimer m_clock;
    int index(const QString &key) const;
    const CommandModel::Input *row(int r) const;
    QString owner(const Ref &) const;
    QString limitKey(const Ref &) const;
    void pump();
    void updateValues();
    void rebuildRows();
};
} // namespace drivelink::gui
