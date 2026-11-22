#include "monitor_model.hpp"
#include "layout_model.hpp"
#include "live_model.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <drivelink/protocol/gen/type_system.hpp>
#include <QCoreApplication>
#include <algorithm>
#include <bit>
#include <cmath>

namespace drivelink::gui {
namespace defs=protocol::defs;
namespace {
QString tr(const char *text) { return QCoreApplication::translate("DriveLink", text); }
QString str(const std::string &s) { return QString::fromStdString(s); }
const ModelType *typeOf(const DeviceSnapshot &s, uint8_t type) {
    const auto it=s.types.find(type);return it==s.types.end()?nullptr:&it->second;
}
// The items of an entry Monitor can plot: signals, then inputs, in id order.
std::vector<MonitorSignal> plottable(const DeviceSnapshot &s, InstanceKey k, uint8_t loop, const QString &owner) {
    std::vector<MonitorSignal> result;
    const auto *type=typeOf(s,k.first);
    if(!type) {return result;}
    for(const auto kind:{ItemKind::Signal,ItemKind::Input}) {
        for(const auto &[id,item]:type->items) {
            const auto &d=item.descriptor;
            if(d.kind!=kind || d.value_type==ValueType::None) {continue;}
            result.push_back({{k.first,k.second,id},loop,item,itemKey(k.first,k.second,id),str(d.name),str(d.unit),owner,
                              kind==ItemKind::Input});
        }
    }
    return result;
}
QString unitOf(const DeviceSnapshot &s, Ref r) {
    const auto signal=monitorSignal(s,r);return signal?signal->unit:QString();
}
}

std::vector<MonitorSection> monitorCatalogue(const DeviceSnapshot &s) {
    std::vector<MonitorSection> sections;
    for(const auto &loop:s.info.loops) {
        const InstanceKey loopKey{defs::kLoopTypeId,loop.id};
        if(!notRunning(s,loopKey).isEmpty()) {continue;}
        const auto name=loopName(s,loop.id);
        MonitorSection head{nodeKey(loopKey.first,loopKey.second),name,
                            tr("%1 · %2").arg(hzText(loopRateHz(s,loop.id)),loop.realtime()?tr("real-time"):tr("non-real-time")),
                            loop.id,true,plottable(s,loopKey,loop.id,name)};
        sections.push_back(std::move(head));
        for(const auto &[key,entry]:s.configured) {
            if(key.first==defs::kLoopTypeId || entry.instance.loop!=loop.id || !notRunning(s,key).isEmpty()) {continue;}
            const auto label=str(entry.instance.label);
            MonitorSection section{nodeKey(key.first,key.second),label,typeName(s,key.first),loop.id,false,
                                   plottable(s,key,loop.id,label)};
            if(!section.items.empty()) {sections.push_back(std::move(section));}
        }
    }
    return sections;
}

std::optional<MonitorSignal> monitorSignal(const DeviceSnapshot &s, Ref r) {
    const InstanceKey k{r.type,r.instance};
    if(!notRunning(s,k).isEmpty()) {return std::nullopt;}
    const auto *entry=configuredEntry(s,k);
    if(!entry) {return std::nullopt;}
    const uint8_t loop=r.type==defs::kLoopTypeId?r.instance:entry->instance.loop;
    const auto label=r.type==defs::kLoopTypeId?loopName(s,loop):str(entry->instance.label);
    for(auto &signal:plottable(s,k,loop,label)) {
        if(signal.ref==r) {return std::move(signal);}
    }
    return std::nullopt;
}

double sampleValue(ValueType type, uint32_t bits) {
    switch(type) {
    case ValueType::Bool:return bits?1.0:0.0;
    case ValueType::U8:case ValueType::Enum:return double(bits&0xFF);
    case ValueType::U16:return double(bits&0xFFFF);
    case ValueType::I32:return double(std::bit_cast<int32_t>(bits));
    case ValueType::F32:return double(std::bit_cast<float>(bits));
    default:return double(bits);
    }
}

QString plotValueText(ValueType type, double value) {
    if(type==ValueType::F32) {return std::isfinite(value)?QString::number(value,'g',5):QString::number(value);}
    return QString::number(qint64(value));
}

uint16_t minDivider(uint32_t rate) {
    return uint16_t(std::clamp<uint32_t>((rate+kMaxGroupRateHz-1)/kMaxGroupRateHz,1,65535));
}

std::vector<RateChoice> rateChoices(uint32_t rate) {
    std::vector<RateChoice> choices;
    if(!rate) {return {{1,0}};}
    // The fastest choice is the loop rate, or the 1 kHz a group can carry
    // (the loop rate divided by minDivider when no 1-2-5 step lands there).
    const auto lowest=minDivider(rate);
    if(lowest>1) {choices.push_back({lowest,double(rate)/lowest});}
    for(uint32_t scale=1;scale<=10000;scale*=10) {
        for(const uint32_t step:{1u,2u,5u}) {
            const auto divider=scale*step;
            if(divider>65535 || (divider>1 && double(rate)/divider<1.0)) {return choices;}
            if(divider<=lowest && !(divider==1 && lowest==1)) {continue;}
            choices.push_back({uint16_t(divider),double(rate)/divider});
        }
    }
    return choices;
}

uint16_t defaultDivider(uint32_t rate) {
    for(const auto &c:rateChoices(rate)) {if(c.hz<=100.0) {return c.divider;}}
    return rateChoices(rate).back().divider;
}

QString hzText(double hz) {
    if(hz>=1000 && std::fmod(hz,100.0)==0) {return tr("%1 kHz").arg(QString::number(hz/1000.0));}
    if(hz==std::floor(hz)) {return tr("%1 Hz").arg(qint64(hz));}
    return tr("%1 Hz").arg(QString::number(hz,'f',1));
}

QString rateLabel(uint32_t rate, uint16_t divider) {
    return tr("%1 (divider %2)").arg(hzText(rate?double(rate)/divider:0)).arg(divider);
}

size_t groupEntryLimit(const DeviceSnapshot &s) {
    const auto it=s.system_params.find(defs::system::kLimitsGroupEntries);
    const auto limit=it==s.system_params.end()?0:valueBits(it->second);
    return limit?std::min<size_t>(limit,64):64;
}

uint32_t loopRateHz(const DeviceSnapshot &s, uint8_t loop) {
    // rate_hz is a live param: the configured value follows a change made
    // after GET_INFO, which reports the rate at the last metadata load.
    if(const auto *entry=configuredEntry(s,{defs::kLoopTypeId,loop})) {
        if(const auto it=entry->params.find(protocol::defs::loop::kRateHz);it!=entry->params.end() && valueBits(it->second)) {
            return valueBits(it->second);
        }
    }
    const auto it=std::find_if(s.info.loops.begin(),s.info.loops.end(),[&](const LoopInfo &l){return l.id==loop;});
    return it==s.info.loops.end()?0:it->rate_hz;
}

double streamBytesPerSecond(const DeviceSnapshot &s, const LinkStream &stream) {
    const auto rate=loopRateHz(s,stream.loop);
    const auto hz=std::min(double(rate)/std::max<uint16_t>(stream.divider,1),double(kMaxGroupRateHz));
    return double(subscriptionFrameBytes(stream.values))*hz;
}

double statusBytesPerSecond(const DeviceSnapshot &s) {
    const auto it=s.system_params.find(defs::system::kStatusRateHz);
    const auto rate=it==s.system_params.end()?10u:valueBits(it->second);
    const size_t loops=s.safeMode()?0:s.info.loops.size();
    return double(kFrameOverhead+19+5*loops)*rate;
}

double linkLoad(const DeviceSnapshot &s, std::span<const LinkStream> streams) {
    double bytes=statusBytesPerSecond(s);
    for(const auto &stream:streams) {bytes+=streamBytesPerSecond(s,stream);}
    return bytes/kLinkBytesPerSecond;
}

QString percentText(double load) { return tr("%1 %").arg(qRound(load*100)); }

bool PlotSet::contains(Ref r) const {
    return std::any_of(plots.begin(),plots.end(),[&](const Plot &p){return std::find(p.series.begin(),p.series.end(),r)!=p.series.end();});
}
size_t PlotSet::count() const {
    size_t n=0;for(const auto &p:plots) {n+=p.series.size();}return n;
}

void placeSignal(PlotSet &set, const DeviceSnapshot &s, Ref r) {
    if(set.contains(r)) {return;}
    const auto unit=unitOf(s,r);
    for(auto &plot:set.plots) {
        if(!plot.series.empty() && unitOf(s,plot.series.front())==unit) {plot.series.push_back(r);return;}
    }
    for(auto &plot:set.plots) {
        if(plot.series.empty()) {plot.series.push_back(r);return;}
    }
    set.plots.push_back({{r}});
}

void removeSignal(PlotSet &set, Ref r) {
    for(auto &plot:set.plots) {std::erase(plot.series,r);}
}

void pruneSignals(PlotSet &set, const DeviceSnapshot &s) {
    for(auto &plot:set.plots) {std::erase_if(plot.series,[&](Ref r){return !monitorSignal(s,r);});}
}

uint16_t dividerFor(const PlotSet &set, const DeviceSnapshot &s, uint8_t loop) {
    const auto rate=loopRateHz(s,loop);
    const auto it=set.dividers.find(loop);
    // A rate chosen before the loop was sped up live keeps within 1 kHz.
    return it!=set.dividers.end()?std::max(it->second,minDivider(rate)):defaultDivider(rate);
}

std::vector<MonitorPlan> monitorPlans(const PlotSet &set, const DeviceSnapshot &s) {
    std::vector<MonitorPlan> plans;
    for(const auto &plot:set.plots) {
        for(const auto &r:plot.series) {
            const auto signal=monitorSignal(s,r);
            if(!signal) {continue;}
            auto it=std::find_if(plans.begin(),plans.end(),[&](const MonitorPlan &p){return p.loop==signal->loop;});
            if(it==plans.end()) {plans.push_back({signal->loop,dividerFor(set,s,signal->loop),{}});it=plans.end()-1;}
            it->refs.push_back(r);
        }
    }
    std::sort(plans.begin(),plans.end(),[](const MonitorPlan &a,const MonitorPlan &b){return a.loop<b.loop;});
    return plans;
}

std::vector<LinkStream> planStreams(std::span<const MonitorPlan> plans) {
    std::vector<LinkStream> streams;
    for(const auto &p:plans) {streams.push_back({p.loop,p.divider,p.refs.size()});}
    return streams;
}

namespace {
double loadWith(const DeviceSnapshot &s, const PlotSet &set, const LinkContext &link) {
    auto streams=link.others;
    const auto plans=monitorPlans(set,s);
    for(const auto &stream:planStreams(plans)) {streams.push_back(stream);}
    return linkLoad(s,streams);
}
// The fastest rate of `loop` at or below `from` at which `set` fits.
void suggest(MonitorCheck &check, const DeviceSnapshot &s, PlotSet set, uint8_t loop, uint16_t from, const LinkContext &link) {
    for(const auto &choice:rateChoices(loopRateHz(s,loop))) {
        if(choice.divider<=from) {continue;}
        set.dividers[loop]=choice.divider;
        const auto load=loadWith(s,set,link);
        if(load<=1.0) {
            check.divider=choice.divider;
            check.suggestion=tr("Plot %1 at %2 (%3)").arg(loopName(s,loop),hzText(choice.hz),percentText(load));
            return;
        }
    }
}
}

MonitorCheck checkAdd(const DeviceSnapshot &s, const PlotSet &set, Ref r, const LinkContext &link) {
    MonitorCheck check;
    const auto signal=monitorSignal(s,r);
    if(!signal) {check.ok=false;check.reason=tr("This signal is not running");return check;}
    if(set.contains(r)) {check.load=loadWith(s,set,link);return check;}
    if(set.count()>=kMaxPlotted) {
        check.ok=false;check.reason=tr("At most %1 signals are plotted at once").arg(kMaxPlotted);return check;
    }
    const auto plans=monitorPlans(set,s);
    const auto group=std::find_if(plans.begin(),plans.end(),[&](const MonitorPlan &p){return p.loop==signal->loop;});
    if(group==plans.end() && link.freeSlots && *link.freeSlots==0) {
        check.ok=false;
        check.reason=tr("No free subscription slot for %1: remove the plots of another loop first").arg(loopName(s,signal->loop));
        return check;
    }
    if(group!=plans.end() && group->refs.size()>=link.entryLimit) {
        check.ok=false;check.reason=tr("%1 already plots %2 signals, the device's group limit").arg(loopName(s,signal->loop)).arg(link.entryLimit);
        return check;
    }
    auto next=set;placeSignal(next,s,r);
    if(next.plots.size()>kMaxPlots) {
        check.ok=false;
        check.reason=tr("All %1 plots are in use and none shows %2: remove a plot first").arg(kMaxPlots)
            .arg(signal->unit.isEmpty()?tr("unitless signals"):signal->unit);
        return check;
    }
    check.load=loadWith(s,next,link);
    if(check.load>1.0) {
        check.ok=false;
        check.reason=tr("Adding %1 needs %2 of the link").arg(signal->name,percentText(check.load));
        suggest(check,s,next,signal->loop,dividerFor(next,s,signal->loop),link);
    }
    return check;
}

MonitorCheck checkRate(const DeviceSnapshot &s, const PlotSet &set, uint8_t loop, uint16_t divider, const LinkContext &link) {
    MonitorCheck check;
    auto next=set;next.dividers[loop]=divider;
    check.load=loadWith(s,next,link);
    if(check.load>1.0 && check.load>loadWith(s,set,link)) {
        check.ok=false;
        check.reason=tr("%1 at %2 needs %3 of the link").arg(loopName(s,loop),hzText(double(loopRateHz(s,loop))/divider),percentText(check.load));
        suggest(check,s,next,loop,divider,link);
    }
    return check;
}

SampleRing::SampleRing(size_t capacity):m_time(std::max<size_t>(capacity,1)),m_value(m_time.size()) {}
void SampleRing::push(int64_t us, double value) {
    const auto cap=m_time.size();
    const auto at=(m_head+m_size)%cap;
    m_time[at]=us;m_value[at]=value;
    if(m_size<cap) {++m_size;} else {m_head=(m_head+1)%cap;}
}
size_t SampleRing::lowerBound(int64_t us) const {
    size_t lo=0,hi=m_size;
    while(lo<hi) {
        const auto mid=lo+(hi-lo)/2;
        if(time(mid)<us) {lo=mid+1;} else {hi=mid;}
    }
    return lo;
}
void SampleRing::setCapacity(size_t capacity) {
    capacity=std::max<size_t>(capacity,1);
    if(capacity==m_time.size()) {return;}
    SampleRing next(capacity);
    const auto keep=std::min(m_size,capacity);
    for(size_t i=m_size-keep;i<m_size;++i) {next.push(time(i),value(i));}
    *this=std::move(next);
}

size_t ringCapacity(double hz) {
    return size_t(std::ceil(std::max(hz,1.0)*(kMaxWindowSeconds+2)))+16;
}

std::vector<QPointF> decimate(const SampleRing &ring, int64_t t0, int64_t t1, double width) {
    std::vector<QPointF> points;
    if(ring.size()==0 || t1<=t0 || width<=0) {return points;}
    auto first=ring.lowerBound(t0);
    if(first>0) {--first;}
    const auto last=ring.lowerBound(t1+1);
    if(first>=last) {return points;}
    const double scale=width/double(t1-t0);
    const auto xOf=[&](size_t i) {return double(ring.time(i)-t0)*scale;};
    const auto columns=size_t(std::ceil(width));
    if(last-first<=2*columns+2) {
        points.reserve(last-first);
        for(size_t i=first;i<last;++i) {points.emplace_back(xOf(i),ring.value(i));}
        return points;
    }
    points.reserve(2*columns+4);
    size_t i=first;
    while(i<last) {
        const auto column=std::floor(xOf(i));
        size_t lo=i,hi=i;
        size_t j=i+1;
        for(;j<last && std::floor(xOf(j))==column;++j) {
            if(ring.value(j)<ring.value(lo)) {lo=j;}
            if(ring.value(j)>ring.value(hi)) {hi=j;}
        }
        const auto a=std::min(lo,hi),b=std::max(lo,hi);
        points.emplace_back(xOf(a),ring.value(a));
        if(b!=a) {points.emplace_back(xOf(b),ring.value(b));}
        i=j;
    }
    return points;
}

std::optional<std::pair<double,double>> valueRange(const SampleRing &ring, int64_t t0, int64_t t1) {
    if(ring.size()==0) {return std::nullopt;}
    auto first=ring.lowerBound(t0);
    if(first>0) {--first;}
    const auto last=ring.lowerBound(t1+1);
    std::optional<std::pair<double,double>> range;
    for(size_t i=first;i<last;++i) {
        const auto v=ring.value(i);
        if(!std::isfinite(v)) {continue;}
        if(!range) {range={v,v};} else {range->first=std::min(range->first,v);range->second=std::max(range->second,v);}
    }
    return range;
}

AxisRange niceAxis(double lo, double hi, int maxTicks) {
    if(!std::isfinite(lo) || !std::isfinite(hi)) {lo=0;hi=1;}
    if(hi<lo) {std::swap(lo,hi);}
    if(hi-lo<1e-12) {
        const double pad=std::max(std::abs(lo)*0.1,1e-6);
        if(lo==0 && hi==0) {lo=-1;hi=1;} else {lo-=pad;hi+=pad;}
    }
    const double raw=(hi-lo)/std::max(maxTicks-1,1);
    const double magnitude=std::pow(10.0,std::floor(std::log10(raw)));
    double step=magnitude*10;
    for(const double m:{1.0,2.0,5.0,10.0}) {if(raw<=m*magnitude) {step=m*magnitude;break;}}
    AxisRange axis;
    axis.min=std::floor(lo/step)*step;axis.max=std::ceil(hi/step)*step;
    for(double t=axis.min;t<=axis.max+step*1e-6;t+=step) {axis.ticks.push_back(std::abs(t)<step*1e-9?0.0:t);}
    return axis;
}

QString tickText(double value, double step) {
    const int decimals=step>=1?0:std::min(9,int(std::ceil(-std::log10(step)-1e-9)));
    return QString::number(value,'f',decimals);
}

std::optional<double> valueAt(const SampleRing &ring, int64_t us) {
    const auto i=ring.lowerBound(us+1);
    if(i==0) {return std::nullopt;}
    return ring.value(i-1);
}

QString durationText(int64_t us) {
    const auto a=std::llabs(us);
    if(a<1000) {return tr("%1 µs").arg(us);}
    if(a<1000000) {return tr("%1 ms").arg(QString::number(double(us)/1000.0,'f',a<10000?2:1));}
    return tr("%1 s").arg(QString::number(double(us)/1e6,'f',3));
}

QString snapshotCsv(std::span<const CsvColumn> columns, int64_t t0, int64_t t1) {
    const auto quote=[](QString text) {
        if(text.contains(',') || text.contains('"') || text.contains('\n')) {text.replace("\"","\"\"");text="\""+text+"\"";}
        return text;
    };
    QString csv="device_time_us,time_s";
    for(const auto &c:columns) {csv+=","+quote(c.name);}
    csv+="\n";
    std::vector<size_t> next(columns.size()),end(columns.size());
    for(size_t c=0;c<columns.size();++c) {
        next[c]=columns[c].ring?columns[c].ring->lowerBound(t0):0;
        end[c]=columns[c].ring?columns[c].ring->lowerBound(t1+1):0;
    }
    for(;;) {
        std::optional<int64_t> at;
        for(size_t c=0;c<columns.size();++c) {
            if(next[c]<end[c]) {const auto t=columns[c].ring->time(next[c]);if(!at || t<*at) {at=t;}}
        }
        if(!at) {break;}
        csv+=QString::number(*at)+","+QString::number(double(*at-t0)/1e6,'f',6);
        for(size_t c=0;c<columns.size();++c) {
            csv+=",";
            if(next[c]<end[c] && columns[c].ring->time(next[c])==*at) {
                csv+=QString::number(columns[c].ring->value(next[c]),'g',9);++next[c];
            }
        }
        csv+="\n";
    }
    return csv;
}
} // namespace drivelink::gui
