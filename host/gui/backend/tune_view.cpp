#include "tune_view.hpp"
#include "command_model.hpp"
#include "layout_model.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTime>
#include <QRegularExpression>
#include <QUrl>
#include <cmath>

namespace drivelink::gui {
namespace defs=protocol::defs;
namespace {
QString str(const std::string &s) { return QString::fromStdString(s); }
std::optional<double> parseNumber(QString text) {
    text=text.trimmed();
    if(text.startsWith('+')) {text.remove(0,1);}
    bool ok=false;const double value=QLocale::c().toDouble(text,&ok);
    if(!ok || !std::isfinite(value)) {return std::nullopt;}
    return value;
}
QString runKey(int number, size_t index) { return QStringLiteral("r%1:%2").arg(number).arg(index); }
// The magnitude window the slider shows when not dragging.
std::optional<GainWindow> shownWindow(double value, const std::optional<GainWindow> &typed, double min, double max) {
    if(value==0) {return std::nullopt;}
    if(typed && std::fabs(value)>=typed->lo && std::fabs(value)<=typed->hi) {return typed;}
    return gainWindow(value,min,max);
}
// A dialog's URL or a plain path ("C:/x" is not a URL scheme).
QString localPath(const QString &url) {
    return url.startsWith(QLatin1String("file:"),Qt::CaseInsensitive)?QUrl(url).toLocalFile():url;
}
QString writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly)) {return file.errorString();}
    if(file.write(bytes)!=bytes.size()) {return file.errorString();}
    if(!file.commit()) {return file.errorString();}
    return {};
}
// A file name without characters Windows refuses.
QString fileSafe(QString name) {
    static const QString bad=QStringLiteral("<>:\"/\\|?*");
    for(auto &ch:name) {if(bad.contains(ch) || ch.unicode()<32) {ch=QChar('_');}}
    return name.trimmed();
}
inline constexpr qint64 kMaxSessionBytes=qint64(512)*1024*1024;
QString endText(RunEnd end) {
    switch(end) {
    case RunEnd::Completed:return {};
    case RunEnd::Stopped:return TuneView::tr("stopped");
    case RunEnd::Disarmed:return TuneView::tr("incomplete: disarmed");
    }
    return {};
}
} // namespace

int GainRows::rowCount(const QModelIndex &p) const { return p.isValid()?0:int(m_view.m_rows.size()); }
QHash<int,QByteArray> GainRows::roleNames() const {
    return {{Key,"key"},{Name,"name"},{Description,"description"},{Unit,"unit"},{Value,"value"},{Text,"text"},
        {Min,"min"},{Max,"max"},{WindowLo,"windowLo"},{WindowHi,"windowHi"},{Position,"position"},{Saved,"saved"},
        {HasSaved,"hasSaved"},{Ticks,"ticks"},{Zero,"zero"},{Negative,"negative"},{Dragging,"dragging"},
        {Sending,"sending"},{Problem,"problem"},{Group,"group"},{GroupTitle,"groupTitle"},{GroupFirst,"groupFirst"},
        {GroupOpen,"groupOpen"},{GroupNote,"groupNote"},{Header,"header"}};
}
QVariant GainRows::data(const QModelIndex &i, int role) const {
    if(!i.isValid() || i.row()<0 || size_t(i.row())>=m_view.m_rows.size()) {return {};}
    const auto &row=m_view.m_rows[size_t(i.row())];
    switch(role) {
    case Header:return row.gain<0;
    case Group:return row.group;
    case GroupTitle:return row.group.isEmpty()?TuneView::tr("Other gains"):TuneView::tr("%1.* gains").arg(row.group);
    case GroupFirst:return row.groupFirst;
    case GroupOpen:return row.groupOpen;
    case GroupNote:return row.groupNote;
    default:break;
    }
    if(row.gain<0) {return role==Key?QVariant(QStringLiteral("header:")+row.group):QVariant();}
    const auto &g=m_view.m_gains[size_t(row.gain)];
    const auto *item=m_view.itemOf(g.item);
    if(!item) {return {};}
    const double min=m_view.rangeOf(g.item,false),max=m_view.rangeOf(g.item,true);
    const auto window=g.dragging && g.window?g.window:shownWindow(g.shown,g.typed,min,max);
    const auto savedIt=m_view.m_controller?m_view.m_saved.find({m_view.m_controller->first,m_view.m_controller->second,g.item}):m_view.m_saved.end();
    const bool hasSaved=m_view.m_savedKnown && savedIt!=m_view.m_saved.end();
    switch(role) {
    case Key:return g.key;
    case Name:return str(item->descriptor.name);
    case Description:return str(item->descriptor.description);
    case Unit:return str(item->descriptor.unit);
    case Value:return g.shown;
    case Text:return gainText(g.shown);
    case Min:return min;
    case Max:return max;
    case WindowLo:return window?QVariant((g.shown<0?-1:1)*window->lo):QVariant();
    case WindowHi:return window?QVariant((g.shown<0?-1:1)*window->hi):QVariant();
    case Position:return window?logPosition(std::fabs(g.shown),*window):0.5;
    case Saved:return hasSaved?QVariant(savedIt->second):QVariant();
    case HasSaved:return hasSaved;
    case Ticks: {
        // Saved value (colour -1) and the gains of the shown runs, at their
        // positions on the log scale; only those inside the window.
        QVariantList ticks;
        if(!window) {return ticks;}
        const auto add=[&](double value,int colour,bool faded) {
            if(value==0 || (value<0)!=(g.shown<0)) {return;}
            const double m=std::fabs(value);
            if(m<window->lo || m>window->hi) {return;}
            ticks<<QVariantMap{{"position",logPosition(m,*window)},{"colour",colour},{"faded",faded},{"value",value}};
        };
        if(hasSaved) {add(savedIt->second,-1,false);}
        for(const auto &e:m_view.shownRuns()) {
            const auto &run=m_view.m_runs[e.index];
            if(run.foreign || (m_view.m_controller && run.controller!=*m_view.m_controller)) {continue;}
            if(const auto it=run.gains.find(g.item);it!=run.gains.end()) {add(it->second,run.colour,e.faded);}
        }
        return ticks;
    }
    case Zero:return g.shown==0;
    case Negative:return g.shown<0;
    case Dragging:return g.dragging;
    case Sending:return g.inflight || g.pending.has_value();
    case Problem:return g.problem;
    default:return {};
    }
}

int RunRows::rowCount(const QModelIndex &p) const { return p.isValid()?0:int(m_view.m_runs.size()); }
QHash<int,QByteArray> RunRows::roleNames() const {
    return {{Number,"number"},{Colour,"colour"},{Time,"time"},{Changed,"changed"},{Legend,"legend"},{End,"end"},
        {Pinned,"pinned"},{Shown,"shown"},{Faded,"faded"},{Rise,"rise"},{Overshoot,"overshoot"},{Settling,"settling"},
        {SteadyError,"steadyError"},{MetricsNote,"metricsNote"},{Response,"response"},{Irregular,"irregular"}};
}
QVariant RunRows::data(const QModelIndex &i, int role) const {
    if(!i.isValid() || i.row()<0 || size_t(i.row())>=m_view.m_runs.size()) {return {};}
    // Newest first.
    const size_t index=m_view.m_runs.size()-1-size_t(i.row());
    const auto &r=m_view.m_runs[index];
    const auto shown=m_view.shownRuns();
    const auto entry=std::find_if(shown.begin(),shown.end(),[&](const OverlayEntry &e){return e.index==index;});
    const QString unit=r.response>=0?r.units[size_t(r.response)]:QString();
    switch(role) {
    case Number:return r.number;
    case Colour:return r.colour;
    case Time:return r.time;
    case Changed:return r.changed.isEmpty()?QStringLiteral("—"):r.changed;
    case Legend:return r.changed.isEmpty()?TuneView::tr("run %1").arg(r.number):TuneView::tr("run %1 · %2").arg(r.number).arg(r.changed.section(QStringLiteral(", "),0,0));
    case End:return endText(r.end);
    case Pinned:return r.pinned;
    case Shown:return entry!=shown.end();
    case Faded:return entry!=shown.end() && entry->faded;
    case Rise:return metricText(r.metrics.rise_s,TuneView::tr("ms"),1000,0);
    case Overshoot:return metricText(r.metrics.overshoot_pct,QStringLiteral("%"),1,0);
    case Settling:return metricText(r.metrics.settling_s,TuneView::tr("ms"),1000,0);
    case SteadyError: {
        const double scale=r.metrics.steady_error?std::fabs(*r.metrics.steady_error):0;
        return metricText(r.metrics.steady_error,unit,1,scale>=10?1:scale>=0.1?2:3);
    }
    case MetricsNote:return str(r.metrics.problem);
    case Response:return r.response>=0?r.names[size_t(r.response)]:QString();
    case Irregular:return r.irregular;
    default:return {};
    }
}

int HistoryRows::rowCount(const QModelIndex &p) const { return p.isValid()?0:int(m_rows.size()); }
QHash<int,QByteArray> HistoryRows::roleNames() const {
    return {{Id,"entryId"},{Time,"time"},{Change,"change"},{Note,"note"},{Run,"run"},{RunColour,"runColour"},
        {Current,"current"},{Mark,"mark"},{CanRestore,"canRestore"},{RestoreReason,"restoreReason"},
        {CanCompare,"canCompare"},{CompareReason,"compareReason"},{Foreign,"foreign"}};
}
QVariant HistoryRows::data(const QModelIndex &i, int role) const {
    if(!i.isValid() || i.row()<0 || size_t(i.row())>=m_rows.size() || m_rows[size_t(i.row())]>=m_view.m_history.size()) {return {};}
    const auto &e=m_view.m_history[m_rows[size_t(i.row())]];
    const auto run=entryRun(e,m_view.m_runs);
    const auto record=run?std::find_if(m_view.m_runs.begin(),m_view.m_runs.end(),[&](const RunRecord &r){return r.number==*run;}):m_view.m_runs.end();
    switch(role) {
    case Id:return e.id;
    case Time:return e.time;
    case Change:
        if(e.source==HistorySource::Loaded) {return TuneView::tr("loaded from device");}
        return e.change.isEmpty()?TuneView::tr("no gain change"):e.change;
    case Note:return e.foreign?(e.note.isEmpty()?TuneView::tr("another device"):TuneView::tr("%1 · another device").arg(e.note)):e.note;
    case Run:return run?*run:0;
    case RunColour:return record!=m_view.m_runs.end()?record->colour:-1;
    case Current: {
        // The newest entry of this device whose gains are the live ones.
        std::map<uint16_t,double> now;
        for(const auto &g:m_view.m_gains) {now[g.item]=g.shown;}
        for(size_t row=0;row<m_rows.size();++row) {
            const auto &x=m_view.m_history[m_rows[row]];
            if(!x.foreign && sameGains(x.gains,now)) {return int(row)==i.row();}
        }
        return false;
    }
    case Mark:return m_view.m_compareA==e.id?QStringLiteral("A"):m_view.m_compareB==e.id?QStringLiteral("B"):QString();
    case CanRestore:return m_view.restoreBlocked(e).isEmpty();
    case RestoreReason:return m_view.restoreBlocked(e);
    case CanCompare:return run.has_value();
    case CompareReason:return run?QString():TuneView::tr("No run with these gains");
    case Foreign:return e.foreign;
    default:return {};
    }
}

