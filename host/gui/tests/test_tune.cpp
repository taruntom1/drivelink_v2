// Tune (H2e-1): the pure rules (tune_model), runs on vdev through the
// connection (a step run on a mock plant, metrics, the target back at 0, a
// manual command suspended during the run, Stop, a disarm mid-run, a refused
// START, gains and Revert gains to saved), the page in the shell, and the
// env-gated bench check.
#include "command_model.hpp"
#include "connection.hpp"
#include "layout_model.hpp"
#include "plot_item.hpp"
#include "safety_keys.hpp"
#include "style.hpp"
#include "test_support.hpp"
#include "tune_model.hpp"
#include "tune_session.hpp"
#include "tune_view.hpp"
#include <drivelink/protocol/defs.hpp>
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_controller_cascade_pid.hpp>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <bit>
#include <cmath>

using namespace drivelink;
using namespace drivelink::gui;
using namespace drivelink::gui::test;
namespace pid=protocol::defs::controller_cascade_pid;
namespace {
constexpr uint16_t kTune=uint16_t(protocol::defs::ItemFlag::Tune);
constexpr uint16_t kTestgen=uint16_t(protocol::defs::ItemFlag::Testgen);
ModelItem item(uint16_t id,const char *name,ItemKind kind,ValueType type,const char *unit="",uint16_t flags=0) {
    ModelItem i;i.descriptor.id=id;i.descriptor.name=name;i.descriptor.kind=kind;i.descriptor.value_type=type;
    i.descriptor.unit=unit;i.descriptor.flags=flags;return i;
}
ModelItem ranged(ModelItem i,float lo,float hi) {
    i.descriptor.has_min=i.descriptor.has_max=true;
    i.descriptor.min_bits=std::bit_cast<uint32_t>(lo);i.descriptor.max_bits=std::bit_cast<uint32_t>(hi);return i;
}
// A controller with gains in two groups, a mode enum, an untuned limit and
// two test-signal inputs; an actuator without one; a failed controller.
DeviceSnapshot sample(uint8_t mode) {
    DeviceSnapshot s;
    auto &loop=s.types[5];loop.descriptor={5,0,"loop"};
    loop.items[1]=item(1,"rate_hz",ItemKind::Param,ValueType::U16);
    auto &c=s.types[32];c.descriptor={32,1,"controller.cascade_pid"};
    c.items[16]=item(16,"mode",ItemKind::Param,ValueType::Enum,"",kTune);
    c.items[16].enums={{0,"open_loop"},{1,"velocity"},{2,"position"}};
    c.items[17]=ranged(item(17,"duty_limit",ItemKind::Param,ValueType::F32),0,1);
    c.items[32]=ranged(item(32,"vel.kp",ItemKind::Param,ValueType::F32,"",kTune),0,1000);
    c.items[33]=ranged(item(33,"vel.ki",ItemKind::Param,ValueType::F32,"",kTune),0,1000);
    c.items[48]=ranged(item(48,"pos.kp",ItemKind::Param,ValueType::F32,"",kTune),0,1000);
    c.items[0x101]=item(0x101,"velocity",ItemKind::Signal,ValueType::F32,"rad/s");
    c.items[0x102]=item(0x102,"duty",ItemKind::Signal,ValueType::F32);
    c.items[0x200]=ranged(item(0x200,"velocity_sp",ItemKind::Input,ValueType::F32,"rad/s",kTestgen),-1000,1000);
    c.items[0x201]=item(0x201,"position_sp",ItemKind::Input,ValueType::F32,"rad",kTestgen);
    auto &a=s.types[112];a.descriptor={112,1,"actuator.mock"};
    a.items[0x200]=ranged(item(0x200,"duty",ItemKind::Input,ValueType::F32),-1,1); // no testgen flag here
    s.info.loops={{0,"control",100,1}};
    s.info.capture_capacity=4*1024*1024;
    s.configured[{5,0}]={{5,0,255,0,"control"},{}};
    s.configured[{32,0}]={{32,0,0,0,"left_drive"},{{16,EnumValue{mode}},{32,0.85f},{33,4.0f},{48,0.0f},{17,1.0f}}};
    s.configured[{32,1}]={{32,1,0,1,"broken"},{}};
    s.configured[{112,0}]={{112,0,0,0,"left_pwm"},{}};
    s.pending=s.configured;
    return s;
}
// A captured 1.5 s step of 2 rad/s at 100 Hz: velocity_sp and a first-order
// velocity with time constant tau.
RunRecord stepRun(const DeviceSnapshot &s,double tau) {
    CaptureSetup cap;cap.refs={{32,0,0x200},{32,0,0x101}};cap.samples=160;cap.pre=10;
    CaptureData data;
    data.status.state=CaptureState::Done;data.status.divider=1;data.status.period_us=10000;
    data.status.trigger_index=10;data.status.refs=cap.refs;
    for(int i=0;i<160;++i) {
        const double t=(i-10)*0.01;
        const float sp=t>=0?2.0f:0.0f,v=t>=0?float(2*(1-std::exp(-t/tau))):0.0f;
        data.data.push_back({std::bit_cast<uint32_t>(sp),std::bit_cast<uint32_t>(v)});
    }
    TestSignal step;step.target={32,0,0x200};step.amplitude=2;step.duration_s=1.5;
    return makeRun(s,data,cap,step);
}
QString readText(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly)?QString::fromUtf8(f.readAll()):QString();
}
}

