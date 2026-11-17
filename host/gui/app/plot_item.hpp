#pragma once
// One Monitor plot: the visible window of some series of the MonitorView,
// drawn as scene-graph line strips. Decimation (at most two points per pixel
// column) runs on the GUI thread in updatePolish; updatePaintNode only copies
// the points. The software renderer (offscreen tests and screenshots) draws
// no custom geometry, so there the lines are painted into an image instead.
// Axes, grid, labels and cursors are QML around the item.
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>
#include <vector>

namespace drivelink::gui {
class MonitorView;
class PlotItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject *monitor READ monitor WRITE setMonitor NOTIFY monitorChanged)
    // Series keys in drawing order, and their colours (same length).
    Q_PROPERTY(QStringList keys READ keys WRITE setKeys NOTIFY keysChanged)
    Q_PROPERTY(QList<QColor> colors READ colors WRITE setColors NOTIFY colorsChanged)
    // The Y axis over the visible window, with round ticks:
    // [{value, text, y}] where y is in item pixels.
    Q_PROPERTY(double axisMin READ axisMin NOTIFY axisChanged)
    Q_PROPERTY(double axisMax READ axisMax NOTIFY axisChanged)
    Q_PROPERTY(QVariantList ticks READ ticks NOTIFY axisChanged)
    // Vertices of the last polished frame (tests and the performance check).
    Q_PROPERTY(int vertices READ vertices NOTIFY drawn)
public:
    explicit PlotItem(QQuickItem *parent=nullptr);
    QObject *monitor() const;
    void setMonitor(QObject *);
    QStringList keys() const { return m_keys; }
    void setKeys(const QStringList &);
    QList<QColor> colors() const { return m_colors; }
    void setColors(const QList<QColor> &);
    double axisMin() const { return m_min; }
    double axisMax() const { return m_max; }
    QVariantList ticks() const { return m_ticks; }
    int vertices() const { return m_vertices; }
signals:
    void monitorChanged();
    void keysChanged();
    void colorsChanged();
    void axisChanged();
    void drawn();
protected:
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &next, const QRectF &previous) override;
    void itemChange(ItemChange, const ItemChangeData &) override;
private:
    struct Line {
        QColor color;
        std::vector<QPointF> points;
    };
    QPointer<MonitorView> m_monitor;
    QStringList m_keys;
    QList<QColor> m_colors;
    double m_min=0,m_max=1;
    QVariantList m_ticks;
    std::vector<Line> m_lines;
    QImage m_image;
    bool m_software=false,m_imageDirty=false;
    int m_vertices=0;
    void refresh();
    double yOf(double value) const;
};
} // namespace drivelink::gui