TuneView::TuneView(QObject *parent):QObject(parent),m_gainRows(*this),m_runRows(*this),m_historyRows(*this) {
    m_clock.start();
    connect(&m_identify,&IdentifyView::notification,this,&TuneView::notification);
    m_sendTimer.setInterval(10);
    connect(&m_sendTimer,&QTimer::timeout,this,[this]{flush(false);});
    // G5: a history entry and an Auto re-run once the gains are still for 0.5 s.
    m_settle.setSingleShot(true);m_settle.setInterval(500);
    connect(&m_settle,&QTimer::timeout,this,[this]{settled();});
    m_autoCheck.setSingleShot(true);m_autoCheck.setInterval(0);
    connect(&m_autoCheck,&QTimer::timeout,this,[this]{tryAuto();});
    m_autosave.setSingleShot(true);m_autosave.setInterval(1000);
    connect(&m_autosave,&QTimer::timeout,this,[this]{writeAutosave(false);});
}
TuneView::~TuneView() {
    // A pending autosave is written before the view goes (closing the app).
    flushAutosave();
    waitAutosave();
}

std::optional<TuneController> TuneView::currentController() const {
    if(!m_snapshot || !m_controller) {return std::nullopt;}
    for(auto &c:tuneControllers(*m_snapshot)) {if(c.key==*m_controller) {return c;}}
    return std::nullopt;
}
const ModelItem *TuneView::itemOf(uint16_t id) const {
    if(!m_snapshot || !m_controller) {return nullptr;}
    const auto t=m_snapshot->types.find(m_controller->first);
    if(t==m_snapshot->types.end()) {return nullptr;}
    const auto i=t->second.items.find(id);
    return i==t->second.items.end()?nullptr:&i->second;
}
double TuneView::deviceValue(uint16_t id) const {
    const auto *item=itemOf(id);
    const auto *entry=m_snapshot && m_controller?configuredEntry(*m_snapshot,*m_controller):nullptr;
    if(!item || !entry) {return 0;}
    const auto it=entry->params.find(id);
    return it==entry->params.end()?0:bitsToNumber(item->descriptor.value_type,valueBits(it->second));
}
double TuneView::rangeOf(uint16_t id, bool max) const {
    const auto *item=itemOf(id);
    if(!item) {return 0;}
    const auto &d=item->descriptor;
    if(max && d.has_max) {return bitsToNumber(d.value_type,d.max_bits);}
    if(!max && d.has_min) {return bitsToNumber(d.value_type,d.min_bits);}
    // The type's own range when the descriptor leaves a side open.
    switch(d.value_type) {
    case ValueType::U8:return max?255.0:0.0;
    case ValueType::U16:return max?65535.0:0.0;
    case ValueType::U32:return max?4294967295.0:0.0;
    case ValueType::I32:return max?double(INT32_MAX):double(INT32_MIN);
    default:return (max?1.0:-1.0)*double(std::numeric_limits<float>::max());
    }
}
double TuneView::loopRate() const {
    const auto c=currentController();
    return c && m_snapshot?double(loopRateHz(*m_snapshot,c->loop)):0.0;
}

QVariantList TuneView::controllers() const {
    QVariantList result;
    if(!m_snapshot) {return result;}
    for(const auto &c:tuneControllers(*m_snapshot)) {
        result<<QVariantMap{{"key",nodeKey(c.key.first,c.key.second)},{"text",QStringLiteral("%1 · %2").arg(c.label,c.typeName)},
            {"subtitle",loopName(*m_snapshot,c.loop)}};
    }
    return result;
}
QString TuneView::controller() const { return m_controller?nodeKey(m_controller->first,m_controller->second):QString(); }
QVariantList TuneView::enums() const {
    QVariantList result;
    if(!m_snapshot || !m_controller) {return result;}
    for(const auto id:gainLayout(*m_snapshot,*m_controller).enums) {
        const auto *item=itemOf(id);
        if(!item) {continue;}
        QStringList options;int index=-1;
        const auto value=uint32_t(deviceValue(id));
        for(size_t i=0;i<item->enums.size();++i) {
            options<<str(item->enums[i].name);
            if(item->enums[i].value==value) {index=int(i);}
        }
        const auto key=itemKey(m_controller->first,m_controller->second,id);
        result<<QVariantMap{{"key",key},{"name",str(item->descriptor.name)},{"options",options},{"index",index},
            {"sending",m_enumSending.value(key)},{"problem",m_enumProblems.value(key)}};
    }
    return result;
}
QVariantMap TuneView::revert() const {
    if(!m_controller) {return {{"enabled",false},{"reason",tr("Pick a controller")}};}
    if(!m_savedKnown) {
        return {{"enabled",false},{"reason",tr("The saved values are unknown: the device had unsaved changes when this window connected")}};
    }
    if(busy()) {return {{"enabled",false},{"reason",tr("Gains are locked while a run is active")}};}
    if(gainsMoving()) {return {{"enabled",false},{"reason",tr("Wait for the gain writes to finish")}};}
    bool differs=false;
    for(const auto &g:m_gains) {
        const auto it=m_saved.find({m_controller->first,m_controller->second,g.item});
        if(it!=m_saved.end() && it->second!=deviceValue(g.item)) {differs=true;}
    }
    if(!differs) {return {{"enabled",false},{"reason",tr("The gains match the saved values")}};}
    return {{"enabled",true},{"reason",QString()}};
}
QVariantList TuneView::targets() const {
    QVariantList result;
    if(const auto c=currentController()) {
        for(const auto &t:c->targets) {
            result<<QVariantMap{{"key",itemKey(t.ref.type,t.ref.instance,t.ref.item)},{"name",str(t.descriptor.name)},
                {"unit",str(t.descriptor.unit)}};
        }
    }
    return result;
}
QStringList TuneView::waveforms() const {
    QStringList names;
    for(int w=0;w<=5;++w) {names<<waveformName(Waveform(w));}
    return names;
}
QVariantMap TuneView::testSignal() const {
    const auto c=currentController();
    QVariantMap map{{"target",itemKey(m_signal.target.type,m_signal.target.instance,m_signal.target.item)},
        {"waveform",int(m_signal.waveform)},{"offset",m_signal.offset},{"amplitude",m_signal.amplitude},
        {"f0",m_signal.f0},{"f1",m_signal.f1},{"duration",m_signal.duration_s},{"repeat",int(m_signal.repeat)},
        {"usesFrequency",waveformUsesFrequency(m_signal.waveform)},{"chirp",waveformIsChirp(m_signal.waveform)}};
    QString unit,problem=tr("Pick a controller"),holds;
    if(c) {
        const auto t=std::find_if(c->targets.begin(),c->targets.end(),[&](const TuneTarget &x){return x.ref==m_signal.target;});
        if(t==c->targets.end()) {problem=tr("Pick a target");}
        else {
            unit=str(t->descriptor.unit);
            problem=testSignalProblem(m_signal,loopRate(),t->descriptor);
            // User decision 2026-11-18: a commandable target returns to 0 when
            // the run ends; another keeps the generator's last value.
            holds=commandable(t->descriptor)?tr("%1 returns to 0 when the run ends.").arg(str(t->descriptor.name))
                :tr("%1 keeps the signal's last value when the run ends (it has no range that includes 0).").arg(str(t->descriptor.name));
        }
    }
    map["unit"]=unit;map["problem"]=problem;map["holds"]=holds;
    return map;
}
QVariantMap TuneView::capture() const {
    QVariantMap map;
    const auto c=currentController();
    QVariantList refs,candidates,rates;
    if(c && m_snapshot) {
        for(const auto &r:m_capture.refs) {
            const auto s=monitorSignal(*m_snapshot,r);
            refs<<QVariantMap{{"key",itemKey(r.type,r.instance,r.item)},{"name",s?s->name:QStringLiteral("?")},
                {"owner",s?s->owner:QString()},{"unit",s?s->unit:QString()}};
        }
        const auto maxRefs=m_snapshot->system_params.contains(defs::system::kLimitsCaptureRefs)?
            valueBits(m_snapshot->system_params.at(defs::system::kLimitsCaptureRefs)):16U;
        for(const auto &section:monitorCatalogue(*m_snapshot)) {
            if(section.loop!=c->loop) {continue;}
            for(const auto &s:section.items) {
                const bool listed=std::find(m_capture.refs.begin(),m_capture.refs.end(),s.ref)!=m_capture.refs.end();
                QString reason;
                if(listed) {reason=tr("Already captured");}
                else if(m_capture.refs.size()>=maxRefs) {reason=tr("At most %1 signals per capture").arg(maxRefs);}
                candidates<<QVariantMap{{"key",s.key},{"name",s.name},{"owner",s.owner},{"unit",s.unit},
                    {"enabled",reason.isEmpty()},{"reason",reason}};
            }
        }
        const auto rate=uint32_t(loopRate());
        int index=-1;
        for(const uint16_t d:{1,2,5,10,20,50,100}) {
            if(rate && double(rate)/d<1) {break;}
            if(d==m_capture.divider) {index=int(rates.size());}
            rates<<QVariantMap{{"divider",d},{"text",rateLabel(rate,d)}};
        }
        if(index<0) {index=int(rates.size());rates<<QVariantMap{{"divider",m_capture.divider},{"text",rateLabel(rate,m_capture.divider)}};}
        map["rateIndex"]=index;
        const auto capacity=m_snapshot->info.capture_capacity;
        map["usage"]=tr("Uses %1 of %2").arg(bytesText(captureBytes(m_capture)),bytesText(capacity));
        map["problem"]=captureProblem(*m_snapshot,m_capture,c->loop,capacity,maxRefs);
        map["seconds"]=rate?double(m_capture.samples-m_capture.pre)*m_capture.divider/rate:0.0;
    } else {map["problem"]=tr("Pick a controller");}
    map["refs"]=refs;map["candidates"]=candidates;map["rates"]=rates;
    map["divider"]=m_capture.divider;map["samples"]=int(m_capture.samples);map["pre"]=int(m_capture.pre);
    map["note"]=m_captureNote;
    return map;
}
std::optional<TuneView::RunSetup> TuneView::runSetup(QString *problem) const {
    const auto fail=[&](const QString &why) {if(problem) {*problem=why;} return std::optional<RunSetup>();};
    const auto c=currentController();
    if(!c) {return fail(tr("Pick a controller"));}
    // The run records the gains it starts with: none may still be on its way.
    if(gainsMoving()) {return fail(tr("Wait for the gain writes to finish"));}
    if(const auto p=testSignal()["problem"].toString();!p.isEmpty()) {return fail(p);}
    if(const auto p=capture()["problem"].toString();!p.isEmpty()) {return fail(p);}
    RunSetup setup{*c,m_signal,m_capture,{}};
    for(const auto &g:m_gains) {setup.gains[g.item]=g.shown;}
    return setup;
}
QVariantMap TuneView::run() const {
    QString blocked;
    if(m_phase!=Phase::Idle) {blocked=tr("A run is in progress");}
    else if(m_identifying) {blocked=tr("Plant identification is running");}
    if(blocked.isEmpty() && m_hooks.runBlocked) {blocked=m_hooks.runBlocked();}
    if(blocked.isEmpty()) {runSetup(&blocked);}
    return {{"active",m_phase!=Phase::Idle},{"stopping",m_stopping},{"text",m_phaseText},{"progress",m_progress},
        {"canRun",blocked.isEmpty()},{"blocked",blocked},{"next",m_nextNumber}};
}

