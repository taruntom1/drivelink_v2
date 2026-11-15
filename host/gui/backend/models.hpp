#pragma once

#include <drivelink/model.hpp>
#include <QAbstractItemModel>
#include <QVariantList>

namespace drivelink::gui {

struct CommandPlan { uint8_t id, loop; std::vector<Ref> refs; };
// Pure planning: no mutation, no module-specific semantics, no slot stealing.
std::vector<CommandPlan> planCommands(const DeviceSnapshot &, const GroupList &);
QVariantMap errorFields(const Error &);

class InstanceTree final : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit InstanceTree(QObject *parent=nullptr) : QAbstractItemModel(parent) {}
    enum Role { Key=Qt::UserRole+1, Type, InstanceId, Label, Loop, Health, IsLoop };
    QModelIndex index(int, int, const QModelIndex & = {}) const override;
    QModelIndex parent(const QModelIndex &) const override;
    int rowCount(const QModelIndex & = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return 1; }
    QVariant data(const QModelIndex &, int) const override;
    QHash<int,QByteArray> roleNames() const override;
    void reset(std::shared_ptr<const DeviceSnapshot>, bool pending=false);
private:
    struct Node { Instance instance; bool loop; int parent=-1; std::vector<int> children; };
    std::vector<Node> m_nodes;
    std::vector<int> m_roots;
};

class DescriptorRows final : public QAbstractListModel {
    Q_OBJECT
public:
    explicit DescriptorRows(QObject *parent=nullptr) : QAbstractListModel(parent) {}
    enum Role { Key=Qt::UserRole+1, Name, Kind, ValueTypeRole, Unit, Description,
                Flags, Minimum, Maximum, Enums, ValueBits, Pending, Arguments, Results };
    int rowCount(const QModelIndex &p={}) const override { return p.isValid()?0:int(m_rows.size()); }
    QVariant data(const QModelIndex &,int) const override;
    QHash<int,QByteArray> roleNames() const override;
    void reset(std::shared_ptr<const DeviceSnapshot>, uint8_t type, uint8_t instance,
               bool pending=false);
private:
    struct Row { QString key; ModelItem item; std::optional<uint32_t> bits; };
    std::vector<Row> m_rows;
    bool m_pending=false;
};
} // namespace drivelink::gui
