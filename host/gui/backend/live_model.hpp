#pragma once
// The Layout detail's live half, computed from the device snapshot and the
// latest subscription sample only: which refs the selected node subscribes,
// signal rows, loop statistics, source states and the action rules. Every rule
// comes from the discovered descriptors (kinds, generated source signals,
// action placement and blob direction); pure functions are tested without a
// device.
#include <drivelink/model.hpp>
#include <QAbstractListModel>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <array>
#include <optional>

namespace drivelink::gui {

// The age a source reports when it holds no valid sample.
inline constexpr uint32_t kNoSample=0xFFFFFFFF;
// A source is stale after this many periods of its producer's loop.
inline constexpr uint32_t kStalePeriods=3;
// The live subscription runs at about this rate (divider = loop rate / 10).
inline constexpr uint32_t kLiveRateHz=10;

// Why an instance or loop is not running now (empty when it is): pending add,
// failed, offline, not in a running loop, safe mode.
QString notRunning(const DeviceSnapshot &, InstanceKey);

// The selected node's subscription. An instance: its sources' generated
// `.value` / `.age_us` signals first, then its own signals in id order;
// signals beyond `entryLimit` are left out and counted in `hidden` (a source
// pair that does not fit shows "Not shown" on its row). A loop: ticks, overruns, last_run_us and max_run_us of its own id.
struct LivePlan {
    uint8_t loop=0;
    uint16_t divider=1;
    std::vector<Ref> refs;
    int hidden=0;
    bool operator==(const LivePlan &) const=default;
};
// Empty when the node is not running or has nothing to subscribe.
std::optional<LivePlan> livePlan(const DeviceSnapshot &, InstanceKey, size_t entryLimit);

// The latest sample of the live subscription, its values in plan order.
struct LiveValues {
    std::vector<uint32_t> values;
    // A new sample arrived within the freshness window.
    bool fresh=false;
};

// Signal rows of an instance (generated source signals excluded): name, unit,
// value text ("" until a sample arrives) and whether it is subscribed.
QVariantList signalRows(const DeviceSnapshot &, InstanceKey, const LivePlan *, const LiveValues *);

// A loop's statistics: ticks, overruns, last/longest run and budget (last run
// over the period). `has` is false until a sample arrives.
QVariantMap loopStatistics(const DeviceSnapshot &, uint8_t loop, const LivePlan *, const LiveValues *);

// A source row's live state. state: "notConnected", "pending" (a staged target
// is never shown as live), "missing" (its producer is not running), "unavailable"
// (no telemetry), "waiting" (no valid sample), "stale" (age above three
// producer periods; the last value stays shown), "valid".
struct SourceLive {
    QString state, valueText, ageText, note;
};
SourceLive sourceLive(const DeviceSnapshot &, InstanceKey consumer, const ItemDescriptor &param,
                      std::optional<uint32_t> value, std::optional<uint32_t> ageUs, bool telemetry);
// Every source param of an instance, keyed by item key: {state, valueText,
// ageText, note}, from the subscription's `.value` / `.age_us` samples.
QVariantMap sourceStates(const DeviceSnapshot &, InstanceKey, const LivePlan *, const LiveValues *);
// Three periods of the producer's loop in microseconds (0 when unknown).
uint32_t staleThresholdUs(const DeviceSnapshot &, uint32_t sourceValue);
QString ageText(uint32_t us);

// Actions. Arguments are parsed with each argument's descriptor; a blob is
// hex text (spaces allowed), at most 512 bytes.
struct ParsedArgs {
    std::array<uint32_t,4> bits{};
    QString problem;
    int field=-1; // the argument the problem is about, -1 for none
};
ParsedArgs parseActionArgs(const ItemDescriptor &, const QStringList &texts);
struct ParsedBlob {
    std::optional<QByteArray> bytes;
    QString problem;
};
ParsedBlob parseHexBlob(const QString &text);
QString hexText(const QByteArray &);
// Result values as text, in descriptor order: {name, unit, text}.
QVariantList actionResultRows(const ItemDescriptor &, const std::array<uint32_t,4> &values);

// Why an action cannot run now (empty when it can). `common` is the
// connection's own block (loading, OTA, a configuration action); `armed`
// includes an ARM in progress.
QString actionBlock(const DeviceSnapshot &, InstanceKey, const ItemDescriptor &, bool armed,
                    const QString &common, bool anotherRunning);
// The instance's action rows: {key, name, description, placement, args, results,
// blob, blocked}.
QVariantList actionRows(const DeviceSnapshot &, InstanceKey, bool armed, const QString &common, bool anotherRunning);

// The selected instance's actions as a list model. Rows with the same keys in
// the same order update in place, so an open action form keeps its text
// while state (blocked, running, the last outcome) changes; another node
// resets the rows. Roles: the actionRows fields plus running and result.
class ActionTable final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ActionTable(QObject *parent=nullptr) : QAbstractListModel(parent) {}
    enum Role { Key=Qt::UserRole+1, Name, Description, Placement, Args, Results, Blob, Blocked, Running, Result };
    int rowCount(const QModelIndex &p={}) const override { return p.isValid()?0:int(m_rows.size()); }
    QVariant data(const QModelIndex &,int) const override;
    QHash<int,QByteArray> roleNames() const override;
    void update(const QVariantList &rows);
    // The row for an item key, as a map of role names (tests).
    QVariantMap row(const QString &key) const;
private:
    std::vector<QVariantMap> m_rows;
};
} // namespace drivelink::gui
