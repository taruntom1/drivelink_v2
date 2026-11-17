#pragma once
#include "device_models.hpp"
#include "models.hpp"
#include "ota_model.hpp"
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <functional>
#include <atomic>
#include <thread>

namespace drivelink::gui {
struct GuiDispatch;
// QML-facing interface; a later agent connection can implement the same surface.
class Connection : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool linked READ linked NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(int loadStep READ loadStep NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool closing READ closing NOTIFY changed)
    Q_PROPERTY(bool linkLost READ linkLost NOTIFY changed)
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY changed)
    Q_PROPERTY(bool armEnabled READ armEnabled NOTIFY changed)
    Q_PROPERTY(bool arming READ arming NOTIFY changed)
    Q_PROPERTY(int armState READ armState NOTIFY changed)
    Q_PROPERTY(bool ackEnabled READ ackEnabled NOTIFY changed)
    Q_PROPERTY(QString ackBlockedReason READ ackBlockedReason NOTIFY changed)
    Q_PROPERTY(bool safeMode READ safeMode NOTIFY changed)
    Q_PROPERTY(bool unsaved READ unsaved NOTIFY changed)
    Q_PROPERTY(bool pendingReboot READ pendingReboot NOTIFY changed)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY changed)
    Q_PROPERTY(QString deviceId READ deviceId NOTIFY changed)
    Q_PROPERTY(QString endpoint READ endpoint NOTIFY changed)
    Q_PROPERTY(int baud READ baud NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
    Q_PROPERTY(QString armBlockedReason READ armBlockedReason NOTIFY changed)
    Q_PROPERTY(QVariantList armChecks READ armChecks NOTIFY changed)
    Q_PROPERTY(QVariantMap lastError READ lastError NOTIFY changed)
    Q_PROPERTY(QVariantMap mismatch READ mismatch NOTIFY changed)
    Q_PROPERTY(QVariantMap info READ info NOTIFY changed)
    Q_PROPERTY(QVariantList limits READ limits NOTIFY changed)
    Q_PROPERTY(QVariantList cpuLoad READ cpuLoad NOTIFY changed)
    Q_PROPERTY(int activeFaults READ activeFaults NOTIFY changed)
    Q_PROPERTY(int faultHistory READ faultHistory NOTIFY changed)
    Q_PROPERTY(bool configBusy READ configBusy NOTIFY changed)
    Q_PROPERTY(QString configAction READ configAction NOTIFY changed)
    Q_PROPERTY(QVariantMap actionBlocked READ actionBlocked NOTIFY changed)
    Q_PROPERTY(QStringList unsavedChanges READ unsavedChanges NOTIFY changed)
    Q_PROPERTY(QVariantMap rebootNotice READ rebootNotice NOTIFY changed)
    Q_PROPERTY(QVariantList recentDevices READ recentDevices NOTIFY recentDevicesChanged)
    // The firmware update dialog (empty when closed) and the Firmware card.
    Q_PROPERTY(QVariantMap ota READ ota NOTIFY changed)
    Q_PROPERTY(QVariantMap firmware READ firmware NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel* instances READ instances CONSTANT)
    Q_PROPERTY(QAbstractItemModel* loops READ loops CONSTANT)
    Q_PROPERTY(QAbstractItemModel* faults READ faults CONSTANT)
public:
    using QObject::QObject;
    virtual bool linked() const=0;
    virtual bool loading() const=0;
    virtual int loadStep() const=0;
    virtual bool ready() const=0;
    virtual bool closing() const=0;
    virtual bool linkLost() const=0;
    virtual bool reconnecting() const=0;
    virtual bool armEnabled() const=0;
    virtual bool arming() const=0;
    virtual int armState() const=0;
    virtual bool ackEnabled() const=0;
    virtual QString ackBlockedReason() const=0;
    virtual bool safeMode() const=0;
    virtual bool unsaved() const=0;
    virtual bool pendingReboot() const=0;
    virtual QString deviceName() const=0;
    virtual QString deviceId() const=0;
    virtual QString endpoint() const=0;
    virtual int baud() const=0;
    virtual QString reason() const=0;
    virtual QString armBlockedReason() const=0;
    virtual QVariantList armChecks() const=0;
    virtual QVariantMap lastError() const=0;
    virtual QVariantMap mismatch() const=0;
    virtual QVariantMap info() const=0;
    virtual QVariantList limits() const=0;
    virtual QVariantList cpuLoad() const=0;
    virtual int activeFaults() const=0;
    virtual int faultHistory() const=0;
    virtual bool configBusy() const=0;
    virtual QString configAction() const=0;
    virtual QVariantMap actionBlocked() const=0;
    virtual QStringList unsavedChanges() const=0;
    virtual QVariantMap rebootNotice() const=0;
    virtual QVariantList recentDevices() const=0;
    virtual QVariantMap ota() const=0;
    virtual QVariantMap firmware() const=0;
    virtual QAbstractItemModel *instances()=0;
    virtual QAbstractItemModel *loops()=0;
    virtual QAbstractItemModel *faults()=0;
    Q_INVOKABLE virtual void connectTo(const QString &endpoint,int baud=921600)=0;
    Q_INVOKABLE virtual void retry()=0;
    Q_INVOKABLE virtual void disconnectDevice()=0;
    Q_INVOKABLE virtual void arm()=0;
    Q_INVOKABLE virtual void disarm()=0;
    Q_INVOKABLE virtual void acknowledge()=0;
    Q_INVOKABLE virtual void rename(const QString &name)=0;
    Q_INVOKABLE virtual void save()=0;
    Q_INVOKABLE virtual void revert()=0;
    Q_INVOKABLE virtual void reboot()=0;
    Q_INVOKABLE virtual void saveAndReboot()=0;
    Q_INVOKABLE virtual void factoryReset()=0;
    Q_INVOKABLE virtual void exitSafeMode()=0;
    Q_INVOKABLE virtual void dismissRebootNotice()=0;
    Q_INVOKABLE virtual QVariantList serialPorts()=0;
    Q_INVOKABLE virtual void forgetRecent(const QString &id)=0;
    // Firmware update (OTA). inspectImage takes a path or a file:// URL and
    // returns {ok, problem} or the image's description; it becomes the image
    // startUpdate writes. The update runs once: REBOOT is never repeated, and
    // CONFIRM follows only the full identity check (never with confirm=false).
    Q_INVOKABLE virtual QVariantMap inspectImage(const QString &fileOrUrl)=0;
    Q_INVOKABLE virtual void startUpdate(bool confirm=true)=0;
    Q_INVOKABLE virtual void abortUpdate()=0;
    Q_INVOKABLE virtual void closeUpdate()=0;
    // Explicit user actions on the running/prepared image (Firmware card and
    // the dialog's unconfirmed state). Each request is sent once.
    Q_INVOKABLE virtual void confirmFirmware()=0;
    Q_INVOKABLE virtual void rollBackFirmware()=0;
    Q_INVOKABLE virtual void abortPreparedUpdate()=0;
    Q_INVOKABLE void copyToClipboard(const QString &text) const;
    // Empty when the device would accept the name (SET_NAME rules).
    Q_INVOKABLE QString nameProblem(const QString &name) const;
signals:
    void changed();
    void notification(QString text,int tone);
    void disconnected();
    // The device refused ARM; the shell shows the refusal under the Arm button.
    void armRefused(QString reason);
    // Separate from changed(): the list only changes on connect/rename/forget.
    void recentDevicesChanged();
};

