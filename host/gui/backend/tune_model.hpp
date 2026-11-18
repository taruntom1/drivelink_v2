#pragma once
// Tune's rules (H2e-1, G5 Tune), pure: which instances can be tuned, the gains
// card (the `Tune` descriptor flag), the log gain slider and % steps, the test
// signal and capture checks, and the run records the plot and tables show.
// Nothing here touches a device or Qt Quick.
//
// Gains (user decision 2026-11-18): the definitions mark the params Tune shows
// with the `Tune` item flag (core v10). Tune-flagged enums are combos above
// the gains; tune-flagged numbers are grouped by the text before their first
// dot. A group whose prefix starts the selected name of a tune-flagged enum
// value (mode `velocity` -> `vel.*`) comes first and is open; a group that no
// value name starts is open too; the others are collapsed (still editable).
#include "monitor_model.hpp"
#include <drivelink/model.hpp>
#include <drivelink/tuning.hpp>
#include <QString>
#include <QStringList>
#include <map>
#include <optional>
#include <vector>

namespace drivelink::gui {

bool tuneFlagged(const ItemDescriptor &);
bool testgenFlagged(const ItemDescriptor &);

struct TuneTarget { Ref ref; ItemDescriptor descriptor; };
// A running instance with at least one running f32 testgen input; loop
// instances and pending ones are not.
struct TuneController {
    InstanceKey key{};
    QString label, typeName;
    uint8_t loop=0;
    std::vector<TuneTarget> targets;
};
std::vector<TuneController> tuneControllers(const DeviceSnapshot &);

struct GainGroup {
    QString prefix;      // "" for undotted names
    bool open=true;
    bool selected=false; // matches the selected enum value
    std::vector<uint16_t> items;
};
struct GainLayout {
    std::vector<uint16_t> enums;
    std::vector<GainGroup> groups; // selected first, then open, then collapsed; items by id
};
GainLayout gainLayout(const DeviceSnapshot &, InstanceKey);

// The log slider's window: ÷4..×4 around |value| at the start of a drag,
// within the descriptor's range on the value's side of 0. None for a value of
// 0 (type one first) or a range with no room.
struct GainWindow { double lo=0, hi=0; };
std::optional<GainWindow> gainWindow(double value, double min, double max);
// A typed window (G5: min and max can be typed): clamped to the same range,
// none when it is empty or crosses 0.
std::optional<GainWindow> typedWindow(double lo, double hi, double min, double max, double value);
double logPosition(double magnitude, GainWindow);
double logMagnitude(double position, GainWindow);
// Three significant digits: the values the slider sends.
double roundGain(double value);
// value × factor (÷2 is 0.5, −10 % 0.9, +10 % 1.1, ×2 2.0, a key 1.01 ...),
// keeping the sign, rounded, then clamped to [min, max]; 0 stays 0.
double stepGain(double value, double factor, double min, double max);
// The text of a gain: up to three significant digits, at least one decimal.
QString gainText(double value);

// The test signal (docs/protocol/core.md "Test generator").
enum class Waveform : uint8_t { Step=0, Square=1, Ramp=2, Sine=3, ChirpLinear=4, ChirpLog=5 };
QString waveformName(Waveform);
bool waveformUsesFrequency(Waveform);
bool waveformIsChirp(Waveform);
struct TestSignal {
    Ref target{};
    Waveform waveform=Waveform::Step;
    double offset=0, amplitude=0, f0=0, f1=0;
    double duration_s=1;
    uint32_t repeat=1; // Tune runs end: at least once, never "until stopped"
};
// Empty when the device would accept it and every output stays inside the
// target's descriptor range (the device would clamp silently).
QString testSignalProblem(const TestSignal &, double loopRateHz, const ItemDescriptor &target);
TestgenConfig testgenConfig(const TestSignal &, uint8_t loop);
// The run's length in seconds (duration × repeat).
inline double signalSeconds(const TestSignal &s) { return s.duration_s*double(std::max<uint32_t>(1,s.repeat)); }

struct CaptureSetup {
    std::vector<Ref> refs;
    uint16_t divider=1;
    uint32_t samples=0, pre=0;
};
inline uint64_t captureBytes(const CaptureSetup &c) { return uint64_t(c.samples)*c.refs.size()*4; }
// Empty when CONFIG would be accepted: 1..maxRefs unique signals or inputs
// of running instances in `loop`, pre < samples, the ring within capacity.
QString captureProblem(const DeviceSnapshot &, const CaptureSetup &, uint8_t loop, uint32_t capacity, uint32_t maxRefs);
// The target, then the controller's first signal in the target's unit; the
// signal's whole length at the loop rate after the trigger (≈6 % before it),
// the divider raised until the ring fits the capacity.
CaptureSetup defaultCapture(const DeviceSnapshot &, const TuneController &, const TuneTarget &, double loopRateHz,
                            double seconds, uint32_t capacity);
// The samples after the trigger that cover `seconds` at this divider, plus pre.
CaptureSetup resizeCapture(CaptureSetup, double loopRateHz, double seconds);
// "18 KB of 4 MB".
QString bytesText(uint64_t bytes);

// One finished run: the capture as series of (µs from the trigger, value),
// what produced it, and how it ended.
enum class RunEnd : uint8_t { Completed, Stopped, Disarmed };
struct RunRecord {
    int number=0, colour=0;
    QString time; // hh:mm:ss when it started
    InstanceKey controller{};
    TestSignal signal;
    CaptureSetup capture;
    std::vector<QString> names, units; // per ref
    std::vector<SampleRing> series;    // per ref
    int64_t startUs=0, endUs=0;        // the capture span around the trigger
    std::map<uint16_t,double> gains;   // the tune-flagged numbers at the start
    // Portable names (H2e-2 session files): the controller's type name and
    // label, and each gain's name, so a set can be matched on another device.
    QString controllerType, controllerLabel;
    std::map<uint16_t,QString> gainNames;
    QString device;                    // the device id it was recorded on
    bool foreign=false;                // recorded on another device than the connected one
    QString changed;                   // gains that differ from the previous run
    RunEnd end=RunEnd::Completed;
    std::optional<int64_t> endedUs;    // Stop or disarm, µs from the trigger
    bool irregular=false, pinned=false;
    int response=-1;                   // index into refs
    StepMetrics metrics;
};
// Builds the record from a Done capture. Throws nothing; a capture without
// a trigger gives an empty series.
RunRecord makeRun(const DeviceSnapshot &, const CaptureData &, const CaptureSetup &, const TestSignal &);
// The first captured ref other than the target in the target's unit (-1).
int defaultResponse(const RunRecord &);
// Step runs only: from the target's last pre-trigger value (else 0) to
// offset + amplitude, over the signal (to the Stop or disarm when earlier).
StepMetrics runMetrics(const RunRecord &);
// "vel.kp 0.85, vel.ki 4.0" for gains that differ from `previous` (all of
// them for the first run).
QString changedGains(const DeviceSnapshot &, InstanceKey, const std::map<uint16_t,double> &now,
                     const std::map<uint16_t,double> *previous);

// What a run uses: the controller, the test signal, the capture and the gains
// at its start (Tune's step runs and plant identification).
struct RunSetup {
    TuneController controller;
    TestSignal signal;
    CaptureSetup capture;
    std::map<uint16_t,double> gains;
};

// The overlay: pinned runs, plus the newest in full and the two before it faded.
struct OverlayEntry { size_t index=0; bool faded=false; };
std::vector<OverlayEntry> overlay(const std::vector<RunRecord> &);
inline constexpr size_t kMaxRuns=50;
// Drops the oldest unpinned runs beyond kMaxRuns (all pinned: the oldest).
void trimRuns(std::vector<RunRecord> &);
QString metricText(const std::optional<double> &, const QString &unit, double scale=1, int decimals=0);
} // namespace drivelink::gui