QVariantList TuneView::chips() const {
    QVariantList result;
    QStringList seen;
    const auto shown=shownRuns();
    for(auto it=shown.rbegin();it!=shown.rend();++it) {
        const auto &r=m_runs[it->index];
        for(size_t i=0;i<r.capture.refs.size();++i) {
            const auto &ref=r.capture.refs[i];
            const auto key=itemKey(ref.type,ref.instance,ref.item);
            if(seen.contains(key)) {continue;}
            seen<<key;
            result<<QVariantMap{{"key",key},{"name",r.names[i]},{"unit",r.units[i]},{"checked",m_chips.value(key)}};
        }
    }
    return result;
}
QVariantList TuneView::plots() const {
    QVariantList result;
    const auto shown=shownRuns();
    if(shown.empty()) {return result;}
    QStringList units;
    for(const auto &chip:chips()) {
        const auto m=chip.toMap();
        if(m["checked"].toBool() && !units.contains(m["unit"].toString())) {units<<m["unit"].toString();}
    }
    while(units.size()>3) {units.removeLast();}
    for(const auto &unit:units) {
        QVariantList series,legend,markers;
        QStringList names;
        for(const auto &e:shown) {
            const auto &r=m_runs[e.index];
            bool any=false;
            for(size_t i=0;i<r.capture.refs.size();++i) {
                const auto &ref=r.capture.refs[i];
                if(r.units[i]!=unit || !m_chips.value(itemKey(ref.type,ref.instance,ref.item))) {continue;}
                const bool target=ref==r.signal.target;
                series<<QVariantMap{{"key",runKey(r.number,i)},{"colour",r.colour},{"faded",e.faded},{"target",target}};
                if(!target && !names.contains(r.names[i])) {names<<r.names[i];}
                any=true;
            }
            if(!any) {continue;}
            legend<<QVariantMap{{"colour",r.colour},{"faded",e.faded},
                {"text",r.changed.isEmpty()?tr("run %1").arg(r.number):tr("run %1 · %2").arg(r.number).arg(r.changed.section(QStringLiteral(", "),0,0))}};
            if(r.endedUs && r.end!=RunEnd::Completed) {
                markers<<QVariantMap{{"us",double(*r.endedUs)},{"colour",r.colour},{"text",endText(r.end)}};
            }
        }
        if(series.isEmpty()) {continue;}
        QString owner;
        if(m_snapshot) {
            const auto &r=m_runs[shown.back().index];
            if(const auto *e=configuredEntry(*m_snapshot,r.controller)) {owner=str(e->instance.label);}
        }
        const auto what=names.isEmpty()?tr("setpoint"):names.join(QStringLiteral(", "));
        result<<QVariantMap{{"unit",unit},{"title",owner.isEmpty()?what:QStringLiteral("%1 · %2").arg(what,owner)},
            {"series",series},{"legend",legend},{"markers",markers}};
    }
    return result;
}
const SampleRing *TuneView::ring(const QString &key) const {
    if(!key.startsWith('r')) {return nullptr;}
    const auto parts=key.mid(1).split(':');
    if(parts.size()!=2) {return nullptr;}
    const int number=parts[0].toInt();const auto index=parts[1].toULongLong();
    for(const auto &r:m_runs) {
        if(r.number==number) {return index<r.series.size()?&r.series[index]:nullptr;}
    }
    return nullptr;
}
int64_t TuneView::plotStart() const {
    const auto shown=shownRuns();
    if(shown.empty()) {return 0;}
    int64_t t=INT64_MAX;
    for(const auto &e:shown) {t=std::min(t,m_runs[e.index].startUs);}
    return t;
}
int64_t TuneView::plotEnd() const {
    const auto shown=shownRuns();
    if(shown.empty()) {return 1000000;}
    int64_t t=INT64_MIN;
    for(const auto &e:shown) {t=std::max(t,m_runs[e.index].endUs);}
    return std::max(t,plotStart()+1);
}