class TuneTests : public QObject {
    Q_OBJECT
private slots:
    void tuneLogic() {
        const auto s=sample(1);
        // Controllers: running instances with a testgen f32 input only.
        const auto list=tuneControllers(s);
        QCOMPARE(list.size(),size_t(1));QCOMPARE(list[0].label,QString("left_drive"));
        QCOMPARE(list[0].targets.size(),size_t(2));
        // Gains (the Tune flag): mode on top; vel.* first and open in
        // velocity mode, pos.* collapsed; duty_limit is not tune-flagged.
        auto layout=gainLayout(s,{32,0});
        QCOMPARE(layout.enums,std::vector<uint16_t>{16});
        QCOMPARE(layout.groups.size(),size_t(2));
        QCOMPARE(layout.groups[0].prefix,QString("vel"));QVERIFY(layout.groups[0].open && layout.groups[0].selected);
        QCOMPARE(layout.groups[0].items,(std::vector<uint16_t>{32,33}));
        QCOMPARE(layout.groups[1].prefix,QString("pos"));QVERIFY(!layout.groups[1].open);
        layout=gainLayout(sample(2),{32,0});
        QCOMPARE(layout.groups[0].prefix,QString("pos"));QVERIFY(layout.groups[0].open);QVERIFY(!layout.groups[1].open);
        // Open loop selects neither: both are collapsed (both match a mode).
        layout=gainLayout(sample(0),{32,0});
        QVERIFY(!layout.groups[0].open && !layout.groups[1].open);
        // A group no enum value names is always open.
        auto other=sample(1);other.types[32].items[34]=ranged(item(34,"ff.gain",ItemKind::Param,ValueType::F32,"",kTune),0,10);
        layout=gainLayout(other,{32,0});
        QCOMPARE(layout.groups.size(),size_t(3));QCOMPARE(layout.groups[1].prefix,QString("ff"));QVERIFY(layout.groups[1].open);

        // The log window: ÷4..×4 within the range, on the value's side of 0.
        auto w=gainWindow(0.85,0,1000);QVERIFY(w);QCOMPARE(w->lo,0.2125);QCOMPARE(w->hi,3.4);
        QVERIFY(!gainWindow(0,0,1000));
        w=gainWindow(800,0,1000);QCOMPARE(w->hi,1000.0);
        w=gainWindow(-2,-5,5);QVERIFY(w);QCOMPARE(w->lo,0.5);QCOMPARE(w->hi,5.0);
        w=gainWindow(1000,0,1000);QVERIFY(w);QCOMPARE(w->hi,1000.0); // at the maximum: room below only
        QVERIFY(!gainWindow(5,5,5)); // no room at all
        QCOMPARE(logPosition(0.85,*gainWindow(0.85,0,1000)),0.5);
        QVERIFY(std::abs(logMagnitude(0.75,{1,16})-8.0)<1e-9);
        QVERIFY(std::abs(logPosition(logMagnitude(0.3,{0.2,3.4}),{0.2,3.4})-0.3)<1e-12);
        w=typedWindow(0.1,2,0,1000,0.85);QVERIFY(w);QCOMPARE(w->lo,0.1);
        QVERIFY(typedWindow(-1,2,0,1000,0.85).has_value()); // magnitudes: |-1| .. 2
        QVERIFY(!typedWindow(0,0,0,1000,0.85));
        // Three significant digits; % steps keep the sign and the range.
        QCOMPARE(roundGain(0.123456),0.123);QCOMPARE(roundGain(1234.5),1230.0);QCOMPARE(roundGain(-0.0456789),-0.0457);
        QCOMPARE(stepGain(0.85,2,0,1000),1.7);QCOMPARE(stepGain(0.85,0.5,0,1000),0.425);
        QCOMPARE(stepGain(0.85,1.1,0,1000),0.935);QCOMPARE(stepGain(-2,1.1,-5,5),-2.2);
        QCOMPARE(stepGain(800,2,0,1000),1000.0);QCOMPARE(stepGain(0,2,0,1000),0.0);
        // A 0.1 % step that rounds back still moves one digit.
        QCOMPARE(stepGain(0.85,1.001,0,1000),0.851);QCOMPARE(stepGain(0.85,0.999,0,1000),0.849);
        QCOMPARE(gainText(0.85),QString("0.85"));QCOMPARE(gainText(4),QString("4.0"));QCOMPARE(gainText(200),QString("200.0"));
        QCOMPARE(gainText(0),QString("0.0"));QCOMPARE(gainText(0.000123),QString("0.000123"));

        // Test signals: the device's rules, and the target's range.
        const auto &target=s.types.at(32).items.at(0x200).descriptor;
        TestSignal t;t.target={32,0,0x200};t.amplitude=2;
        QCOMPARE(testSignalProblem(t,100,target),QString());
        t.waveform=Waveform::Sine;QVERIFY(testSignalProblem(t,100,target).contains("half the loop rate"));
        t.f0=10;QCOMPARE(testSignalProblem(t,100,target),QString());
        t.f0=50;QVERIFY(!testSignalProblem(t,100,target).isEmpty());
        t.waveform=Waveform::ChirpLog;t.f0=1;t.f1=60;QVERIFY(testSignalProblem(t,100,target).contains("End frequency"));
        t.f1=20;QCOMPARE(testSignalProblem(t,100,target),QString());
        t.waveform=Waveform::Step;t.offset=999;QVERIFY(testSignalProblem(t,100,target).contains("range"));
        t.offset=0;t.amplitude=0;QVERIFY(testSignalProblem(t,100,target).contains("Amplitude"));
        t.amplitude=1;t.duration_s=0;QVERIFY(testSignalProblem(t,100,target).contains("Duration"));
        t.duration_s=1.5;t.repeat=0;QVERIFY(!testSignalProblem(t,100,target).isEmpty());
        t.repeat=2;
        const auto config=testgenConfig(t,0);
        QCOMPARE(config.duration_ms,1500u);QCOMPARE(config.repeat,2u);QCOMPARE(config.frequency_start,0.0f);QCOMPARE(config.frequency_end,0.0f);
        QCOMPARE(signalSeconds(t),3.0);

        // Capture: defaults, the size, and the checks against the device.
        const auto &ctrl=list[0];
        auto cap=defaultCapture(s,ctrl,ctrl.targets[0],100,1.5,s.info.capture_capacity);
        QCOMPARE(cap.refs,(std::vector<Ref>{{32,0,0x200},{32,0,0x101}}));
        QCOMPARE(cap.divider,uint16_t(1));QCOMPARE(cap.samples,uint32_t(150+10));QCOMPARE(cap.pre,uint32_t(10));
        QCOMPARE(captureProblem(s,cap,0,s.info.capture_capacity,16),QString());
        QVERIFY(captureProblem(s,cap,0,100,16).contains("Needs"));
        auto small=defaultCapture(s,ctrl,ctrl.targets[0],100,1.5,600); // 2 refs × 4 B: at most 75 samples
        QVERIFY(small.divider>1);QVERIFY(captureBytes(small)<=600);
        auto dup=cap;dup.refs.push_back(dup.refs[0]);QVERIFY(captureProblem(s,dup,0,s.info.capture_capacity,16).contains("twice"));
        auto foreign=cap;foreign.refs.push_back({112,0,0x200});QCOMPARE(captureProblem(s,foreign,0,s.info.capture_capacity,16),QString());
        auto failed=cap;failed.refs.push_back({32,1,0x101});QVERIFY(captureProblem(s,failed,0,s.info.capture_capacity,16).contains("not running"));
        QVERIFY(captureProblem(s,cap,0,s.info.capture_capacity,1).contains("At most"));
        auto pre=cap;pre.pre=pre.samples;QVERIFY(!captureProblem(s,pre,0,s.info.capture_capacity,16).isEmpty());
        QCOMPARE(bytesText(18*1024),QString("18 KB"));QCOMPARE(bytesText(4*1024*1024),QString("4.0 MB"));

        // A run from capture data: times from the trigger, metrics for a step.
        CaptureData data;
        data.status.state=CaptureState::Done;data.status.divider=1;data.status.period_us=10000;
        data.status.trigger_index=10;data.status.refs=cap.refs;
        for(int i=0;i<160;++i) {
            const double t=(i-10)*0.01;
            const float sp=t>=0?2.0f:0.0f,v=t>=0?float(2*(1-std::exp(-t/0.1))):0.0f;
            data.data.push_back({std::bit_cast<uint32_t>(sp),std::bit_cast<uint32_t>(v)});
        }
        TestSignal step;step.target={32,0,0x200};step.amplitude=2;step.duration_s=1.5;
        auto run=makeRun(s,data,cap,step);
        QCOMPARE(run.startUs,int64_t(-100000));QCOMPARE(run.endUs,int64_t(1490000));
        QCOMPARE(run.series.size(),size_t(2));QCOMPARE(run.series[1].time(10),int64_t(0));
        QCOMPARE(run.response,1);QCOMPARE(run.names[1],QString("velocity"));
        const auto m=runMetrics(run);
        QVERIFY2(m.rise_s && std::abs(*m.rise_s-0.22)<0.02,m.problem.c_str());
        QVERIFY(m.overshoot_pct && *m.overshoot_pct<0.1);QVERIFY(m.settling_s);QVERIFY(m.steady_error && std::abs(*m.steady_error)<0.01);
        step.waveform=Waveform::Sine;run.signal=step;QVERIFY(!runMetrics(run).problem.empty());
        // A stop ends the metrics window there: at 0.3 s (3 tau) it has not settled, at 0.6 s it has.
        run.signal.waveform=Waveform::Step;run.endedUs=300000;QVERIFY(!runMetrics(run).settling_s);
        run.endedUs=600000;QVERIFY(runMetrics(run).settling_s && *runMetrics(run).settling_s<0.6);
        // Changed gains against the previous run.
        std::map<uint16_t,double> a{{32,0.6},{33,4}},b{{32,0.85},{33,4}};
        QCOMPARE(changedGains(s,{32,0},b,&a),QString("vel.kp 0.85"));
        QCOMPARE(changedGains(s,{32,0},a,nullptr),QString("vel.kp 0.6, vel.ki 4.0"));
        QCOMPARE(changedGains(s,{32,0},a,&a),QString("no gain change"));

        // The overlay: newest full, two faded, pinned older runs too.
        std::vector<RunRecord> runs(6);
        for(int i=0;i<6;++i) {runs[size_t(i)].number=i+1;}
        runs[0].pinned=true;
        auto o=overlay(runs);
        QCOMPARE(o.size(),size_t(4));QCOMPARE(o[0].index,size_t(0));QVERIFY(!o[0].faded);
        QVERIFY(o[1].faded && o[2].faded && !o[3].faded);QCOMPARE(o[3].index,size_t(5));
        std::vector<RunRecord> many(kMaxRuns+2);many[0].pinned=true;many[0].number=-1;many[1].number=-2;
        trimRuns(many);QCOMPARE(many.size(),kMaxRuns);QCOMPARE(many[0].number,-1); // the oldest unpinned went
        QCOMPARE(metricText(0.0381,"ms",1000,0),QString("38 ms"));QCOMPARE(metricText(std::nullopt,"%"),QString("—"));
    }

    // The view without a device: the gains card rows, the sender's 50 ms
    // coalescing and one write in flight per gain, Revert to the saved values.
    void tuneView() {
        TuneView view;
        struct Write {std::vector<Ref> refs;std::vector<uint32_t> bits;std::function<void(QString)> done;};
        std::vector<Write> writes;
        TuneView::Hooks hooks;
        hooks.write=[&](std::vector<Ref> r,std::vector<uint32_t> b,std::function<void(QString)> d) {writes.push_back({r,b,d});};
        hooks.runBlocked=[]{return QString();};
        view.setHooks(hooks);
        auto snap=sample(1);
        snap.status=StatusFrame{};snap.ready=true;
        view.setSnapshot(std::make_shared<const DeviceSnapshot>(snap),"dev");
        QCOMPARE(view.controller(),QString("32:0"));
        QCOMPARE(view.gainCount(),3); // vel.kp, vel.ki, pos.kp
        auto *rows=view.gains();
        // Rows: a header per group, then its gains.
        QCOMPARE(rows->rowCount(),5);
        QVERIFY(rows->data(rows->index(0,0),GainRows::Header).toBool());
        QCOMPARE(rows->data(rows->index(0,0),GainRows::GroupTitle).toString(),QString("vel.* gains"));
        QCOMPARE(rows->data(rows->index(1,0),GainRows::Name).toString(),QString("vel.kp"));
        QVERIFY(rows->data(rows->index(3,0),GainRows::Header).toBool());
        QVERIFY(!rows->data(rows->index(4,0),GainRows::GroupOpen).toBool());
        QCOMPARE(float(rows->data(rows->index(1,0),GainRows::Saved).toDouble()),0.85f); // clean device: saved = live
        QVERIFY(!view.revert()["enabled"].toBool());
        // A drag: the first value goes at once, the rest coalesce while it is in flight.
        view.gainPress("32:0:32");
        view.gainDrag("32:0:32",0.75);view.gainDrag("32:0:32",0.8);view.gainDrag("32:0:32",0.9);
        QCOMPARE(writes.size(),size_t(1));
        QVERIFY(view.gainBusy());
        // Run waits for the drag and its writes: the run records its gains.
        QVERIFY(view.run()["blocked"].toString().contains("Wait"));QVERIFY(!view.runSetup());
        writes.back().done({});
        // Still within 50 ms of the first send: the latest waits for the timer.
        QVERIFY(waitFor([&]{return writes.size()==2;},500));
        const double sent=std::bit_cast<float>(writes[1].bits[0]);
        QCOMPARE(float(sent),float(roundGain(0.85*std::pow(16.0,0.9-0.5))));
        view.gainRelease("32:0:32");writes.back().done({});
        QVERIFY(waitFor([&]{return !view.gainBusy();},500));
        QCOMPARE(writes.size(),size_t(2)); // the release value was the one in flight
        // Accepted but not read back yet: the row keeps showing the value sent,
        // and a run started now records it.
        QCOMPARE(float(rows->data(rows->index(1,0),GainRows::Value).toDouble()),float(sent));
        QCOMPARE(float(view.runSetup()->gains.at(32)),float(sent));
        // A refusal shows on the row.
        view.gainText("32:0:33","5");QCOMPARE(writes.size(),size_t(3));
        writes.back().done("Locked");QCOMPARE(rows->data(rows->index(2,0),GainRows::Problem).toString(),QString("Locked"));
        QVERIFY(!view.gainText("32:0:33","2000").isEmpty());QCOMPARE(writes.size(),size_t(3));
        // The device changed vel.kp and has unsaved changes: Revert writes the saved value.
        auto dirty=snap;dirty.status->flags=8;
        dirty.configured[{32,0}].params[32]=1.7f;
        view.setSnapshot(std::make_shared<const DeviceSnapshot>(dirty),"dev");
        QVERIFY(view.revert()["enabled"].toBool());
        // Not while a gain write is in flight: never two writes of one gain.
        QCOMPARE(view.gainText("32:0:33","3"),QString());QCOMPARE(writes.size(),size_t(4));
        QVERIFY(!view.revert()["enabled"].toBool());QVERIFY(view.revert()["reason"].toString().contains("Wait"));
        view.revertGains();QCOMPARE(writes.size(),size_t(4));
        writes.back().done({});
        QVERIFY(view.revert()["enabled"].toBool());
        view.revertGains();QCOMPARE(writes.size(),size_t(5));
        QCOMPARE(writes.back().refs,(std::vector<Ref>{{32,0,32}}));QCOMPARE(std::bit_cast<float>(writes.back().bits[0]),0.85f);
        writes.back().done({});
        // Accepted: the row shows the reverted value until the read-back.
        QCOMPARE(float(rows->data(rows->index(1,0),GainRows::Value).toDouble()),0.85f);
        // Another device: saved values unknown while it is dirty.
        view.setSnapshot(std::make_shared<const DeviceSnapshot>(dirty),"other");
        QVERIFY(!view.revert()["enabled"].toBool());QVERIFY(view.revert()["reason"].toString().contains("unknown"));
        // Gain 0: no slider window and no steps.
        view.gainStep("32:0:48",2);QCOMPARE(writes.size(),size_t(5));
        QVERIFY(rows->data(rows->index(4,0),GainRows::Zero).toBool());
    }

