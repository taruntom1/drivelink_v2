#include "identify_view.hpp"
#include "command_model.hpp"
#include "layout_model.hpp"
#include "monitor_model.hpp"
#include <drivelink/protocol/gen/type_system.hpp>
#include <QLocale>
#include <QTime>
#include <cmath>

namespace drivelink::gui {
namespace defs=protocol::defs;
namespace {
QString str(const std::string &s) { return QString::fromStdString(s); }
QString num(double v, int decimals) { return QLocale::c().toString(v,'f',decimals); }
// "85 ms" below a second, "1.25 s" above.
QString seconds(double s) {
    if(!std::isfinite(s)) {return QStringLiteral("—");}
    return std::fabs(s)<1?QStringLiteral("%1 ms").arg(num(s*1000,s*1000<10?1:0)):QStringLiteral("%1 s").arg(num(s,2));
}
QString endNote(RunEnd end) {
    switch(end) {
    case RunEnd::Completed:return {};
    case RunEnd::Stopped:return IdentifyView::tr("The step was stopped early");
    case RunEnd::Disarmed:return IdentifyView::tr("The step was cut by a disarm");
    }
    return {};
}
} // namespace

IdentifyView::IdentifyView(QObject *parent):QObject(parent) {}
IdentifyView::~IdentifyView() { if(m_fitThread) {m_fitThread->wait();m_fitThread.reset();} }

std::optional<TuneController> IdentifyView::controllerInfo() const {
    if(!m_snapshot || !m_controller) {return std::nullopt;}
    for(auto &c:tuneControllers(*m_snapshot)) {if(c.key==*m_controller) {return c;}}
    return std::nullopt;
}
const ItemDescriptor *IdentifyView::inputDescriptor() const {
    const auto c=controllerInfo();
    if(!c || !m_snapshot) {return nullptr;}
    const auto t=m_snapshot->types.find(m_step.input.type);
    if(t==m_snapshot->types.end() || m_step.input.type!=c->key.first || m_step.input.instance!=c->key.second) {return nullptr;}
    const auto i=t->second.items.find(m_step.input.item);
    return i==t->second.items.end()?nullptr:&i->second.descriptor;
}
double IdentifyView::loopRate() const {
    const auto c=controllerInfo();
    return c && m_snapshot?double(loopRateHz(*m_snapshot,c->loop)):0.0;
}
uint32_t IdentifyView::capacity() const { return m_snapshot?m_snapshot->info.capture_capacity:0; }
uint32_t IdentifyView::maxRefs() const {
    if(!m_snapshot) {return 0;}
    const auto it=m_snapshot->system_params.find(defs::system::kLimitsCaptureRefs);
    return it==m_snapshot->system_params.end()?16U:valueBits(it->second);
}

void IdentifyView::setShown(bool on) {
    if(on==m_shown) {return;}
    m_shown=on;emit changed();
}
void IdentifyView::setContext(std::shared_ptr<const DeviceSnapshot> snapshot, std::optional<InstanceKey> controller,
                              const QString &deviceId) {
    const bool otherDevice=deviceId!=m_deviceId;
    const bool other=otherDevice || controller!=m_controller;
    m_snapshot=std::move(snapshot);
    if(other) {
        m_deviceId=deviceId;m_controller=controller;
        // Another controller or device: confirm again; its result is not this one's.
        m_confirmed=false;m_modeChosen=false;
        if(otherDevice || m_resultController!=m_controller) {m_result.reset();m_rings.clear();m_resultController.reset();emit dataChanged();}
        defaults();
    } else if(!inputDescriptor()) {
        defaults();
    }
    emit changed();emit runChanged();
}
void IdentifyView::defaults() {
    const auto c=controllerInfo();
    if(!c) {return;}
    if(const auto input=defaultStepInput(*c)) {
        m_step.input=input->ref;
        // The default step: 30 % of the input's positive range.
        const auto &d=input->descriptor;
        const double hi=d.has_max?bitsToNumber(d.value_type,d.max_bits):1.0;
        if(std::fabs(m_step.to)>std::fabs(hi) || m_step.to==0) {m_step.to=roundGain(0.3*hi);}
        if(std::fabs(m_step.from)>std::fabs(hi)) {m_step.from=0;}
    } else {
        m_step.input={};
    }
    if(!m_modeChosen) {
        m_mode=-1;
        if(const auto mode=defaultMode(*m_snapshot,c->key)) {
            const auto choices=modeChoices(*m_snapshot,c->key);
            for(size_t i=0;i<choices.size();++i) {
                if(choices[i].item==mode->item && choices[i].value==mode->value) {m_mode=int(i);}
            }
        }
    }
}
void IdentifyView::reset() {
    if(m_fitThread) {m_fitThread->wait();m_fitThread.reset();m_fitResult.reset();}
    m_snapshot.reset();m_controller.reset();m_deviceId.clear();
    m_confirmed=false;m_modeChosen=false;m_stopping=false;m_lastEpoch=0;
    m_phase=Phase::Idle;m_phaseText.clear();m_progress=-1;
    m_result.reset();m_resultController.reset();m_rings.clear();
    emit changed();emit runChanged();emit dataChanged();
}
void IdentifyView::refreshRunState() {
    // A DISARM (or another window's ARM) clears the confirmation; a tick
    // given while disarmed carries into the next ARM.
    const uint64_t epoch=m_hooks.armEpoch?m_hooks.armEpoch():0;
    if(epoch!=m_lastEpoch) {
        if(m_lastEpoch!=0 && m_confirmed) {m_confirmed=false;emit changed();}
        m_lastEpoch=epoch;
    }
    emit runChanged();
}

QVariantMap IdentifyView::availability() const {
    const auto c=controllerInfo();
    if(!c) {return {{"available",false},{"reason",tr("Pick a controller first")}};}
    if(stepInputs(*c).empty()) {
        return {{"available",false},{"reason",tr("%1 has no test-signal input that can be commanded back to 0").arg(c->label)}};
    }
    if(responseCandidates(*m_snapshot,c->key).empty()) {
        return {{"available",false},
            {"reason",tr("%1 has no signal with a unit that a PID gain group (<prefix>.kp, .ki) is named after").arg(c->label)}};
    }
    return {{"available",true},{"reason",QString()}};
}
QVariantList IdentifyView::inputs() const {
    QVariantList result;
    if(const auto c=controllerInfo()) {
        for(const auto &t:stepInputs(*c)) {
            result<<QVariantMap{{"key",itemKey(t.ref.type,t.ref.instance,t.ref.item)},{"name",str(t.descriptor.name)},
                {"unit",str(t.descriptor.unit)}};
        }
    }
    return result;
}
QStringList IdentifyView::modes() const {
    QStringList result{tr("Leave the mode unchanged")};
    if(m_snapshot && m_controller) {
        for(const auto &c:modeChoices(*m_snapshot,*m_controller)) {result<<c.label;}
    }
    return result;
}
QVariantMap IdentifyView::step() const {
    const auto *d=inputDescriptor();
    const auto name=d?str(d->name):QString();
    QString problem=d?identifyProblem(m_step,loopRate(),*d):tr("Pick a step input");
    QString captureText;
    if(const auto s=setup(problem.isEmpty()?&problem:nullptr)) {
        const double rate=loopRate()/std::max<uint16_t>(1,s->run.capture.divider);
        captureText=tr("%1 signals at %2 Hz · %3 of %4").arg(s->run.capture.refs.size()).arg(QLocale::c().toString(rate,'g',4),
            bytesText(captureBytes(s->run.capture)),bytesText(capacity()));
    }
    return {{"input",d?itemKey(m_step.input.type,m_step.input.instance,m_step.input.item):QString()},{"inputName",name},
        {"inputUnit",d?str(d->unit):QString()},{"mode",m_mode+1},{"from",m_step.from},{"to",m_step.to},
        {"seconds",m_step.seconds},{"problem",problem},
        {"summary",tr("%1 step %2 → %3 · captured %4 s").arg(name,num(m_step.from,2),num(m_step.to,2),num(m_step.seconds,1))},
        {"capture",captureText}};
}
void IdentifyView::setConfirmed(bool on) {
    if(on==m_confirmed || active()) {return;}
    m_confirmed=on;
    emit changed();emit runChanged();
}
QVariantList IdentifyView::checks() const {
    QVariantList result;
    result<<QVariantMap{{"status",m_confirmed?Pass:Pending},
        {"label",tr("The actuator can move freely (wheels off the ground, nothing it can harm)")},
        {"detail",m_confirmed?QString():tr("Tick to confirm; a DISARM clears it")},{"tag",QString()}};
    const auto *d=inputDescriptor();
    const auto input=d?str(d->name):tr("the input");
    std::optional<ModeChoice> mode;
    if(m_snapshot && m_controller && m_mode>=0) {
        const auto choices=modeChoices(*m_snapshot,*m_controller);
        if(size_t(m_mode)<choices.size()) {mode=choices[size_t(m_mode)];}
    }
    if(mode && mode->name==QLatin1String("open_loop")) {
        result<<QVariantMap{{"status",Warning},{"label",tr("Open loop: nothing limits the response but %1").arg(input)},
            {"detail",tr("%1 for the step, then written back").arg(mode->label)},{"tag",tr("warning")}};
    } else if(mode) {
        result<<QVariantMap{{"status",Warning},{"label",tr("%1 holds during the step").arg(mode->label)},
            {"detail",tr("%1 must be the input this mode obeys; written back afterwards").arg(input)},{"tag",tr("warning")}};
    } else {
        result<<QVariantMap{{"status",Warning},{"label",tr("The mode stays as it is")},
            {"detail",tr("%1 must be the input the current mode obeys").arg(input)},{"tag",tr("warning")}};
    }
    return result;
}
std::optional<IdentifyView::Setup> IdentifyView::setup(QString *problem) const {
    const auto fail=[&](const QString &why) {if(problem) {*problem=why;} return std::optional<Setup>();};
    const auto c=controllerInfo();
    if(!c || !m_snapshot) {return fail(tr("Pick a controller first"));}
    if(const auto a=availability();!a["available"].toBool()) {return fail(a["reason"].toString());}
    const auto *d=inputDescriptor();
    if(!d) {return fail(tr("Pick a step input"));}
    if(const auto p=identifyProblem(m_step,loopRate(),*d);!p.isEmpty()) {return fail(p);}
    Setup s;
    s.step=m_step;
    const auto candidates=responseCandidates(*m_snapshot,c->key);
    s.run.controller=*c;
    s.run.signal=identifySignal(m_step);
    s.run.capture=identifyCapture(m_step,candidates,loopRate(),capacity(),maxRefs());
    if(const auto p=captureProblem(*m_snapshot,s.run.capture,c->loop,capacity(),maxRefs());!p.isEmpty()) {return fail(p);}
    if(m_mode>=0) {
        const auto choices=modeChoices(*m_snapshot,c->key);
        if(size_t(m_mode)>=choices.size()) {return fail(tr("Pick the mode for the step"));}
        s.mode=choices[size_t(m_mode)];
        s.previous=enumValue(*m_snapshot,c->key,s.mode->item);
        if(!s.previous) {return fail(tr("The mode's current value is unknown"));}
    }
    s.openLoop=runsOpenLoop(*m_snapshot,c->key,s.mode,*d);
    return s;
}
QVariantMap IdentifyView::run() const {
    QString blocked;
    if(active()) {blocked=tr("The step is running");}
    if(blocked.isEmpty() && m_hooks.runBlocked) {blocked=m_hooks.runBlocked();}
    if(blocked.isEmpty()) {setup(&blocked);}
    if(blocked.isEmpty() && !m_confirmed) {blocked=tr("Confirm the first check");}
    return {{"active",active()},{"stopping",m_stopping},{"text",m_phaseText},{"progress",m_progress},
        {"canRun",blocked.isEmpty()},{"blocked",blocked},{"done",current()!=nullptr}};
}
void IdentifyView::setPhase(Phase phase, const QString &text, double progress) {
    m_phase=phase;m_phaseText=text;m_progress=progress;
    if(phase==Phase::Idle) {m_stopping=false;}
    emit runChanged();emit changed();
}

void IdentifyView::setInput(const QString &key) {
    if(active()) {return;}
    const auto ref=parseItemKey(key);
    const auto c=controllerInfo();
    if(!ref || !c) {return;}
    const auto inputs=stepInputs(*c);
    if(std::none_of(inputs.begin(),inputs.end(),[&](const TuneTarget &t){return t.ref==*ref;})) {return;}
    m_step.input=*ref;
    emit changed();emit runChanged();
}
void IdentifyView::setMode(int index) {
    if(active() || !m_snapshot || !m_controller) {return;}
    const auto n=int(modeChoices(*m_snapshot,*m_controller).size());
    if(index<0 || index>n) {return;}
    m_mode=index-1;m_modeChosen=true;
    emit changed();emit runChanged();
}
QString IdentifyView::setField(const QString &field, const QString &text) {
    if(active()) {return tr("Locked while the step runs");}
    auto t=text.trimmed();
    if(t.startsWith('+')) {t.remove(0,1);}
    bool ok=false;const double v=QLocale::c().toDouble(t,&ok);
    if(!ok || !std::isfinite(v)) {return tr("Not a number");}
    if(field==QLatin1String("from")) {m_step.from=v;}
    else if(field==QLatin1String("to")) {m_step.to=v;}
    else if(field==QLatin1String("seconds")) {
        if(!(v>0)) {return tr("Capture more than 0 s");}
        m_step.seconds=v;
    } else {return tr("Unknown field");}
    emit changed();emit runChanged();
    return {};
}
void IdentifyView::startStep() {
    if(!run()["canRun"].toBool() || !m_hooks.start) {return;}
    m_hooks.start();
}
void IdentifyView::stopStep() {
    if(!active() || !m_hooks.stop) {return;}
    m_hooks.stop();
}

void IdentifyView::addResult(std::shared_ptr<const DeviceSnapshot> snapshot, RunRecord record, const Setup &s) {
    if(m_fitThread) {m_fitThread->wait();m_fitThread.reset();m_fitResult.reset();}
    m_fitTime=QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    const auto key=s.run.controller.key;
    const double rate=snapshot?double(loopRateHz(*snapshot,s.run.controller.loop)):0.0;
    // The fit runs on a QThread worker: the GUI thread handles DISARM, and
    // QThread avoids the MinGW emutls/FLS QAdoptedThread teardown crash.
    auto result=std::make_shared<IdentifyResult>();
    m_fitResult=result;
    m_fitThread.reset(QThread::create([snapshot,record=std::move(record),step=s.step,mode=s.mode,rate,open=s.openLoop,result]() mutable {
        auto r=snapshot?makeIdentify(*snapshot,std::move(record),step,mode,rate):IdentifyResult{};
        r.openLoop=open;
        *result=std::move(r);
    }));
    QThread *raw=m_fitThread.get();
    connect(raw,&QThread::finished,this,[this,raw] {
        if(m_fitThread.get()==raw) {fitDone();}
    },Qt::QueuedConnection);
    m_resultController=key;
    m_result.reset();m_rings.clear();
    m_fitThread->start();
    emit changed();emit dataChanged();
}
void IdentifyView::waitFit() {
    if(m_fitThread) {m_fitThread->wait();fitDone();}
}
void IdentifyView::fitDone() {
    if(!m_fitThread) {return;}
    m_fitThread->wait();
    m_fitThread.reset();
    auto result=std::move(m_fitResult);
    if(!result) {return;}
    // The controller changed while it was fitted: the result is not this one's.
    if(m_resultController!=m_controller) {m_resultController.reset();emit changed();return;}
    m_result=std::move(*result);m_resultTime=m_fitTime;
    rebuildRings();
    emit changed();emit dataChanged();
}
const IdentifyResult *IdentifyView::current() const {
    return m_result && m_resultController && m_resultController==m_controller?&*m_result:nullptr;
}
void IdentifyView::rebuildRings() {
    m_rings.clear();
    const auto *r=current();
    if(!r || r->response<0 || size_t(r->response)>=r->fits.size()) {return;}
    const auto &f=r->fits[size_t(r->response)];
    if(f.index<0 || size_t(f.index)>=r->run.series.size()) {return;}
    const auto &y=r->run.series[size_t(f.index)];
    SampleRing measured(std::max<size_t>(16,y.size())),fitted(std::max<size_t>(16,y.size()));
    for(size_t i=0;i<y.size();++i) {
        measured.push(y.time(i),y.value(i));
        const double v=fopdtValue(f.fit,double(y.time(i))/1e6);
        if(std::isfinite(v)) {fitted.push(y.time(i),v);}
    }
    m_rings.push_back(std::move(measured));m_rings.push_back(std::move(fitted));
}
void IdentifyView::setResponse(int index) {
    if(!m_result || index<0 || size_t(index)>=m_result->fits.size()) {return;}
    m_result->response=index;
    rebuildRings();
    emit changed();emit dataChanged();
}
void IdentifyView::setSpeed(int index) {
    if(index<0 || index>2) {return;}
    m_speed=index;emit changed();
}
QStringList IdentifyView::speeds() const { return {tr("Robust"),tr("Balanced"),tr("Fast")}; }

QVariantMap IdentifyView::result() const {
    const bool fitting=m_fitThread!=nullptr;
    const auto *r=current();
    if(!r) {return {{"available",false},{"fitting",fitting}};}
    QVariantMap m{{"available",true},{"fitting",fitting},{"time",m_resultTime},{"end",endNote(r->run.end)},
        {"response",r->response}};
    QString inputName;
    for(size_t i=0;i<r->run.capture.refs.size();++i) {
        if(r->run.capture.refs[i]==r->step.input && i<r->run.names.size()) {inputName=r->run.names[i];}
    }
    m["summary"]=tr("%1 step %2 → %3 · captured %4 s").arg(inputName,num(r->step.from,2),num(r->step.to,2),num(r->step.seconds,1));
    QStringList responses;
    for(const auto &f:r->fits) {responses<<(f.unit.isEmpty()?f.name:QStringLiteral("%1 (%2)").arg(f.name,f.unit));}
    m["responses"]=responses;
    if(r->response<0 || size_t(r->response)>=r->fits.size()) {m["problem"]=tr("Nothing was captured to fit");return m;}
    const auto &f=r->fits[size_t(r->response)];
    QString inputUnit;
    for(size_t i=0;i<r->run.capture.refs.size();++i) {
        if(r->run.capture.refs[i]==r->step.input && i<r->run.units.size()) {inputUnit=r->run.units[i];}
    }
    m["gain"]=f.fit.gain?gainText(*f.fit.gain):QStringLiteral("—");
    m["gainUnit"]=gainUnit(f.unit,inputUnit.isEmpty()?inputName:inputUnit);
    m["timeConstant"]=f.fit.time_constant_s?seconds(*f.fit.time_constant_s):QStringLiteral("—");
    m["deadTime"]=f.fit.dead_time_s?seconds(*f.fit.dead_time_s):QStringLiteral("—");
    m["error"]=f.fit.error_pct?QStringLiteral("%1 %").arg(num(*f.fit.error_pct,1)):QStringLiteral("—");
    const auto q=fitQuality(f.fit);
    m["quality"]=int(q);m["qualityText"]=f.fit.error_pct?qualityText(q):QString();
    m["problem"]=str(f.fit.problem);
    return m;
}
std::optional<Suggestion> IdentifyView::currentSuggestion() const {
    const auto *r=current();
    if(!r || !m_snapshot || !m_controller || r->response<0 || size_t(r->response)>=r->fits.size()) {return std::nullopt;}
    return suggestGains(*m_snapshot,*m_controller,r->fits[size_t(r->response)],TuneSpeed(m_speed),r->loopPeriodS,r->openLoop);
}
QVariantMap IdentifyView::suggestion() const {
    QVariantMap m{{"speed",m_speed}};
    const auto s=currentSuggestion();
    if(!s) {m["gains"]=QVariantList();m["problem"]=QString();m["canApply"]=false;m["applyReason"]=tr("Run the step first");return m;}
    QVariantList gains;
    for(const auto &g:s->gains) {
        gains<<QVariantMap{{"key",itemKey(m_controller->first,m_controller->second,g.item)},{"name",g.name},
            {"current",gainText(g.current)},{"suggested",gainText(g.suggested)},{"clamped",g.clamped}};
    }
    m["gains"]=gains;m["problem"]=s->problem;
    m["closedLoop"]=s->problem.isEmpty()?tr("τc %1").arg(seconds(s->pi.closed_loop_s)):QString();
    QString reason=s->problem;
    if(reason.isEmpty() && active()) {reason=tr("The step is running");}
    if(reason.isEmpty() && m_hooks.applyBlocked) {reason=m_hooks.applyBlocked();}
    if(reason.isEmpty() && std::all_of(s->gains.begin(),s->gains.end(),[](const SuggestedGain &g){return float(g.current)==float(g.suggested);})) {
        reason=tr("The gains already have these values");
    }
    m["canApply"]=reason.isEmpty();m["applyReason"]=reason;
    return m;
}
void IdentifyView::applySuggested() {
    const auto s=currentSuggestion();
    const auto *r=current();
    if(!s || !r || !suggestion()["canApply"].toBool() || !m_hooks.apply) {return;}
    ResolvedGains set{*m_controller,{}};
    for(const auto &g:s->gains) {set.values.push_back({g.item,g.suggested});}
    const auto &f=r->fits[size_t(r->response)].fit;
    const auto note=tr("suggested (%1): K %2, T %3, L %4").arg(speeds().value(m_speed),gainText(f.gain.value_or(NAN)),
        seconds(f.time_constant_s.value_or(NAN)),seconds(f.dead_time_s.value_or(NAN)));
    if(const auto why=m_hooks.apply(set,note);!why.isEmpty()) {emit notification(tr("Apply suggested gains: %1").arg(why),2);}
    emit changed();
}

QVariantList IdentifyView::plots() const {
    const auto *r=current();
    if(!r || m_rings.size()<2) {return {};}
    const auto &f=r->fits[size_t(r->response)];
    QVariantList series{QVariantMap{{"key","m"},{"colour",0},{"faded",false},{"target",false}},
        QVariantMap{{"key","f"},{"colour",1},{"faded",false},{"target",false}}};
    QVariantList legend{QVariantMap{{"colour",0},{"faded",false},{"text",tr("measured")}},
        QVariantMap{{"colour",1},{"faded",false},{"text",tr("fitted: first order + dead time")}}};
    QVariantList markers;
    if(r->run.endedUs && r->run.end!=RunEnd::Completed) {
        markers<<QVariantMap{{"us",double(*r->run.endedUs)},{"colour",0},{"text",endNote(r->run.end)}};
    }
    const auto title=r->controllerLabel.isEmpty()?tr("%1 · measured vs fitted model").arg(f.name)
        :tr("%1 · %2 · measured vs fitted model").arg(r->controllerLabel,f.name);
    return {QVariantMap{{"unit",f.unit},{"title",title},{"series",series},{"legend",legend},{"markers",markers}}};
}
const SampleRing *IdentifyView::ring(const QString &key) const {
    if(m_rings.size()<2) {return nullptr;}
    if(key==QLatin1String("m")) {return &m_rings[0];}
    if(key==QLatin1String("f")) {return &m_rings[1];}
    return nullptr;
}
int64_t IdentifyView::plotStart() const {
    if(m_rings.empty() || m_rings[0].size()==0) {return 0;}
    const int64_t first=m_rings[0].time(0);
    const auto *r=current();
    if(!r || r->response<0 || size_t(r->response)>=r->fits.size()) {return first;}
    // From a tenth of the step's length before it (the baseline half is long).
    const auto &f=r->fits[size_t(r->response)].fit;
    if(!f.gain) {return first;}
    return std::max(first,int64_t((f.step_time-0.1*r->step.seconds)*1e6));
}
int64_t IdentifyView::plotEnd() const {
    const auto *r=current();
    if(!r || m_rings.empty() || m_rings[0].size()==0) {return plotStart()+1;}
    // Up to the step's end: the return to 0 after it is not part of the model.
    int64_t end=m_rings[0].time(m_rings[0].size()-1);
    if(r->stepEndUs>0) {end=std::min(end,r->stepEndUs);}
    return std::max(end,plotStart()+1);
}
} // namespace drivelink::gui