void TuneView::setSnapshot(std::shared_ptr<const DeviceSnapshot> snapshot, const QString &deviceId) {
    if(deviceId!=m_deviceId) {
        // The last device's unsaved session goes to its recovery file first.
        flushAutosave();
        m_runRows.beginResetModel();m_runs.clear();m_chips.clear();m_nextNumber=1;m_nextColour=0;m_runRows.endResetModel();
        m_deviceId=deviceId;m_saved.clear();m_savedKnown=false;m_controller.reset();
        m_print.clear(); // another device: read its snapshot afresh
        m_gainRows.beginResetModel();m_gains.clear();m_rows.clear();m_gainRows.endResetModel();
        clearSession();
        // An autosaved session of this device (the app closed with unsaved runs).
        m_recoveryOffer.reset();
        if(const QFileInfo file(recoveryPath());!m_deviceId.isEmpty() && file.exists()) {m_recoveryOffer=file.lastModified();}
        emit dataChanged();
    }
    if(!snapshot) {m_snapshot.reset();m_print.clear();return;}
    // Every status frame publishes a snapshot: only what Tune shows (the
    // layout, params, loops, capacity and the unsaved flag) notifies QML.
    std::vector<uint64_t> print{snapshot->generation,uint64_t(snapshot->ready),snapshot->info.capture_capacity,
        uint64_t(snapshot->status && (snapshot->status->flags&8)),uint64_t(snapshot->status.has_value())};
    for(const auto &l:snapshot->info.loops) {print.push_back(uint64_t(l.id)<<32|l.rate_hz);}
    for(const auto &[k,entry]:snapshot->configured) {
        print.push_back(uint64_t(k.first)<<24|uint64_t(k.second)<<16|uint64_t(entry.instance.loop)<<8|entry.instance.state);
        for(const auto &[id,value]:entry.params) {print.push_back(uint64_t(id)<<32|valueBits(value));}
    }
    for(const auto &[id,value]:snapshot->system_params) {print.push_back(uint64_t(id)<<32|valueBits(value));}
    for(const auto &[id,value]:snapshot->pending_system_params) {print.push_back(uint64_t(1)<<48|uint64_t(id)<<32|valueBits(value));}
    m_snapshot=std::move(snapshot);
    if(print==m_print) {return;}
    m_print=std::move(print);
    // While the device reports no unsaved changes its live values are the saved ones.
    if(m_snapshot->ready && m_snapshot->status && !(m_snapshot->status->flags&8)) {
        m_saved.clear();
        for(const auto &[k,entry]:m_snapshot->configured) {
            const auto t=m_snapshot->types.find(k.first);
            if(t==m_snapshot->types.end()) {continue;}
            for(const auto &[id,item]:t->second.items) {
                const auto &d=item.descriptor;
                const auto v=entry.params.find(id);
                if(d.kind!=ItemKind::Param || !tuneFlagged(d) || d.value_type==ValueType::Enum || v==entry.params.end()) {continue;}
                m_saved[{k.first,k.second,id}]=bitsToNumber(d.value_type,valueBits(v->second));
            }
        }
        m_savedKnown=true;
    }
    const auto list=tuneControllers(*m_snapshot);
    const bool present=m_controller && std::any_of(list.begin(),list.end(),[&](const TuneController &c){return c.key==*m_controller;});
    if(!present) {
        if(m_controller || !list.empty()) {
            m_controller=list.empty()?std::nullopt:std::optional<InstanceKey>(list.front().key);
            // Auto re-run starts again only after a Run on this controller.
            m_autoEpoch=0;m_autoPending=false;m_changePending=false;m_settle.stop();
            rebuildGains();defaultsForTarget();
        }
        historyChanged();
    } else {
        rebuildGains();
        reconcileCapture();
        refreshHistoryRows();
    }
    m_identify.setContext(m_snapshot,m_controller,m_deviceId);
    emit changed();emit runChanged();
}
void TuneView::reset() {
    m_sendTimer.stop();
    m_snapshot.reset();m_print.clear();m_controller.reset();
    m_gainRows.beginResetModel();m_gains.clear();m_rows.clear();m_gainRows.endResetModel();
    m_enumProblems.clear();m_enumSending.clear();m_saved.clear();m_savedKnown=false;
    m_phase=Phase::Idle;m_phaseText.clear();m_progress=-1;m_stopping=false;
    // Closed: nothing runs by itself after a reconnect.
    m_settle.stop();m_autoCheck.stop();m_autoEpoch=0;m_autoPending=false;m_changePending=false;
    m_settleNote.clear();m_settleSource=HistorySource::Changed;
    m_identifying=false;m_identify.reset();
    flushAutosave();
    historyChanged();
    emit changed();emit runChanged();
}
void TuneView::rebuildGains() {
    if(!m_snapshot || !m_controller) {
        m_gainRows.beginResetModel();m_gains.clear();m_rows.clear();m_gainRows.endResetModel();return;
    }
    const auto layout=gainLayout(*m_snapshot,*m_controller);
    std::vector<Row> rows;
    std::vector<uint16_t> items;
    QStringList enumNames;
    for(const auto id:layout.enums) {if(const auto *i=itemOf(id)) {enumNames<<str(i->descriptor.name);}}
    for(const auto &group:layout.groups) {
        Row header;header.group=group.prefix;header.groupFirst=true;header.groupOpen=group.open;
        if(!group.open) {header.groupNote=tr("Not used by the selected %1").arg(enumNames.join(QStringLiteral(", ")));}
        rows.push_back(header);
        for(const auto id:group.items) {
            Row row=header;row.groupFirst=false;
            items.push_back(id);
            row.gain=int(items.size()-1);
            rows.push_back(row);
        }
    }
    const auto key=[&](uint16_t id){return itemKey(m_controller->first,m_controller->second,id);};
    bool same=items.size()==m_gains.size();
    for(size_t i=0;same && i<items.size();++i) {same=m_gains[i].key==key(items[i]);}
    const bool sameShape=rows.size()==m_rows.size() && std::equal(rows.begin(),rows.end(),m_rows.begin(),
        [](const Row &a,const Row &b){return a.gain==b.gain && a.group==b.group;});
    const bool sameRows=same && sameShape && std::equal(rows.begin(),rows.end(),m_rows.begin(),
        [](const Row &a,const Row &b){return a.groupOpen==b.groupOpen && a.groupFirst==b.groupFirst;});
    if(!same || !sameShape) {
        m_gainRows.beginResetModel();
        m_gains.clear();
        for(const auto id:items) {
            Gain g;g.item=id;g.key=key(id);g.shown=deviceValue(id);
            // A window typed earlier (or opened from a session file) for this gain.
            if(const auto w=m_windows.constFind(g.key);w!=m_windows.constEnd()) {g.typed=GainWindow{w->lo,w->hi};}
            m_gains.push_back(g);
        }
        m_rows=std::move(rows);
        m_gainRows.endResetModel();
        // The controller's first history entry: its gains as read from the device.
        if(const auto c=currentController();c && lastEntry(m_history,c->typeName,c->label,m_deviceId)<0 && !m_gains.empty()) {
            recordCurrent(HistorySource::Loaded,{});
            historyChanged();
        }
        return;
    }
    // In place: a drag in progress keeps its delegate.
    m_rows=std::move(rows);
    updateGains();
    if(!sameRows && !m_rows.empty()) {emit m_gainRows.dataChanged(m_gainRows.index(0),m_gainRows.index(int(m_rows.size())-1));}
}
void TuneView::updateGains() {
    const auto now=m_clock.elapsed();
    for(auto &g:m_gains) {
        if(g.applied) {
            const auto *item=itemOf(g.item);
            const bool read=item && numberToBits(item->descriptor.value_type,*g.applied)==numberToBits(item->descriptor.value_type,deviceValue(g.item));
            if(read || now-g.appliedAt>2000) {g.applied.reset();}
        }
        if(!g.dragging && !g.inflight && !g.pending) {g.shown=g.applied?*g.applied:deviceValue(g.item);}
    }
    if(!m_rows.empty()) {emit m_gainRows.dataChanged(m_gainRows.index(0),m_gainRows.index(int(m_rows.size())-1));}
    // Run and Revert wait for gain writes: tell QML when that flips.
    if(const bool moving=gainsMoving();moving!=m_moving) {m_moving=moving;refreshHistoryRows();emit changed();emit runChanged();}
}
int TuneView::gainIndex(const QString &key) const {
    for(size_t i=0;i<m_gains.size();++i) {if(m_gains[i].key==key) {return int(i);}}
    return -1;
}
bool TuneView::gainsMoving() const {
    return gainBusy() || std::any_of(m_gains.begin(),m_gains.end(),[](const Gain &g){return g.dragging;});
}
bool TuneView::gainBusy() const {
    return std::any_of(m_gains.begin(),m_gains.end(),[](const Gain &g){return g.inflight || g.pending;});
}
void TuneView::setGain(int index, double value, bool now) {
    if(index<0) {return;}
    auto &g=m_gains[size_t(index)];
    const auto *item=itemOf(g.item);
    if(!item) {return;}
    value=std::clamp(value,rangeOf(g.item,false),rangeOf(g.item,true));
    if(item->descriptor.value_type!=ValueType::F32) {value=std::round(value);}
    g.shown=value;g.pending=value;g.problem.clear();
    gainChanged();
    flush(now);
    updateGains();
}
bool TuneView::gainsLocked() const {
    // User decision 2026-11-18 (H2e-2): with Auto re-run on, a gain change
    // during a run cancels it; with it off the gains stay locked.
    // Plant identification (H2e-3) locks them throughout.
    return m_identifying || (m_phase!=Phase::Idle && !m_autoOn);
}
void TuneView::setIdentifying(bool on) {
    if(on==m_identifying) {return;}
    m_identifying=on;
    updateGains();refreshHistoryRows();
    emit changed();emit runChanged();
    if(!on && m_autoPending) {m_autoCheck.start();}
}
QString TuneView::applyBlocked() const {
    if(!m_snapshot || !m_controller) {return tr("Not connected");}
    if(busy()) {return tr("Gains are locked while a run is active");}
    if(gainsMoving()) {return tr("Wait for the gain writes to finish");}
    return {};
}
QString TuneView::applyGains(const ResolvedGains &set, const QString &note) {
    if(const auto why=applyBlocked();!why.isEmpty()) {return why;}
    if(set.key!=*m_controller) {return tr("Select the controller the step ran on");}
    for(const auto &[id,value]:set.values) {
        if(gainIndex(itemKey(set.key.first,set.key.second,id))<0) {return tr("%1 is not a gain of the selected controller").arg(id);}
    }
    writeSet(set,HistorySource::Changed,note);
    return {};
}
void TuneView::gainChanged() {
    m_changePending=true;m_settle.stop();
    m_settleSource=HistorySource::Changed;m_settleNote.clear();
    // During a run (only possible with Auto re-run on): the run is cancelled
    // and not kept; the next one follows once the gains are still again.
    if(m_phase!=Phase::Idle && m_hooks.cancel) {m_hooks.cancel();}
}
void TuneView::flush(bool force) {
    const auto now=m_clock.elapsed();
    bool waiting=false;
    for(size_t i=0;i<m_gains.size();++i) {
        auto &g=m_gains[i];
        if(!g.pending) {continue;}
        // At most every 50 ms while dragging (latest value), never two at once.
        if(g.inflight || (!force && now-g.lastSent<50)) {waiting=true;continue;}
        const auto *item=itemOf(g.item);
        if(!item || !m_hooks.write || !m_controller) {g.pending.reset();continue;}
        const double value=*g.pending;g.pending.reset();
        g.inflight=true;g.lastSent=now;
        const Ref ref{m_controller->first,m_controller->second,g.item};
        const auto key=g.key;
        m_hooks.write({ref},{numberToBits(item->descriptor.value_type,value)},[this,key,value](QString error) {
            const int i=gainIndex(key);
            if(i<0) {return;}
            auto &done=m_gains[size_t(i)];
            done.inflight=false;
            if(error.isEmpty()) {
                done.applied=value;done.appliedAt=m_clock.elapsed();
                // A value the device did not keep (no new snapshot follows)
                // stops being shown after 2 s.
                QTimer::singleShot(2100,this,[this] {updateGains();});
                // The gains settle 0.5 s after the last accepted write.
                if(m_changePending) {m_settle.start();}
            }
            else {
                done.applied.reset();
                done.problem=error;
                // A refused value is not shown as applied.
                if(!done.pending && !done.dragging) {done.shown=deviceValue(done.item);}
            }
            flush(!done.dragging);
            updateGains();
        });
    }
    if(waiting) {if(!m_sendTimer.isActive()) {m_sendTimer.start();}}
    else {m_sendTimer.stop();}
}

