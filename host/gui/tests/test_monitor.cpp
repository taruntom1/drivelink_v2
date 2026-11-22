// Monitor (H2d-1): the pure rules, the view without a device, its groups on
// vdev, the page in the shell and the plot item's cost at 1 kHz. Manual
// command (H2d-2): the hold rule, the command model, its groups on vdev, the
// card in the shell and the bench check.
#include "command_model.hpp"
#include "connection.hpp"
#include "layout_model.hpp"
#include "monitor_model.hpp"
#include "monitor_view.hpp"
#include "plot_item.hpp"
#include "safety_keys.hpp"
#include "style.hpp"
#include "test_support.hpp"
#include <drivelink/protocol/gen/registry.hpp>
#include <drivelink/protocol/gen/type_loop.hpp>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <cmath>
#include <limits>

using namespace drivelink;
using namespace drivelink::gui;
using namespace drivelink::gui::test;
namespace {
ModelItem item(uint16_t id,const char *name,ItemKind kind,ValueType type,const char *unit="") {
    ModelItem i;i.descriptor.id=id;i.descriptor.name=name;i.descriptor.kind=kind;i.descriptor.value_type=type;
    i.descriptor.unit=unit;return i;
}
// Two loops (control 1 kHz real-time, sensor 100 Hz); an actuator with
// signals and an input, a failed instance and an IMU.
DeviceSnapshot sample() {
    DeviceSnapshot s;
    auto &loop=s.types[5];loop.descriptor={5,0,"loop"};
    loop.items[1]=item(1,"rate_hz",ItemKind::Param,ValueType::U16);
    for(const auto &[id,name]:std::initializer_list<std::pair<uint16_t,const char *>>{
            {protocol::defs::loop::kTicks,"ticks"},{protocol::defs::loop::kOverruns,"overruns"},
            {protocol::defs::loop::kLastRunUs,"last_run_us"},{protocol::defs::loop::kMaxRunUs,"max_run_us"}}) {
        loop.items[id]=item(id,name,ItemKind::Signal,ValueType::U32,id>=protocol::defs::loop::kLastRunUs?"us":"");
    }
    auto &act=s.types[112];act.descriptor={112,1,"actuator.mock"};
    act.items[0x100]=item(0x100,"velocity",ItemKind::Signal,ValueType::F32,"rad/s");
    act.items[0x101]=item(0x101,"position",ItemKind::Signal,ValueType::F32,"rad");
    act.items[0x102]=item(0x102,"count",ItemKind::Signal,ValueType::I32);
    act.items[0x200]=item(0x200,"duty",ItemKind::Input,ValueType::F32);
    act.items[0x300]=item(0x300,"drive",ItemKind::Action,ValueType::None);
    auto &imu=s.types[114];imu.descriptor={114,2,"imu.mock"};
    imu.items[0x100]=item(0x100,"yaw",ItemKind::Signal,ValueType::F32,"rad");
    imu.items[0x110]=item(0x110,"gyro.x",ItemKind::Signal,ValueType::F32,"rad/s");
    s.info.loops={{0,"control",1000,1},{1,"sensor",100,0}};
    s.system_params[protocol::defs::system::kLimitsGroupEntries]=uint16_t(64);
    auto add=[&](uint8_t t,uint8_t i,uint8_t l,const char *label,uint8_t state=0) {
        s.configured[{t,i}]={{t,i,uint8_t(t==5?255:l),state,label},{}};
    };
    add(5,0,0,"control");add(5,1,0,"sensor");add(112,0,0,"left_pwm");add(112,1,0,"broken",1);add(114,0,1,"imu");
    s.pending=s.configured;
    s.pending[{114,1}]={{114,1,1,0,"later"},{}};
    return s;
}
std::shared_ptr<const DeviceSnapshot> shared(DeviceSnapshot s) {return std::make_shared<const DeviceSnapshot>(std::move(s));}
QStringList keys(const QVariantList &rows) {
    QStringList list;for(const auto &r:rows) {list<<r.toMap()["key"].toString();}return list;
}
}