    // H2e-2, pure: history rules, name matching, A/B metrics, session files
    // (round trip and refusals), the run CSV.
    void tuneSessionLogic() {
        const auto s=sample(1);
        const std::map<uint16_t,QString> names{{32,"vel.kp"},{33,"vel.ki"},{48,"pos.kp"}};
        // Equal as f32; the changes and the equal gains as text.
        QVERIFY(sameGains({{32,0.85}},{{32,double(0.85f)}}));
        QVERIFY(!sameGains({{32,0.85}},{{32,0.86}}));QVERIFY(!sameGains({{32,0.85}},{{32,0.85},{33,1}}));
        QCOMPARE(gainChanges(names,{{32,0.6},{33,4}},{{32,0.85},{33,4}}),QString("vel.kp 0.6 → 0.85"));
        QCOMPARE(equalGains(names,{{32,0.6},{33,4}},{{32,0.85},{33,4}}),QString("vel.ki equal"));
        QCOMPARE(equalGains(names,{{32,0.6}},{{32,0.85}}),QString());
        // Entries: a set equal to the controller's last one is not added; the
        // limit is per controller.
        std::vector<HistoryEntry> h;
        HistoryEntry e;e.controllerType="controller.cascade_pid";e.controllerLabel="left_drive";e.gains={{32,1}};e.id=1;
        QCOMPARE(recordEntry(h,e),0);
        e.id=2;QCOMPARE(recordEntry(h,e),0);QCOMPARE(h.size(),size_t(1));
        HistoryEntry other=e;other.controllerLabel="right_drive";other.id=3;QCOMPARE(recordEntry(h,other),1);
        e.id=4;e.gains={{32,2}};QCOMPARE(recordEntry(h,e),2);
        QCOMPARE(lastEntry(h,"controller.cascade_pid","left_drive",""),2);QCOMPARE(lastEntry(h,"x","left_drive",""),-1);
        // Another device's entry is never joined by a set recorded here.
        QCOMPARE(lastEntry(h,"controller.cascade_pid","left_drive","dev"),-1);
        HistoryEntry here=e;here.id=5;here.device="dev";QCOMPARE(recordEntry(h,here),3);
        for(int i=0;i<int(kMaxHistory)+5;++i) {e.id=10+i;e.gains={{32,3.0+i}};recordEntry(h,e);}
        QCOMPARE(size_t(std::count_if(h.begin(),h.end(),[](const HistoryEntry &x){return x.controllerLabel=="left_drive";})),kMaxHistory);
        QVERIFY(std::any_of(h.begin(),h.end(),[](const HistoryEntry &x){return x.controllerLabel=="right_drive";}));
        // The newest run of an entry still in the list.
        std::vector<RunRecord> runs(2);runs[0].number=4;runs[1].number=7;
        HistoryEntry withRuns;withRuns.runs={4,7,9};
        QCOMPARE(entryRun(withRuns,runs),std::optional<int>(7));withRuns.runs={9};QVERIFY(!entryRun(withRuns,runs));

        // Matching by type and label and gain names, never by ids alone.
        auto set=resolveGains(s,"controller.cascade_pid","left_drive",{{132,0.5},{133,2}},{{132,"vel.kp"},{133,"vel.ki"}});
        QVERIFY2(set.has_value(),qPrintable(set?QString():set.error()));
        QCOMPARE(set->key,(InstanceKey{32,0}));
        QCOMPARE(set->values,(std::vector<std::pair<uint16_t,double>>{{32,0.5},{33,2}}));
        QVERIFY(resolveGains(s,"controller.cascade_pid","right_drive",{{32,1}},names).error().contains("No running"));
        QVERIFY(resolveGains(s,"controller.cascade_pid","left_drive",{{99,1}},{{99,"ff.gain"}}).error().contains("no gain"));
        QVERIFY(resolveGains(s,"controller.cascade_pid","left_drive",{{17,1}},{{17,"duty_limit"}}).error().contains("not a live gain"));
        QVERIFY(resolveGains(s,"controller.cascade_pid","left_drive",{{32,5000}},names).error().contains("range"));
        // Values the type cannot hold (an edited file): no infinite f32, no
        // integer conversion out of range or of a fraction.
        auto open=s;
        open.types[32].items[40]=item(40,"ff.gain",ItemKind::Param,ValueType::F32,"",kTune);
        open.types[32].items[41]=item(41,"vel.n",ItemKind::Param,ValueType::U32,"",kTune);
        QVERIFY(resolveGains(open,"controller.cascade_pid","left_drive",{{40,1e39}},{{40,"ff.gain"}}).error().contains("range"));
        QVERIFY(resolveGains(open,"controller.cascade_pid","left_drive",{{40,1e30}},{{40,"ff.gain"}}).has_value());
        QVERIFY(resolveGains(open,"controller.cascade_pid","left_drive",{{41,-1}},{{41,"vel.n"}}).error().contains("range"));
        QVERIFY(resolveGains(open,"controller.cascade_pid","left_drive",{{41,1e12}},{{41,"vel.n"}}).error().contains("range"));
        QVERIFY(resolveGains(open,"controller.cascade_pid","left_drive",{{41,1.5}},{{41,"vel.n"}}).error().contains("whole"));
        QCOMPARE(gainNamesOf(s,{32,0}).size(),size_t(4)); // mode, vel.kp, vel.ki, pos.kp

        // A/B metrics: B − A with signs and units.
        auto a=stepRun(s,0.1),b=stepRun(s,0.05);
        a.metrics=runMetrics(a);b.metrics=runMetrics(b);
        const auto rows=compareMetrics(a,b);
        QCOMPARE(rows.size(),size_t(4));QCOMPARE(rows[0].name,QString("Rise time"));
        QVERIFY2(rows[0].change.startsWith(QString::fromUtf8("−")) && rows[0].change.endsWith("ms"),qPrintable(rows[0].change));
        QVERIFY(rows[3].a.endsWith("rad/s"));
        a.signal.waveform=Waveform::Sine;a.metrics=runMetrics(a);QCOMPARE(compareMetrics(a,b)[0].change,QString("—"));

        // A session round trip.
        SessionData d;
        d.deviceId="1cdbd446ff38";d.deviceName="bench";d.name="left_drive tuning";d.savedAt="2026-11-18T10:00:00";
        auto r=stepRun(s,0.1);
        r.number=3;r.colour=2;r.time="18:44:12";r.controller={32,0};r.controllerType="controller.cascade_pid";r.controllerLabel="left_drive";
        r.gains={{32,0.85},{33,4}};r.gainNames={{32,"vel.kp"},{33,"vel.ki"}};r.changed="vel.kp 0.85";
        r.end=RunEnd::Stopped;r.endedUs=600000;r.pinned=true;r.irregular=true;
        d.runs.push_back(r);
        HistoryEntry he;he.id=2;he.time="18:44:00";he.controller={32,0};he.controllerType=r.controllerType;he.controllerLabel=r.controllerLabel;
        he.gains=r.gains;he.names=r.gainNames;he.source=HistorySource::Restored;he.change="vel.kp 0.6 → 0.85";he.note="restored 18:41:30";he.runs={3,99};
        d.history.push_back(he);
        d.windows.push_back({"controller.cascade_pid","left_drive","vel.kp",0.1,2});
        TestSignal sig;sig.target={32,0,0x200};sig.waveform=Waveform::ChirpLog;sig.f0=1;sig.f1=20;sig.amplitude=0.5;sig.duration_s=2;sig.repeat=3;
        d.setup=SessionSetup{{32,0},sig,{{{32,0,0x200}},2,500,20}};
        d.nextNumber=4;d.nextColour=3;d.nextHistory=3;
        const auto bytes=writeSession(d);
        QVERIFY(bytes.contains("\"format\":\"drivelink-tune-session\""));QVERIFY(bytes.contains("\"version\":1"));
        auto back=readSession(bytes);
        QVERIFY2(back.has_value(),qPrintable(back?QString():back.error()));
        QCOMPARE(back->deviceId,d.deviceId);QCOMPARE(back->name,d.name);QCOMPARE(back->nextNumber,4);QCOMPARE(back->nextHistory,3);
        QCOMPARE(back->runs.size(),size_t(1));
        const auto &rb=back->runs[0];
        QCOMPARE(rb.number,3);QCOMPARE(rb.colour,2);QCOMPARE(rb.controllerLabel,QString("left_drive"));
        QCOMPARE(rb.gains,r.gains);QCOMPARE(rb.gainNames,r.gainNames);QCOMPARE(int(rb.end),int(RunEnd::Stopped));
        QCOMPARE(rb.endedUs,r.endedUs);QVERIFY(rb.pinned && rb.irregular);QCOMPARE(rb.response,1);
        QCOMPARE(rb.capture.refs,r.capture.refs);QCOMPARE(rb.names,r.names);QCOMPARE(rb.units,r.units);
        QCOMPARE(rb.series.size(),size_t(2));QCOMPARE(rb.series[1].size(),r.series[1].size());
        for(size_t i=0;i<r.series[1].size();++i) {
            QCOMPARE(rb.series[1].time(i),r.series[1].time(i));QCOMPARE(rb.series[1].value(i),r.series[1].value(i));
        }
        QCOMPARE(rb.startUs,r.startUs);QCOMPARE(rb.endUs,r.endUs);
        QVERIFY(rb.metrics.rise_s.has_value()); // recomputed on reading
        QCOMPARE(back->history.size(),size_t(1));
        QCOMPARE(back->history[0].runs,std::vector<int>{3}); // run 99 is not in the file
        QCOMPARE(int(back->history[0].source),int(HistorySource::Restored));QCOMPARE(back->history[0].note,he.note);
        QCOMPARE(back->windows.size(),size_t(1));QCOMPARE(back->windows[0].gain,QString("vel.kp"));
        QVERIFY(back->setup);QCOMPARE(int(back->setup->signal.waveform),int(Waveform::ChirpLog));QCOMPARE(back->setup->signal.repeat,3u);
        QCOMPARE(back->setup->capture.divider,uint16_t(2));QCOMPARE(back->setup->capture.samples,500u);
        // Refusals.
        const auto refused=[](QByteArray text) {auto x=readSession(text);return x?QString():x.error();};
        QVERIFY(refused("not json").contains("JSON"));
        QVERIFY(refused("{\"format\":\"other\",\"version\":1}").contains("Not a DriveLink"));
        QVERIFY(refused(QByteArray(bytes).replace("\"version\":1","\"version\":2")).contains("newer"));
        QVERIFY(refused(QByteArray(bytes).replace("\"nextNumber\":4","\"nextNumber\":2")).contains("nextNumber"));
        QVERIFY(!refused(QByteArray(bytes).replace("\"response\":1","\"response\":5")).isEmpty());
        QVERIFY(!refused(QByteArray(bytes).replace("\"end\":1","\"end\":7")).isEmpty());
        auto broken=QJsonDocument::fromJson(bytes).object();
        auto runsJson=broken["runs"].toArray();auto run0=runsJson[0].toObject();auto series=run0["series"].toArray();
        auto col=series[0].toObject();col["t"]=QString("@@@");series[0]=col;run0["series"]=series;runsJson[0]=run0;broken["runs"]=runsJson;
        QVERIFY(refused(QJsonDocument(broken).toJson()).contains("base64"));
        col["t"]=QString::fromLatin1(QByteArray(12,'\0').toBase64());series[0]=col;run0["series"]=series;runsJson[0]=run0;broken["runs"]=runsJson;
        QVERIFY(refused(QJsonDocument(broken).toJson()).contains("8-byte"));
        // Times that go back.
        QByteArray back16(16,'\0');qToLittleEndian<qint64>(5,back16.data());qToLittleEndian<qint64>(4,back16.data()+8);
        col["t"]=QString::fromLatin1(back16.toBase64());col["v"]=QString::fromLatin1(QByteArray(16,'\0').toBase64());
        series[0]=col;run0["series"]=series;runsJson[0]=run0;broken["runs"]=runsJson;
        QVERIFY(refused(QJsonDocument(broken).toJson()).contains("go back"));
        run0=QJsonDocument::fromJson(bytes).object()["runs"].toArray()[0].toObject();
        run0["names"]=QJsonArray{"velocity_sp"};runsJson[0]=run0;broken["runs"]=runsJson;
        QVERIFY(refused(QJsonDocument(broken).toJson()).contains("do not match"));
        runsJson=QJsonArray{QJsonDocument::fromJson(bytes).object()["runs"].toArray()[0],QJsonDocument::fromJson(bytes).object()["runs"].toArray()[0]};
        broken=QJsonDocument::fromJson(bytes).object();broken["runs"]=runsJson;
        QVERIFY(refused(QJsonDocument(broken).toJson()).contains("twice"));

        // CSV: the run, then time and one column per signal.
        const auto csv=runCsv(r);
        const auto lines=csv.split('\n');
        QVERIFY(lines[0].startsWith("# DriveLink tune run 3"));
        QVERIFY(csv.contains("# gains: vel.kp=0.85 vel.ki=4"));QVERIFY(csv.contains("# ended: stopped at 0.600000 s"));
        const int header=int(lines.indexOf(QRegularExpression("^time_s,.*")));
        QVERIFY(header>0);QCOMPARE(lines[header],QString("time_s,velocity_sp (rad/s),velocity (rad/s)"));
        QCOMPARE(lines[header+1],QString("-0.100000,0,0"));
        QCOMPARE(lines.size()-header-2,int(r.series[0].size())); // a row per sample, then the final newline
        QCOMPARE(sessionFilePath("C:/a/b"),QString("C:/a/b.tune.json"));QCOMPARE(sessionFilePath("x.JSON"),QString("x.JSON"));
        QCOMPARE(csvFilePath("run"),QString("run.csv"));QCOMPARE(csvFilePath("run.csv"),QString("run.csv"));
    }