void TuneView::selectController(const QString &key) {
    if(busy()) {return;}
    const auto k=parseNodeKey(key);
    if(!k || (m_controller && *k==*m_controller) || !m_snapshot) {return;}
    const auto list=tuneControllers(*m_snapshot);
    if(!std::any_of(list.begin(),list.end(),[&](const TuneController &c){return c.key==*k;})) {return;}
    m_controller=*k;
    // Auto re-run starts again only after a Run on this controller; A/B ends.
    m_autoEpoch=0;m_autoPending=false;m_changePending=false;m_settle.stop();
    m_compareA.reset();m_compareB.reset();
    rebuildGains();defaultsForTarget();
    historyChanged();
    m_identify.setContext(m_snapshot,m_controller,m_deviceId);
    emit changed();emit runChanged();emit dataChanged();
}
void TuneView::setEnum(const QString &key, int index) {
    const auto ref=parseItemKey(key);
    const auto *item=ref?itemOf(ref->item):nullptr;
    if(!item || index<0 || size_t(index)>=item->enums.size() || !m_hooks.write || busy()) {return;}
    m_enumSending[key]=true;m_enumProblems.remove(key);emit changed();
    m_hooks.write({*ref},{item->enums[size_t(index)].value},[this,key](QString error) {
        m_enumSending.remove(key);
        if(!error.isEmpty()) {m_enumProblems[key]=error;}
        emit changed();
    });
}
void TuneView::gainPress(const QString &key) {
    const int i=gainIndex(key);
    if(i<0 || gainsLocked()) {return;}
    auto &g=m_gains[size_t(i)];
    if(g.shown==0) {return;}
    g.window=shownWindow(g.shown,g.typed,rangeOf(g.item,false),rangeOf(g.item,true));
    if(!g.window) {return;}
    g.dragging=true;
    updateGains();
}
void TuneView::gainDrag(const QString &key, double position) {
    const int i=gainIndex(key);
    if(i<0 || gainsLocked()) {return;}
    auto &g=m_gains[size_t(i)];
    if(!g.dragging || !g.window) {return;}
    const double sign=g.shown<0?-1:1;
    setGain(i,sign*roundGain(logMagnitude(position,*g.window)),false);
}
void TuneView::gainRelease(const QString &key) {
    const int i=gainIndex(key);
    if(i<0) {return;}
    auto &g=m_gains[size_t(i)];
    if(!g.dragging) {return;}
    // Re-centred on the new value; the last value goes out at once.
    g.dragging=false;g.window.reset();
    flush(true);
    // Nothing left to send (the last value was already accepted): settle now.
    if(m_changePending && !gainBusy()) {m_settle.start();}
    updateGains();
}
QString TuneView::gainText(const QString &key, const QString &text) {
    const int i=gainIndex(key);
    if(i<0) {return tr("Unknown gain");}
    if(gainsLocked()) {return tr("Gains are locked while a run is active");}
    const auto value=parseNumber(text);
    if(!value) {updateGains();return tr("Not a number");}
    const auto &g=m_gains[size_t(i)];
    const double min=rangeOf(g.item,false),max=rangeOf(g.item,true);
    if(*value<min || *value>max) {
        updateGains();
        return tr("Allowed %1 to %2").arg(QLocale::c().toString(min,'g',6),QLocale::c().toString(max,'g',6));
    }
    setGain(i,*value,true);
    return {};
}
void TuneView::gainStep(const QString &key, double factor) {
    const int i=gainIndex(key);
    if(i<0 || m_gains[size_t(i)].shown==0 || m_gains[size_t(i)].dragging || gainsLocked()) {return;}
    const auto &g=m_gains[size_t(i)];
    setGain(i,stepGain(g.shown,factor,rangeOf(g.item,false),rangeOf(g.item,true)),true);
}
void TuneView::gainKey(const QString &key, bool up, bool shift, bool ctrl) {
    const int i=gainIndex(key);
    if(i<0 || m_gains[size_t(i)].shown==0 || m_gains[size_t(i)].dragging || gainsLocked()) {return;}
    const double percent=ctrl?0.001:shift?0.10:0.01;
    const auto &g=m_gains[size_t(i)];
    // Repeated keys coalesce like a drag (latest value, at most every 50 ms).
    setGain(i,stepGain(g.shown,up?1+percent:1-percent,rangeOf(g.item,false),rangeOf(g.item,true)),false);
}
QString TuneView::setWindow(const QString &key, const QString &lo, const QString &hi) {
    const int i=gainIndex(key);
    if(i<0) {return tr("Unknown gain");}
    auto &g=m_gains[size_t(i)];
    const auto a=parseNumber(lo),b=parseNumber(hi);
    if(!a || !b) {return tr("Not a number");}
    const auto w=typedWindow(*a,*b,rangeOf(g.item,false),rangeOf(g.item,true),g.shown);
    if(!w) {return tr("The window must be on the gain's side of 0, inside its range");}
    if(std::fabs(g.shown)<w->lo || std::fabs(g.shown)>w->hi) {return tr("The window must contain the gain");}
    g.typed=w;
    // Kept per gain with the session (by controller and gain name).
    if(const auto c=currentController()) {
        const auto *item=itemOf(g.item);
        m_windows[g.key]=SessionWindow{c->typeName,c->label,item?str(item->descriptor.name):QString(),w->lo,w->hi};
        markDirty();
    }
    updateGains();
    return {};
}
void TuneView::revertGains() {
    if(!revert()["enabled"].toBool() || !m_hooks.write) {return;}
    // revert() is disabled while a gain write is in flight: never two writes of one gain.
    ResolvedGains set{*m_controller,{}};
    for(const auto &g:m_gains) {
        const auto it=m_saved.find({m_controller->first,m_controller->second,g.item});
        if(it==m_saved.end() || !itemOf(g.item) || it->second==deviceValue(g.item)) {continue;}
        set.values.emplace_back(g.item,it->second);
    }
    writeSet(set,HistorySource::Reverted,tr("saved values"));
}
// One batch for a gain set (Revert, Restore): never while a gain write is in
// flight (the callers check), never two writes of one gain.
void TuneView::writeSet(const ResolvedGains &set, HistorySource source, const QString &note) {
    if(set.values.empty() || !m_hooks.write || !m_controller || set.key!=*m_controller) {return;}
    std::vector<Ref> refs;std::vector<uint32_t> bits;std::vector<double> values;
    for(const auto &[id,value]:set.values) {
        const int at=gainIndex(itemKey(set.key.first,set.key.second,id));
        const auto *item=itemOf(id);
        if(at<0 || !item) {continue;}
        refs.push_back({set.key.first,set.key.second,id});
        bits.push_back(numberToBits(item->descriptor.value_type,value));
        values.push_back(value);
        auto &g=m_gains[size_t(at)];
        g.pending.reset();g.shown=value;g.inflight=true;g.problem.clear();
    }
    if(refs.empty()) {return;}
    gainChanged();
    m_settleSource=source;m_settleNote=note;
    updateGains();
    const auto what=source==HistorySource::Reverted?tr("Revert gains to saved"):source==HistorySource::Changed?
        tr("Apply suggested gains"):tr("Restore");
    m_hooks.write(refs,bits,[this,refs,values,what](QString error) {
        // The controller may have changed meanwhile: only its own gains count.
        for(size_t i=0;i<refs.size();++i) {
            if(!m_controller || refs[i].type!=m_controller->first || refs[i].instance!=m_controller->second) {break;}
            const int at=gainIndex(itemKey(refs[i].type,refs[i].instance,refs[i].item));
            if(at<0) {continue;}
            auto &g=m_gains[size_t(at)];
            g.inflight=false;
            if(error.isEmpty()) {g.applied=values[i];g.appliedAt=m_clock.elapsed();}
        }
        if(!error.isEmpty()) {emit notification(tr("%1: %2").arg(what,error),2);}
        else if(m_changePending) {m_settle.start();}
        flush(true);updateGains();emit changed();
    });
}
void TuneView::setTarget(const QString &key) {
    if(m_phase!=Phase::Idle) {return;}
    const auto ref=parseItemKey(key);
    const auto c=currentController();
    if(!ref || !c) {return;}
    if(std::none_of(c->targets.begin(),c->targets.end(),[&](const TuneTarget &t){return t.ref==*ref;})) {return;}
    if(*ref==m_signal.target) {return;}
    m_signal.target=*ref;
    defaultsForTarget();
    emit changed();emit runChanged();
}
void TuneView::setWaveform(int index) {
    if(m_phase!=Phase::Idle) {return;}
    if(index<0 || index>5) {return;}
    m_signal.waveform=Waveform(index);
    const double rate=loopRate();
    if(waveformUsesFrequency(m_signal.waveform) && !(m_signal.f0>0)) {m_signal.f0=rate>0?std::min(1.0,rate/10):1.0;}
    if(waveformIsChirp(m_signal.waveform) && !(m_signal.f1>0)) {m_signal.f1=rate>0?std::min(20.0,rate/10):10.0;}
    emit changed();emit runChanged();
}
QString TuneView::setSignalField(const QString &field, const QString &text) {
    if(m_phase!=Phase::Idle) {return tr("Locked while a run is active");}
    const auto value=parseNumber(text);
    if(!value) {emit changed();return tr("Not a number");}
    if(field=="offset") {m_signal.offset=*value;}
    else if(field=="amplitude") {m_signal.amplitude=*value;}
    else if(field=="f0") {m_signal.f0=*value;}
    else if(field=="f1") {m_signal.f1=*value;}
    else if(field=="duration") {m_signal.duration_s=*value;}
    else if(field=="repeat") {
        if(*value<1 || *value>1000 || std::floor(*value)!=*value) {emit changed();return tr("Repeat 1 to 1000 times");}
        m_signal.repeat=uint32_t(*value);
    } else {return tr("Unknown field");}
    if((field=="duration" || field=="repeat") && !m_captureEdited && std::isfinite(m_signal.duration_s) && m_signal.duration_s>0) {
        m_capture=resizeCapture(m_capture,loopRate(),signalSeconds(m_signal));
    }
    emit changed();emit runChanged();
    return {};
}
void TuneView::addCaptureRef(const QString &key) {
    if(m_phase!=Phase::Idle) {return;}
    const auto ref=parseItemKey(key);
    if(!ref || std::find(m_capture.refs.begin(),m_capture.refs.end(),*ref)!=m_capture.refs.end()) {return;}
    m_capture.refs.push_back(*ref);
    emit changed();emit runChanged();
}
void TuneView::removeCaptureRef(const QString &key) {
    if(m_phase!=Phase::Idle) {return;}
    const auto ref=parseItemKey(key);
    if(!ref) {return;}
    std::erase(m_capture.refs,*ref);
    emit changed();emit runChanged();
}
void TuneView::setDivider(int divider) {
    if(m_phase!=Phase::Idle) {return;}
    if(divider<1 || divider>65535) {return;}
    m_capture.divider=uint16_t(divider);
    m_capture=resizeCapture(m_capture,loopRate(),signalSeconds(m_signal));m_captureEdited=false;
    emit changed();emit runChanged();
}
QString TuneView::setSamples(const QString &samples, const QString &pre) {
    if(m_phase!=Phase::Idle) {return tr("Locked while a run is active");}
    const auto n=parseNumber(samples),p=parseNumber(pre);
    if(!n || !p || *n<2 || *p<0 || std::floor(*n)!=*n || std::floor(*p)!=*p || *n>1e8) {emit changed();return tr("Whole numbers: at least 2 samples");}
    if(*p>=*n) {emit changed();return tr("Fewer samples before the trigger than in all");}
    m_capture.samples=uint32_t(*n);m_capture.pre=uint32_t(*p);m_captureEdited=true;
    emit changed();emit runChanged();
    return {};
}
void TuneView::toggleChip(const QString &key) {
    m_chips[key]=!m_chips.value(key);
    emit changed();emit dataChanged();
}
void TuneView::startRun() {
    if(!run()["canRun"].toBool() || !m_hooks.start) {return;}
    // G5: Auto re-run starts only after a Run the user pressed under this ARM.
    m_autoEpoch=m_hooks.armEpoch?m_hooks.armEpoch():0;
    // This Run carries the latest gains: no re-run for them afterwards.
    m_autoPending=false;m_changePending=false;m_autoNote.clear();
    m_hooks.start();
    emit runChanged();
}
void TuneView::stopRun() {
    if(m_phase==Phase::Idle || !m_hooks.stop) {return;}
    m_hooks.stop();
}
void TuneView::pinRun(int number, bool pinned) {
    for(auto &r:m_runs) {if(r.number==number) {r.pinned=pinned;}}
    markDirty();
    runsChanged();
}
void TuneView::removeRun(int number) {
    std::erase_if(m_runs,[&](const RunRecord &r){return r.number==number;});
    markDirty();
    runsChanged();
    historyChanged();
}
void TuneView::setPhase(Phase phase, const QString &text, double progress) {
    const bool locked=gainsLocked();
    m_phase=phase;m_phaseText=text;m_progress=progress;
    if(phase==Phase::Idle) {m_stopping=false;}
    if(locked!=gainsLocked()) {updateGains();}
    refreshHistoryRows();
    emit runChanged();
    if(phase==Phase::Idle && m_autoPending) {m_autoCheck.start();}
}
void TuneView::refreshRunState() {
    m_identify.refreshRunState();
    emit runChanged();
    if(m_autoPending) {m_autoCheck.start();}
}
void TuneView::addRun(RunRecord record, const RunSetup &setup) {
    record.number=m_nextNumber++;
    record.colour=m_nextColour++%8;
    record.time=QTime::currentTime().toString(QStringLiteral("hh:mm:ss"));
    record.controller=setup.controller.key;
    record.controllerType=setup.controller.typeName;record.controllerLabel=setup.controller.label;
    record.device=m_deviceId;
    record.gains=setup.gains;
    if(m_snapshot) {record.gainNames=gainNamesOf(*m_snapshot,record.controller);}
    const RunRecord *previous=nullptr;
    for(auto it=m_runs.rbegin();it!=m_runs.rend();++it) {if(it->controller==record.controller && !it->foreign) {previous=&*it;break;}}
    if(m_snapshot) {record.changed=changedGains(*m_snapshot,record.controller,record.gains,previous?&previous->gains:nullptr);}
    record.metrics=runMetrics(record);
    // The target and the response of a new run are drawn unless unchecked before.
    const auto check=[&](const Ref &ref) {
        const auto key=itemKey(ref.type,ref.instance,ref.item);
        if(!m_chips.contains(key)) {m_chips[key]=true;}
    };
    check(record.signal.target);
    if(record.response>=0) {check(record.capture.refs[size_t(record.response)]);}
    for(const auto &ref:record.capture.refs) {const auto key=itemKey(ref.type,ref.instance,ref.item);if(!m_chips.contains(key)) {m_chips[key]=false;}}
    // The run joins the history entry of its gains (a new one when they
    // changed elsewhere, such as on Layout).
    HistoryEntry e;
    e.id=m_nextHistory;e.time=record.time;e.controller=record.controller;
    e.controllerType=record.controllerType;e.controllerLabel=record.controllerLabel;
    e.gains=record.gains;e.names=record.gainNames;e.device=m_deviceId;
    const int last=lastEntry(m_history,e.controllerType,e.controllerLabel,m_deviceId);
    if(last>=0) {e.change=gainChanges(e.names,m_history[size_t(last)].gains,e.gains);}
    else {e.source=HistorySource::Loaded;}
    const int at=recordEntry(m_history,std::move(e));
    if(m_history[size_t(at)].id==m_nextHistory) {++m_nextHistory;}
    m_history[size_t(at)].runs.push_back(record.number);
    m_runs.push_back(std::move(record));
    trimRuns(m_runs);
    // A recorded run replaces an offered recovery (the banner says so).
    m_recoveryOffer.reset();
    // A new run is shown: A/B ends (G5 A/B shows only its two runs).
    m_compareA.reset();m_compareB.reset();
    markDirty();
    runsChanged();
    historyChanged();
}
void TuneView::runsChanged() {
    m_runRows.beginResetModel();m_runRows.endResetModel();
    updateGains();
    emit changed();emit dataChanged();emit runChanged();
}