class LocalConnection final : public Connection {
    Q_OBJECT
public:
    explicit LocalConnection(QObject *parent=nullptr);
    ~LocalConnection() override;
    bool linked() const override;
    bool loading() const override { return m_loading; }
    int loadStep() const override { return m_loadStep; }
    bool ready() const override;
    bool closing() const override {return m_closing;}
    bool linkLost() const override {return m_linkLost;}
    bool reconnecting() const override {return m_reconnecting;}
    bool armEnabled() const override;
    bool arming() const override {return m_arming;}
    int armState() const override;
    bool ackEnabled() const override;
    QString ackBlockedReason() const override;
    bool safeMode() const override;
    bool unsaved() const override;
    bool pendingReboot() const override;
    QString deviceName() const override;
    QString deviceId() const override;
    QString endpoint() const override {return m_endpoint;}
    int baud() const override {return m_baud;}
    QString reason() const override;
    QString armBlockedReason() const override;
    QVariantList armChecks() const override;
    QVariantMap lastError() const override {return m_error;}
    QVariantMap mismatch() const override {return m_mismatch;}
    QVariantMap info() const override;
    QVariantList limits() const override;
    QVariantList cpuLoad() const override;
    int activeFaults() const override;
    int faultHistory() const override;
    bool configBusy() const override {return !m_configAction.isEmpty();}
    QString configAction() const override {return m_configAction;}
    QVariantMap actionBlocked() const override;
    QStringList unsavedChanges() const override;
    QVariantMap rebootNotice() const override {return m_notice;}
    QVariantList recentDevices() const override {return m_recent.list();}
    QVariantMap ota() const override;
    QVariantMap firmware() const override;
    QAbstractItemModel *instances() override {return &m_instances;}
    QAbstractItemModel *loops() override {return &m_loops;}
    QAbstractItemModel *faults() override {return &m_faults;}
    void connectTo(const QString &,int baud=921600) override;
    void retry() override;
    void disconnectDevice() override;
    void arm() override;
    void disarm() override;
    void acknowledge() override;
    void rename(const QString &) override;
    void save() override;
    void revert() override;
    void reboot() override;
    void saveAndReboot() override;
    void factoryReset() override;
    void exitSafeMode() override;
    void dismissRebootNotice() override;
    QVariantList serialPorts() override;
    void forgetRecent(const QString &id) override;
    QVariantMap inspectImage(const QString &fileOrUrl) override;
    void startUpdate(bool confirm=true) override;
    void abortUpdate() override;
    void closeUpdate() override;
    void confirmFirmware() override;
    void rollBackFirmware() override;
    void abortPreparedUpdate() override;
    // Why an update cannot start now (empty when it can).
    QString updateBlocked() const;
    std::optional<OtaStatus> otaStatus() const {return m_otaStatus;}
    // Tests and screenshots: hold the upload once it reaches `percent`
    // (-1 releases). Abort and close still end a held upload.
    void setOtaPauseAt(int percent) {m_otaPause->store(percent);}
    // Tests and screenshots keep their recent devices apart from the user's.
    void setSettingsPath(const QString &path) {m_recent=RecentDevices(path);emit recentDevicesChanged();}
    // The reconnect window after a requested reboot (default 30 s).
    void setReconnectWindow(int ms) {m_reconnectWindowMs=ms;}
    std::shared_ptr<const DeviceSnapshot> snapshot() const {return m_snapshot;}
    // Tests/next screens can select descriptor rows without adding module rules.
    void selectRows(DescriptorRows &,uint8_t type,uint8_t instance,bool pending=false) const;
    // Groups this connection configured on the device, for tests.
    std::vector<uint8_t> ownedSubscriptions() const;
    // Tests and the bench check: run a device operation on this connection's
    // service executor; `done` receives the error text (empty on success) on
    // the GUI thread. Returns false without a session.
    bool runOnDevice(std::function<void(Client &)> operation,std::function<void(QString)> done);
private:
    struct Context;
    enum class Op {Rename,Save,Revert,Reboot,SaveAndReboot,FactoryReset,ExitSafeMode};
    // Firmware update state (connection_ota.cpp).
    std::optional<OtaJob> m_ota;
    std::shared_ptr<const OtaImage> m_chosenImage;
    QString m_chosenFile;
    std::optional<OtaStatus> m_otaStatus;
    uint64_t m_otaStatusSerial=0,m_otaStatusGeneration=0,m_otaRun=0,m_connectionSerial=0;
    uint64_t m_otaRebootSerial=0,m_otaRebootGeneration=0;
    bool m_otaStatusPending=false;
    std::shared_ptr<std::atomic<bool>> m_otaCancel=std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<int>> m_otaPause=std::make_shared<std::atomic<int>>(-1);
    // Candidates this GUI uploaded, per device id, for confirmation with the
    // identity check after the dialog was closed.
    QHash<QString,OtaCandidate> m_candidates;
    void readOtaStatus();
    void otaReboot();
    void otaRefresh();
    void otaConfirm(bool explicitRequest);
    void otaStop(OtaJob::Phase phase,const QString &message);
    void otaInterrupted(const QString &why);
    QString firmwareActionBlocked() const;
    std::shared_ptr<Context> m_context;
    std::thread m_opener;
    std::shared_ptr<GuiDispatch> m_dispatch;
    std::shared_ptr<const DeviceSnapshot> m_snapshot;
    InstanceTree m_instances;
    LoopTable m_loops;
    FaultList m_faults;
    RecentDevices m_recent;
    QTimer m_timer;
    QElapsedTimer m_seen,m_closeElapsed,m_lossElapsed,m_reconnectElapsed;
    QString m_endpoint,m_blocked,m_configAction,m_subsNote,m_recordedName;
    QVariantMap m_error,m_mismatch,m_notice;
    // Changes this GUI made since the last SAVE, per device id.
    QHash<QString,QStringList> m_changes;
    int m_baud=921600,m_loadStep=0,m_reconnectWindowMs=30000,m_pollTick=0;
    bool m_loading=false,m_closing=false,m_groupsReady=false,m_setupPending=false;
    bool m_arming=false,m_armPending=false,m_disarmPending=false,m_disarmAgain=false;
    bool m_ackPending=false,m_linkLost=false,m_waitingDisarm=false;
    bool m_observingArmed=false,m_explicitDisarmAcknowledged=false;
    bool m_disarmWhenOpened=false,m_subsPending=false,m_subsAttempted=false;
    bool m_reconnecting=false,m_expectReboot=false,m_afterLoss=false,m_recorded=false;
    QString m_rebootCause;
    // What the previous connection knew, for the reboot notice after reconnect.
    struct Previous { QString id,name; bool dirty=false; std::optional<uint64_t> uptimeUs; };
    struct StaleSub { uint8_t id,loop,epoch; std::vector<Ref> refs; };
    std::vector<StaleSub> m_staleSubs;
    QString m_staleId;
    QElapsedTimer m_rebootElapsed,m_diagElapsed;
    bool m_savedBeforeReboot=false;
    void clearRebootExpectation();
    std::optional<Previous> m_previous;
    std::optional<DiagInfo> m_diag;
    uint64_t m_setupGeneration=0,m_subsGeneration=0;
    bool m_setupAttempted=false;
    std::optional<uint16_t> m_statusCounter;
    void refresh(const std::shared_ptr<Context> &,bool metadataEvent=false);
    void setupGroups();
    void setupLoopStats();
    void pollLoopStats();
    void runOp(Op,const QString &name={});
    void showRebootNotice(std::optional<uint8_t> reason,bool dirty,const QString &deviceId);
    void reconnectAfterReboot();
    void report(const Error &);
    void finishClose(bool lost=false);
    void clearAndClose();
    bool current(const std::shared_ptr<Context> &) const;
    QString commonBlock() const;
};
} // namespace drivelink::gui