    // H2e-2, the view with fake hooks: history entries after the gains
    // settle, Auto re-run only under the ARM of the user's Run, a gain change
    // during a run cancels it, Restore, A/B, files and the recovery file.
    void tuneSessionView() {
        QTemporaryDir dir;
        TuneView view;
        view.setRecoveryFolder(dir.filePath("recovery"));
        struct Write {std::vector<Ref> refs;std::vector<uint32_t> bits;std::function<void(QString)> done;};
        std::vector<Write> writes;
        int starts=0,cancels=0;uint64_t epoch=0;QString blocked;
        TuneView::Hooks hooks;
        hooks.write=[&](std::vector<Ref> r,std::vector<uint32_t> b,std::function<void(QString)> d) {writes.push_back({r,b,d});};
        hooks.runBlocked=[&]{return blocked;};
        hooks.start=[&]{++starts;};
        hooks.stop=[]{};
        hooks.cancel=[&]{++cancels;};
        hooks.armEpoch=[&]{return epoch;};
        hooks.autoBlocked=[]{return QString();};
        view.setHooks(hooks);
        auto snap=sample(1);snap.status=StatusFrame{};snap.ready=true;
        view.setSnapshot(std::make_shared<const DeviceSnapshot>(snap),"dev");
        // The first entry: the gains as read; not an unsaved change.
        QCOMPARE(view.historyCount(),1);
        auto *rows=view.history();
        QCOMPARE(rows->data(rows->index(0,0),HistoryRows::Change).toString(),QString("loaded from device"));
        QVERIFY(rows->data(rows->index(0,0),HistoryRows::Current).toBool());
        QVERIFY(!view.unsaved());
        QVERIFY(!view.autoRerun()["allowed"].toBool());
        QCOMPARE(view.autoRerun()["text"].toString(),QString("starts after your first Run"));
        // A typed gain settles after 0.5 s: a new entry; no Auto re-run before a Run.
        QCOMPARE(view.gainText("32:0:32","1.0"),QString());writes.back().done({});
        QVERIFY(waitFor([&]{return view.historyCount()==2;},1500));
        QCOMPARE(rows->data(rows->index(0,0),HistoryRows::Change).toString(),QString("vel.kp 0.85 → 1.0"));
        QVERIFY(view.unsaved());QCOMPARE(starts,0);
        // Under an ARM, the user's Run allows Auto re-run for that ARM only.
        epoch=1;
        QVERIFY2(view.run()["canRun"].toBool(),qPrintable(view.run()["blocked"].toString()));
        view.startRun();QCOMPARE(starts,1);
        QVERIFY(view.autoRerun()["allowed"].toBool());
        // The run ends (the connection adds it): it joins the current entry.
        view.setPhase(TuneView::Phase::Running);
        // During a run with Auto re-run on, a change cancels the run.
        QCOMPARE(view.gainText("32:0:32","1.2"),QString());QCOMPARE(cancels,1);
        view.setPhase(TuneView::Phase::Idle); // the connection ends the cancelled run
        writes.back().done({});
        QVERIFY(waitFor([&]{return starts==2;},1500)); // re-run after 0.5 s quiet
        QCOMPARE(view.historyCount(),3);
        auto setup=*view.runSetup();
        auto run=stepRun(snap,0.1);view.addRun(run,setup);
        QCOMPARE(view.runCount(),1);
        QCOMPARE(view.historyList().back().runs,std::vector<int>{1});
        QCOMPARE(rows->data(rows->index(0,0),HistoryRows::Run).toInt(),1);
        // The same gains again: no new run; another change re-runs.
        QCOMPARE(view.gainText("32:0:32","1.2"),QString());writes.back().done({});
        QTest::qWait(800);QCOMPARE(starts,2);
        // A change and the user's Run within 0.5 s: that Run carries it, no re-run after.
        QCOMPARE(view.gainText("32:0:32","1.25"),QString());writes.back().done({});
        view.startRun();QCOMPARE(starts,3);
        view.setPhase(TuneView::Phase::Running);QTest::qWait(800);
        view.setPhase(TuneView::Phase::Idle);view.addRun(stepRun(snap,0.08),*view.runSetup());
        QTest::qWait(100);QCOMPARE(starts,3);
        // A transient block (a release, a run): the re-run waits for it.
        blocked="release";
        QCOMPARE(view.gainText("32:0:32","1.3"),QString());writes.back().done({});
        QTest::qWait(800);QCOMPARE(starts,3);QVERIFY(view.autoRerun()["pending"].toBool());
        blocked.clear();view.refreshRunState();
        QVERIFY(waitFor([&]{return starts==4;},500));
        view.addRun(stepRun(snap,0.05),*view.runSetup());
        // Another ARM: no Auto re-run until the next Run.
        epoch=2;
        QCOMPARE(view.gainText("32:0:32","1.4"),QString());writes.back().done({});
        QTest::qWait(800);QCOMPARE(starts,4);QVERIFY(!view.autoRerun()["pending"].toBool());
        // Auto re-run off: gains lock during a run again.
        view.setAutoRerun(false);view.setPhase(TuneView::Phase::Running);
        QVERIFY(view.gainText("32:0:32","1.5").contains("locked"));
        view.setPhase(TuneView::Phase::Idle);view.setAutoRerun(true);
        // Loaded, 1.0, 1.2, 1.25, 1.3, 1.4 (1.5 was refused locally).
        QCOMPARE(view.historyCount(),6);
        // Restore the first entry: one batch of the gains that differ; a new entry.
        const int first=rows->data(rows->index(5,0),HistoryRows::Id).toInt();
        QVERIFY(rows->data(rows->index(5,0),HistoryRows::CanRestore).toBool());
        QVERIFY(!rows->data(rows->index(0,0),HistoryRows::CanRestore).toBool()); // the current set
        const auto before=writes.size();
        view.restoreEntry(first);
        QCOMPARE(writes.size(),before+1);
        QCOMPARE(writes.back().refs,(std::vector<Ref>{{32,0,32}}));QCOMPARE(std::bit_cast<float>(writes.back().bits[0]),0.85f);
        QVERIFY(!rows->data(rows->index(5,0),HistoryRows::CanRestore).toBool()); // writing
        writes.back().done({});
        QVERIFY(waitFor([&]{return view.historyCount()==7;},1500));
        QVERIFY(view.historyList().back().note.startsWith("restored"));
        // A/B: entries with runs only; the overlay shows just A and B.
        const auto runsOf=[&](int row){return rows->data(rows->index(row,0),HistoryRows::Run).toInt();};
        int withA=-1,withB=-1;
        for(int i=0;i<view.historyCount();++i) {if(runsOf(i)>0) {(withB<0?withB:withA)=i;}}
        QVERIFY(withA>=0 && withB>=0);
        QVERIFY(!rows->data(rows->index(0,0),HistoryRows::CanCompare).toBool());
        view.pickCompare(rows->data(rows->index(withA,0),HistoryRows::Id).toInt());
        QVERIFY(view.compare()["picking"].toBool());
        view.pickCompare(rows->data(rows->index(withB,0),HistoryRows::Id).toInt());
        auto cmp=view.compare();
        QVERIFY(cmp["active"].toBool());
        QCOMPARE(cmp["metrics"].toList().size(),4);
        QVERIFY(cmp["gainChanges"].toString().startsWith("vel.kp"));
        QCOMPARE(rows->data(rows->index(withA,0),HistoryRows::Mark).toString(),QString("A"));
        QCOMPARE(view.plots().size(),1);
        // Picking A again unmarks it; B becomes A.
        view.pickCompare(rows->data(rows->index(withA,0),HistoryRows::Id).toInt());
        QVERIFY(view.compare()["picking"].toBool());
        QCOMPARE(view.compare()["a"].toMap()["run"].toInt(),runsOf(withB));
        view.exitCompare();QVERIFY(!view.compare()["active"].toBool() && !view.compare()["picking"].toBool());

        // Files: save, export, and the recovery file of an unsaved session.
        const auto file=dir.filePath("left");
        QCOMPARE(view.saveSession(QUrl::fromLocalFile(file).toString()),QString());
        QVERIFY(QFile::exists(file+".tune.json"));QVERIFY(!view.unsaved());
        QCOMPARE(view.session()["file"].toString(),QString("left.tune.json"));QCOMPARE(view.session()["name"].toString(),QString("left"));
        QCOMPARE(view.exportRun(1,dir.filePath("run1")),QString());
        QVERIFY(readText(dir.filePath("run1.csv")).startsWith("# DriveLink tune run 1"));
        QVERIFY(!view.exportRun(42,dir.filePath("x")).isEmpty());
        view.pinRun(1,true);QVERIFY(view.unsaved());
        // Another device: the unsaved session goes to its recovery file first.
        auto other=snap;
        view.setSnapshot(std::make_shared<const DeviceSnapshot>(other),"other");
        QCOMPARE(view.runCount(),0);QCOMPARE(view.historyCount(),1);QVERIFY(!view.recovery()["available"].toBool());
        QVERIFY(QFile::exists(dir.filePath("recovery/dev.tune.json")));
        // Open the saved file here: runs from another device; Restore by name.
        QCOMPARE(view.openSession(file+".tune.json"),QString());
        QCOMPARE(view.runCount(),3);QVERIFY(view.runList().front().foreign);QVERIFY(!view.unsaved());
        QVERIFY(view.historyCount()>=6);
        QVERIFY(rows->data(rows->index(1,0),HistoryRows::Foreign).toBool());
        // The live gains are this device's first entry, and the current one.
        QVERIFY(rows->data(rows->index(0,0),HistoryRows::Current).toBool());
        QVERIFY(!rows->data(rows->index(1,0),HistoryRows::Current).toBool());
        // Opening a session ends Auto re-run's permission: it may bring another signal.
        view.startRun();QVERIFY(view.autoRerun()["allowed"].toBool());
        // Saved here, the runs keep the device they were recorded on.
        QCOMPARE(view.saveSession(dir.filePath("mixed")),QString());
        QCOMPARE(view.openSession(dir.filePath("mixed.tune.json")),QString());
        QVERIFY(!view.autoRerun()["allowed"].toBool());
        QFile mixed(dir.filePath("mixed.tune.json"));QVERIFY(mixed.open(QIODevice::ReadOnly));
        const auto reread=readSession(mixed.readAll());
        QVERIFY(reread);QCOMPARE(reread->deviceId,QString("other"));
        QCOMPARE(reread->runs.front().device,QString("dev"));QCOMPARE(reread->history.front().device,QString("dev"));
        // Back on the first device: its recovery is offered and restores the pin.
        view.setSnapshot(std::make_shared<const DeviceSnapshot>(snap),"dev");
        QVERIFY(view.recovery()["available"].toBool());
        // A gain change does not replace the offered recovery (only a run does).
        const auto recovered=readText(dir.filePath("recovery/dev.tune.json"));
        QCOMPARE(view.gainText("32:0:32","0.9"),QString());writes.back().done({});
        QTest::qWait(1800);
        QVERIFY(view.recovery()["available"].toBool());QCOMPARE(readText(dir.filePath("recovery/dev.tune.json")),recovered);
        view.restoreRecovery();
        QCOMPARE(view.runCount(),3);QVERIFY(view.runList().front().pinned);QVERIFY(!view.runList().front().foreign);
        QVERIFY(view.unsaved());QVERIFY(!view.recovery()["available"].toBool());
        // Saved: the recovery file goes.
        QCOMPARE(view.saveSession(file),QString());
        QVERIFY(!QFile::exists(dir.filePath("recovery/dev.tune.json")));
        // A bad file is refused and changes nothing.
        QFile bad(dir.filePath("bad.tune.json"));QVERIFY(bad.open(QIODevice::WriteOnly));bad.write("{}");bad.close();
        QVERIFY(view.openSession(bad.fileName()).contains("Not a DriveLink"));QCOMPARE(view.runCount(),3);
    }