class MonitorTests : public QObject {
    Q_OBJECT
private slots:
    // Catalogue, values, rates, the link budget (against the architecture
    // table), placement, plans, refusals and their lower rate.
    void monitorLogic() {
        const auto s=sample();
        const auto catalogue=monitorCatalogue(s);
        QCOMPARE(catalogue.size(),size_t(4));
        QCOMPARE(catalogue[0].key,QString("5:0"));QVERIFY(catalogue[0].isLoop);QCOMPARE(catalogue[0].items.size(),size_t(4));
        QCOMPARE(catalogue[1].key,QString("112:0"));QCOMPARE(catalogue[1].title,QString("left_pwm"));
        QCOMPARE(catalogue[1].items.size(),size_t(4)); // three signals, then the input
        QVERIFY(!catalogue[1].items[2].input);QVERIFY(catalogue[1].items[3].input);QCOMPARE(catalogue[1].items[3].name,QString("duty"));
        QCOMPARE(catalogue[2].key,QString("5:1"));QCOMPARE(catalogue[3].key,QString("114:0"));QCOMPARE(catalogue[3].loop,uint8_t(1));
        QVERIFY(catalogue[0].subtitle.contains("1 kHz"));
        // Failed, pending-only and safe mode have nothing to plot.
        QVERIFY(!monitorSignal(s,{112,1,0x100}));QVERIFY(!monitorSignal(s,{114,1,0x100}));QVERIFY(!monitorSignal(s,{112,0,0x300}));
        auto safe=s;safe.info.flags|=8;QVERIFY(monitorCatalogue(safe).empty());
        QCOMPARE(monitorSignal(s,{5,1,protocol::defs::loop::kTicks})->owner,QString("sensor"));
        // Values.
        QCOMPARE(sampleValue(ValueType::Bool,7),1.0);QCOMPARE(sampleValue(ValueType::U8,0x1FF),255.0);
        QCOMPARE(sampleValue(ValueType::I32,std::bit_cast<uint32_t>(int32_t(-5))),-5.0);
        QCOMPARE(sampleValue(ValueType::F32,std::bit_cast<uint32_t>(1.5f)),1.5);
        QCOMPARE(plotValueText(ValueType::F32,1.1629939),QString("1.163"));QCOMPARE(plotValueText(ValueType::I32,-3),QString("-3"));
        // Rates.
        QCOMPARE(rateChoices(1000).size(),size_t(10));QCOMPARE(rateChoices(1000).back().divider,uint16_t(1000));
        QCOMPARE(rateChoices(100).size(),size_t(7));QCOMPARE(rateChoices(0).size(),size_t(1));
        QCOMPARE(defaultDivider(1000),uint16_t(10));QCOMPARE(defaultDivider(200),uint16_t(2));
        QCOMPARE(defaultDivider(50),uint16_t(1));QCOMPARE(defaultDivider(333),uint16_t(5));
        // A group carries at most 1 kHz: faster loops start at minDivider.
        QCOMPARE(minDivider(1000),uint16_t(1));QCOMPARE(minDivider(2000),uint16_t(2));QCOMPARE(minDivider(2500),uint16_t(3));
        QCOMPARE(rateChoices(2000).front().divider,uint16_t(2));QCOMPARE(rateChoices(2000)[1].divider,uint16_t(5));
        QCOMPARE(rateChoices(3000).front().divider,uint16_t(3));QCOMPARE(rateChoices(3000)[1].divider,uint16_t(5));
        QCOMPARE(rateLabel(1000,5),QString("200 Hz (divider 5)"));QCOMPARE(hzText(2000),QString("2 kHz"));
        QCOMPARE(hzText(166.5),QString("166.5 Hz"));
        // The architecture's link table: 2 motors × 3 signals plus an IMU
        // group of 10 at 100 Hz plus the status stream at 10 Hz.
        QCOMPARE(subscriptionFrameBytes(6),size_t(45));
        const std::vector<LinkStream> at1k{{0,1,6},{1,1,10}},at100{{0,10,6},{1,1,10}},four{{0,1,12},{1,1,10}};
        QCOMPARE(percentText(linkLoad(s,at1k)),QString("56 %"));
        QCOMPARE(percentText(linkLoad(s,at100)),QString("12 %"));
        QCOMPARE(percentText(linkLoad(s,four)),QString("82 %"));
        QCOMPARE(statusBytesPerSecond(s),double((9+19+5*2)*10));
        // A rate changed live (rate_hz param) wins over GET_INFO's.
        auto live=s;live.configured[{5,0}].params[protocol::defs::loop::kRateHz]=uint16_t(2000);
        QCOMPARE(loopRateHz(live,0),2000u);QCOMPARE(loopRateHz(s,0),1000u);QVERIFY(monitorCatalogue(live)[0].subtitle.contains("2 kHz"));
        // At 2 kHz a divider-1 group (another client's) still carries 1 kHz;
        // a plotted loop's chosen divider 1 becomes 2.
        QCOMPARE(streamBytesPerSecond(live,{0,1,4}),37.0*1000);
        {PlotSet sped;sped.dividers[0]=1;QCOMPARE(dividerFor(sped,live,0),uint16_t(2));QCOMPARE(dividerFor(sped,s,0),uint16_t(1));}
        // Placement: by unit, else the first empty plot, else a new one.
        PlotSet set;
        placeSignal(set,s,{112,0,0x100});placeSignal(set,s,{114,0,0x100});
        QCOMPARE(set.plots.size(),size_t(2));
        set.plots.push_back({});
        placeSignal(set,s,{114,0,0x110});QCOMPARE(set.plots[0].series.size(),size_t(2)); // rad/s joins rad/s
        placeSignal(set,s,{112,0,0x102});QCOMPARE(set.plots[2].series.size(),size_t(1)); // no unit: the empty plot
        placeSignal(set,s,{112,0,0x100});QCOMPARE(set.count(),size_t(4));                // already plotted
        // Plans: one group per loop, refs in plot order, the default rate.
        auto plans=monitorPlans(set,s);
        QCOMPARE(plans.size(),size_t(2));
        QCOMPARE(plans[0].loop,uint8_t(0));QCOMPARE(plans[0].divider,uint16_t(10));
        QCOMPARE(plans[0].refs,(std::vector<Ref>{{112,0,0x100},{112,0,0x102}}));
        QCOMPARE(plans[1].refs,(std::vector<Ref>{{114,0,0x110},{114,0,0x100}})); // gyro.x is on the first plot
        removeSignal(set,{112,0,0x102});QCOMPARE(set.plots.size(),size_t(3));QVERIFY(set.plots[2].series.empty());
        auto pruned=set;auto booted=s;booted.configured.erase({114,0});pruneSignals(pruned,booted);QCOMPARE(pruned.count(),size_t(1));
        // Refusals: no free slot for a new loop; the entry limit; the cap.
        PlotSet one;placeSignal(one,s,{112,0,0x100});
        LinkContext none;none.freeSlots=0;
        QVERIFY(checkAdd(s,one,{112,0,0x101},none).ok);
        auto check=checkAdd(s,one,{114,0,0x100},none);QVERIFY(!check.ok);QVERIFY(check.reason.contains("No free subscription slot"));
        LinkContext tight;tight.entryLimit=1;
        check=checkAdd(s,one,{112,0,0x101},tight);QVERIFY(!check.ok);QVERIFY(check.reason.contains("group limit"));
        QVERIFY(!checkAdd(s,one,{112,1,0x100},{}).ok);
        // Over 100 %: another client's 10 values at 1 kHz plus two plotted
        // at 1 kHz is 98 %; a third is refused and 500 Hz fits (85 %).
        LinkContext busy;busy.others={{0,1,10}};busy.freeSlots=2;
        PlotSet fast;fast.dividers[0]=1;placeSignal(fast,s,{112,0,0x100});placeSignal(fast,s,{112,0,0x101});
        QVERIFY(checkAdd(s,fast,{112,0,0x100},busy).ok);
        check=checkAdd(s,fast,{112,0,0x102},busy);
        QVERIFY(!check.ok);QVERIFY(check.reason.contains("102 %"));
        QCOMPARE(check.divider,std::optional<uint16_t>(2));QCOMPARE(check.suggestion,QString("Plot control at 500 Hz (85 %)"));
        // A faster rate is checked the same way.
        PlotSet slow;placeSignal(slow,s,{112,0,0x100});placeSignal(slow,s,{112,0,0x101});placeSignal(slow,s,{112,0,0x102});
        QVERIFY(checkRate(s,slow,0,2,busy).ok);
        check=checkRate(s,slow,0,1,busy);QVERIFY(!check.ok);QCOMPARE(check.divider,std::optional<uint16_t>(2));
        // The cap.
        PlotSet full;
        for(uint16_t i=0;i<kMaxPlotted;++i) {full.plots[0].series.push_back({112,0,uint16_t(0x100+i)});}
        check=checkAdd(s,full,{114,0,0x100},{});QVERIFY(!check.ok);QVERIFY(check.reason.contains("16"));
        // At most 8 plots: a signal that would need a ninth is refused.
        PlotSet eight;eight.plots.clear();
        for(size_t i=0;i<kMaxPlots;++i) {eight.plots.push_back({{{112,0,0x102}}});}
        check=checkAdd(s,eight,{114,0,0x110},{});QVERIFY(!check.ok);QVERIFY(check.reason.contains("All 8 plots"));
        // Over 100 % already (a live loop-rate change, another client): a
        // slower rate that still leaves it over is accepted, a faster one not.
        LinkContext over;over.others={{0,1,14}};over.freeSlots=2;
        PlotSet heavy;heavy.dividers[0]=1;
        placeSignal(heavy,s,{112,0,0x100});placeSignal(heavy,s,{112,0,0x101});placeSignal(heavy,s,{112,0,0x102});
        check=checkRate(s,heavy,0,2,over);QVERIFY(check.ok);QVERIFY(check.load>1.0);
        heavy.dividers[0]=2;QVERIFY(!checkRate(s,heavy,0,1,over).ok);
        // The device's group limit.
        auto limited=s;limited.system_params[protocol::defs::system::kLimitsGroupEntries]=uint16_t(8);
        QCOMPARE(groupEntryLimit(limited),size_t(8));QCOMPARE(groupEntryLimit(s),size_t(64));
    }
    // Rings, decimation, axes, cursor values, loss counting and the CSV.
    void plotLogic() {
        SampleRing ring(4);
        for(int i=0;i<6;++i) {ring.push(i*10,i);}
        QCOMPARE(ring.size(),size_t(4));QCOMPARE(ring.time(0),int64_t(20));QCOMPARE(ring.value(3),5.0);
        QCOMPARE(ring.lowerBound(25),size_t(1));QCOMPARE(ring.lowerBound(0),size_t(0));QCOMPARE(ring.lowerBound(99),size_t(4));
        ring.setCapacity(2);QCOMPARE(ring.size(),size_t(2));QCOMPARE(ring.time(0),int64_t(40));
        ring.setCapacity(8);ring.push(60,6);QCOMPARE(ring.size(),size_t(3));QCOMPARE(ring.value(2),6.0);
        QCOMPARE(ringCapacity(1000),size_t(62016));
        // Decimation: raw points when few; at most two per column when many,
        // keeping each column's extremes, plus the sample before the window.
        SampleRing wave(20000);
        for(int i=0;i<10000;++i) {wave.push(int64_t(i)*1000,i==5000?10:std::sin(i*0.01));}
        auto points=decimate(wave,1000000,9000000,100);
        QVERIFY(points.size()<=2*101+2);QVERIFY(points.size()>=100);
        QVERIFY(points.front().x()<0);
        QVERIFY(std::any_of(points.begin(),points.end(),[](const QPointF &p){return p.y()>9;}));
        for(size_t i=1;i<points.size();++i) {QVERIFY(points[i].x()>=points[i-1].x());}
        SampleRing few(8);few.push(0,1);few.push(500,2);few.push(1000,3);
        points=decimate(few,0,1000,100);QCOMPARE(points.size(),size_t(3));QCOMPARE(points[1],QPointF(50,2));
        QVERIFY(decimate(few,0,1000,0).empty());QVERIFY(decimate(SampleRing(4),0,10,10).empty());
        const auto range=valueRange(few,600,1000);QVERIFY(range);QCOMPARE(range->first,2.0);QCOMPARE(range->second,3.0);
        // Axes: round ticks; a flat or empty range is padded.
        auto axis=niceAxis(0.3,9.3);QCOMPARE(axis.min,0.0);QCOMPARE(axis.max,10.0);
        QCOMPARE(axis.ticks,(std::vector<double>{0,5,10}));
        axis=niceAxis(-0.013,0.042);QCOMPARE(axis.ticks.size(),size_t(5));QCOMPARE(axis.ticks[1],0.0);
        axis=niceAxis(5,5);QVERIFY(axis.min<5 && axis.max>5);
        axis=niceAxis(0,0);QCOMPARE(axis.min,-1.0);QCOMPARE(axis.max,1.0);
        QCOMPARE(tickText(0.25,0.05),QString("0.25"));QCOMPARE(tickText(20,5),QString("20"));
        // Cursor values: the last sample at or before the time.
        QVERIFY(!valueAt(few,-1));QCOMPARE(*valueAt(few,500),2.0);QCOMPARE(*valueAt(few,700),2.0);QCOMPARE(*valueAt(few,5000),3.0);
        QCOMPARE(durationText(999),QString::fromUtf8("999 µs"));QCOMPARE(durationText(12500),QString("12.5 ms"));
        QCOMPARE(durationText(2500),QString("2.50 ms"));QCOMPARE(durationText(1250000),QString("1.250 s"));
        QCOMPARE(counterGap(10,11),0u);QCOMPARE(counterGap(10,13),2u);QCOMPARE(counterGap(65535,0),0u);QCOMPARE(counterGap(65534,1),2u);
        // CSV: a row per distinct time, empty where a series has no sample.
        SampleRing a(8),b(8);a.push(100,1);a.push(200,2);b.push(150,7.5);b.push(200,8);b.push(900,9);
        const std::array<CsvColumn,2> columns{{{"left.velocity [rad/s]",&a},{"odd, \"name\"",&b}}};
        const auto csv=snapshotCsv(columns,100,800);
        QCOMPARE(csv,QString("device_time_us,time_s,left.velocity [rad/s],\"odd, \"\"name\"\"\"\n"
                             "100,0.000000,1,\n150,0.000050,,7.5\n200,0.000100,2,8\n"));
    }
    // The view without a device: add, refuse, the lower rate, colours,
    // samples, pause, cursors, snapshot, Boot.
    void monitorView() {
        MonitorView view;QSignalSpy plans(&view,&MonitorView::plansChanged);
        view.setSnapshot(shared(sample()));plans.clear();
        QCOMPARE(view.browser().size(),4);
        view.toggleSignal("112:0:256");view.toggleSignal("114:0:256");
        QCOMPARE(view.plotted(),2);QCOMPARE(plans.count(),2);QCOMPARE(view.plans().size(),size_t(2));
        QCOMPARE(view.plots().size(),2);
        const auto first=view.plots()[0].toMap()["series"].toList()[0].toMap();
        QCOMPARE(first["color"].toInt(),0);QCOMPARE(first["owner"].toString(),QString("left_pwm"));
        QCOMPARE(view.plots()[1].toMap()["series"].toList()[0].toMap()["color"].toInt(),1);
        // Colours stay with a signal; a freed colour is reused.
        view.toggleSignal("112:0:256");QCOMPARE(view.plotted(),1);
        QCOMPARE(view.plots()[1].toMap()["series"].toList()[0].toMap()["color"].toInt(),1);
        view.toggleSignal("112:0:257");QCOMPARE(view.plots()[1].toMap()["series"].toList().size(),2);
        QCOMPARE(view.plots()[1].toMap()["series"].toList()[1].toMap()["color"].toInt(),0);
        // A refusal changes nothing; its lower rate applies in one click.
        LinkContext busy;busy.others={{0,1,9}};busy.freeSlots=4;
        view.setLink(busy,4,8);
        QCOMPARE(view.link()["groups"].toInt(),4);QCOMPARE(view.link()["total"].toInt(),8);
        view.setRate(0,1);QVERIFY(view.refusal().isEmpty());
        view.toggleSignal("112:0:256");QVERIFY(view.refusal().isEmpty());
        const auto before=plans.count();
        view.toggleSignal("112:0:258");
        QVERIFY(!view.refusal().isEmpty());QCOMPARE(plans.count(),before);QCOMPARE(view.plotted(),3);
        QCOMPARE(view.refusal()["divider"].toInt(),2);
        view.applySuggestion();QVERIFY(view.refusal().isEmpty());QCOMPARE(view.plotted(),4);
        QCOMPARE(view.plans()[0].divider,uint16_t(2));
        view.toggleSignal("112:1:256");QVERIFY(view.refusal()["reason"].toString().contains("not running"));
        view.dismissRefusal();QVERIFY(view.refusal().isEmpty());
        // Samples: into the rings and the browser values; nothing while paused.
        const std::vector<Ref> refs{{112,0,0x100},{112,0,0x102}};
        auto sinks=view.sinks(refs);QVERIFY(sinks[0].ring && sinks[1].ring);
        QSignalSpy data(&view,&MonitorView::dataChanged);
        for(int i=0;i<100;++i) {
            view.push(sinks[0],1000000+i*2000,std::bit_cast<uint32_t>(float(i)));
            view.push(sinks[1],1000000+i*2000,std::bit_cast<uint32_t>(int32_t(-i)));
        }
        view.setLatest(sinks[0],std::bit_cast<uint32_t>(1.25f));view.endAppend();
        QCOMPARE(data.count(),1);QCOMPARE(view.nowUs(),1198000.0);QCOMPARE(view.ring("112:0:256")->size(),size_t(100));
        QCOMPARE(view.values()["112:0:256"].toString(),QString("1.25 rad/s"));
        // A signal not plotted (its group's pass still in flight) keeps no value.
        const std::vector<Ref> gone{{114,0,0x110}};
        view.setLatest(view.sinks(gone)[0],1);view.endAppend();QVERIFY(!view.values().contains("114:0:272"));
        // Lost frames are published once per poll.
        QSignalSpy changes(&view,&MonitorView::changed);
        view.addLost(2);view.addLost(3);QCOMPARE(changes.count(),0);
        view.endAppend();QCOMPARE(changes.count(),1);QCOMPARE(view.link()["lost"].toInt(),5);
        QVERIFY(!view.cursors());view.setCursors(true);QVERIFY(!view.cursors()); // only while paused
        view.setPaused(true);view.push(sinks[0],1200000,0);QCOMPARE(view.ring("112:0:256")->size(),size_t(100));
        view.setCursors(true);QVERIFY(view.cursors());
        view.setCursor(0,1100000);view.setCursor(1,1150000);
        auto readout=view.readout();QCOMPARE(readout["dt"].toString(),QString("50.0 ms"));
        const auto row=readout["rows"].toList()[0].toMap();
        QCOMPARE(row["key"].toString(),QString("112:0:256"));QCOMPARE(row["a"].toString(),QString("50"));
        QCOMPARE(row["b"].toString(),QString("75"));QCOMPARE(row["delta"].toString(),QString("25"));
        view.setCursor(0,-2e7);QCOMPARE(view.cursorA(),view.nowUs()-10e6); // clamped to the window
        // Snapshot CSV into the chosen directory.
        QTemporaryDir dir;view.setSnapshotDirectory(dir.filePath("snapshots"));
        QSignalSpy notes(&view,&MonitorView::notification);
        const auto path=view.snapshot("02444c000001");QVERIFY(!path.isEmpty());QVERIFY(path.contains("02444c000001-"));
        QCOMPARE(notes.count(),1);QCOMPARE(notes[0][1].toInt(),1);
        QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));
        const auto lines=QString::fromUtf8(file.readAll()).split('\n',Qt::SkipEmptyParts);
        QCOMPARE(lines.size(),101);QVERIFY(lines[0].contains("left_pwm.velocity [rad/s]"));
        // Resume starts from now: the paused stretch has no data.
        view.setPaused(false);QVERIFY(!view.cursors());QCOMPARE(view.ring("112:0:256")->size(),size_t(0));
        // Plots: add, remove (at least one stays).
        const auto plotsBefore=view.plots().size();
        view.addPlot();QCOMPARE(view.plots().size(),plotsBefore+1);
        for(int i=0;i<10;++i) {view.removePlot(0);}
        QCOMPARE(view.plots().size(),1);QCOMPARE(view.plotted(),0);
        // Boot: data cleared, signals that no longer run dropped.
        view.toggleSignal("114:0:256");view.toggleSignal("112:0:256");
        auto next=sample();next.generation=7;next.configured.erase({114,0});
        sinks=view.sinks(refs);view.push(sinks[0],5,0);view.endAppend();
        view.setSnapshot(shared(next));
        QCOMPARE(view.plotted(),1);QCOMPARE(view.nowUs(),0.0);QCOMPARE(view.ring("112:0:256")->size(),size_t(0));
        // plotNode: a node's signals (not its inputs).
        view.reset();view.setSnapshot(shared(sample()));
        QCOMPARE(view.plotNode("112:0"),3);QCOMPARE(view.plotted(),3);QCOMPARE(view.plotNode("112:0"),0);
        QCOMPARE(keys(view.plots()[0].toMap()["series"].toList()),QStringList{"112:0:256"});
        // A divider kept across a live loop-rate change stays the combo's choice.
        view.setRate(0,500);
        auto slower=sample();slower.configured[{5,0}].params[protocol::defs::loop::kRateHz]=uint16_t(50);
        view.setSnapshot(shared(slower));
        QVariantMap rate;
        for(const auto &r:view.rates()) {if(r.toMap()["loop"].toInt()==0) {rate=r.toMap();}}
        QCOMPARE(rate["divider"].toInt(),500);
        const auto choices=rate["choices"].toList();
        QCOMPARE(rate["index"].toInt(),choices.size()-1);QCOMPARE(choices.back().toMap()["divider"].toInt(),500);
        // A lost link keeps the plot set and rates for the same device; another device starts empty.
        const auto kept=view.plotted();QVERIFY(kept>0);
        view.lose();QCOMPARE(view.plotted(),kept);QCOMPARE(view.nowUs(),0.0);
        auto rebooted=slower;rebooted.generation=9;view.setSnapshot(shared(rebooted));
        QCOMPARE(view.plotted(),kept);QCOMPARE(view.plans()[0].divider,uint16_t(500));
        auto another=rebooted;another.info.device_id[0]^=1;view.setSnapshot(shared(another));
        QCOMPARE(view.plotted(),0);
    }
    // Groups on vdev: free slots only, add/remove/rate reconfigure, samples
    // into the rings, a refused add without a free slot, the Layout live group
    // released off-page, and close clears exactly what it owns.
    void monitorOperations() {
        Fixture fixture;
        {
            Session s(fixture.endpoint.toStdString());Client cl(s);cl.info();
            cl.add(112,"first",0);cl.add(114,"imu",1);cl.system(5);cl.system(8);
        }
        QTest::qWait(150);
        LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);
        QVERIFY2(waitFor([&]{return c.armEnabled() && c.snapshot()->info.loops.size()==2;},15000),
                 qPrintable(QString("armEnabled %1, loops %2, blocked '%3', error '%4'").arg(c.armEnabled())
                     .arg(c.snapshot()?int(c.snapshot()->info.loops.size()):-1).arg(c.armBlockedReason(),c.lastError()["text"].toString())));
        QVERIFY(waitFor([&]{return c.ownedSubscriptions().size()==2;}));
        auto &view=c.monitorView();
        auto groupsNow=[&] {
            GroupList list;bool done=false;QString error;
            c.runOnDevice([&](Client &client){list=client.groups();},[&](QString e){error=e;done=true;});
            if(!waitFor([&]{return done;}) || !error.isEmpty()) {throw std::runtime_error("LIST failed: "+error.toStdString());}
            return list;
        };
        auto group=[&](uint8_t id) {
            for(const auto &g:groupsNow().groups) {if(g.kind==GroupKind::Subscription && g.id==id) {return g;}}
            return Group{};
        };
        // Layout's live group exists while Layout is shown, and goes off-page.
        c.selectNode("5:0");QVERIFY(waitFor([&]{return c.liveSubscription().has_value();}));
        const auto liveSlot=*c.liveSubscription();
        c.setPage("monitor");
        QVERIFY(waitFor([&]{return !c.liveSubscription() && !group(liveSlot).configured && view.link()["total"].toInt()==8;}));
        QCOMPARE(view.link()["groups"].toInt(),2);
        // One group per plotted loop, on a free slot.
        const uint8_t extra=1;
        const InstanceKey second{114,0};
        view.toggleSignal("112:0:256");
        QVERIFY(waitFor([&]{return c.monitorSubscriptions().size()==1;}));
        const auto slot=c.monitorSubscriptions()[0];
        auto g=group(slot);QVERIFY(g.configured);QCOMPARE(g.loop,uint8_t(0));QCOMPARE(g.divider,uint16_t(1));
        QCOMPARE(g.refs,(std::vector<Ref>{{112,0,0x100}}));
        const auto statistics=c.ownedSubscriptions();
        QVERIFY(std::find(statistics.begin(),statistics.end(),slot)==statistics.end());
        QVERIFY(waitFor([&]{return view.ring("112:0:256") && view.ring("112:0:256")->size()>=3;}));
        QVERIFY(waitFor([&]{return view.values().contains("112:0:256");}));
        // Adding to the same loop reconfigures the same slot.
        view.toggleSignal("5:0:258");
        QVERIFY(waitFor([&]{return group(slot).refs.size()==2;}));
        QCOMPARE(c.monitorSubscriptions(),std::vector<uint8_t>{slot});
        // Another loop: a second group; its rate change reconfigures it.
        view.toggleSignal(itemKey(second.first,second.second,0x100));
        QVERIFY(waitFor([&]{return c.monitorSubscriptions().size()==2;}));
        const auto both=c.monitorSubscriptions();
        const auto other=*std::find_if(both.begin(),both.end(),[&](uint8_t id){return id!=slot;});
        QCOMPARE(group(other).loop,extra);
        view.setRate(extra,2);QVERIFY(waitFor([&]{return group(other).divider==2;}));
        QVERIFY(waitFor([&]{return view.link()["groups"].toInt()==4;}));
        // Removing a loop's last signal clears its slot.
        view.toggleSignal(itemKey(second.first,second.second,0x100));
        QVERIFY(waitFor([&]{return c.monitorSubscriptions().size()==1 && !group(other).configured;}));
        // Layout again: its live group returns on a free slot; Monitor keeps its group.
        c.setPage("layout");
        QVERIFY(waitFor([&]{return c.liveSubscription().has_value();}));
        QCOMPARE(c.monitorSubscriptions(),std::vector<uint8_t>{slot});
        c.setPage("monitor");QVERIFY(waitFor([&]{return !c.liveSubscription();}));
        // Boot: data cleared, the same plot set set up again under the new boot.
        {
            const auto before=c.snapshot()->generation;
            c.reboot();
            QVERIFY2(waitFor([&]{return c.ready() && c.snapshot()->generation!=before && c.monitorSubscriptions().size()==1;},15000),
                     qPrintable(QString("ready %1, booted %2, groups %3, plotted %4, note '%5', error '%6'")
                         .arg(c.ready()).arg(c.snapshot() && c.snapshot()->generation!=before).arg(c.monitorSubscriptions().size())
                         .arg(view.plotted()).arg(view.link()["note"].toString(),c.lastError()["text"].toString())));
            QCOMPARE(view.plotted(),2);
            QVERIFY(waitFor([&]{return view.ring("112:0:256") && view.ring("112:0:256")->size()>=3;}));
            const auto g=group(c.monitorSubscriptions()[0]);
            QVERIFY(g.configured);QCOMPARE(g.refs.size(),size_t(2));
        }
        // Another client takes every free slot: a new loop is refused locally.
        // (vdev serves one TCP client at a time: these go through this
        // connection's executor but are not recorded as owned.)
        std::vector<uint8_t> foreign;
        {
            bool done=false;QString error;
            c.runOnDevice([&](Client &client) {
                for(const auto &f:client.groups().groups) {
                    if(f.kind!=GroupKind::Subscription || f.configured) {continue;}
                    const std::vector<Ref> refs{{5,0,protocol::defs::loop::kTicks}};
                    client.subConfigure(f.id,0,5,refs);foreign.push_back(f.id);
                }
            },[&](QString e){error=e;done=true;});
            QVERIFY(waitFor([&]{return done;}));QVERIFY2(error.isEmpty(),qPrintable(error));
        }
        QVERIFY(!foreign.empty());
        c.setPage("device");c.setPage("monitor");
        QVERIFY(waitFor([&]{return view.link()["groups"].toInt()==8;}));
        view.toggleSignal(itemKey(second.first,second.second,0x100));
        QVERIFY(view.refusal()["reason"].toString().contains("No free subscription slot"));
        QCOMPARE(c.monitorSubscriptions().size(),size_t(1));
        // Layout again: no free slot for the live group, Monitor keeps its own.
        c.setPage("layout");
        QVERIFY(waitFor([&]{return c.live()["note"].toString().contains("No free subscription slot");}));
        // Close clears exactly what this GUI owns; foreign groups stay.
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();}));
        Session s(fixture.endpoint.toStdString());Client client(s);
        const auto list=client.groups();
        for(const auto &f:list.groups) {
            if(f.kind!=GroupKind::Subscription) {continue;}
            const bool theirs=std::find(foreign.begin(),foreign.end(),f.id)!=foreign.end();
            QCOMPARE(f.configured,theirs);
        }
    }
    // The page in the shell: Plot in Monitor from Layout, the plot draws,
    // Space pauses (not while typing), cursors and their readout, snapshot.
    void monitorInShell() {
        Q_INIT_RESOURCE(qmake_DriveLink_Shell);
        Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);registerShellTypes();
        Fixture fixture;
        {
            Session s(fixture.endpoint.toStdString());Client cl(s);cl.info();
            cl.add(112,"left_pwm",0);cl.system(5);cl.system(8);
        }
        QTest::qWait(150);
        QTemporaryDir dir;LocalConnection connection;connection.setSettingsPath(dir.filePath("gui.ini"));
        connection.monitorView().setSnapshotDirectory(dir.filePath("snapshots"));
        SafetyKeys keys;
        QQmlApplicationEngine engine;QVERIFY(installStyle(engine));
        engine.rootContext()->setContextProperty("connection",&connection);
        engine.rootContext()->setContextProperty("safetyKeys",&keys);
        engine.loadFromModule("DriveLink.Shell","Shell");QVERIFY(!engine.rootObjects().isEmpty());
        auto *window=qobject_cast<QQuickWindow *>(engine.rootObjects().front());QVERIFY(window);
        window->resize(1920,1080);
        connection.connectTo(fixture.endpoint);
        QVERIFY(waitFor([&]{return connection.armEnabled() && connection.snapshot()->configured.contains({112,0});},15000));
        auto shown=[&](const char *name)->QQuickItem * {
            auto *item=visualChild(window->contentItem(),name);
            return item && item->isVisible()?item:nullptr;
        };
        auto click=[&](QQuickItem *item) {
            QTest::qWait(100);
            QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene({item->width()/2,item->height()/2}).toPoint());
        };
        window->setProperty("page",1);connection.selectNode("112:0");
        QVERIFY(waitFor([&]{return shown("plotSignals") && shown("plotSignals")->isEnabled();}));
        click(shown("plotSignals"));
        QVERIFY(waitFor([&]{return window->property("page").toInt()==3 && shown("monitorPage");}));
        QVERIFY(connection.monitorView().plotted()>=2);
        QVERIFY(waitFor([&]{return connection.monitorSubscriptions().size()==1 && !connection.liveSubscription();}));
        auto *plot=shown("monitorPlotItem_0");QVERIFY(plot);
        QVERIFY(waitFor([&]{return plot->property("vertices").toInt()>=2;}));
        QVERIFY(shown("monitorChip_112:0:256"));
        // Nothing under a shell overlay reacts to Space.
        window->setProperty("drawerOpen",true);QTest::qWait(50);
        QTest::keyClick(window,Qt::Key_Space);QTest::qWait(100);QVERIFY(!connection.monitorView().paused());
        window->setProperty("drawerOpen",false);QTest::qWait(50);
        // Space pauses and resumes, also right after a click on a signal row
        // (the row does not take it); in the search field it types instead.
        auto *row=shown("monitorSignal_5:0:258");QVERIFY(row);
        const auto before=connection.monitorView().plotted();
        click(row);QVERIFY(waitFor([&]{return connection.monitorView().plotted()!=before;}));
        const auto afterClick=connection.monitorView().plotted();
        QTest::keyClick(window,Qt::Key_Space);QVERIFY(waitFor([&]{return connection.monitorView().paused();}));
        QCOMPARE(connection.monitorView().plotted(),afterClick);
        click(shown("monitorCursors"));QVERIFY(waitFor([&]{return shown("monitorReadout");}));
        QVERIFY(!connection.monitorView().readout()["rows"].toList().isEmpty());
        auto *search=shown("monitorSearch");QVERIFY(search);
        click(search);QTRY_VERIFY(search->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_V);QTest::keyClick(window,Qt::Key_Space);
        QCOMPARE(search->property("text").toString(),QString("v "));QVERIFY(connection.monitorView().paused());
        search->setProperty("text","");search->setFocus(false);
        click(shown("monitorPause"));QVERIFY(waitFor([&]{return !connection.monitorView().paused() && !shown("monitorReadout");}));
        // Snapshot CSV: a toast with the path, no dialog.
        QVERIFY(waitFor([&]{return connection.monitorView().nowUs()>0;}));
        click(shown("monitorSnapshot"));
        QVERIFY(waitFor([&]{return QDir(dir.filePath("snapshots")).entryList(QDir::Files).size()==1;}));
        // A refused add shows the reason in the Signals card.
        connection.monitorView().toggleSignal("112:1:256");
        QVERIFY(waitFor([&]{return shown("monitorRefusal");}));
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
    }
    // H2d-2 hold rule and the manual command rules: which inputs are
    // commanded, the group split, the slider span, steps, ramp, Latch,
    // typed values, Hold all, the zero after a live write, frames.
    void commandLogic() {
        auto ranged=[](ValueType type,double lo,double hi,bool hasRange=true) {
            ItemDescriptor d;d.kind=ItemKind::Input;d.value_type=type;d.has_min=d.has_max=hasRange;
            d.min_bits=numberToBits(type,lo);d.max_bits=numberToBits(type,hi);d.name="in";return d;
        };
        QVERIFY(commandable(ranged(ValueType::F32,-1,1)));
        QVERIFY(commandable(ranged(ValueType::U16,0,100)));
        QVERIFY(commandable(ranged(ValueType::I32,-5,5)));
        QVERIFY(!commandable(ranged(ValueType::F32,-1,1,false)));      // position_sp: no range
        QVERIFY(!commandable(ranged(ValueType::F32,0.001,1000)));      // excludes 0
        QVERIFY(!commandable(ranged(ValueType::F32,0,0)));             // only 0
        QVERIFY(!commandable(ranged(ValueType::Bool,0,1)));
        QVERIFY(!commandable(ranged(ValueType::Enum,0,3)));
        auto inf=ranged(ValueType::F32,-1,1);inf.max_bits=std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity());
        QVERIFY(!commandable(inf));
        QVERIFY(commandRefusal(ranged(ValueType::F32,-1,1,false)).contains("range"));
        // Planning: per loop, value groups (commandable inputs) and
        // keepalive-only groups (the rest), each split by the entry limit.
        {
            auto s=sample();s.ready=true;s.connected=true;s.info.max_payload=1024;
            auto &act=s.types[112];
            ModelItem duty=act.items[0x200];duty.descriptor=ranged(ValueType::F32,-1,1);duty.descriptor.id=0x200;act.items[0x200]=duty;
            ModelItem extra=duty;extra.descriptor.id=0x202;act.items[0x202]=extra;
            act.items[0x201]=item(0x201,"position_sp",ItemKind::Input,ValueType::F32);
            GroupList groups;groups.entry_capacity=64;
            for(uint8_t id=0;id<6;++id) {Group g;g.kind=GroupKind::Command;g.id=id;groups.groups.push_back(g);}
            auto plans=planCommands(s,groups);
            QCOMPARE(plans.size(),size_t(2));
            QVERIFY(plans[0].values);QCOMPARE(plans[0].loop,uint8_t(0));
            QCOMPARE(plans[0].refs,(std::vector<Ref>{{112,0,0x200},{112,0,0x202}}));
            QVERIFY(!plans[1].values);QCOMPARE(plans[1].refs,(std::vector<Ref>{{112,0,0x201}}));
            s.system_params[protocol::defs::system::kLimitsGroupEntries]=uint16_t(1);
            plans=planCommands(s,groups);
            QCOMPARE(plans.size(),size_t(3));QVERIFY(plans[0].values && plans[1].values && !plans[2].values);
            groups.groups.resize(2);
            QVERIFY_EXCEPTION_THROWN(planCommands(s,groups),Error); // not enough slots: ARM unavailable
        }
        // The model: one value group of an f32 duty [-1, 1] and an i32 count [-10, 10].
        auto count=ranged(ValueType::I32,-10,10);
        CommandModel m;
        m.configure({{4,{{{112,0,0x200},ranged(ValueType::F32,-1,1)},{{112,0,0x203},count}}}});
        QCOMPARE(m.inputs().size(),size_t(2));
        QCOMPARE(m.inputs()[0].limit,0.25);QCOMPARE(m.inputs()[0].decimals,2);QCOMPARE(m.inputs()[0].from(),-0.25);
        QCOMPARE(m.inputs()[1].limit,3.0);QCOMPARE(m.inputs()[1].step,1.0);
        QVERIFY_EXCEPTION_THROWN(m.configure({{1,{{{112,0,0x201},ranged(ValueType::F32,-1,1,false)}}}}),Error);
        m.configure({{4,{{{112,0,0x200},ranged(ValueType::F32,-1,1)},{{112,0,0x203},count}}}});
        // Disabled (disarmed): nothing is accepted or sent.
        m.setShown(0,true);m.setShown(1,true);
        QVERIFY(!m.press(0));QVERIFY(m.tick(0).empty());
        m.setEnabled(true);
        auto value=[](const CommandModel::Frame &f,size_t i,ValueType t){return bitsToNumber(t,f.bits.at(i));};
        // Drag: clamped to the limit, on the shown step; the frame carries
        // every input of the group; frames are at least 20 ms apart.
        QVERIFY(m.press(0));QVERIFY(m.drag(0,0.1234));QCOMPARE(m.inputs()[0].value,0.12);
        auto frames=m.tick(1000);QCOMPARE(frames.size(),size_t(1));QCOMPARE(frames[0].group,uint8_t(4));
        QCOMPARE(frames[0].bits.size(),size_t(2));
        QCOMPARE(value(frames[0],0,ValueType::F32),double(0.12f));QCOMPARE(value(frames[0],1,ValueType::I32),0.0);
        QVERIFY(m.tick(1005).empty());
        m.drag(0,0.9);QCOMPARE(m.inputs()[0].value,0.25);
        QVERIFY(m.tick(1010).empty());QCOMPARE(m.tick(1020).size(),size_t(1));
        // Release without Latch: a ramp to 0 in 20 ms steps; every frame a
        // shown value (on the step, never growing), the last exactly 0.
        m.release(0,2000);QVERIFY(m.inputs()[0].ramping);
        double last=0.25;int sent=0;
        for(int64_t t=2000;t<=2400;t+=CommandModel::kStepMs) {
            for(const auto &f:m.tick(t)) {
                const double v=value(f,0,ValueType::F32);++sent;
                QVERIFY(v<=last+1e-9);last=v;
                QCOMPARE(v,double(float(m.inputs()[0].value)));
                QVERIFY(std::abs(m.inputs()[0].value*100-std::round(m.inputs()[0].value*100))<1e-9);
            }
        }
        QCOMPARE(last,0.0);QVERIFY(!m.inputs()[0].ramping);QVERIFY(sent>=10);QVERIFY(!m.active());
        // Latch keeps the value on release; a typed value needs Latch.
        QVERIFY(!m.type(0,0.1));
        m.press(0);m.drag(0,0.2);m.setLatched(0,true,3000);m.release(0,3000);
        QCOMPARE(m.inputs()[0].value,0.2);QVERIFY(!m.inputs()[0].ramping);
        QCOMPARE(*m.type(0,-0.155),-0.16);QCOMPARE(*m.type(0,-7),-0.25);
        m.tick(3100);
        // Unlatching a value not held ramps it to 0.
        m.setLatched(0,false,3200);QVERIFY(m.inputs()[0].ramping);
        for(int64_t t=3200;t<=3500;t+=CommandModel::kStepMs) {m.tick(t);}
        QCOMPARE(m.inputs()[0].value,0.0);
        // Pressing stops a ramp where it is.
        m.press(1);m.drag(1,2.6);QCOMPARE(m.inputs()[1].value,3.0);m.release(1,4000);m.tick(4000);m.tick(4040);
        const double stopped=m.inputs()[1].value;QVERIFY(stopped<3 && stopped>0);
        m.press(1);QVERIFY(!m.inputs()[1].ramping);QCOMPARE(m.inputs()[1].value,stopped);
        m.release(1,4100);for(int64_t t=4100;t<=4400;t+=20) {m.tick(t);}
        // Hold all: every input to 0 at once, unlatched.
        m.press(0);m.drag(0,0.1);m.setLatched(0,true,5000);m.release(0,5000);m.tick(5000);
        m.holdAll();QCOMPARE(m.inputs()[0].value,0.0);QVERIFY(!m.inputs()[0].latched);
        frames=m.tick(5100);QCOMPARE(frames.size(),size_t(1));QCOMPARE(value(frames[0],0,ValueType::F32),0.0);
        // After a live write: the instance's inputs to 0 and a fresh frame,
        // even at 0 already.
        QVERIFY(m.tick(5200).empty());
        m.zeroInstance(112,0);frames=m.tick(5300);QCOMPARE(frames.size(),size_t(1));
        m.zeroInstance(113,0);QVERIFY(m.tick(5400).empty());
        // Remove returns the input to 0; a frame the caller could not queue is retried.
        m.press(0);m.drag(0,0.05);m.release(0,6000);m.setLatched(0,true,6000);
        m.tick(6100);
        m.remove(0);QVERIFY(!m.inputs()[0].shown);frames=m.tick(6200);QCOMPARE(frames.size(),size_t(1));
        m.retry(4);QVERIFY(m.active());QCOMPARE(m.tick(6300).size(),size_t(1));
        // The limit: clamped to the descriptor bound; the step follows it.
        m.setShown(0,true);
        m.setLimit(0,5);QCOMPARE(m.inputs()[0].limit,1.0);
        m.setLimit(0,0.05);QCOMPARE(m.inputs()[0].decimals,3);QCOMPARE(m.inputs()[0].step,0.001);
        m.press(0);m.drag(0,0.04567);QCOMPARE(m.inputs()[0].value,0.046);
        m.setLimit(0,0.02);QCOMPARE(m.inputs()[0].value,0.02);m.release(0,7000);
        // Disabling (DISARM) returns every value to 0 locally and sends nothing.
        m.press(1);m.drag(1,2);m.setLatched(1,true,8000);m.release(1,8000);
        m.setEnabled(false);
        QCOMPARE(m.inputs()[1].value,0.0);QVERIFY(!m.inputs()[1].latched);QVERIFY(m.tick(9000).empty());QVERIFY(!m.active());
        m.setEnabled(true);QVERIFY(m.tick(9100).empty());
        // An unsigned input: the slider span starts at 0.
        CommandModel u;u.configure({{0,{{{112,0,0x204},ranged(ValueType::U16,0,100)}}}});
        u.setEnabled(true);u.setShown(0,true);
        QCOMPARE(u.inputs()[0].from(),0.0);QCOMPARE(u.inputs()[0].to(),25.0);
        u.press(0);u.drag(0,-5);QCOMPARE(u.inputs()[0].value,0.0);u.drag(0,7.6);QCOMPARE(u.inputs()[0].value,8.0);
        QCOMPARE(numberToBits(ValueType::U16,8),8u);
        // A range finer than the card's 4 decimals could only send 0.
        QVERIFY(!commandable(ranged(ValueType::F32,-5e-5,5e-5)));
        QVERIFY(commandable(ranged(ValueType::F32,-2e-4,2e-4)));
        // A limit change re-quantizes toward zero: never a larger value.
        CommandModel g;g.configure({{1,{{{112,0,0x200},ranged(ValueType::F32,-100,100)}}}});
        g.setEnabled(true);g.setShown(0,true);g.setLimit(0,5);
        g.press(0);g.drag(0,4.56);g.setLatched(0,true,0);g.release(0,0);QCOMPARE(g.inputs()[0].value,4.56);
        g.setLimit(0,10);QCOMPARE(g.inputs()[0].decimals,1);QCOMPARE(g.inputs()[0].value,4.5);
        g.setLimit(0,5);g.type(0,-4.56);g.setLimit(0,10);QCOMPARE(g.inputs()[0].value,-4.5);
        // A live param write in flight (user decision 2026-11-18): the
        // instance goes to 0 at its start, takes no press, Latch or typed
        // value, and its group sends a fresh frame when the write ends.
        CommandModel w;
        w.configure({{2,{{{112,0,0x200},ranged(ValueType::F32,-1,1)},{{112,1,0x200},ranged(ValueType::F32,-1,1)}}}});
        w.setEnabled(true);w.setShown(0,true);w.setShown(1,true);
        w.press(0);w.drag(0,0.2);w.setLatched(0,true,0);w.release(0,0);
        w.press(1);w.drag(1,0.1);w.setLatched(1,true,0);w.release(1,0);w.tick(0);
        w.beginWrite(112,0);
        QCOMPARE(w.inputs()[0].value,0.0);QVERIFY(!w.inputs()[0].latched);QCOMPARE(w.inputs()[1].value,0.1);
        frames=w.tick(100);QCOMPARE(frames.size(),size_t(1));
        QCOMPARE(value(frames[0],0,ValueType::F32),0.0);QCOMPARE(value(frames[0],1,ValueType::F32),double(0.1f));
        QVERIFY(!w.press(0));w.setLatched(0,true,100);QVERIFY(!w.inputs()[0].latched);
        // Another input of the group keeps working; its frames carry 0 for the written one.
        QVERIFY(w.press(1));w.drag(1,0.15);w.release(1,200);
        frames=w.tick(200);QCOMPARE(value(frames[0],0,ValueType::F32),0.0);
        w.beginWrite(112,0);w.endWrite(112,0); // two writes in flight, one ended
        QVERIFY(!w.press(0));
        QCOMPARE(w.tick(300).size(),size_t(1)); // each ended write sends a fresh frame
        w.endWrite(112,0);frames=w.tick(400);QCOMPARE(frames.size(),size_t(1)); // the fresh frame
        QVERIFY(w.press(0));
        w.endWrite(112,0);QVERIFY(w.press(0)); // an extra end (a Boot in between) is harmless
    }
    // Manual command on vdev: the group split as configured on the device,
    // nothing accepted while disarmed, the commanded input read back over
    // Monitor's subscription, the dead-man ramp, Latch, a live write that
    // zeroes the instance, Hold all, DISARM, idle keepalives, and close.
    void commandOperations() {
        Fixture fixture;
        uint8_t freeId=0,drivenId=0,encId=0,driveId=0;
        {
            Session s(fixture.endpoint.toStdString());Client cl(s);cl.info();
            freeId=cl.add(112,"free",0).instance;drivenId=cl.add(112,"driven",0).instance;
            encId=cl.add(113,"enc",0).instance;driveId=cl.add(32,"drive",0).instance;
            const std::array<Ref,3> refs{{{113,encId,1},{32,driveId,1},{32,driveId,2}}};
            const std::array<uint32_t,3> values{{112u<<8|drivenId,112u<<8|drivenId,113u<<8|encId}};
            cl.set(refs,values);cl.system(5);cl.system(8);
        }
        QTest::qWait(150);
        LocalConnection c;QTemporaryDir dir;c.setSettingsPath(dir.filePath("gui.ini"));
        c.connectTo(fixture.endpoint);
        QVERIFY2(waitFor([&]{return c.armEnabled() && c.snapshot()->configured.contains({32,driveId});},15000),
                 qPrintable(c.armBlockedReason()+" / "+c.lastError()["text"].toString()));
        QCOMPARE(c.snapshot()->configured.at({32,driveId}).instance.state,uint8_t(0));
        auto &cv=c.commandView();
        const Ref duty{112,freeId,0x200},position{32,driveId,0x201},velocitySp{32,driveId,0x200};
        // The split as planned and as configured on the device.
        const auto plans=c.commandGroups();
        auto planOf=[&](const Ref &r)->std::optional<CommandPlan> {
            for(const auto &p:plans) {if(std::find(p.refs.begin(),p.refs.end(),r)!=p.refs.end()) {return p;}}
            return std::nullopt;
        };
        QVERIFY(planOf(duty) && planOf(duty)->values);
        QVERIFY(planOf(velocitySp) && planOf(velocitySp)->values);
        QVERIFY(planOf(position) && !planOf(position)->values);
        {
            GroupList list;bool done=false;
            c.runOnDevice([&](Client &client){list=client.groups();},[&](QString){done=true;});
            QVERIFY(waitFor([&]{return done;}));
            for(const auto &p:plans) {
                const auto g=std::find_if(list.groups.begin(),list.groups.end(),[&](const Group &g){return g.kind==GroupKind::Command && g.id==p.id;});
                QVERIFY(g!=list.groups.end() && g->configured);QCOMPARE(g->refs,p.refs);
            }
        }
        // Candidates: commandable inputs enabled, position_sp disabled with the reason.
        const auto dutyKey=itemKey(duty.type,duty.instance,duty.item);
        bool sawPosition=false;
        for(const auto &v:cv.candidates()) {
            const auto row=v.toMap();
            if(row["key"]==itemKey(position.type,position.instance,position.item)) {
                sawPosition=true;QVERIFY(!row["enabled"].toBool());QVERIFY(row["reason"].toString().contains("range"));
            }
            if(row["key"]==dutyKey) {QVERIFY(row["enabled"].toBool());}
        }
        QVERIFY(sawPosition);
        // Plot the commanded input and the mock's response.
        auto &view=c.monitorView();c.setPage("monitor");
        const auto velocityKey=itemKey(112,freeId,0x100);
        view.toggleSignal(dutyKey);view.toggleSignal(velocityKey);
        auto latest=[&](const QString &key)->std::optional<double> {
            const auto *ring=view.ring(key);
            if(!ring || ring->size()==0) {return std::nullopt;}
            return ring->value(ring->size()-1);
        };
        QVERIFY(waitFor([&]{return latest(dutyKey).has_value();}));
        // Disarmed: the card refuses.
        cv.add(dutyKey);QCOMPARE(cv.rows()->rowCount(),1);
        QVERIFY(!cv.enabled());QVERIFY(cv.blocked().contains("Arm"));
        cv.press(dutyKey);cv.drag(dutyKey,0.2);QCOMPARE(cv.model().inputs()[size_t(cv.model().find(duty))].value,0.0);
        // Armed: a drag is sent and read back; the mock responds.
        c.arm();QVERIFY(waitFor([&]{return c.armState()==1 && cv.enabled();}));
        const auto at=[&]{return cv.model().inputs()[size_t(cv.model().find(duty))];};
        cv.press(dutyKey);cv.drag(dutyKey,0.2);QCOMPARE(at().value,0.2);
        QVERIFY2(waitFor([&]{return latest(dutyKey) && std::abs(*latest(dutyKey)-0.2)<1e-6;}),
                 qPrintable(QString::number(latest(dutyKey).value_or(-1))));
        QVERIFY(waitFor([&]{return latest(velocityKey) && *latest(velocityKey)>0.5;}));
        // Release ramps to 0 (dead-man).
        cv.release(dutyKey);QVERIFY(at().ramping);
        QVERIFY(waitFor([&]{return at().value==0 && !at().ramping && latest(dutyKey) && *latest(dutyKey)==0;}));
        // Latch keeps the value after release; idle keepalives keep the
        // device armed past the watchdog (1 s on this fixture).
        cv.press(dutyKey);cv.drag(dutyKey,0.15);cv.setLatched(dutyKey,true);cv.release(dutyKey);
        QTest::qWait(1500);
        QCOMPARE(c.armState(),1);QCOMPARE(at().value,0.15);
        QVERIFY(waitFor([&]{return latest(dutyKey) && std::abs(*latest(dutyKey)-0.15)<1e-6;}));
        // A typed value while latched.
        QCOMPARE(cv.typeValue(dutyKey,"+0.1"),QString());
        QVERIFY(waitFor([&]{return latest(dutyKey) && std::abs(*latest(dutyKey)-0.1)<1e-6;}));
        QVERIFY(!cv.typeValue(dutyKey,"abc").isEmpty());
        // A live write to the commanded instance: its inputs return to 0.
        c.setParamText(itemKey(112,freeId,2),"12");
        QVERIFY(waitFor([&]{return at().value==0 && !at().latched && latest(dutyKey) && *latest(dutyKey)==0;}));
        // Hold all.
        cv.press(dutyKey);cv.drag(dutyKey,0.1);cv.setLatched(dutyKey,true);cv.release(dutyKey);
        QVERIFY(waitFor([&]{return latest(dutyKey) && std::abs(*latest(dutyKey)-0.1)<1e-6;}));
        QVERIFY(cv.commanding());
        cv.holdAll();QVERIFY(waitFor([&]{return latest(dutyKey) && *latest(dutyKey)==0;}));QVERIFY(!cv.commanding());
        // A page change releases a held slider.
        cv.press(dutyKey);cv.drag(dutyKey,0.1);c.setPage("layout");QVERIFY(at().ramping || at().value==0);
        QVERIFY(waitFor([&]{return at().value==0;}));
        c.setPage("monitor");
        // DISARM: values return to 0 locally at once; nothing more is accepted.
        cv.press(dutyKey);cv.drag(dutyKey,0.2);cv.setLatched(dutyKey,true);cv.release(dutyKey);
        QVERIFY(waitFor([&]{return latest(dutyKey) && std::abs(*latest(dutyKey)-0.2)<1e-6;}));
        c.disarm();
        QVERIFY(!cv.enabled());QCOMPARE(at().value,0.0);QVERIFY(!at().latched);
        QVERIFY(waitFor([&]{return c.armState()==0;}));
        QVERIFY(waitFor([&]{return latest(dutyKey) && *latest(dutyKey)==0;}));
        cv.press(dutyKey);cv.drag(dutyKey,0.2);QCOMPARE(at().value,0.0);
        // Armed by another client (here through this connection's executor,
        // not this window's ARM): the card refuses.
        {
            bool done=false;QString error;
            c.runOnDevice([](Client &client){client.arm();},[&](QString e){error=e;done=true;});
            QVERIFY(waitFor([&]{return done;}));QVERIFY2(error.isEmpty(),qPrintable(error));
        }
        QVERIFY(waitFor([&]{return c.armState()==1;}));QTest::qWait(200);
        QVERIFY(!cv.enabled());QVERIFY(cv.blocked().contains("Another client"));
        c.disarm();QVERIFY(waitFor([&]{return c.armState()==0;}));
        // Close clears this window's command groups.
        c.disconnectDevice();QVERIFY(waitFor([&]{return !c.closing() && !c.linked();}));
        QCOMPARE(cv.rows()->rowCount(),0);
        Session s(fixture.endpoint.toStdString());Client client(s);
        for(const auto &g:client.groups().groups) {QVERIFY(g.kind!=GroupKind::Command || !g.configured);}
    }
    // The Manual command card in the shell: add an input from the list,
    // disabled while disarmed, a mouse drag sends and its release ramps to
    // 0, Latch and a typed value, and Esc in the value field disarms.
    void commandInShell() {
        Q_INIT_RESOURCE(qmake_DriveLink_Shell);
        Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);registerShellTypes();
        Fixture fixture;
        {
            Session s(fixture.endpoint.toStdString());Client cl(s);cl.info();
            cl.add(112,"left_pwm",0);cl.system(5);cl.system(8);
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
        QVERIFY(waitFor([&]{return connection.armEnabled() && connection.snapshot()->configured.contains({112,0});},15000));
        auto shown=[&](const QString &name)->QQuickItem * {
            auto *item=visualChild(window->contentItem(),name);
            return item && item->isVisible()?item:nullptr;
        };
        auto center=[&](QQuickItem *item,double fx=0.5) {return item->mapToScene({item->width()*fx,item->height()/2}).toPoint();};
        auto click=[&](QQuickItem *item) {QTest::qWait(100);QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,center(item));};
        window->setProperty("page",3);
        QVERIFY(waitFor([&]{return shown("monitorCommand");}));
        auto &cv=connection.commandView();
        const QString key="112:0:512";
        click(shown("commandAdd"));
        QVERIFY(waitFor([&]{return shown("candidate:"+key);}));
        click(shown("candidate:"+key));
        QVERIFY(waitFor([&]{return shown("command:"+key);}));
        auto *control=shown("command:"+key);
        auto *slider=visualChild(control,"commandSlider");QVERIFY(slider);
        QVERIFY(!slider->isEnabled());QVERIFY(shown("commandBlocked"));
        connection.arm();
        QVERIFY(waitFor([&]{return connection.armState()==1 && slider->isEnabled();}));
        const auto at=[&]{return cv.model().inputs()[size_t(cv.model().find({112,0,0x200}))];};
        // A drag with the mouse: held and sending while pressed, a ramp to 0 after.
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,center(slider,0.5));
        QTest::mouseMove(window,center(slider,0.7));QTest::mouseMove(window,center(slider,0.85));
        QVERIFY(waitFor([&]{return at().held && at().value>0.05;}));
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,center(slider,0.85));
        QVERIFY(waitFor([&]{return !at().held && at().value==0;}));
        // Latch, then a typed value in the field.
        auto *latch=visualChild(control,"commandLatch");QVERIFY(latch);click(latch);
        QVERIFY(waitFor([&]{return at().latched;}));
        auto *field=visualChild(control,"commandValue");QVERIFY(field);
        QVERIFY(waitFor([&]{return !field->property("readOnly").toBool();}));
        click(field);QTRY_VERIFY(field->hasActiveFocus());
        field->setProperty("text","0.1");QTest::keyClick(window,Qt::Key_Return);
        QVERIFY(waitFor([&]{return std::abs(at().value-0.1)<1e-9;}));
        // Esc in the field disarms: the value returns to 0 locally.
        click(field);QTRY_VERIFY(field->hasActiveFocus());
        QTest::keyClick(window,Qt::Key_Escape);
        QVERIFY(waitFor([&]{return connection.armState()==0;}));
        QCOMPARE(at().value,0.0);QVERIFY(!slider->isEnabled());
        connection.disconnectDevice();QVERIFY(waitFor([&]{return !connection.closing();}));
    }
    // 16 series at 1 kHz over a 10 s window: decimation and a frame of the
    // plot item (offscreen, software). Logged; the bounds are loose.
    void plotPerformance() {
        Q_INIT_RESOURCE(qmake_DriveLink_Shell);
        Q_INIT_RESOURCE(drivelink_gui_shell_raw_qml_0);registerShellTypes();
        DeviceSnapshot s=sample();
        auto &act=s.types[112];
        for(uint16_t i=0;i<16;++i) {act.items[uint16_t(0x400+i)]=item(uint16_t(0x400+i),"wave",ItemKind::Signal,ValueType::F32);}
        MonitorView view;view.setSnapshot(shared(s));view.setRate(0,1);
        std::vector<Ref> refs;QStringList keys;
        for(uint16_t i=0;i<16;++i) {
            refs.push_back({112,0,uint16_t(0x400+i)});keys<<itemKey(112,0,uint16_t(0x400+i));view.toggleSignal(keys.back());
        }
        QCOMPARE(view.plotted(),16);
        const auto sinks=view.sinks(refs);
        for(int t=0;t<10000;++t) {
            for(size_t k=0;k<sinks.size();++k) {view.push(sinks[k],int64_t(t)*1000,std::bit_cast<uint32_t>(float(std::sin(t*0.01+k))));}
        }
        view.endAppend();
        QElapsedTimer timer;timer.start();
        size_t points=0;
        for(const auto &key:keys) {points+=decimate(*view.ring(key),view.windowStart(),int64_t(view.nowUs()),1600).size();}
        const auto decimateMs=timer.nsecsElapsed()/1e6;
        QQmlEngine engine;QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport DriveLink.Shell\nWindow {\n width: 1700; height: 400; visible: true\n"
                          " PlotItem { objectName: 'plot'; anchors.fill: parent }\n}\n",QUrl());
        std::unique_ptr<QObject> root(component.create());QVERIFY2(root,qPrintable(component.errorString()));
        auto *window=qobject_cast<QQuickWindow *>(root.get());QVERIFY(window);
        auto *plot=window->findChild<PlotItem *>("plot");QVERIFY(plot);
        QList<QColor> colors;for(int i=0;i<16;++i) {colors<<QColor::fromHsv(i*22,200,220);}
        plot->setSource(&view);plot->setKeys(keys);plot->setColors(colors);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->grabWindow();
        timer.restart();
        constexpr int frames=10;
        for(int i=0;i<frames;++i) {
            for(size_t k=0;k<sinks.size();++k) {view.push(sinks[k],int64_t(10000+i)*1000,0);}
            view.endAppend();window->grabWindow();
        }
        const auto frameMs=timer.nsecsElapsed()/1e6/frames;
        qInfo("plot performance: 16 series x 10000 samples: decimation %.1f ms (%zu points), frame %.1f ms, %d vertices",
              decimateMs,points,frameMs,plot->vertices());
        QVERIFY(plot->vertices()>0 && plot->vertices()<=16*(2*1700+4));
        QVERIFY2(decimateMs<100,"decimation is far too slow");
        QVERIFY2(frameMs<150,"a plot frame is far too slow");
    }
    // Bare bench board (DRIVELINK_GUI_BENCH_ENDPOINT, existing firmware,
    // nothing flashed): the fastest loop's four statistics plotted at its
    // full rate (1 kHz, about 41 % of the link); then beside a second 1 kHz
    // group of another client (about 84 %); then with the loop sped to
    // 2 kHz live, where the plot stays at the 1 kHz a group carries
    // (kMaxGroupRateHz, two ticks per sample). Rates come from device
    // timestamps, ticks from the device counter; under 100 % nothing is lost.
    // The board is restored on every exit: the other group cleared, the rate
    // set back, REVERT (a board has a saved configuration), and close.
    void benchMonitor() {
        const auto endpoint=qEnvironmentVariable("DRIVELINK_GUI_BENCH_ENDPOINT");
        if(endpoint.isEmpty()) {QSKIP("set DRIVELINK_GUI_BENCH_ENDPOINT to run on a device");}
        QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));c.connectTo(endpoint);
        QVERIFY2(waitFor([&]{return c.ready() && c.snapshot()->status.has_value();},30000),qPrintable(c.lastError()["message"].toString()));
        QVERIFY2(!c.unsaved() && !c.pendingReboot(),"refusing to run with unsaved or pending changes");
        auto &view=c.monitorView();
        c.setPage("monitor");QVERIFY(waitFor([&]{return view.link()["total"].toInt()>0;},10000));
        const bool vdev=c.info()["firmware"].toString().endsWith("-vdev");
        // The fastest loop.
        const auto &loops=c.snapshot()->info.loops;QVERIFY(!loops.empty());
        const auto loop=std::max_element(loops.begin(),loops.end(),[](const LoopInfo &a,const LoopInfo &b){return a.rate_hz<b.rate_hz;})->id;
        const auto saved=loopRateHz(*c.snapshot(),loop);
        const auto rateKey=itemKey(5,loop,protocol::defs::loop::kRateHz);
        qInfo("bench: device %s firmware %s, loop %d at %u Hz, groups %d of %d",qPrintable(c.deviceId()),
              qPrintable(c.info()["firmware"].toString()),loop,saved,view.link()["groups"].toInt(),view.link()["total"].toInt());
        const std::vector<Ref> statistics{{5,loop,protocol::defs::loop::kTicks},{5,loop,protocol::defs::loop::kOverruns},
                                          {5,loop,protocol::defs::loop::kLastRunUs},{5,loop,protocol::defs::loop::kMaxRunUs}};
        const auto onDevice=[&](std::function<void(Client &)> operation) {
            bool done=false;QString error;
            c.runOnDevice(std::move(operation),[&](QString e){error=e;done=true;});
            if(!waitFor([&]{return done;},10000)) {return QString("no reply");}
            return error;
        };
        std::optional<uint8_t> other;bool sped=false;
        struct Restore {std::function<void()> run;~Restore() {run();}} restore{[&] {
            if(other) {
                const auto id=*other;
                const auto error=onDevice([id](Client &client){client.groupClear(id,false);});
                if(!error.isEmpty()) {qWarning("bench restore: clearing group %d: %s",id,qPrintable(error));}
            }
            if(sped) {
                c.setParamText(rateKey,QString::number(saved));
                if(!waitFor([&]{return c.snapshot() && loopRateHz(*c.snapshot(),loop)==saved && !c.configBusy();},10000)) {qWarning("bench restore: rate not set back");}
            }
            if(!vdev && c.unsaved()) {
                c.revert();
                if(!waitFor([&]{return !c.unsaved() && !c.configBusy();},10000)) {qWarning("bench restore: REVERT failed: %s",qPrintable(c.lastError()["text"].toString()));}
            }
            if(c.linked()) {c.disconnectDevice();waitFor([&]{return !c.closing() && !c.linked();},10000);}
        }};
        view.setRate(loop,1);QVERIFY(view.refusal().isEmpty());
        QStringList keys;
        for(const auto &r:statistics) {
            keys<<itemKey(r.type,r.instance,r.item);view.toggleSignal(keys.back());
            QVERIFY2(view.refusal().isEmpty(),qPrintable(view.refusal()["reason"].toString()));
        }
        QVERIFY(waitFor([&]{return c.monitorSubscriptions().size()==1;},10000));
        const auto mine=c.monitorSubscriptions()[0];
        // The plotted group's divider on the device.
        const auto deviceDivider=[&] {
            uint16_t divider=0;
            onDevice([&,mine](Client &client) {
                for(const auto &g:client.groups().groups) {if(g.kind==GroupKind::Subscription && g.id==mine) {divider=g.divider;}}
            });
            return divider;
        };
        // Measures over `seconds` of device time: rate from timestamps, the
        // ticks advanced per sample, and frames lost.
        const auto measure=[&](double rate,double ticksPerSample,int seconds) {
            QVERIFY(waitFor([&]{return view.ring(keys[0]) && view.ring(keys[0])->size()>size_t(rate/2);},10000));
            const auto lostBefore=view.link()["lost"].toLongLong();
            const auto from=int64_t(view.nowUs());
            QVERIFY(waitFor([&]{return int64_t(view.nowUs())-from>=int64_t(seconds)*1000000;},seconds*1000+10000));
            const auto *ticks=view.ring(keys[0]);
            const auto first=ticks->lowerBound(from),last=ticks->size()-1;QVERIFY(last>first+10);
            const double span=double(ticks->time(last)-ticks->time(first));
            const double measured=double(last-first)/span*1e6;
            const double advanced=(ticks->value(last)-ticks->value(first))/double(last-first);
            const auto lost=view.link()["lost"].toLongLong()-lostBefore;
            qInfo("bench: predicted %.0f Hz, %.0f ticks per sample, %s of the link (groups %d); measured %.2f Hz over %.2f s, %.3f ticks per sample, %lld lost",
                  rate,ticksPerSample,qPrintable(view.link()["percent"].toString()),view.link()["groups"].toInt(),measured,span/1e6,advanced,lost);
            // vdev runs its loops at the desktop timer's pace; a board is exact.
            // (A skipped timer period advances ticks without a sample.)
            if(!vdev) {
                QVERIFY2(std::abs(measured-rate)<=rate*0.01,"measured rate differs from the prediction");
                QVERIFY2(std::abs(advanced-ticksPerSample)<0.001*ticksPerSample,"ticks per sample differ from the prediction");
            }
            QCOMPARE(lost,0LL);
        };
        // 1: alone at the loop rate.
        measure(std::min<double>(saved,kMaxGroupRateHz),std::max<double>(1,minDivider(saved)),5);if(QTest::currentTestFailed()) {return;}
        // 2: another client's group of the same four values at the loop rate
        // (not owned: close leaves it, so Restore clears it).
        QString error=onDevice([&](Client &client) {
            for(const auto &g:client.groups().groups) {
                if(g.kind!=GroupKind::Subscription || g.configured) {continue;}
                client.subConfigure(g.id,loop,1,statistics);other=g.id;return;
            }
        });
        QVERIFY2(error.isEmpty() && other,qPrintable(error.isEmpty()?QString("no free slot"):error));
        const auto groupsBefore=view.link()["groups"].toInt();
        c.setPage("device");c.setPage("monitor");
        QVERIFY(waitFor([&]{return view.link()["groups"].toInt()==groupsBefore+1;},10000));
        QVERIFY(view.link()["load"].toDouble()<1.0);
        measure(std::min<double>(saved,kMaxGroupRateHz),std::max<double>(1,minDivider(saved)),5);if(QTest::currentTestFailed()) {return;}
        // 3: the loop at 2 kHz, set live (not saved): the plot keeps 1 kHz.
        sped=true;c.setParamText(rateKey,"2000");
        QVERIFY(waitFor([&]{return loopRateHz(*c.snapshot(),loop)==2000 && !c.configBusy();},10000));
        QVERIFY(waitFor([&]{return deviceDivider()==2;},10000));
        QVERIFY(view.link()["load"].toDouble()<1.0);
        measure(1000,2,5);if(QTest::currentTestFailed()) {return;}
        restore.run();restore.run=[]{};
        QVERIFY(!c.linked());
        Session s(endpoint.toStdString());Client client(s);
        for(const auto &g:client.groups().groups) {QVERIFY2(!g.configured,"a group is still configured after close");}
        qInfo("bench: restored, %u Hz, groups idle",saved);
    }
    // Manual command on a device (deferred: a real motor; dry-run on vdev
    // with a mock actuator). Env: DRIVELINK_GUI_BENCH_ENDPOINT, and
    // DRIVELINK_GUI_BENCH_INPUT (an item key, type:instance:item),
    // DRIVELINK_GUI_BENCH_VALUE (small, e.g. 0.1), optional
    // DRIVELINK_GUI_BENCH_RESPONSE (a signal key) with
    // DRIVELINK_GUI_BENCH_MIN_RESPONSE (its least |value| while commanded).
    // Arms, commands the value with the slider path, checks the read-back
    // and the response, releases (ramp to 0), Latch then Hold all, a live
    // write zeroing the instance is not exercised (it would change a
    // param), and disarms. Nothing is saved; the board keeps its layout.
    void benchCommand() {
        const auto endpoint=qEnvironmentVariable("DRIVELINK_GUI_BENCH_ENDPOINT");
        const auto inputKey=qEnvironmentVariable("DRIVELINK_GUI_BENCH_INPUT");
        if(endpoint.isEmpty() || inputKey.isEmpty()) {QSKIP("set DRIVELINK_GUI_BENCH_ENDPOINT and DRIVELINK_GUI_BENCH_INPUT to run on a device");}
        bool ok=false;const double target=qEnvironmentVariable("DRIVELINK_GUI_BENCH_VALUE","0.1").toDouble(&ok);
        QVERIFY2(ok && target!=0,"DRIVELINK_GUI_BENCH_VALUE must be a non-zero number");
        const auto responseKey=qEnvironmentVariable("DRIVELINK_GUI_BENCH_RESPONSE");
        const double minResponse=qEnvironmentVariable("DRIVELINK_GUI_BENCH_MIN_RESPONSE","0").toDouble();
        const auto ref=parseItemKey(inputKey);QVERIFY2(ref,"DRIVELINK_GUI_BENCH_INPUT is not type:instance:item");
        QTemporaryDir dir;LocalConnection c;c.setSettingsPath(dir.filePath("gui.ini"));c.connectTo(endpoint);
        QVERIFY2(waitFor([&]{return c.armEnabled();},30000),qPrintable(c.armBlockedReason()+" / "+c.lastError()["message"].toString()));
        struct Restore {std::function<void()> run;~Restore() {run();}} restore{[&] {
            c.disarm();waitFor([&]{return c.armState()==0;},5000);
            c.disconnectDevice();waitFor([&]{return !c.closing();},10000);
        }};
        auto &cv=c.commandView();auto &view=c.monitorView();
        const int index=cv.model().find(*ref);
        QVERIFY2(index>=0,"the input is not in a value group (hold rule: a finite range including 0)");
        cv.add(inputKey);
        if(std::abs(target)>cv.model().inputs()[size_t(index)].limit) {QCOMPARE(cv.setLimitText(inputKey,QString::number(std::abs(target))),QString());}
        c.setPage("monitor");
        view.toggleSignal(inputKey);if(!responseKey.isEmpty()) {view.toggleSignal(responseKey);}
        auto latest=[&](const QString &key)->std::optional<double> {
            const auto *ring=view.ring(key);
            if(!ring || ring->size()==0) {return std::nullopt;}
            return ring->value(ring->size()-1);
        };
        QVERIFY(waitFor([&]{return latest(inputKey).has_value();},10000));
        const auto at=[&]{return cv.model().inputs()[size_t(index)];};
        qInfo("bench: device %s firmware %s, input %s to %g (limit %g)",qPrintable(c.deviceId()),qPrintable(c.info()["firmware"].toString()),
              qPrintable(inputKey),target,at().limit);
        c.arm();QVERIFY2(waitFor([&]{return c.armState()==1 && cv.enabled();},5000),qPrintable(cv.blocked()));
        cv.press(inputKey);cv.drag(inputKey,target);
        const double shown=at().value;QVERIFY(std::abs(shown-target)<=at().step);
        QVERIFY2(waitFor([&]{return latest(inputKey) && std::abs(*latest(inputKey)-shown)<1e-6;},3000),
                 qPrintable(QString::number(latest(inputKey).value_or(NAN))));
        QTest::qWait(1000);
        if(!responseKey.isEmpty()) {
            const auto response=latest(responseKey);
            qInfo("bench: %s = %g while commanded",qPrintable(responseKey),response.value_or(NAN));
            QVERIFY2(response && std::abs(*response)>=minResponse,"no response to the command");
        }
        cv.release(inputKey);
        QVERIFY(waitFor([&]{return at().value==0 && latest(inputKey) && *latest(inputKey)==0;},3000));
        cv.press(inputKey);cv.drag(inputKey,target);cv.setLatched(inputKey,true);cv.release(inputKey);
        QTest::qWait(500);QCOMPARE(c.armState(),1);
        QVERIFY(latest(inputKey) && std::abs(*latest(inputKey)-shown)<1e-6);
        cv.holdAll();QVERIFY(waitFor([&]{return latest(inputKey) && *latest(inputKey)==0;},3000));
        c.disarm();QVERIFY(waitFor([&]{return c.armState()==0;},5000));QCOMPARE(at().value,0.0);
        qInfo("bench: commanded, ramped, latched, held and disarmed");
    }
};
QTEST_MAIN(MonitorTests)
#include "test_monitor.moc"
