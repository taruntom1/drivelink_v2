#include "plot_item.hpp"
#include <QPainter>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <cmath>

namespace drivelink::gui {
PlotItem::PlotItem(QQuickItem *parent):QQuickItem(parent) {
    setFlag(ItemHasContents,true);
}

void PlotItem::setSource(QObject *object) {
    if(object==m_object) {return;}
    if(m_object) {disconnect(m_object,nullptr,this,nullptr);}
    m_object=object;
    m_source=dynamic_cast<PlotSource *>(object);
    if(object && m_source) {
        // Any PlotSource QObject: its own changed() and dataChanged() signals.
        connect(object,SIGNAL(dataChanged()),this,SLOT(refresh()));
        connect(object,SIGNAL(changed()),this,SLOT(refresh()));
    }
    emit sourceChanged();refresh();
}

void PlotItem::setKeys(const QStringList &keys) {
    if(keys==m_keys) {return;}
    m_keys=keys;emit keysChanged();refresh();
}

void PlotItem::setColors(const QList<QColor> &colors) {
    if(colors==m_colors) {return;}
    m_colors=colors;emit colorsChanged();polish();
}

void PlotItem::geometryChange(const QRectF &next, const QRectF &previous) {
    QQuickItem::geometryChange(next,previous);
    if(next.size()!=previous.size()) {refresh();}
}

void PlotItem::itemChange(ItemChange change, const ItemChangeData &data) {
    QQuickItem::itemChange(change,data);
    // A hidden page costs nothing; it catches up when shown.
    if(change==ItemVisibleHasChanged && data.boolValue) {refresh();}
}

double PlotItem::yOf(double value) const {
    const double h=height();
    return h-(value-m_min)/(m_max-m_min)*h;
}

// The axis follows the data (GUI thread, on each poll); the lines follow at
// the next polish.
void PlotItem::refresh() {
    const auto *source=source_();
    if(!isVisible() || !source) {return;}
    std::optional<std::pair<double,double>> range;
    const auto t1=source->plotEnd(),t0=source->plotStart();
    for(const auto &key:m_keys) {
        const auto *ring=source->ring(key);
        if(!ring) {continue;}
        if(const auto r=valueRange(*ring,t0,t1)) {
            if(!range) {range=r;} else {range->first=std::min(range->first,r->first);range->second=std::max(range->second,r->second);}
        }
    }
    const auto axis=range?niceAxis(range->first,range->second):niceAxis(0,1);
    const double step=axis.ticks.size()>1?axis.ticks[1]-axis.ticks[0]:1;
    const bool moved=axis.min!=m_min || axis.max!=m_max;
    m_min=axis.min;m_max=axis.max;
    QVariantList ticks;
    for(const auto t:axis.ticks) {ticks<<QVariantMap{{"value",t},{"text",tickText(t,step)},{"y",yOf(t)}};}
    if(moved || ticks!=m_ticks) {m_ticks=ticks;emit axisChanged();}
    polish();
}

void PlotItem::updatePolish() {
    m_lines.clear();
    m_software=window() && window()->rendererInterface()->graphicsApi()==QSGRendererInterface::Software;
    if(const auto *source=source_();source && width()>0 && height()>0) {
        const auto t1=source->plotEnd(),t0=source->plotStart();
        for(qsizetype i=0;i<m_keys.size();++i) {
            const auto *ring=source->ring(m_keys[i]);
            if(!ring) {continue;}
            Line line{i<m_colors.size()?m_colors[i]:QColor(Qt::gray),{}};
            for(const auto &p:decimate(*ring,t0,t1,width())) {
                if(std::isfinite(p.y())) {line.points.emplace_back(p.x(),yOf(p.y()));}
            }
            m_lines.push_back(std::move(line));
        }
    }
    if(m_software) {
        // Painted on the GUI thread; the render thread only uploads it.
        const auto dpr=window()->effectiveDevicePixelRatio();
        const QSize size(qMax(1,qCeil(width()*dpr)),qMax(1,qCeil(height()*dpr)));
        if(m_image.size()!=size) {m_image=QImage(size,QImage::Format_ARGB32_Premultiplied);}
        m_image.setDevicePixelRatio(dpr);
        m_image.fill(Qt::transparent);
        // Cosmetic one-pixel lines without antialiasing: the raster engine's
        // fast path, about ten times cheaper than smooth wide lines.
        QPainter painter(&m_image);
        for(const auto &line:m_lines) {
            if(line.points.size()<2) {continue;}
            painter.setPen(QPen(line.color,0));
            painter.drawPolyline(line.points.data(),int(line.points.size()));
        }
        m_imageDirty=true;
    }
    int vertices=0;
    for(const auto &line:m_lines) {vertices+=int(line.points.size());}
    m_vertices=vertices;
    update();
    emit drawn();
}

QSGNode *PlotItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    if(m_software) {
        auto *node=dynamic_cast<QSGImageNode *>(old);
        if(!node) {delete old;node=window()->createImageNode();node->setOwnsTexture(true);}
        if(m_imageDirty && !m_image.isNull()) {node->setTexture(window()->createTextureFromImage(m_image));m_imageDirty=false;}
        node->setRect(boundingRect());
        return node;
    }
    // One geometry node per series; nodes are reused while the count holds.
    if(old && dynamic_cast<QSGImageNode *>(old)) {delete old;old=nullptr;}
    auto *root=old?old:new QSGNode;
    while(root->childCount()>int(m_lines.size())) {
        auto *child=root->lastChild();root->removeChildNode(child);delete child;
    }
    while(root->childCount()<int(m_lines.size())) {
        auto *node=new QSGGeometryNode;
        auto *geometry=new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(),0);
        geometry->setDrawingMode(QSGGeometry::DrawLineStrip);
        // RHI backends draw one-pixel lines only (wider is OpenGL-only).
        geometry->setLineWidth(1.0f);
        node->setGeometry(geometry);node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGFlatColorMaterial);node->setFlag(QSGNode::OwnsMaterial);
        root->appendChildNode(node);
    }
    auto *child=root->firstChild();
    for(const auto &line:m_lines) {
        auto *node=static_cast<QSGGeometryNode *>(child);
        auto *geometry=node->geometry();
        geometry->allocate(int(line.points.size()));
        auto *v=geometry->vertexDataAsPoint2D();
        for(const auto &p:line.points) {(v++)->set(float(p.x()),float(p.y()));}
        node->markDirty(QSGNode::DirtyGeometry);
        auto *material=static_cast<QSGFlatColorMaterial *>(node->material());
        if(material->color()!=line.color) {material->setColor(line.color);node->markDirty(QSGNode::DirtyMaterial);}
        child=child->nextSibling();
    }
    return root;
}
} // namespace drivelink::gui