// ---- Gain history, A/B and Auto re-run (H2e-2) ------------------------------
void TuneView::settled() {
    // Still moving (a write in flight, a drag): wait for the next completion.
    if(gainsMoving()) {return;}
    const bool fresh=recordCurrent(m_settleSource,m_settleNote);
    m_settleSource=HistorySource::Changed;m_settleNote.clear();
    if(fresh) {historyChanged();}
    if(!m_changePending) {return;}
    m_changePending=false;
    // An Auto re-run when the gains differ from the controller's last run.
    if(!m_autoOn || !gainsDifferFromLastRun()) {return;}
    m_autoPending=true;
    tryAuto();
}
bool TuneView::gainsDifferFromLastRun() const {
    const auto c=currentController();
    if(!c) {return false;}
    std::map<uint16_t,double> now;
    for(const auto &g:m_gains) {now[g.item]=g.shown;}
    const auto last=std::find_if(m_runs.rbegin(),m_runs.rend(),[&](const RunRecord &r){return r.controller==c->key && !r.foreign;});
    return last==m_runs.rend() || !sameGains(last->gains,now);
}
void TuneView::tryAuto() {
    if(!m_autoPending) {return;}
    const auto drop=[&](const QString &why) {
        m_autoPending=false;m_autoNote=why;emit runChanged();
    };
    // Only under the ARM of the user's last Run (user decision 2026-11-18:
    // DISARM, a fault, Boot, link loss or another controller end it).
    const uint64_t epoch=m_hooks.armEpoch?m_hooks.armEpoch():0;
    if(!m_autoOn) {return drop({});}
    if(epoch==0 || epoch!=m_autoEpoch) {return drop(tr("starts after your first Run"));}
    // Still changing, or a run (or its release) in progress: wait.
    if(gainsMoving() || m_settle.isActive() || busy()) {emit runChanged();return;}
    if(m_hooks.autoBlocked) {
        if(const auto why=m_hooks.autoBlocked();!why.isEmpty()) {return drop(tr("skipped: %1").arg(why));}
    }
    if(m_hooks.runBlocked) {
        if(const auto why=m_hooks.runBlocked();!why.isEmpty()) {emit runChanged();return;} // a release or setup: checked again
    }
    // A run since (the user's Run while it waited) may already carry these gains.
    if(!gainsDifferFromLastRun()) {return drop({});}
    QString problem;
    if(!runSetup(&problem)) {return drop(tr("skipped: %1").arg(problem));}
    m_autoPending=false;m_autoNote.clear();
    if(m_hooks.start) {m_hooks.start();}
    emit runChanged();
}
bool TuneView::recordCurrent(HistorySource source, const QString &note) {
    const auto c=currentController();
    if(!c || !m_snapshot || m_gains.empty()) {return false;}
    HistoryEntry e;
    e.id=m_nextHistory;e.time=QTime::currentTime().toString(QStringLiteral("hh:mm:ss"));
    e.controller=c->key;e.controllerType=c->typeName;e.controllerLabel=c->label;
    for(const auto &g:m_gains) {e.gains[g.item]=g.shown;}
    e.names=gainNamesOf(*m_snapshot,c->key);
    e.source=source;e.note=note;e.device=m_deviceId;
    const int last=lastEntry(m_history,c->typeName,c->label,m_deviceId);
    if(last>=0) {e.change=gainChanges(e.names,m_history[size_t(last)].gains,e.gains);}
    else {e.source=HistorySource::Loaded;e.note.clear();}
    const bool loaded=e.source==HistorySource::Loaded;
    const int at=recordEntry(m_history,std::move(e));
    if(m_history[size_t(at)].id!=m_nextHistory) {return false;}
    ++m_nextHistory;
    // The first entry is what the device had: not an unsaved change.
    if(!loaded) {markDirty();}
    return true;
}
void TuneView::historyChanged() {
    std::vector<size_t> rows;
    if(const auto c=currentController()) {
        for(size_t i=m_history.size();i-->0;) {
            if(sameController(m_history[i].controllerType,m_history[i].controllerLabel,c->typeName,c->label)) {rows.push_back(i);}
        }
    }
    // A/B entries that went (trimmed, or their runs removed) end the comparison.
    const auto valid=[&](const std::optional<int> &id) {
        const auto *e=id?entryById(*id):nullptr;
        return e && entryRun(*e,m_runs).has_value();
    };
    if(m_compareA && !valid(m_compareA)) {m_compareA=m_compareB;m_compareB.reset();}
    if(m_compareA && !valid(m_compareA)) {m_compareA.reset();}
    if(m_compareB && !valid(m_compareB)) {m_compareB.reset();}
    m_historyRows.beginResetModel();m_historyRows.m_rows=std::move(rows);m_historyRows.endResetModel();
    emit changed();emit dataChanged();
}
void TuneView::refreshHistoryRows() {
    if(!m_historyRows.m_rows.empty()) {
        emit m_historyRows.dataChanged(m_historyRows.index(0),m_historyRows.index(int(m_historyRows.m_rows.size())-1));
    }
}
const HistoryEntry *TuneView::entryById(int id) const {
    const auto it=std::find_if(m_history.begin(),m_history.end(),[&](const HistoryEntry &e){return e.id==id;});
    return it==m_history.end()?nullptr:&*it;
}
QString TuneView::restoreBlocked(const HistoryEntry &e) const {
    if(!m_snapshot || !m_controller) {return tr("Not connected");}
    if(busy()) {return tr("Gains are locked while a run is active");}
    if(gainsMoving()) {return tr("Wait for the gain writes to finish");}
    const auto set=resolveGains(*m_snapshot,e.controllerType,e.controllerLabel,e.gains,e.names);
    if(!set) {return set.error();}
    if(set->key!=*m_controller) {return tr("Select %1 first").arg(e.controllerLabel);}
    std::map<uint16_t,double> now,wanted;
    for(const auto &g:m_gains) {now[g.item]=g.shown;}
    for(const auto &[id,value]:set->values) {wanted[id]=value;}
    bool differs=false;
    for(const auto &[id,value]:wanted) {
        const auto it=now.find(id);
        if(it==now.end()) {return tr("%1 is not a gain of the selected controller").arg(id);}
        if(float(it->second)!=float(value)) {differs=true;}
    }
    if(!differs) {return tr("These are the current gains");}
    return {};
}
void TuneView::restoreEntry(int id) {
    const auto *e=entryById(id);
    if(!e || !m_snapshot) {return;}
    if(const auto why=restoreBlocked(*e);!why.isEmpty()) {emit notification(tr("Restore: %1").arg(why),2);return;}
    const auto set=resolveGains(*m_snapshot,e->controllerType,e->controllerLabel,e->gains,e->names);
    if(!set) {return;}
    // Only the gains that differ are written.
    ResolvedGains changes{set->key,{}};
    for(const auto &[item,value]:set->values) {
        const int at=gainIndex(itemKey(set->key.first,set->key.second,item));
        if(at>=0 && float(m_gains[size_t(at)].shown)!=float(value)) {changes.values.emplace_back(item,value);}
    }
    writeSet(changes,HistorySource::Restored,tr("restored %1").arg(e->time));
}
std::vector<OverlayEntry> TuneView::shownRuns() const {
    if(m_compareA && m_compareB) {
        const auto *a=entryById(*m_compareA),*b=entryById(*m_compareB);
        const auto ra=a?entryRun(*a,m_runs):std::nullopt,rb=b?entryRun(*b,m_runs):std::nullopt;
        if(ra && rb) {
            std::vector<OverlayEntry> result;
            for(size_t i=0;i<m_runs.size();++i) {
                if(m_runs[i].number==*ra || m_runs[i].number==*rb) {result.push_back({i,false});}
            }
            return result;
        }
    }
    return overlay(m_runs);
}
QVariantMap TuneView::compare() const {
    const auto side=[&](const std::optional<int> &id)->QVariantMap {
        const auto *e=id?entryById(*id):nullptr;
        if(!e) {return {};}
        const auto run=entryRun(*e,m_runs);
        const auto r=run?std::find_if(m_runs.begin(),m_runs.end(),[&](const RunRecord &x){return x.number==*run;}):m_runs.end();
        return {{"id",e->id},{"time",e->time},{"run",run.value_or(0)},{"colour",r!=m_runs.end()?r->colour:-1}};
    };
    QVariantMap map{{"a",side(m_compareA)},{"b",side(m_compareB)}};
    const bool active=m_compareA && m_compareB;
    map["active"]=active;map["picking"]=m_compareA.has_value() && !active;
    if(active) {
        const auto *a=entryById(*m_compareA),*b=entryById(*m_compareB);
        const auto ra=entryRun(*a,m_runs),rb=entryRun(*b,m_runs);
        const auto runA=std::find_if(m_runs.begin(),m_runs.end(),[&](const RunRecord &x){return x.number==*ra;});
        const auto runB=std::find_if(m_runs.begin(),m_runs.end(),[&](const RunRecord &x){return x.number==*rb;});
        auto names=a->names;
        names.insert(b->names.begin(),b->names.end());
        const auto changes=gainChanges(names,a->gains,b->gains);
        map["gainChanges"]=changes.isEmpty()?tr("The gains are equal"):changes;
        map["equal"]=equalGains(names,a->gains,b->gains);
        QVariantList rows;
        for(const auto &m:compareMetrics(*runA,*runB)) {
            rows<<QVariantMap{{"name",m.name},{"a",m.a},{"b",m.b},{"change",m.change}};
        }
        map["metrics"]=rows;
        map["restoreA"]=restoreBlocked(*a);map["restoreB"]=restoreBlocked(*b);
        map["note"]=runA->metrics.problem.empty() && runB->metrics.problem.empty()?QString():
            str(!runA->metrics.problem.empty()?runA->metrics.problem:runB->metrics.problem);
    }
    return map;
}
void TuneView::pickCompare(int id) {
    const auto *e=entryById(id);
    if(!e || !entryRun(*e,m_runs)) {return;}
    if(m_compareA==id) {m_compareA=m_compareB;m_compareB.reset();}
    else if(m_compareB==id) {m_compareB.reset();}
    else if(!m_compareA) {m_compareA=id;}
    else {m_compareB=id;}
    refreshHistoryRows();
    m_runRows.beginResetModel();m_runRows.endResetModel();
    updateGains();
    emit changed();emit dataChanged();
}
void TuneView::exitCompare() {
    if(!m_compareA && !m_compareB) {return;}
    m_compareA.reset();m_compareB.reset();
    refreshHistoryRows();
    m_runRows.beginResetModel();m_runRows.endResetModel();
    updateGains();
    emit changed();emit dataChanged();
}
void TuneView::restoreCompare(bool b) {
    const auto id=b?m_compareB:m_compareA;
    if(id) {restoreEntry(*id);}
}
void TuneView::setAutoRerun(bool on) {
    if(on==m_autoOn) {return;}
    m_autoOn=on;
    if(!on) {m_autoPending=false;}
    m_autoNote.clear();
    // Gains lock again during a run when it goes off.
    updateGains();
    emit runChanged();emit changed();
}
QVariantMap TuneView::autoRerun() const {
    const uint64_t epoch=m_hooks.armEpoch?m_hooks.armEpoch():0;
    const bool allowed=epoch!=0 && epoch==m_autoEpoch;
    QString text;
    if(!m_autoOn) {text=tr("off: gains are locked during a run");}
    else if(!allowed) {text=tr("starts after your first Run");}
    else if(m_autoPending) {text=tr("waiting to re-run");}
    else if(!m_autoNote.isEmpty()) {text=m_autoNote;}
    else {text=tr("re-runs 0.5 s after a gain change; a change during a run cancels it");}
    return {{"on",m_autoOn},{"allowed",allowed},{"pending",m_autoPending},{"text",text}};
}

