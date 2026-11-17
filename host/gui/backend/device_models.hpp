#pragma once

#include <drivelink/model.hpp>
#include <drivelink/ports.hpp>
#include <QAbstractListModel>
#include <QDateTime>
#include <QVariantList>
#include <optional>

namespace drivelink::gui {

// Readable text for device refusals and codes. Only core/system ids are
// named here; module text comes from the device's descriptors.
QString refusalText(uint8_t result, uint8_t detail);
QString errorText(const Error &);
QString resetReasonText(uint8_t reason);
QString disarmReasonText(uint8_t reason);
QString diagSourceName(const DeviceSnapshot &, uint8_t source);

// Loop statistics read from the GUI's subscription groups.
struct LoopRunTime { uint32_t last_us=0, max_us=0; uint64_t counter=0; };

// The Device page's loop table: one row per loop instance, keyed by loop id
// (ids can have gaps). Budget is computed on the host: run time over period.
class LoopTable final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit LoopTable(QObject *parent=nullptr) : QAbstractListModel(parent) {}
    enum Role { LoopId=Qt::UserRole+1, Name, RateHz, Realtime, Core, Running, State, LastRunUs,
                MaxRunUs, Budget, Overruns, HasRunTime, RunTimeNote };
    int rowCount(const QModelIndex &p={}) const override { return p.isValid()?0:int(m_rows.size()); }
    QVariant data(const QModelIndex &,int) const override;
    QHash<int,QByteArray> roleNames() const override;
    void reset(const std::shared_ptr<const DeviceSnapshot> &);
    void setStatus(const StatusFrame *);
    void setRunTime(uint8_t loop, std::optional<LoopRunTime>, const QString &note={});
    // Pure helper, tested: fraction of the period spent running (0 when unknown).
    static double budget(uint32_t run_us, uint32_t rate_hz);
private:
    struct Row {
        uint8_t id=0, state=0; QString name, core; uint32_t rate=0; bool realtime=false, running=false;
        std::optional<uint32_t> overruns; std::optional<LoopRunTime> run; QString note;
    };
    std::vector<Row> m_rows;
    static void setStatusRows(std::vector<Row> &, const StatusFrame *);
};

// Diagnostic entries with the device's own code names, severities and flags.
class FaultList final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit FaultList(QObject *parent=nullptr) : QAbstractListModel(parent) {}
    enum Role { Source=Qt::UserRole+1, SourceName, InstanceId, Code, Name, Severity, Kind, Disarms,
                BlocksArm, Active, Occurred, Count, InstanceLabel };
    int rowCount(const QModelIndex &p={}) const override { return p.isValid()?0:int(m_rows.size()); }
    QVariant data(const QModelIndex &,int) const override;
    QHash<int,QByteArray> roleNames() const override;
    void reset(const std::shared_ptr<const DeviceSnapshot> &);
    int activeCount() const;
private:
    struct Row { DiagEntry entry; QString source, name, label; uint8_t severity=0, kind=0, flags=0; };
    std::vector<Row> m_rows;
};

// Recently connected devices keyed by device id (QSettings, newest first).
class RecentDevices final {
public:
    static constexpr int kLimit=8;
    // An empty path uses the application's configuration directory.
    explicit RecentDevices(QString path={});
    QVariantList list() const;
    void remember(const QString &id, const QString &name, const QString &endpoint, int baud,
                  const QString &firmware, const QDateTime &when=QDateTime::currentDateTimeUtc());
    void forget(const QString &id);
    // The most recent device seen on an endpoint, for the port list.
    QVariantMap onEndpoint(const QString &endpoint) const;
    // The last firmware update this GUI ran on a device (Firmware card).
    void recordUpdate(const QString &id, const QString &text);
    QString lastUpdate(const QString &id) const;
    QString path() const { return m_path; }
private:
    QString m_path;
    QVariantList m_cache; // read once; every write refreshes it
    QVariantList read() const;
    void write(const QVariantList &);
};

// Ports from libdrivelink plus the device last seen on each one.
QVariantList portRows(const std::vector<SerialPortInfo> &, const RecentDevices &, const QString &activeEndpoint);
} // namespace drivelink::gui
