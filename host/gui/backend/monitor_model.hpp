#pragma once
// Monitor's pure rules (G5): the signal catalogue, plot rates, the link
// budget, group plans, sample rings, decimation, axes, cursors and the
// snapshot CSV. Every rule comes from the discovered descriptors and the
// device's group list; nothing here touches a device or Qt Quick.
#include <drivelink/model.hpp>
#include <QPointF>
#include <QString>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace drivelink::gui {

// ESP → host at 921 600 baud 8N1, and the 9-byte frame overhead.
inline constexpr double kLinkBytesPerSecond=92160;
inline constexpr size_t kFrameOverhead=9;
// The longest plot window and the most signals plotted at once.
inline constexpr int kMaxWindowSeconds=60;
inline constexpr size_t kMaxPlotted=16;
inline constexpr size_t kMaxPlots=8;

// A plottable item: a signal, or an input (its latest commanded value).
struct MonitorSignal {
    Ref ref;
    uint8_t loop=0;
    ModelItem item;
    QString key,name,unit,owner;
    bool input=false;
};
// A running loop (its statistic signals) or a running instance (its
// signals, then its inputs). Browser order: each running loop, then the
// running instances in it.
struct MonitorSection {
    QString key,title,subtitle;
    uint8_t loop=0;
    bool isLoop=false;
    std::vector<MonitorSignal> items;
};
std::vector<MonitorSection> monitorCatalogue(const DeviceSnapshot &);
std::optional<MonitorSignal> monitorSignal(const DeviceSnapshot &, Ref);
// A sample's slot bits as a plot value (bool 0/1, enum and unsigned values
// as numbers, i32 sign-extended, f32 as float).
double sampleValue(ValueType, uint32_t bits);
// A plot value as text: integers without decimals, floats to five
// significant digits.
QString plotValueText(ValueType, double);

// The most samples a subscription group delivers per second: the firmware's
// link task (core_link.cpp, a 1000 µs period) sends each group's latest
// sample once per run, so a faster group overwrites samples (the device
// counter shows them as lost). Plans, choices and the budget respect it.
inline constexpr uint32_t kMaxGroupRateHz=1000;
// The smallest divider that keeps a loop's group within kMaxGroupRateHz.
uint16_t minDivider(uint32_t loopRateHz);

// Plot rates of a loop: its rate divided by 1, 2, 5, 10, 20, 50, 100, …
// down to about 1 Hz, from minDivider (offered first when no step lands on it).
struct RateChoice {
    uint16_t divider=1;
    double hz=0;
};
std::vector<RateChoice> rateChoices(uint32_t loopRateHz);
// The fastest choice at or below 100 Hz (the loop rate when it is lower).
uint16_t defaultDivider(uint32_t loopRateHz);
QString hzText(double hz);
// "200 Hz (divider 5)".
QString rateLabel(uint32_t loopRateHz, uint16_t divider);

// Link budget (host-architecture "Link budget"). A stream is one
// subscription group at its rate.
struct LinkStream {
    uint8_t loop=0;
    uint16_t divider=1;
    size_t values=0;
};
inline size_t subscriptionFrameBytes(size_t values) { return kFrameOverhead+12+4*values; }
uint32_t loopRateHz(const DeviceSnapshot &, uint8_t loop);
double streamBytesPerSecond(const DeviceSnapshot &, const LinkStream &);
// The status stream: (9 + 19 + 5 per running loop) bytes at system.status.rate_hz.
double statusBytesPerSecond(const DeviceSnapshot &);
// Share of the link (1.0 is 100 %) used by the status stream and these streams.
double linkLoad(const DeviceSnapshot &, std::span<const LinkStream>);
QString percentText(double load);

// What Monitor plots: stacked plots of signals, and the plot rate per loop.
struct PlotSet {
    struct Plot {
        std::vector<Ref> series;
        bool operator==(const Plot &) const=default;
    };
    std::vector<Plot> plots{Plot{}};
    std::map<uint8_t,uint16_t> dividers;
    bool contains(Ref) const;
    size_t count() const;
    bool operator==(const PlotSet &) const=default;
};
// The plot of the signal's unit, else the first empty plot, else a new one.
void placeSignal(PlotSet &, const DeviceSnapshot &, Ref);
void removeSignal(PlotSet &, Ref);
// Drops signals that no longer exist or no longer run (after a Boot or a
// layout change); empty plots stay.
void pruneSignals(PlotSet &, const DeviceSnapshot &);
uint16_t dividerFor(const PlotSet &, const DeviceSnapshot &, uint8_t loop);