// ---- Session files (H2e-2) --------------------------------------------------
QVariantMap TuneView::session() const {
    QString name=m_sessionName;
    if(name.isEmpty()) {
        const auto c=currentController();
        name=c?tr("%1 tuning").arg(c->label):tr("Tuning");
    }
    const bool any=!m_runs.empty() || std::any_of(m_history.begin(),m_history.end(),[](const HistoryEntry &e){return e.source!=HistorySource::Loaded;});
    QString openReason;
    if(!m_snapshot) {openReason=tr("Connect first");}
    else if(busy()) {openReason=tr("Not while a run is active");}
    else if(gainsMoving()) {openReason=tr("Wait for the gain writes to finish");}
    const auto docs=QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return {{"name",name},{"file",m_sessionPath.isEmpty()?QString():QFileInfo(m_sessionPath).fileName()},{"unsaved",m_dirty},
        {"canSave",any},{"saveReason",any?QString():tr("No runs or gain changes yet")},
        {"canOpen",openReason.isEmpty()},{"openReason",openReason},
        {"folder",QUrl::fromLocalFile(m_sessionPath.isEmpty()?docs:QFileInfo(m_sessionPath).absolutePath()).toString()},
        {"runs",int(m_runs.size())}};
}
QString TuneView::suggestedName(const QString &kind, int number) const {
    const auto base=session()["name"].toString();
    if(kind==QLatin1String("csv")) {return fileSafe(tr("%1 run %2").arg(base).arg(number))+QStringLiteral(".csv");}
    return fileSafe(base)+QStringLiteral(".tune.json");
}
SessionData TuneView::sessionData() const {
    SessionData d;
    d.deviceId=m_deviceId;d.name=session()["name"].toString();
    d.deviceName=m_snapshot?str(m_snapshot->info.name):QString();
    d.savedAt=QDateTime::currentDateTime().toString(Qt::ISODate);
    d.runs=m_runs;d.history=m_history;
    for(const auto &w:m_windows) {d.windows.push_back(w);}
    if(m_controller) {d.setup=SessionSetup{*m_controller,m_signal,m_capture};}
    d.nextNumber=m_nextNumber;d.nextColour=m_nextColour;d.nextHistory=m_nextHistory;
    return d;
}
QString TuneView::saveSession(const QString &url) {
    const auto path=sessionFilePath(localPath(url));
    if(path.isEmpty()) {return tr("No file chosen");}
    auto data=sessionData();
    data.name=QFileInfo(path).fileName().remove(QRegularExpression(QStringLiteral("(\\.tune)?\\.json$"),
                                                                    QRegularExpression::CaseInsensitiveOption));
    if(const auto why=tooLarge();!why.isEmpty()) {return why;}
    if(const auto error=writeFile(path,writeSession(data));!error.isEmpty()) {return tr("Could not save %1: %2").arg(path,error);}
    m_sessionPath=path;m_sessionName=data.name;m_dirty=false;m_autosaveDue=false;m_autosave.stop();
    // Saved: nothing left to recover (after a write in flight lands).
    waitAutosave();
    if(!m_recoveryOffer) {QFile::remove(recoveryPath());}
    emit changed();
    return {};
}
QString TuneView::openSession(const QString &url) {
    if(const auto why=session()["openReason"].toString();!why.isEmpty()) {return why;}
    const auto path=localPath(url);
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) {return tr("Could not open %1: %2").arg(path,file.errorString());}
    if(file.size()>kMaxSessionBytes) {return tr("%1 is too large for a tune session").arg(path);}
    auto data=readSession(file.readAll());
    if(!data) {return tr("Could not open %1: %2").arg(QFileInfo(path).fileName(),data.error());}
    const bool foreign=data->deviceId!=m_deviceId;
    const auto other=data->deviceName.isEmpty()?data->deviceId:tr("%1 (%2)").arg(data->deviceName,data->deviceId);
    const int runs=int(data->runs.size());
    applySession(std::move(*data),false);
    m_sessionPath=path;m_dirty=false;m_autosaveDue=false;m_autosave.stop();
    waitAutosave();
    if(!m_recoveryOffer) {QFile::remove(recoveryPath());}
    emit notification(foreign?tr("Opened %1: %2 runs recorded on another device, %3; Restore needs a controller of the same type and name")
                               .arg(QFileInfo(path).fileName()).arg(runs).arg(other)
                             :tr("Opened %1: %2 runs").arg(QFileInfo(path).fileName()).arg(runs),foreign?2:0);
    emit changed();
    return {};
}
void TuneView::applySession(SessionData d, bool recovered) {
    const bool foreign=d.deviceId!=m_deviceId;
    m_runRows.beginResetModel();
    m_runs=std::move(d.runs);
    int maxNumber=0;
    for(auto &r:m_runs) {r.foreign=r.device!=m_deviceId;maxNumber=std::max(maxNumber,r.number);}
    m_nextNumber=std::max(d.nextNumber,maxNumber+1);m_nextColour=d.nextColour;
    m_runRows.endResetModel();
    m_history=std::move(d.history);
    int maxId=0;
    for(auto &e:m_history) {e.foreign=e.device!=m_deviceId;maxId=std::max(maxId,e.id);}
    m_nextHistory=std::max(d.nextHistory,maxId+1);
    m_compareA.reset();m_compareB.reset();
    // Another test signal may come with it: Auto re-run needs a Run again.
    m_autoPending=false;m_changePending=false;m_settle.stop();m_autoEpoch=0;
    // Chips: each run's target and response drawn, its other signals not.
    m_chips.clear();
    for(const auto &r:m_runs) {
        for(size_t i=0;i<r.capture.refs.size();++i) {
            const auto &ref=r.capture.refs[i];
            const auto key=itemKey(ref.type,ref.instance,ref.item);
            const bool drawn=ref==r.signal.target || int(i)==r.response;
            m_chips[key]=m_chips.value(key) || drawn;
        }
    }
    // Typed windows of gains this device has (by controller and gain name).
    m_windows.clear();
    if(m_snapshot) {
        for(const auto &w:d.windows) {
            for(const auto &c:tuneControllers(*m_snapshot)) {
                if(c.typeName!=w.controllerType || c.label!=w.controllerLabel) {continue;}
                for(const auto &[id,name]:gainNamesOf(*m_snapshot,c.key)) {
                    if(name==w.gain) {m_windows[itemKey(c.key.first,c.key.second,id)]=w;}
                }
            }
        }
    }
    for(auto &g:m_gains) {
        const auto w=m_windows.constFind(g.key);
        g.typed=w==m_windows.constEnd()?std::nullopt:std::optional<GainWindow>(GainWindow{w->lo,w->hi});
    }
    // The controller, test signal and capture of the session: only on the
    // device it was recorded on, when that controller still runs.
    if(!foreign && d.setup && m_snapshot) {
        const auto list=tuneControllers(*m_snapshot);
        if(std::any_of(list.begin(),list.end(),[&](const TuneController &c){return c.key==d.setup->controller;})) {
            if(!m_controller || *m_controller!=d.setup->controller) {m_controller=d.setup->controller;m_autoEpoch=0;rebuildGains();}
            m_signal=d.setup->signal;m_capture=d.setup->capture;m_captureEdited=true;m_captureNote.clear();
            reconcileCapture();
        }
    }
    m_sessionName=d.name;
    m_recoveryOffer.reset();
    if(recovered) {m_sessionPath.clear();}
    // The live gains as an entry of this device when they differ from its
    // last one (the first is "loaded from device").
    recordCurrent(HistorySource::Changed,tr("on the device"));
    runsChanged();
    historyChanged();
    emit runChanged();
}
void TuneView::clearSession() {
    m_history.clear();m_nextHistory=1;
    m_compareA.reset();m_compareB.reset();
    m_windows.clear();
    m_sessionName.clear();m_sessionPath.clear();
    m_dirty=false;m_autosaveDue=false;m_autosave.stop();
    m_settle.stop();m_changePending=false;m_autoPending=false;m_autoEpoch=0;m_autoNote.clear();
    historyChanged();
}
QString TuneView::exportRun(int number, const QString &url) {
    const auto it=std::find_if(m_runs.begin(),m_runs.end(),[&](const RunRecord &r){return r.number==number;});
    if(it==m_runs.end()) {return tr("Run %1 is gone").arg(number);}
    const auto path=csvFilePath(localPath(url));
    if(const auto error=writeFile(path,runCsv(*it).toUtf8());!error.isEmpty()) {return tr("Could not export %1: %2").arg(path,error);}
    emit notification(tr("Run %1 exported to %2").arg(number).arg(QFileInfo(path).fileName()),0);
    return {};
}