    // A step run on vdev: left_drive (cascade_pid in velocity mode) on a mock
    // plant at 100 Hz.
    void tuneOperations() {
        Fixture fixture;
        uint8_t driven=0,enc=0,drive=0;
        {
            Session s(fixture.endpoint.toStdString());Client cl(s);cl.info();
            driven=cl.add(112,"driven",0).instance;enc=cl.add(113,"enc",0).instance;drive=cl.add(32,"drive",0).instance;
            const std::array<Ref,3> refs{{{113,enc,1},{32,drive,1},{32,drive,2}}};
            const std::array<uint32_t,3> values{{112u<<8|driven,112u<<8|driven,113u<<8|enc}};
            cl.set(refs,values);
            // 100 Hz: a desktop vdev at 1 kHz can stall the loop after ARM.
            const std::array<Ref,1> rate{{{5,0,1}}};const std::array<uint32_t,1> hz{{100}};cl.set(rate,hz);
            cl.system(5);cl.system(8);
        }
        QTest::qWait(150);
        LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        QStringList toasts;
        QObject::connect(&c,&Connection::notification,[&](QString text,int){toasts<<text;});
        c.connectTo(fixture.endpoint);
        QVERIFY2(waitFor([&]{return c.armEnabled() && c.snapshot()->configured.contains({32,drive});},15000),
                 qPrintable(c.armBlockedReason()+" / "+c.lastError()["text"].toString()));
        auto &tv=c.tuneView();
        const auto ctrl=nodeKey(32,drive);
        tv.selectController(ctrl);
        QCOMPARE(tv.controller(),ctrl);
        // H2e-1's rules: with Auto re-run off the gains lock during a run
        // (tuneAuto covers it on).
        tv.setAutoRerun(false);
        // The real descriptors carry the Tune flag (core v10).
        QVERIFY(tv.gainCount()>=8);
        const auto enums=tv.enums();QCOMPARE(enums.size(),1);QCOMPARE(enums[0].toMap()["name"].toString(),QString("mode"));
        // Disarmed: Run is refused with "Arm first".
        QVERIFY(tv.run()["blocked"].toString().contains("Arm first"));
        // Velocity mode and PI gains, live.
        bool done=false;
        auto pause=[&]{QTest::qWait(400);};
        tv.setEnum(enums[0].toMap()["key"].toString(),1);pause();
        QCOMPARE(tv.gainText(itemKey(32,drive,pid::kVelKp),"0.05"),QString());
        QCOMPARE(tv.gainText(itemKey(32,drive,pid::kVelKi),"0.5"),QString());
        QVERIFY(waitFor([&]{
            const auto &p=c.snapshot()->configured.at({32,drive}).params;
            return valueBits(p.at(pid::kMode))==1 && std::abs(std::bit_cast<float>(valueBits(p.at(pid::kVelKp)))-0.05f)<1e-6 &&
                   std::abs(std::bit_cast<float>(valueBits(p.at(pid::kVelKi)))-0.5f)<1e-6;}));
        // The change list has one line per param, however many writes.
        tv.gainText(itemKey(32,drive,pid::kVelKp),"0.06");tv.gainText(itemKey(32,drive,pid::kVelKp),"0.05");
        QVERIFY(waitFor([&]{return !tv.gainBusy();}));
        QCOMPARE(c.unsavedChanges().filter("drive.vel.kp").size(),1);
        // A 1 s step of 2 rad/s on velocity_sp, captured at 100 Hz.
        tv.setTarget(itemKey(32,drive,pid::kVelocitySp));
        QCOMPARE(tv.setSignalField("amplitude","2"),QString());
        QCOMPARE(tv.setSignalField("duration","1"),QString());
        QVERIFY(tv.testSignal()["problem"].toString().isEmpty());
        QVERIFY2(tv.capture()["problem"].toString().isEmpty(),qPrintable(tv.capture()["problem"].toString()));
        // Read the target back through Monitor, and latch a manual command on
        // the controller's duty_sp: the run suspends it at 0.
        auto &mv=c.monitorView();c.setPage("monitor");
        const auto spKey=itemKey(32,drive,pid::kVelocitySp);
        mv.toggleSignal(spKey);
        auto latest=[&]()->std::optional<double> {
            const auto *ring=mv.ring(spKey);
            if(!ring || ring->size()==0) {return std::nullopt;}
            return ring->value(ring->size()-1);
        };
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1 && c.commandView().enabled();}));
        auto &cv=c.commandView();const Ref dutySp{32,drive,pid::kDutySp};const auto dutyKey=itemKey(32,drive,pid::kDutySp);
        cv.add(dutyKey);cv.press(dutyKey);cv.drag(dutyKey,0.1);cv.setLatched(dutyKey,true);cv.release(dutyKey);
        const auto duty=[&]{return cv.model().inputs()[size_t(cv.model().find(dutySp))];};
        QCOMPARE(duty().value,0.1);
        QVERIFY2(tv.run()["canRun"].toBool(),qPrintable(tv.run()["blocked"].toString()));
        tv.startRun();
        QVERIFY(c.tuneRunning());
        QCOMPARE(duty().value,0.0);QVERIFY(!duty().latched);QVERIFY(duty().writes>0);
        QVERIFY(!tv.run()["canRun"].toBool());
        QVERIFY(waitFor([&]{return latest() && std::abs(*latest()-2.0)<1e-6;},5000));
        QVERIFY2(waitFor([&]{return !c.tuneRunning();},10000),qPrintable(toasts.join("; ")));
        QCOMPARE(tv.runCount(),1);
        const auto &run=tv.runList().back();
        QCOMPARE(int(run.end),int(RunEnd::Completed));QVERIFY(!run.endedUs);
        QCOMPARE(run.capture.refs.front(),(Ref{32,drive,pid::kVelocitySp}));
        // Every post-trigger sample; fewer before it when START follows ARM at once.
        QCOMPARE(run.series[0].size()-run.series[0].lowerBound(0),size_t(run.capture.samples-run.capture.pre));
        QVERIFY2(run.metrics.rise_s.has_value(),run.metrics.problem.c_str());
        QVERIFY(*run.metrics.rise_s>0.01 && *run.metrics.rise_s<1.0);
        // The suspension ended with the signal: velocity_sp read back at 0
        // (the fresh frame), duty_sp free again and at 0.
        QVERIFY2(waitFor([&]{return latest() && *latest()==0;},3000),qPrintable(QString::number(latest().value_or(-1))));
        QCOMPARE(duty().writes,0);QCOMPARE(duty().value,0.0);
        QCOMPARE(c.armState(),1);
        QCOMPARE(tv.plots().size(),1); // rad/s: setpoint and velocity

        // Stop keeps the capture: a 5 s signal stopped after about 0.5 s.
        QCOMPARE(tv.setSignalField("duration","5"),QString());
        QCOMPARE(tv.setSamples("160","10"),QString());
        tv.startRun();QVERIFY(waitFor([&]{return tv.phase()==TuneView::Phase::Running && tv.run()["progress"].toDouble()>=0;}));
        QTest::qWait(500);tv.stopRun();
        QVERIFY2(waitFor([&]{return !c.tuneRunning();},10000),qPrintable(toasts.join("; ")));
        QCOMPARE(tv.runCount(),2);
        QCOMPARE(int(tv.runList().back().end),int(RunEnd::Stopped));
        QVERIFY(tv.runList().back().endedUs && *tv.runList().back().endedUs>200000 && *tv.runList().back().endedUs<2000000);
        QVERIFY(waitFor([&]{return latest() && *latest()==0;},3000));

        // Given up while the signal runs (another client cleared the capture):
        // STOP goes once and the controller stays held at 0 until STATUS shows
        // the signal stopped; only then does the fresh frame return
        // velocity_sp to 0 (a frame before STOP would be discarded).
        toasts.clear();
        tv.startRun();QVERIFY(waitFor([&]{return tv.phase()==TuneView::Phase::Running && tv.run()["progress"].toDouble()>=0;}));
        // During a run the gains and the mode are locked.
        QVERIFY(!tv.gainText(itemKey(32,drive,pid::kVelKp),"0.06").isEmpty());
        QVERIFY(!tv.revert()["enabled"].toBool());
        QVERIFY(waitFor([&]{return latest() && std::abs(*latest()-2.0)<1e-6;},5000));
        c.runOnDevice([&](Client &client){client.captureAbort();},[&](QString){done=true;});
        QVERIFY(waitFor([&]{return done;}));done=false;
        QVERIFY2(waitFor([&]{return !c.tuneRunning();},5000),qPrintable(toasts.join("; ")));
        QVERIFY2(!toasts.filter("cleared").isEmpty() && !toasts.filter("may still be running").isEmpty(),qPrintable(toasts.join("; ")));
        QCOMPARE(tv.runCount(),2);
        QVERIFY(waitFor([&]{return duty().writes==0;},3000));
        QVERIFY2(waitFor([&]{return latest() && *latest()==0;},3000),qPrintable(QString::number(latest().value_or(-1))));
        QVERIFY(waitFor([&]{return tv.run()["canRun"].toBool();},3000));
        TestgenStatus after;
        c.runOnDevice([&](Client &client){after=client.testgenStatus(0);},[&](QString){done=true;});
        QVERIFY(waitFor([&]{return done;}));done=false;
        QCOMPARE(int(after.state),int(TestgenState::Done));QCOMPARE(int(after.reason),int(TestgenReason::Stopped));

        // A refused START (another run active on the loop) aborts the capture.
        c.runOnDevice([&](Client &client) {
            TestgenConfig other;other.loop=0;other.target={32,drive,pid::kVelocitySp};other.waveform=0;other.amplitude=0.5f;other.duration_ms=3000;
            client.testgenStart(other);
        },[&](QString){done=true;});
        QVERIFY(waitFor([&]{return done;}));done=false;
        toasts.clear();
        tv.startRun();
        QVERIFY(waitFor([&]{return !c.tuneRunning();},5000));
        QVERIFY(!toasts.filter("did not start").isEmpty());QCOMPARE(tv.runCount(),2);
        CaptureStatus status;
        c.runOnDevice([&](Client &client){status=client.captureStatus();client.testgenStop(0);},[&](QString){done=true;});
        QVERIFY(waitFor([&]{return done;}));done=false;
        QCOMPARE(int(status.state),int(CaptureState::Idle));
        QVERIFY(waitFor([&]{return duty().writes==0;}));
        QTest::qWait(300); // the other run's terminal event

        // A disarm cuts the run: the capture keeps recording, the run is incomplete.
        toasts.clear();
        QVERIFY(waitFor([&]{return tv.run()["canRun"].toBool();},3000));
        tv.startRun();QVERIFY(waitFor([&]{return tv.phase()==TuneView::Phase::Running;}));
        QTest::qWait(300);c.disarm();
        QVERIFY2(waitFor([&]{return !c.tuneRunning();},10000),qPrintable(toasts.join("; ")));
        QCOMPARE(tv.runCount(),3);QCOMPARE(int(tv.runList().back().end),int(RunEnd::Disarmed));
        QVERIFY(!toasts.filter("disarm").isEmpty());
        QVERIFY(tv.run()["blocked"].toString().contains("Arm first"));
        QVERIFY(!tv.plots().isEmpty() && !tv.plots().front().toMap()["markers"].toList().isEmpty());

        // Revert gains to saved: the device was saved before these changes.
        QVERIFY(waitFor([&]{return tv.revert()["enabled"].toBool();}));
        tv.revertGains();
        QVERIFY(waitFor([&]{
            const auto &p=c.snapshot()->configured.at({32,drive}).params;
            return std::bit_cast<float>(valueBits(p.at(pid::kVelKp)))==0.0f && std::bit_cast<float>(valueBits(p.at(pid::kVelKi)))==0.0f;}));
        // Runs stay for the same device after a close; another device clears them.
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        QCOMPARE(tv.runCount(),3);QCOMPARE(tv.controller(),QString());
    }

    // H2e-2 on vdev: no Auto re-run before the user's Run; after it a gain
    // change re-runs; a gain change during a run cancels it (STOP and ABORT,
    // not kept, the target back at 0) and re-runs; not while Tune is hidden;
    // a new ARM needs a Run again.
    void tuneAuto() {
        Fixture fixture;
        uint8_t drive=0;
        {
            Session s(fixture.endpoint.toStdString());Client cl(s);cl.info();
            const auto driven=cl.add(112,"driven",0).instance;const auto enc=cl.add(113,"enc",0).instance;drive=cl.add(32,"drive",0).instance;
            const std::array<Ref,6> refs{{{113,enc,1},{32,drive,1},{32,drive,2},{32,drive,pid::kMode},{32,drive,pid::kVelKp},{32,drive,pid::kVelKi}}};
            const std::array<uint32_t,6> values{{112u<<8|driven,112u<<8|driven,113u<<8|enc,1,std::bit_cast<uint32_t>(0.05f),std::bit_cast<uint32_t>(0.5f)}};
            cl.set(refs,values);
            const std::array<Ref,1> rate{{{5,0,1}}};const std::array<uint32_t,1> hz{{100}};cl.set(rate,hz);
            cl.system(5);cl.system(8);
        }
        QTest::qWait(150);
        LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        QStringList toasts;
        QObject::connect(&c,&Connection::notification,[&](QString text,int){toasts<<text;});
        c.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return c.armEnabled() && c.snapshot()->configured.contains({32,drive});},15000));
        c.setPage("tune");
        auto &tv=c.tuneView();
        tv.selectController(nodeKey(32,drive));
        QVERIFY(tv.autoRerun()["on"].toBool());
        tv.setTarget(itemKey(32,drive,pid::kVelocitySp));
        QCOMPARE(tv.setSignalField("amplitude","1"),QString());QCOMPARE(tv.setSignalField("duration","0.5"),QString());
        const auto kp=itemKey(32,drive,pid::kVelKp);
        // velocity_sp read back through a Monitor plot (its group keeps
        // running while Tune is shown).
        auto &mv=c.monitorView();const auto spKey=itemKey(32,drive,pid::kVelocitySp);
        c.setPage("monitor");mv.toggleSignal(spKey);
        const auto velocitySp=[&]()->std::optional<double> {
            const auto *ring=mv.ring(spKey);
            if(!ring || ring->size()==0) {return std::nullopt;}
            return ring->value(ring->size()-1);
        };
        QVERIFY(waitFor([&]{return velocitySp().has_value();},5000));
        c.setPage("tune");
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1 && tv.run()["canRun"].toBool();},5000));
        // Before the user's first Run: a gain change does not run.
        QVERIFY(!tv.autoRerun()["allowed"].toBool());
        QCOMPARE(tv.gainText(kp,"0.06"),QString());
        QTest::qWait(1200);QCOMPARE(tv.runCount(),0);QVERIFY(!c.tuneRunning());
        // The user's Run, then a gain change: a run follows by itself.
        tv.startRun();
        QVERIFY2(waitFor([&]{return !c.tuneRunning() && tv.runCount()==1;},10000),qPrintable(toasts.join("; ")));
        QVERIFY(tv.autoRerun()["allowed"].toBool());
        QCOMPARE(tv.gainText(kp,"0.07"),QString());
        QVERIFY2(waitFor([&]{return tv.runCount()==2 && !c.tuneRunning();},10000),qPrintable(toasts.join("; ")+" / "+tv.autoRerun()["text"].toString()));
        QCOMPARE(float(tv.runList().back().gains.at(pid::kVelKp)),0.07f);
        // Its history entry holds that run.
        QCOMPARE(tv.historyList().back().runs,std::vector<int>{2});
        // A change during a run cancels it: not kept, a re-run with the new gains.
        QCOMPARE(tv.setSignalField("duration","3"),QString());
        tv.startRun();
        QVERIFY(waitFor([&]{return tv.phase()==TuneView::Phase::Running && tv.run()["progress"].toDouble()>=0;},5000));
        QTest::qWait(300);
        toasts.clear();
        QCOMPARE(tv.gainText(kp,"0.08"),QString());
        QVERIFY2(waitFor([&]{return !toasts.filter("cancelled").isEmpty();},3000),qPrintable(toasts.join("; ")));
        // The re-run (3 s) starts once the cancelled signal shows it stopped.
        QVERIFY2(waitFor([&]{return c.tuneRunning();},5000),qPrintable(toasts.join("; ")+" / "+tv.autoRerun()["text"].toString()));
        tv.stopRun();
        QVERIFY2(waitFor([&]{return !c.tuneRunning();},10000),qPrintable(toasts.join("; ")));
        QCOMPARE(tv.runCount(),3); // the cancelled run was not kept
        QCOMPARE(float(tv.runList().back().gains.at(pid::kVelKp)),0.08f);
        QCOMPARE(int(tv.runList().back().end),int(RunEnd::Stopped));
        QVERIFY2(waitFor([&]{return velocitySp()==0.0;},3000),qPrintable(QString::number(velocitySp().value_or(-1))));
        // A change while START is still in flight: cancelled when it is
        // answered (STOP and ABORT), not kept, then re-run.
        QCOMPARE(tv.setSignalField("duration","0.5"),QString());
        tv.startRun();QCOMPARE(int(tv.phase()),int(TuneView::Phase::Starting));
        toasts.clear();
        QCOMPARE(tv.gainText(kp,"0.085"),QString());
        QVERIFY2(waitFor([&]{return !toasts.filter("cancelled").isEmpty();},5000),qPrintable(toasts.join("; ")));
        QVERIFY2(waitFor([&]{return tv.runCount()==4 && !c.tuneRunning();},10000),qPrintable(toasts.join("; ")+" / "+tv.autoRerun()["text"].toString()));
        QCOMPARE(float(tv.runList().back().gains.at(pid::kVelKp)),0.085f);
        QCOMPARE(int(tv.runList().back().end),int(RunEnd::Completed));
        QVERIFY(waitFor([&]{return velocitySp()==0.0;},3000));
        // Tune not shown: skipped.
        c.setPage("device");
        QCOMPARE(tv.gainText(kp,"0.09"),QString());
        QTest::qWait(1200);QCOMPARE(tv.runCount(),4);
        QVERIFY(tv.autoRerun()["text"].toString().contains("not shown"));
        c.setPage("tune");
        // A new ARM: no Auto re-run until the next Run.
        c.disarm();QVERIFY(waitFor([&]{return c.armState()==0;}));
        QVERIFY(!tv.autoRerun()["allowed"].toBool());
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1 && tv.run()["canRun"].toBool();},5000));
        QVERIFY(!tv.autoRerun()["allowed"].toBool());
        QCOMPARE(tv.gainText(kp,"0.1"),QString());
        QTest::qWait(1200);QCOMPARE(tv.runCount(),4);QVERIFY(!c.tuneRunning());
        // Session and history follow the device: autosaved beside the settings.
        QVERIFY(tv.unsaved());
        c.disarm();QVERIFY(waitFor([&]{return c.armState()==0;}));
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing();}));
        QVERIFY(QDir(dir.filePath("tune-recovery")).entryList({"*.tune.json"}).size()==1);
    }

    // The page in the shell: Run disabled with "Arm first", F5, a gain drag
    // with the mouse sends, Esc during a run disarms and ends it.
    void tuneInShell() {
        Q_INIT_RESOURCE(qmake_DriveLink_Shell);
        Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);registerShellTypes();
        Fixture fixture;
        uint8_t drive=0;
        {
            Session s(fixture.endpoint.toStdString());Client cl(s);cl.info();
            const auto driven=cl.add(112,"driven",0).instance;const auto enc=cl.add(113,"enc",0).instance;drive=cl.add(32,"drive",0).instance;
            // Velocity mode: its vel.* gains are the open group.
            const std::array<Ref,5> refs{{{113,enc,1},{32,drive,1},{32,drive,2},{32,drive,pid::kVelKp},{32,drive,pid::kMode}}};
            const std::array<uint32_t,5> values{{112u<<8|driven,112u<<8|driven,113u<<8|enc,std::bit_cast<uint32_t>(0.5f),1}};
            cl.set(refs,values);
            const std::array<Ref,1> rate{{{5,0,1}}};const std::array<uint32_t,1> hz{{100}};cl.set(rate,hz);
            cl.system(5);cl.system(8);
        }
        QTest::qWait(150);
        QTemporaryDir dir;LocalConnection connection;connection.setSettingsPath(dir.filePath("gui.ini"));
        SafetyKeys keys;
        QObject::connect(&keys,&SafetyKeys::escapeRequested,&connection,&Connection::disarm);
        QQmlApplicationEngine engine;QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("connection",&connection);
        engine.rootContext()->setContextProperty("safetyKeys",&keys);
        engine.loadFromModule("DriveLink.Shell","Shell");QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());QVERIFY(window);
        window->resize(1920,1080);window->show();QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.armEnabled() && connection.snapshot()->configured.contains({32,drive});},15000));
        auto shown=[&](const QString &name)->QQuickItem * {
            auto *item=visualChild(window->contentItem(),name);
            return item && item->isVisible()?item:nullptr;
        };
        auto center=[&](QQuickItem *item,double fx=0.5) {return item->mapToScene({item->width()*fx,item->height()/2}).toPoint();};
        window->setProperty("page",2);
        QVERIFY(waitFor([&]{return shown("tunePage") && shown("tuneRun");}));
        auto &tv=connection.tuneView();
        QVERIFY(!shown("tuneRun")->isEnabled());
        // F5 while disarmed does nothing.
        QTest::keyClick(window,Qt::Key_F5);QTest::qWait(200);QVERIFY(!connection.tuneRunning());
        // A gain drag with the mouse: sent live, the slider re-centred after.
        QVERIFY(waitFor([&]{return shown("gain_vel.kp");}));
        auto *slider=visualChild(shown("gain_vel.kp"),"gainSlider");QVERIFY(slider && slider->isEnabled());
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,center(slider,0.5));
        QTest::mouseMove(window,center(slider,0.7));QTest::mouseMove(window,center(slider,0.9));
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,center(slider,0.9));
        QVERIFY(waitFor([&]{
            const auto v=std::bit_cast<float>(valueBits(connection.snapshot()->configured.at({32,drive}).params.at(pid::kVelKp)));
            return v>0.6f && !tv.gainBusy();}));
        // Armed: F5 runs; Esc disarms and the run ends incomplete.
        tv.setTarget(itemKey(32,drive,pid::kVelocitySp));tv.setSignalField("duration","3");
        connection.arm();QVERIFY(waitFor([&]{return connection.armState()==1 && shown("tuneRun")->isEnabled();}));
        QTest::keyClick(window,Qt::Key_F5);
        QVERIFY(waitFor([&]{return tv.phase()==TuneView::Phase::Running;}));
        QTest::keyClick(window,Qt::Key_Escape);
        QVERIFY(waitFor([&]{return connection.armState()==0;}));
        QVERIFY(waitFor([&]{return !connection.tuneRunning();},10000));
        QCOMPARE(tv.runCount(),1);QCOMPARE(int(tv.runList().back().end),int(RunEnd::Disarmed));
        QVERIFY(waitFor([&]{return shown("tunePlotItem_0") && shown("tunePlotItem_0")->property("vertices").toInt()>2;}));
        QVERIFY(shown("tuneMetricRow_1"));
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
    }

    // A Tune run on a device. Env: DRIVELINK_GUI_BENCH_ENDPOINT, and either
    // DRIVELINK_GUI_BENCH_STAGE_MOCK=1 (bare board: adds an actuator.mock in
    // loop 0, saves and reboots; afterwards removes it, saves and reboots
    // again) or DRIVELINK_GUI_BENCH_TUNE (the controller, type:instance) with
    // DRIVELINK_GUI_BENCH_TARGET (its test-signal input) and optional
    // DRIVELINK_GUI_BENCH_RESPONSE (a signal). DRIVELINK_GUI_BENCH_STEP (the
    // step, default 0.2), DRIVELINK_GUI_BENCH_DURATION (s, default 1) and
    // DRIVELINK_GUI_BENCH_MIN_RESPONSE (the least |response| at the end of the
    // step). A real motor (deferred row) needs the first-run procedure:
    // wheels off the ground, a current-limited supply, a hand on the power.
    // Arms, runs a step (completed, every post-trigger sample, the target back
    // at 0 after it), then a 3 s step stopped after 0.5 s (stopped, back at 0),
    // and disarms. No param is written; nothing else is saved.
    // H2e-2: with DRIVELINK_GUI_BENCH_GAIN (a tune gain of the controller, an
    // item key) it also checks Auto re-run: the gain +10 % re-runs by itself
    // (completed, with the new gain); during a 3 s run the gain back to its
    // value cancels the run (not kept, the target at 0) and re-runs, which is
    // stopped. The gain ends at its original value (live, not saved).
    void benchTune() {
        const auto endpoint=qEnvironmentVariable("DRIVELINK_GUI_BENCH_ENDPOINT");
        const bool stage=qEnvironmentVariable("DRIVELINK_GUI_BENCH_STAGE_MOCK")=="1";
        auto controllerKey=qEnvironmentVariable("DRIVELINK_GUI_BENCH_TUNE");
        auto targetKey=qEnvironmentVariable("DRIVELINK_GUI_BENCH_TARGET");
        auto responseKey=qEnvironmentVariable("DRIVELINK_GUI_BENCH_RESPONSE");
        const auto gainKey=qEnvironmentVariable("DRIVELINK_GUI_BENCH_GAIN");
        if(endpoint.isEmpty() || (!stage && (controllerKey.isEmpty() || targetKey.isEmpty()))) {
            QSKIP("set DRIVELINK_GUI_BENCH_ENDPOINT and DRIVELINK_GUI_BENCH_STAGE_MOCK=1, or _TUNE and _TARGET, to run on a device");
        }
        bool ok=false;
        const double step=qEnvironmentVariable("DRIVELINK_GUI_BENCH_STEP","0.2").toDouble(&ok);QVERIFY2(ok && step!=0,"DRIVELINK_GUI_BENCH_STEP");
        const double duration=qEnvironmentVariable("DRIVELINK_GUI_BENCH_DURATION","1").toDouble(&ok);QVERIFY2(ok && duration>0,"DRIVELINK_GUI_BENCH_DURATION");
        const double minResponse=qEnvironmentVariable("DRIVELINK_GUI_BENCH_MIN_RESPONSE","0").toDouble();
        // Reboots and waits for the device to answer again.
        const auto reboot=[&] {
            try {Session s(endpoint.toStdString());Client cl(s);cl.info();cl.system(uint8_t(protocol::defs::SystemOp::Reboot));} catch(const Error &) {}
            QTest::qWait(1500);
            for(int i=0;i<40;++i) {
                try {Session s(endpoint.toStdString());Client cl(s);cl.info();return true;} catch(const Error &) {QTest::qWait(250);}
            }
            return false;
        };
        std::optional<uint8_t> staged;
        if(stage) {
            Session s(endpoint.toStdString());Client cl(s);const auto info=cl.info();
            QVERIFY2(!(info.flags&1),"refusing to stage: pending layout changes");
            staged=cl.add(112,"tune_bench",0).instance;
            cl.system(uint8_t(protocol::defs::SystemOp::Save));
        }
        if(staged) {
            QVERIFY2(reboot(),"the device did not come back after staging");
            controllerKey=nodeKey(112,*staged);targetKey=itemKey(112,*staged,0x200);responseKey=itemKey(112,*staged,0x100);
        }
        QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));
        struct Restore {std::function<void()> run;~Restore() {run();}} restore{[&] {
            if(c.linked()) {
                c.disarm();waitFor([&]{return c.armState()==0;},5000);
                c.disconnectDevice();waitFor([&]{return !c.closing();},10000);
            }
            if(staged) {
                try {
                    Session s(endpoint.toStdString());Client cl(s);cl.info();
                    cl.remove(112,*staged);cl.system(uint8_t(protocol::defs::SystemOp::Save));
                } catch(const Error &e) {qWarning("bench restore: %s",e.what());}
                if(!reboot()) {qWarning("bench restore: the device did not come back");}
                qInfo("bench: staged mock removed, saved and rebooted");
            }
        }};
        c.connectTo(endpoint);
        QVERIFY2(waitFor([&]{return c.armEnabled();},30000),qPrintable(c.armBlockedReason()+" / "+c.lastError()["message"].toString()));
        QVERIFY2(!c.unsaved(),"refusing to run with unsaved changes");
        auto &tv=c.tuneView();auto &view=c.monitorView();
        tv.selectController(controllerKey);QCOMPARE(tv.controller(),controllerKey);
        // Auto re-run runs only while Tune is shown; reads go through Monitor's
        // group, which keeps running on the Tune page.
        std::optional<double> originalGain;
        if(!gainKey.isEmpty()) {
            const auto *gains=tv.gains();
            for(int i=0;i<gains->rowCount();++i) {
                if(gains->data(gains->index(i,0),GainRows::Key).toString()==gainKey) {originalGain=gains->data(gains->index(i,0),GainRows::Value).toDouble();}
            }
            QVERIFY2(originalGain && *originalGain!=0,"DRIVELINK_GUI_BENCH_GAIN: not a non-zero tune gain of the controller");
        }
        tv.setTarget(targetKey);
        QCOMPARE(tv.setSignalField("amplitude",QString::number(step)),QString());
        QCOMPARE(tv.setSignalField("duration",QString::number(duration)),QString());
        QVERIFY2(tv.testSignal()["problem"].toString().isEmpty(),qPrintable(tv.testSignal()["problem"].toString()));
        c.setPage("monitor");view.toggleSignal(targetKey);if(!responseKey.isEmpty()) {view.toggleSignal(responseKey);}
        auto latest=[&](const QString &key)->std::optional<double> {
            const auto *ring=view.ring(key);
            if(!ring || ring->size()==0) {return std::nullopt;}
            return ring->value(ring->size()-1);
        };
        QVERIFY(waitFor([&]{return latest(targetKey).has_value();},10000));
        qInfo("bench: device %s firmware %s, %s: step %g on %s for %g s",qPrintable(c.deviceId()),qPrintable(c.info()["firmware"].toString()),
              qPrintable(controllerKey),step,qPrintable(targetKey),duration);
        c.arm();QVERIFY2(waitFor([&]{return c.armState()==1 && tv.run()["canRun"].toBool();},5000),qPrintable(tv.run()["blocked"].toString()));
        std::optional<double> during;
        tv.startRun();
        QVERIFY(waitFor([&]{return latest(targetKey) && std::abs(*latest(targetKey)-step)<1e-6;},5000));
        QTest::qWait(int(duration*900));
        if(!responseKey.isEmpty()) {during=latest(responseKey);}
        QVERIFY2(waitFor([&]{return !c.tuneRunning();},int(duration*1000)+15000),qPrintable(tv.run()["text"].toString()));
        QVERIFY(tv.runCount()==1);
        const auto &run=tv.runList().back();
        QCOMPARE(int(run.end),int(RunEnd::Completed));
        QCOMPARE(run.series[0].size()-run.series[0].lowerBound(0),size_t(run.capture.samples-run.capture.pre));
        qInfo("bench: run 1: %zu samples, irregular %d, response at the end %g, rise %s, overshoot %s",
              run.series[0].size(),int(run.irregular),during.value_or(NAN),
              qPrintable(metricText(run.metrics.rise_s,"ms",1000,0)),qPrintable(metricText(run.metrics.overshoot_pct,"%")));
        if(!c.info()["firmware"].toString().endsWith("-vdev")) {QVERIFY2(!run.irregular,"a board's capture should be regular");}
        if(!responseKey.isEmpty()) {QVERIFY2(during && std::abs(*during)>=minResponse,"no response to the step");}
        QVERIFY2(waitFor([&]{return latest(targetKey) && *latest(targetKey)==0;},2000),"the target did not return to 0");
        // Stop: a 3 s step stopped after 0.5 s; the capture keeps recording.
        QCOMPARE(tv.setSignalField("duration","3"),QString());
        tv.startRun();QVERIFY(waitFor([&]{return tv.phase()==TuneView::Phase::Running;},5000));
        QTest::qWait(500);tv.stopRun();
        QVERIFY(waitFor([&]{return latest(targetKey) && *latest(targetKey)==0;},2000));
        QVERIFY2(waitFor([&]{return !c.tuneRunning();},20000),qPrintable(tv.run()["text"].toString()));
        QCOMPARE(int(tv.runList().back().end),int(RunEnd::Stopped));
        qInfo("bench: run 2 stopped at %.3f s",double(tv.runList().back().endedUs.value_or(-1))/1e6);
        if(originalGain) {
            c.setPage("tune");
            const auto gainItem=parseItemKey(gainKey)->item;
            QCOMPARE(tv.setSignalField("duration",QString::number(duration)),QString());
            // +10 %: a run follows by itself with the new gain.
            const double nudged=stepGain(*originalGain,1.1,-1e30,1e30);
            QCOMPARE(tv.gainText(gainKey,QString::number(nudged,'g',9)),QString());
            QVERIFY2(waitFor([&]{return tv.runCount()==3 && !c.tuneRunning();},int(duration*1000)+15000),qPrintable(tv.autoRerun()["text"].toString()));
            QCOMPARE(int(tv.runList().back().end),int(RunEnd::Completed));
            QCOMPARE(float(tv.runList().back().gains.at(gainItem)),float(nudged));
            QVERIFY2(waitFor([&]{return latest(targetKey) && *latest(targetKey)==0;},2000),"the target did not return to 0 after the auto re-run");
            qInfo("bench: auto re-run 3 with %s %g completed",qPrintable(gainKey),nudged);
            // Back during a 3 s run: cancelled (not kept), then re-run; Stop it.
            QCOMPARE(tv.setSignalField("duration","3"),QString());
            tv.startRun();QVERIFY(waitFor([&]{return tv.phase()==TuneView::Phase::Running;},5000));
            QTest::qWait(500);
            QCOMPARE(tv.gainText(gainKey,QString::number(*originalGain,'g',9)),QString());
            QVERIFY2(waitFor([&]{return !c.tuneRunning();},5000),"the run was not cancelled");
            QVERIFY2(waitFor([&]{return latest(targetKey) && *latest(targetKey)==0;},2000),"the target did not return to 0 after the cancel");
            QVERIFY2(waitFor([&]{return tv.phase()==TuneView::Phase::Running;},8000),qPrintable(tv.autoRerun()["text"].toString()));
            QTest::qWait(300);tv.stopRun();
            QVERIFY2(waitFor([&]{return !c.tuneRunning();},20000),qPrintable(tv.run()["text"].toString()));
            QCOMPARE(tv.runCount(),4); // the cancelled run was not kept
            QCOMPARE(float(tv.runList().back().gains.at(gainItem)),float(*originalGain));
            QVERIFY(waitFor([&]{return latest(targetKey) && *latest(targetKey)==0;},2000));
            qInfo("bench: run cancelled by a gain change, re-run 4 stopped; %s back at %g",qPrintable(gainKey),*originalGain);
        }
        c.disarm();QVERIFY(waitFor([&]{return c.armState()==0;},5000));
        qInfo("bench: completed, stopped, back at 0 and disarmed");
    }
};

QTEST_MAIN(TuneTests)
#include "test_tune.moc"