// One subscription group per plotted loop: its refs in plot order.
struct MonitorPlan {
    uint8_t loop=0;
    uint16_t divider=1;
    std::vector<Ref> refs;
    bool operator==(const MonitorPlan &) const=default;
};
std::vector<MonitorPlan> monitorPlans(const PlotSet &, const DeviceSnapshot &);
std::vector<LinkStream> planStreams(std::span<const MonitorPlan>);

// The device's `limits.group_entries` (64 when unknown).
size_t groupEntryLimit(const DeviceSnapshot &);

// Whether a change fits: the link (at most 100 %), the free subscription
// slots (a new loop needs one), the group entry limit, kMaxPlotted and
// kMaxPlots. Refused over budget, it offers the fastest rate of that loop
// that fits. A rate change that lowers the load is always accepted, even
// while the link stays over 100 % (another client or a live loop-rate
// change can put it there).
struct MonitorCheck {
    bool ok=true;
    QString reason;
    double load=0;                    // the link after the change
    std::optional<uint16_t> divider;  // the suggested rate, when one fits
    QString suggestion;               // "Plot control at 500 Hz (53 %)"
};
struct LinkContext {
    std::vector<LinkStream> others;   // every other configured subscription group
    std::optional<size_t> freeSlots;  // unconfigured subscription slots (unknown before a list)
    size_t entryLimit=64;
};
MonitorCheck checkAdd(const DeviceSnapshot &, const PlotSet &, Ref, const LinkContext &);
MonitorCheck checkRate(const DeviceSnapshot &, const PlotSet &, uint8_t loop, uint16_t divider, const LinkContext &);

// A fixed-capacity ring of (device time µs, value), oldest first. Device
// timestamps of one series only increase.
class SampleRing {
public:
    explicit SampleRing(size_t capacity=16);
    void push(int64_t us, double value);
    void clear() { m_head=0; m_size=0; }
    size_t size() const { return m_size; }
    size_t capacity() const { return m_time.size(); }
    int64_t time(size_t i) const { return m_time[(m_head+i)%m_time.size()]; }
    double value(size_t i) const { return m_value[(m_head+i)%m_time.size()]; }
    // The first index whose time is at or after `us` (size() if none).
    size_t lowerBound(int64_t us) const;
    // Keeps the newest samples that fit.
    void setCapacity(size_t capacity);
private:
    std::vector<int64_t> m_time;
    std::vector<double> m_value;
    size_t m_head=0,m_size=0;
};
// Enough for the longest window at this rate, with a margin.
size_t ringCapacity(double hz);

// What a PlotItem draws from (Monitor's live rings, Tune's runs): a ring per
// series key and the time span shown, in µs. The QObject that implements it
// emits changed() and dataChanged().
class PlotSource {
public:
    virtual ~PlotSource()=default;
    virtual const SampleRing *ring(const QString &key) const=0;
    virtual int64_t plotStart() const=0;
    virtual int64_t plotEnd() const=0;
};

// The series over [t0, t1] as points: x in pixels (0..width), y the raw
// value. At most two points (min and max, in time order) per pixel column,
// plus the sample before t0 so the line enters from the left edge.
std::vector<QPointF> decimate(const SampleRing &, int64_t t0, int64_t t1, double width);
// Lowest and highest value over [t0, t1] (and the sample before t0).
std::optional<std::pair<double,double>> valueRange(const SampleRing &, int64_t t0, int64_t t1);
// Round ticks 1, 2 or 5 × 10^n covering [lo, hi]; a flat range is padded.
struct AxisRange {
    double min=0,max=1;
    std::vector<double> ticks;
};
AxisRange niceAxis(double lo, double hi, int maxTicks=5);
QString tickText(double value, double step);

// The value at a cursor: the last sample at or before `us` (none before the first).
std::optional<double> valueAt(const SampleRing &, int64_t us);
// "12.5 ms", "1.250 s".
QString durationText(int64_t us);
// Frames missing between two counters of one group (the counter wraps at 65536).
inline uint32_t counterGap(uint16_t previous, uint16_t now) { return uint16_t(now-previous-1); }

// The snapshot CSV over [t0, t1]: device_time_us, time_s (from t0), then one
// column per series; a row per distinct timestamp, empty where a series has
// no sample at that time.
struct CsvColumn {
    QString name;
    const SampleRing *ring=nullptr;
};
QString snapshotCsv(std::span<const CsvColumn>, int64_t t0, int64_t t1);
} // namespace drivelink::gui