// ---- Autosave and recovery (user decision 2026-11-18) -----------------------
void TuneView::markDirty() {
    m_dirty=true;m_autosaveDue=true;
    m_autosave.start();
    emit changed();
}
QString TuneView::recoveryPath() const {
    if(m_recoveryFolder.isEmpty() || m_deviceId.isEmpty()) {return {};}
    QString id=m_deviceId;
    for(auto &ch:id) {if(!ch.isLetterOrNumber() && ch!='-' && ch!='_') {ch='_';}}
    return QDir(m_recoveryFolder).filePath(id+QStringLiteral(".tune.json"));
}
void TuneView::flushAutosave() { writeAutosave(true); }
QString TuneView::tooLarge() const {
    const auto n=sessionSamples(m_runs);
    if(n<=kMaxSessionTotalSamples) {return {};}
    return tr("The session holds %1 samples, at most %2: remove or unpin runs").arg(n).arg(kMaxSessionTotalSamples);
}
void TuneView::writeAutosave(bool now) {
    if(!m_autosaveDue) {return;}
    // An offered recovery is kept until a run is recorded here (user
    // decision 2026-11-18): smaller changes wait.
    if(m_recoveryOffer) {return;}
    const auto path=recoveryPath();
    if(path.isEmpty()) {return;} // no device or folder yet: still due
    m_autosave.stop();
    if(!m_dirty) {m_autosaveDue=false;return;}
    if(const auto why=tooLarge();!why.isEmpty()) {
        m_autosaveDue=false;
        if(!m_tooLargeNoted) {m_tooLargeNoted=true;emit notification(tr("Not autosaved: %1").arg(why),2);}
        return;
    }
    m_tooLargeNoted=false;
    if(m_autosaveThread) {
        // The last write still runs: later, or (closing) after it.
        if(!now) {m_autosave.start();return;}
        waitAutosave();
    }
    m_autosaveDue=false;
    auto write=[data=sessionData(),path,folder=m_recoveryFolder]() {
        QDir().mkpath(folder);
        return writeFile(path,writeSession(data));
    };
    if(now) {
        if(const auto error=write();!error.isEmpty()) {emit notification(tr("Could not autosave the tuning session: %1").arg(error),2);}
        return;
    }
    // The destructor waits for the thread, so `this` outlives it.
    const auto serial=++m_autosaveSerial;
    m_autosaveError.clear();
    m_autosaveThread.reset(QThread::create([this,write=std::move(write)]() {
        m_autosaveError=write();
    }));
    connect(m_autosaveThread.get(),&QThread::finished,this,[this,serial] {
        if(!m_autosaveThread || m_autosaveSerial!=serial) {return;}
        autosaveDone();
    },Qt::QueuedConnection);
    m_autosaveThread->start();
}
void TuneView::autosaveDone() {
    if(!m_autosaveThread) {return;}
    m_autosaveThread->wait();
    const auto error=std::move(m_autosaveError);
    m_autosaveError.clear();
    m_autosaveThread.reset();
    if(!error.isEmpty()) {emit notification(tr("Could not autosave the tuning session: %1").arg(error),2);}
    if(m_autosaveDue) {m_autosave.start();}
}
void TuneView::waitAutosave() {
    if(!m_autosaveThread) {return;}
    m_autosaveThread->wait();
    const auto error=std::move(m_autosaveError);
    m_autosaveError.clear();
    m_autosaveThread.reset();
    if(!error.isEmpty()) {emit notification(tr("Could not autosave the tuning session: %1").arg(error),2);}
}
QVariantMap TuneView::recovery() const {
    if(!m_recoveryOffer) {return {{"available",false}};}
    return {{"available",true},{"time",QLocale::c().toString(*m_recoveryOffer,QStringLiteral("yyyy-MM-dd hh:mm"))}};
}
void TuneView::restoreRecovery() {
    if(!m_recoveryOffer) {return;}
    if(const auto why=session()["openReason"].toString();!why.isEmpty()) {emit notification(tr("Restore the session: %1").arg(why),2);return;}
    QFile file(recoveryPath());
    std::expected<SessionData,QString> data=std::unexpected(QString());
    if(!file.open(QIODevice::ReadOnly)) {data=std::unexpected(file.errorString());}
    else if(file.size()>kMaxSessionBytes) {data=std::unexpected(tr("too large"));}
    else {data=readSession(file.readAll());}
    file.close();
    if(!data) {
        emit notification(tr("The autosaved session could not be read: %1").arg(data.error()),2);
        discardRecovery();return;
    }
    applySession(std::move(*data),true);
    // Still unsaved: it stays in the recovery file until saved.
    m_dirty=true;
    emit changed();
}
void TuneView::discardRecovery() {
    waitAutosave();
    if(const auto path=recoveryPath();!path.isEmpty()) {QFile::remove(path);}
    m_recoveryOffer.reset();
    // Changes made while the offer showed are autosaved now.
    if(m_autosaveDue) {m_autosave.start();}
    emit changed();
}
void TuneView::defaultsForTarget() {
    const auto c=currentController();
    m_captureEdited=false;m_captureNote.clear();
    if(!c || !m_snapshot) {m_capture={};return;}
    auto target=std::find_if(c->targets.begin(),c->targets.end(),[&](const TuneTarget &t){return t.ref==m_signal.target;});
    if(target==c->targets.end()) {target=c->targets.begin();}
    const auto &d=target->descriptor;
    const double bound=std::max(std::fabs(d.has_min?bitsToNumber(d.value_type,d.min_bits):0.0),
                                std::fabs(d.has_max?bitsToNumber(d.value_type,d.max_bits):0.0));
    TestSignal s;
    s.target=target->ref;s.waveform=Waveform::Step;s.offset=0;
    // A small first step: 10 % of the range, at most 1 (the user sets the real one).
    s.amplitude=bound>0?roundGain(std::min(1.0,bound*0.1)):1.0;
    s.duration_s=1;s.repeat=1;
    m_signal=s;
    m_capture=defaultCapture(*m_snapshot,*c,*target,loopRate(),signalSeconds(s),m_snapshot->info.capture_capacity);
}
void TuneView::reconcileCapture() {
    if(!m_snapshot) {return;}
    const auto c=currentController();
    if(!c) {return;}
    // Signals that stopped running, or moved loop, leave the capture.
    const auto before=m_capture.refs.size();
    std::erase_if(m_capture.refs,[&](const Ref &r) {
        const auto s=monitorSignal(*m_snapshot,r);return !s || s->loop!=c->loop;
    });
    if(m_capture.refs.size()!=before) {m_captureNote=tr("Signals that stopped running left the capture");}
    if(std::none_of(c->targets.begin(),c->targets.end(),[&](const TuneTarget &t){return t.ref==m_signal.target;})) {defaultsForTarget();}
}
} // namespace drivelink::gui
