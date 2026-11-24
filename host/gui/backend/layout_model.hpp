#pragma once
// The Layout screen's data, computed from the device snapshot only: every
// rule comes from the discovered descriptors (kinds, flags, loop masks, ref
// categories, generated source signals), never from a module table. Pure
// functions are tested without a device.
#include <drivelink/model.hpp>
#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <optional>

namespace drivelink::gui {

// "type:instance" and "type:instance:item", as the other models use.
QString nodeKey(uint8_t type, uint8_t instance);
QString itemKey(uint8_t type, uint8_t instance, uint16_t item);
std::optional<InstanceKey> parseNodeKey(const QString &);
std::optional<Ref> parseItemKey(const QString &);

// Text for a value of an item, and back. GPIO params show "No pin" for 0xFF;
// enums use the device's names; f32 uses the shortest round-trip form.
QString formatValue(const ModelItem &, uint32_t bits);
struct ParsedValue { std::optional<uint32_t> bits; QString problem; };
ParsedValue parseValueText(const ModelItem &, const QString &text);
QString rangeText(const ModelItem &);
bool isWiring(const ItemDescriptor &);
bool isGpio(const ItemDescriptor &);

// The layout views. The pending view is the effective layout SAVE writes.
const ModelInstance *pendingEntry(const DeviceSnapshot &, InstanceKey);
const ModelInstance *configuredEntry(const DeviceSnapshot &, InstanceKey);
// Pending entry, else the configured one (an instance removed in pending).
const ModelInstance *anyEntry(const DeviceSnapshot &, InstanceKey);
QString typeName(const DeviceSnapshot &, uint8_t type);
QString loopName(const DeviceSnapshot &, uint8_t loop);
QString healthText(uint8_t state);
// "add", "remove", "label", "change" or empty (no staged change).
QString pendingState(const DeviceSnapshot &, InstanceKey);

// A wiring/source value as text: the target's label (and item), "Not wired",
// "Not connected", or the raw reference when it names nothing known.
QString wiringText(const DeviceSnapshot &, uint32_t value);
QString sourceText(const DeviceSnapshot &, uint32_t value);

// Loops for a module type at Add instance. `realtime` is the pending value.
struct LoopChoice { uint8_t id=0; QString name; uint32_t rate=0; bool realtime=false, compatible=false; };
std::vector<LoopChoice> loopChoices(const DeviceSnapshot &, uint8_t type);
// What ADD_INSTANCE with loop 0xFF picks: the lowest-numbered compatible loop.
std::optional<uint8_t> defaultLoop(const DeviceSnapshot &, uint8_t type);
QString typeNeed(uint8_t loopMask);

// Why Remove is refused by the device (Referenced), from the pending layout:
// instances wired to it, sources reading it, instances in a loop. Empty when
// nothing refers to it.
QString removeBlocker(const DeviceSnapshot &, InstanceKey);

struct Candidate { uint32_t value=0; QString text, detail, reason; bool enabled=true; };
// Instances of the param's ref category; other-loop entries disabled with the
// reason. The first entry is "Not wired" (0).
std::vector<Candidate> wiringCandidates(const DeviceSnapshot &, InstanceKey consumer, const ItemDescriptor &param);
// Signals and inputs whose value type matches the generated `<name>.value`
// signal, any loop, the consumer itself excluded. First entry: Disconnect (0).
std::vector<Candidate> sourceCandidates(const DeviceSnapshot &, InstanceKey consumer, const ItemDescriptor &param);

// Every staged change: the running system settings and the configured view
// against the pending ones.
struct PendingChange { QString kind, key, subject, from, to; };
std::vector<PendingChange> pendingChanges(const DeviceSnapshot &);

// Pending counts against limits.loops / limits.instances (0 = not reported).
struct LayoutCounts { int loops=0, loopLimit=0, instances=0, instanceLimit=0; };
LayoutCounts layoutCounts(const DeviceSnapshot &);

// The instance and pin name already holding `pin` in the pending layout, for a
// GpioConflict refusal ("right_enc (pin_a)"); empty when none is found.
QString gpioOwner(const DeviceSnapshot &, Ref self, uint32_t pin);
// A refused layout op or SET in words, with the snapshot naming what is in
// the way where it can (GpioConflict, Referenced).
QString layoutRefusal(const DeviceSnapshot &, const Error &, std::optional<Ref> item={},
                      std::optional<uint32_t> value={}, std::optional<InstanceKey> removed={});

// The Layout tree: one flat row per loop (depth 0) and module (depth 1, or 2
// under the instance wired to it), from both layout views.
class LayoutTree final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit LayoutTree(QObject *parent=nullptr) : QAbstractListModel(parent) {}
    enum Role { Key=Qt::UserRole+1, IsLoop, Depth, Label, TypeName, LoopId, Health, HealthText,
                Pending, RateHz, Realtime, Budget, HasBudget };
    int rowCount(const QModelIndex &p={}) const override { return p.isValid()?0:int(m_rows.size()); }
    QVariant data(const QModelIndex &,int) const override;
    QHash<int,QByteArray> roleNames() const override;
    void reset(const std::shared_ptr<const DeviceSnapshot> &);
    // Budget (run time over period) per loop id, from the Device loop table.
    void setBudgets(const QHash<int,double> &);
    QStringList keys() const;
private:
    struct Row { QString key, label, type, health, pending; uint8_t loop=0, state=0; int depth=0;
                 bool isLoop=false, realtime=false; uint32_t rate=0; };
    std::vector<Row> m_rows;
    QHash<int,double> m_budgets;
};

// A row's edit state, kept by the connection per item key.
struct ParamEdit { QString state; QString note; QString text; };

// The param table of one instance or loop. Wiring and source params are
// listed by their own cards, not here.
class ParamTable final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ParamTable(QObject *parent=nullptr) : QAbstractListModel(parent) {}
    enum Role { Key=Qt::UserRole+1, Name, Group, Unit, Description, Editor, ValueText, PendingText,
                IsPending, Unsaved, Locked, ReadOnly, RebootRequired, RangeText, Enums, ValueBits,
                EditState, EditNote, EditText, Minimum, Maximum, ValueType };
    int rowCount(const QModelIndex &p={}) const override { return p.isValid()?0:int(m_rows.size()); }
    QVariant data(const QModelIndex &,int) const override;
    QHash<int,QByteArray> roleNames() const override;
    // filter: 0 All, 1 Changed (pending or changed by this GUI since SAVE),
    // 2 Pending. `text` matches the name, case-insensitive.
    void reset(const std::shared_ptr<const DeviceSnapshot> &, std::optional<InstanceKey>, bool armed,
               const QHash<QString,ParamEdit> &, const QSet<QString> &changed, int filter, const QString &text);
    // Counts for the filter chips (unfiltered).
    int total() const { return m_total; }
    int changedCount() const { return m_changed; }
    int pendingCount() const { return m_pending; }
private:
    struct Row { QString key, name, group, unit, description, editor, value, pendingValue, range,
                 editState, editNote, editText; QVariantList enums; uint32_t bits=0; int valueType=0;
                 QVariant minimum, maximum;
                 bool pending=false, unsaved=false, locked=false, readOnly=false, reboot=false; };
    std::vector<Row> m_rows;
    int m_total=0, m_changed=0, m_pending=0;
};
} // namespace drivelink::gui
