#pragma once
#include "models.hpp"
#include <QElapsedTimer>
#include <QTimer>
#include <thread>

namespace drivelink::gui {
struct GuiDispatch;
// QML-facing interface; a later agent connection can implement the same surface.
class Connection : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool linked READ linked NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool closing READ closing NOTIFY changed)
    Q_PROPERTY(bool linkLost READ linkLost NOTIFY changed)
    Q_PROPERTY(bool armEnabled READ armEnabled NOTIFY changed)
    Q_PROPERTY(bool arming READ arming NOTIFY changed)
    Q_PROPERTY(int armState READ armState NOTIFY changed)
    Q_PROPERTY(bool safeMode READ safeMode NOTIFY changed)
    Q_PROPERTY(bool unsaved READ unsaved NOTIFY changed)
    Q_PROPERTY(bool pendingReboot READ pendingReboot NOTIFY changed)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY changed)
    Q_PROPERTY(QString deviceId READ deviceId NOTIFY changed)
    Q_PROPERTY(QString endpoint READ endpoint NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
    Q_PROPERTY(QString armBlockedReason READ armBlockedReason NOTIFY changed)
    Q_PROPERTY(QVariantMap lastError READ lastError NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel* instances READ instances CONSTANT)
public:
    using QObject::QObject;
    virtual bool linked() const=0;
    virtual bool loading() const=0;
    virtual bool ready() const=0;
    virtual bool closing() const=0;
    virtual bool linkLost() const=0;
    virtual bool armEnabled() const=0;
    virtual bool arming() const=0;
    virtual int armState() const=0;
    virtual bool safeMode() const=0;
    virtual bool unsaved() const=0;
    virtual bool pendingReboot() const=0;
    virtual QString deviceName() const=0;
    virtual QString deviceId() const=0;
    virtual QString endpoint() const=0;
    virtual QString reason() const=0;
    virtual QString armBlockedReason() const=0;
    virtual QVariantMap lastError() const=0;
    virtual QAbstractItemModel *instances()=0;
    Q_INVOKABLE virtual void connectTo(const QString &endpoint)=0;
    Q_INVOKABLE virtual void disconnectDevice()=0;
    Q_INVOKABLE virtual void arm()=0;
    Q_INVOKABLE virtual void disarm()=0;
    Q_INVOKABLE virtual void acknowledge()=0;
signals:
    void changed();
    void notification(QString text,int tone);
    void disconnected();
};

class LocalConnection final : public Connection {
    Q_OBJECT
public:
    explicit LocalConnection(QObject *parent=nullptr);
    ~LocalConnection() override;
    bool linked() const override;
    bool loading() const override { return m_loading; }
    bool ready() const override;
    bool closing() const override {return m_closing;}
    bool linkLost() const override {return m_linkLost;}
    bool armEnabled() const override;
    bool arming() const override {return m_arming;}
    int armState() const override;
    bool safeMode() const override;
    bool unsaved() const override;
    bool pendingReboot() const override;
    QString deviceName() const override;
    QString deviceId() const override;
    QString endpoint() const override {return m_endpoint;}
    QString reason() const override;
    QString armBlockedReason() const override;
    QVariantMap lastError() const override {return m_error;}
    QAbstractItemModel *instances() override {return &m_instances;}
    void connectTo(const QString &) override;
    void disconnectDevice() override;
    void arm() override;
    void disarm() override;
    void acknowledge() override;
    std::shared_ptr<const DeviceSnapshot> snapshot() const {return m_snapshot;}
    // Tests/next screens can select descriptor rows without adding module rules.
    void selectRows(DescriptorRows &,uint8_t type,uint8_t instance,bool pending=false) const;
private:
    struct Context;
    std::shared_ptr<Context> m_context;
    std::thread m_opener;
    std::shared_ptr<GuiDispatch> m_dispatch;
    std::shared_ptr<const DeviceSnapshot> m_snapshot;
    InstanceTree m_instances;
    QTimer m_timer;
    QElapsedTimer m_seen,m_closeElapsed;
    QString m_endpoint,m_blocked;
    QVariantMap m_error;
    bool m_loading=false,m_closing=false,m_groupsReady=false,m_setupPending=false;
    bool m_arming=false,m_armPending=false,m_disarmPending=false,m_disarmAgain=false;
    bool m_ackPending=false,m_linkLost=false,m_waitingDisarm=false;
    bool m_observingArmed=false,m_explicitDisarmAcknowledged=false;
    bool m_disarmWhenOpened=false;
    uint64_t m_setupGeneration=0;
    bool m_setupAttempted=false;
    std::optional<uint16_t> m_statusCounter;
    void refresh(const std::shared_ptr<Context> &,bool metadataEvent=false);
    void setupGroups();
    void report(const Error &);
    void finishClose(bool lost=false);
    void clearAndClose();
    bool current(const std::shared_ptr<Context> &) const;
};
} // namespace drivelink::gui
