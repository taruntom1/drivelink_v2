#include "monitor_view.hpp"
#include "layout_model.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>

namespace drivelink::gui {
std::optional<MonitorSignal> MonitorView::signalOf(const QString &key) const {
    const auto ref=parseItemKey(key);
    if(!ref || !m_snapshot) {return std::nullopt;}
    return monitorSignal(*m_snapshot,*ref);
}

double MonitorView::rateOf(Ref r) const {
    if(!m_snapshot) {return 1;}
    const auto signal=monitorSignal(*m_snapshot,r);
    if(!signal) {return 1;}
    return double(loopRateHz(*m_snapshot,signal->loop))/dividerFor(m_set,*m_snapshot,signal->loop);
}

QVariantList MonitorView::browser() const {
    QVariantList sections;
    for(const auto &section:m_catalogue) {
        QVariantList rows;
        for(const auto &s:section.items) {
            rows<<QVariantMap{{"key",s.key},{"name",s.name},{"unit",s.unit},{"input",s.input},
                              {"checked",m_set.contains(s.ref)},{"color",m_colors.value(s.key,-1)}};
        }
        sections<<QVariantMap{{"key",section.key},{"title",section.title},{"subtitle",section.subtitle},
                              {"loop",section.loop},{"isLoop",section.isLoop},{"rows",rows}};
    }
    return sections;
}

QVariantMap MonitorView::values() const {
    QVariantMap map;
    if(!m_snapshot) {return map;}
    for(auto it=m_latest.begin();it!=m_latest.end();++it) {
        const auto signal=signalOf(it.key());
        if(!signal) {continue;}
        // Floats to five significant digits; enums and bools by name.
        const auto type=signal->item.descriptor.value_type;
        const auto text=type==ValueType::F32?plotValueText(type,sampleValue(type,it.value())):formatValue(signal->item,it.value());
        map[it.key()]=signal->unit.isEmpty()?text:text+" "+signal->unit;
    }
    return map;
}

QVariantList MonitorView::rates() const {
    QVariantList rows;
    if(!m_snapshot) {return rows;}
    const auto plans=monitorPlans(m_set,*m_snapshot);
    for(const auto &section:m_catalogue) {
        if(!section.isLoop) {continue;}
        const auto rate=loopRateHz(*m_snapshot,section.loop);
        const auto divider=dividerFor(m_set,*m_snapshot,section.loop);
        QVariantList choices;int index=-1,i=0;
        for(const auto &c:rateChoices(rate)) {
            if(c.divider==divider) {index=i;}
            choices<<QVariantMap{{"divider",c.divider},{"text",rateLabel(rate,c.divider)}};++i;
        }
        // A divider chosen before a live loop-rate change stays listed, so
        // the combo shows the rate the group really plots at.
        if(index<0) {index=i;choices<<QVariantMap{{"divider",divider},{"text",rateLabel(rate,divider)}};}
        const auto plotted=std::find_if(plans.begin(),plans.end(),[&](const MonitorPlan &p){return p.loop==section.loop;});
        rows<<QVariantMap{{"loop",section.loop},{"name",section.title},{"divider",divider},{"index",index},
                          {"choices",choices},{"plotted",plotted==plans.end()?0:int(plotted->refs.size())}};
    }
    return rows;
}

QVariantMap MonitorView::link() const {
    QVariantMap map;
    if(!m_snapshot) {return map;}
    auto streams=m_link.others;
    const auto plans=monitorPlans(m_set,*m_snapshot);
    for(const auto &s:planStreams(plans)) {streams.push_back(s);}
    const auto load=linkLoad(*m_snapshot,streams);
    map["load"]=load;map["percent"]=percentText(load);
    map["text"]=tr("%1 of 921600 baud").arg(percentText(load));
    map["groups"]=int(m_used);map["total"]=int(m_total);map["lost"]=qint64(m_lost);map["note"]=m_note;
    return map;
}

QVariantList MonitorView::plots() const {
    QVariantList list;
    if(!m_snapshot) {return list;}
    for(size_t p=0;p<m_set.plots.size();++p) {
        QVariantList series;QString unit;
        for(const auto &r:m_set.plots[p].series) {
            const auto signal=monitorSignal(*m_snapshot,r);
            if(!signal) {continue;}
            if(series.isEmpty()) {unit=signal->unit;}
            series<<QVariantMap{{"key",signal->key},{"name",signal->name},{"owner",signal->owner},{"unit",signal->unit},
                                {"color",m_colors.value(signal->key,0)},{"input",signal->input}};
        }
        list<<QVariantMap{{"index",int(p)},{"unit",unit},{"series",series}};
    }
    return list;
}

QVariantMap MonitorView::readout() const {
    if(!m_cursors || !m_snapshot) {return {};}
    QVariantList rows;
    for(const auto &plot:m_set.plots) {
        for(const auto &r:plot.series) {
            const auto signal=monitorSignal(*m_snapshot,r);
            const auto *data=signal?ring(signal->key):nullptr;
            if(!data) {continue;}
            const auto a=valueAt(*data,m_cursorA),b=valueAt(*data,m_cursorB);
            const auto text=[&](std::optional<double> v) {return v?plotValueText(signal->item.descriptor.value_type,*v):QString("–");};
            rows<<QVariantMap{{"key",signal->key},{"name",signal->owner+"."+signal->name},{"unit",signal->unit},
                              {"color",m_colors.value(signal->key,0)},{"a",text(a)},{"b",text(b)},
                              {"delta",a && b?plotValueText(signal->item.descriptor.value_type==ValueType::F32?ValueType::F32:ValueType::I32,*b-*a):QString("–")}};
        }
    }
    return {{"dt",durationText(m_cursorB-m_cursorA)},{"rows",rows}};
}

void MonitorView::toggleSignal(const QString &key) {
    const auto ref=parseItemKey(key);
    if(!ref || !m_snapshot) {return;}
    if(m_set.contains(*ref)) {removeSignal(key);return;}
    const auto check=checkAdd(*m_snapshot,m_set,*ref,m_link);
    const auto signal=monitorSignal(*m_snapshot,*ref);
    if(!check.ok) {refuse(key,signal?signal->loop:0,check);return;}
    auto next=m_set;placeSignal(next,*m_snapshot,*ref);
    m_refusal.clear();apply(next);
}

void MonitorView::removeSignal(const QString &key) {
    const auto ref=parseItemKey(key);
    if(!ref || !m_set.contains(*ref)) {return;}
    auto next=m_set;gui::removeSignal(next,*ref);
    m_refusal.clear();apply(next);
}

int MonitorView::plotNode(const QString &nodeKey) {
    int added=0;
    for(const auto &section:m_catalogue) {
        if(section.key!=nodeKey) {continue;}
        for(const auto &signal:section.items) {
            if(signal.input || m_set.contains(signal.ref)) {continue;}
            const auto check=checkAdd(*m_snapshot,m_set,signal.ref,m_link);
            if(!check.ok) {refuse(signal.key,signal.loop,check);return added;}
            auto next=m_set;placeSignal(next,*m_snapshot,signal.ref);
            m_refusal.clear();apply(next);++added;
        }
    }
    return added;
}

void MonitorView::setRate(int loop, int divider) {
    if(!m_snapshot || loop<0 || loop>255 || divider<1 || divider>65535) {return;}
    const auto check=checkRate(*m_snapshot,m_set,uint8_t(loop),uint16_t(divider),m_link);
    if(!check.ok) {refuse({},uint8_t(loop),check);return;}
    auto next=m_set;next.dividers[uint8_t(loop)]=uint16_t(divider);
    m_refusal.clear();apply(next);
}

void MonitorView::applySuggestion() {
    if(m_refusal.isEmpty() || !m_snapshot || !m_refusal.contains("divider")) {return;}
    auto next=m_set;
    next.dividers[uint8_t(m_refusal["loop"].toInt())]=uint16_t(m_refusal["divider"].toInt());
    const auto key=m_refusal["key"].toString();
    if(const auto ref=parseItemKey(key)) {
        // Checked again: the link may have changed since the refusal.
        const auto check=checkAdd(*m_snapshot,next,*ref,m_link);
        if(!check.ok) {refuse(key,uint8_t(m_refusal["loop"].toInt()),check);return;}
        placeSignal(next,*m_snapshot,*ref);
    }
    m_refusal.clear();apply(next);
}

void MonitorView::dismissRefusal() { m_refusal.clear();emit changed(); }

void MonitorView::addPlot() {
    if(m_set.plots.size()>=kMaxPlots) {return;}
    auto next=m_set;next.plots.push_back({});apply(next);
}

void MonitorView::removePlot(int index) {
    if(index<0 || size_t(index)>=m_set.plots.size()) {return;}
    auto next=m_set;next.plots.erase(next.plots.begin()+index);
    if(next.plots.empty()) {next.plots.push_back({});}
    apply(next);
}

void MonitorView::setPaused(bool paused) {
    if(paused==m_paused) {return;}
    m_paused=paused;
    // Resume starts the plots again from now: the paused stretch has no data.
    if(!paused) {clearData();m_cursors=false;}
    emit changed();emit cursorsChanged();emit dataChanged();
}

void MonitorView::setWindowSeconds(int seconds) {
    if(!windowChoices().contains(seconds) || seconds==m_window) {return;}
    m_window=seconds;
    if(m_cursors) {setCursor(0,double(m_cursorA));setCursor(1,double(m_cursorB));}
    emit changed();emit dataChanged();
}

void MonitorView::setCursors(bool shown) {
    if(shown && !m_paused) {return;}
    if(shown==m_cursors) {return;}
    m_cursors=shown;
    if(shown) {
        const auto span=int64_t(m_window)*1000000;
        m_cursorA=m_now-span*6/10;m_cursorB=m_now-span*3/10;
    }
    emit changed();emit cursorsChanged();
}

void MonitorView::setCursor(int which, double us) {
    if(!m_cursors) {return;}
    const auto t=std::clamp<int64_t>(int64_t(us),windowStart(),m_now);
    (which==0?m_cursorA:m_cursorB)=t;
    emit cursorsChanged();
}

QString MonitorView::snapshot(const QString &deviceId) {
    std::vector<CsvColumn> columns;
    if(m_snapshot) {
        for(const auto &plot:m_set.plots) {
            for(const auto &r:plot.series) {
                const auto signal=monitorSignal(*m_snapshot,r);
                if(!signal) {continue;}
                auto name=signal->owner+"."+signal->name;
                if(!signal->unit.isEmpty()) {name+=" ["+signal->unit+"]";}
                columns.push_back({name,ring(signal->key)});
            }
        }
    }
    if(columns.empty()) {emit notification(tr("Nothing to save: plot a signal first"),2);return {};}
    auto directory=m_directory;
    if(directory.isEmpty()) {
        directory=QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)+"/DriveLink/snapshots";
    }
    const auto id=deviceId.isEmpty()?QStringLiteral("device"):deviceId;
    const auto path=QDir(directory).filePath(QString("%1-%2.csv").arg(id,QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
    QFile file(path);
    if(!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly|QIODevice::Text)) {
        emit notification(tr("The snapshot could not be saved to %1").arg(QDir::toNativeSeparators(path)),2);return {};
    }
    const auto text=snapshotCsv(columns,windowStart(),m_now).toUtf8();
    if(file.write(text)!=text.size() || !file.flush()) {
        emit notification(tr("The snapshot could not be saved to %1").arg(QDir::toNativeSeparators(path)),2);return {};
    }
    emit notification(tr("Snapshot saved to %1").arg(QDir::toNativeSeparators(path)),1);
    return path;
}

void MonitorView::setSnapshot(std::shared_ptr<const DeviceSnapshot> snapshot) {
    // Plots kept over a lost link belong to that device only.
    if(m_snapshot && snapshot && m_snapshot->info.device_id!=snapshot->info.device_id) {reset();}
    const bool booted=m_snapshot && snapshot && m_snapshot->generation!=snapshot->generation;
    m_snapshot=std::move(snapshot);
    if(!m_snapshot) {return;}
    // Known before the first group list, so an early add (Layout's "Plot in
    // Monitor") is checked against the device's group limit.
    m_link.entryLimit=groupEntryLimit(*m_snapshot);
    // Status frames publish new snapshots ten times a second; the browser
    // and the plans change only with the running layout.
    auto catalogue=monitorCatalogue(*m_snapshot);
    const auto sameSection=[](const MonitorSection &a,const MonitorSection &b) {
        return a.key==b.key && a.title==b.title && a.subtitle==b.subtitle &&
            std::equal(a.items.begin(),a.items.end(),b.items.begin(),b.items.end(),[](const MonitorSignal &x,const MonitorSignal &y) {
                return x.key==y.key && x.name==y.name && x.unit==y.unit && x.owner==y.owner;
            });
    };
    if(!booted && std::equal(catalogue.begin(),catalogue.end(),m_catalogue.begin(),m_catalogue.end(),sameSection)) {return;}
    m_catalogue=std::move(catalogue);
    // Device time restarts at a Boot, so the old samples cannot share an axis.
    if(booted) {clearData();m_paused=false;m_cursors=false;m_latest.clear();emit cursorsChanged();emit dataChanged();}
    // Rings follow a loop rate changed live, too.
    auto next=m_set;pruneSignals(next,*m_snapshot);apply(next);
}

void MonitorView::setLink(LinkContext context, size_t used, size_t total) {
    m_link=std::move(context);m_used=used;m_total=total;emit changed();
}

void MonitorView::setNote(const QString &note) {
    if(note!=m_note) {m_note=note;emit changed();}
}

void MonitorView::addLost(uint64_t frames) {
    if(frames) {m_lost+=frames;m_lostDirty=true;}
}

std::vector<MonitorView::Sink> MonitorView::sinks(std::span<const Ref> refs) {
    std::vector<Sink> result;
    for(const auto &r:refs) {
        Sink sink;sink.key=itemKey(r.type,r.instance,r.item);
        const auto it=m_rings.find(sink.key);
        if(it!=m_rings.end()) {sink.ring=&*it;}
        if(m_snapshot) {
            if(const auto signal=monitorSignal(*m_snapshot,r)) {sink.type=signal->item.descriptor.value_type;}
        }
        result.push_back(std::move(sink));
    }
    return result;
}

void MonitorView::endAppend() {
    if(m_lostDirty) {m_lostDirty=false;emit changed();}
    if(m_valuesDirty) {m_valuesDirty=false;emit valuesChanged();}
    if(m_appended) {m_appended=false;emit dataChanged();}
}

void MonitorView::lose() {
    // The plot set and rates stay for the reconnect (a requested reboot
    // reopens the link); the data and the slot picture do not.
    m_link={};m_used=0;m_total=0;m_note.clear();m_refusal.clear();m_paused=false;m_cursors=false;
    clearData();m_latest.clear();
    emit changed();emit valuesChanged();emit cursorsChanged();emit dataChanged();
}

void MonitorView::reset() {
    m_snapshot.reset();m_catalogue.clear();m_set={};m_colors.clear();m_link={};m_used=0;m_total=0;m_lost=0;
    m_note.clear();m_refusal.clear();m_paused=false;m_cursors=false;m_window=10;
    clearData();m_rings.clear();m_latest.clear();
    emit changed();emit valuesChanged();emit cursorsChanged();emit dataChanged();
}

std::vector<MonitorPlan> MonitorView::plans() const {
    return m_snapshot?monitorPlans(m_set,*m_snapshot):std::vector<MonitorPlan>{};
}

const SampleRing *MonitorView::ring(const QString &key) const {
    const auto it=m_rings.constFind(key);
    return it==m_rings.constEnd()?nullptr:&*it;
}

void MonitorView::apply(const PlotSet &next) {
    m_set=next;
    // Rings follow the plotted signals and their rates; colours stay with a
    // signal while it is plotted.
    QSet<QString> keep;
    for(const auto &plot:m_set.plots) {
        for(const auto &r:plot.series) {
            const auto key=itemKey(r.type,r.instance,r.item);keep.insert(key);
            const auto capacity=ringCapacity(rateOf(r));
            auto it=m_rings.find(key);
            if(it==m_rings.end()) {m_rings.insert(key,SampleRing(capacity));}
            else {it->setCapacity(capacity);}
            if(!m_colors.contains(key)) {
                int color=0;
                const auto used=m_colors.values();
                while(used.contains(color)) {++color;}
                m_colors.insert(key,color);
            }
        }
    }
    for(auto it=m_rings.begin();it!=m_rings.end();) {if(keep.contains(it.key())) {++it;} else {it=m_rings.erase(it);}}
    for(auto it=m_colors.begin();it!=m_colors.end();) {if(keep.contains(it.key())) {++it;} else {it=m_colors.erase(it);}}
    for(auto it=m_latest.begin();it!=m_latest.end();) {if(keep.contains(it.key())) {++it;} else {it=m_latest.erase(it);}}
    emit changed();emit valuesChanged();emit dataChanged();emit plansChanged();
}

void MonitorView::refuse(const QString &key, uint8_t loop, const MonitorCheck &check) {
    m_refusal={{"key",key},{"loop",loop},{"reason",check.reason},{"suggestion",check.suggestion}};
    if(check.divider) {m_refusal["divider"]=*check.divider;}
    emit changed();
}

void MonitorView::clearData() {
    for(auto &ring:m_rings) {ring.clear();}
    m_now=0;m_cursorA=0;m_cursorB=0;
}
} // namespace drivelink::gui
