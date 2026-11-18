#pragma once
// The Tune session (H2e-2, G5 Tune), pure: the gain history, the A/B
// comparison, session files and the CSV of a run. Nothing here touches a
// device, a file or Qt Quick.
//
// Gain history (user decision 2026-11-18): an entry when the controller's
// gain writes have been quiet for 0.5 s and the set differs from its last
// entry (Restore and Revert make one too); the first is "loaded from device".
// A run joins the entry of the gains it ran with. Entries and runs carry the
// controller's type name and label and each gain's name, so a set recorded on
// another device is matched by those names, never by item ids alone.
//
// Session files: versioned JSON (`format`, `version`), keyed by the device id,
// with base64 columns (little-endian i64 µs and f64 values) for the series.
#include "tune_model.hpp"
#include <QByteArray>
#include <expected>
#include <map>
#include <optional>
#include <vector>

namespace drivelink::gui {

enum class HistorySource : uint8_t { Loaded=0, Changed=1, Restored=2, Reverted=3 };
struct HistoryEntry {
    int id=0;
    QString time;                        // hh:mm:ss
    InstanceKey controller{};            // on the device it was recorded on
    QString controllerType, controllerLabel;
    std::map<uint16_t,double> gains;
    std::map<uint16_t,QString> names;
    HistorySource source=HistorySource::Changed;
    QString change;                      // "vel.kp 0.6 → 0.85" against the entry before
    QString note;                        // "restored 18:41:30", "saved values"
    std::vector<int> runs;               // run numbers made with this set
    QString device;                      // the device id it was recorded on
    bool foreign=false;                  // recorded on another device than the connected one
};
// Per controller.
inline constexpr size_t kMaxHistory=200;

// Equal as the device stores them (f32).
bool sameGains(const std::map<uint16_t,double> &, const std::map<uint16_t,double> &);
// "vel.kp 0.6 → 0.85, vel.ki 4.0 → 5.0" for the gains that differ.
QString gainChanges(const std::map<uint16_t,QString> &names, const std::map<uint16_t,double> &from,
                    const std::map<uint16_t,double> &to);
// "vel.ki, vel.kd equal" for the gains that do not differ ("" when none).
QString equalGains(const std::map<uint16_t,QString> &names, const std::map<uint16_t,double> &a,
                   const std::map<uint16_t,double> &b);
bool sameController(const QString &typeA, const QString &labelA, const QString &typeB, const QString &labelB);
// The index of the controller's last entry recorded on `device` (-1): a set
// recorded here never joins another device's entry.
int lastEntry(const std::vector<HistoryEntry> &, const QString &type, const QString &label, const QString &device);
// Appends the entry unless its gains equal the controller's last entry of
// its device (then nothing changes); drops that controller's oldest entries
// beyond kMaxHistory. Returns the index of the entry that holds the set.
int recordEntry(std::vector<HistoryEntry> &, HistoryEntry);
// The newest run number of an entry that is still in `runs` (none).
std::optional<int> entryRun(const HistoryEntry &, const std::vector<RunRecord> &);

// A recorded set on the connected device: the running controller with the
// same type name and label, each gain found by name among its tune-flagged
// live numbers, a value its type holds (finite f32, whole integers) inside its
// range. The error says why not.
struct ResolvedGains {
    InstanceKey key{};
    std::vector<std::pair<uint16_t,double>> values; // item id, value
};
std::expected<ResolvedGains,QString> resolveGains(const DeviceSnapshot &, const QString &type, const QString &label,
                                                  const std::map<uint16_t,double> &gains,
                                                  const std::map<uint16_t,QString> &names);
// The tune-flagged gain names of an instance (item id -> name).
std::map<uint16_t,QString> gainNamesOf(const DeviceSnapshot &, InstanceKey);

// A/B: the step metrics side by side, with B − A.
struct MetricComparison { QString name, a, b, change; };
std::vector<MetricComparison> compareMetrics(const RunRecord &a, const RunRecord &b);

// A typed slider window, kept per controller and gain name.
struct SessionWindow { QString controllerType, controllerLabel, gain; double lo=0, hi=0; };
// What the next run uses, kept with the session; applied on open only on the
// device it was recorded on.
struct SessionSetup {
    InstanceKey controller{};
    TestSignal signal;
    CaptureSetup capture;
};
struct SessionData {
    // The device the session belongs to; each run and entry also names the
    // device it was recorded on (the session's when the file omits it).
    QString deviceId, deviceName, name, savedAt;
    std::vector<RunRecord> runs;          // oldest first; metrics are recomputed on reading
    std::vector<HistoryEntry> history;    // oldest first
    std::vector<SessionWindow> windows;
    std::optional<SessionSetup> setup;
    int nextNumber=1, nextColour=0, nextHistory=1;
};
inline constexpr int kSessionVersion=1;
inline constexpr const char *kSessionFormat="drivelink-tune-session";
// At most this many samples per series in a file (a 4 MiB capture of one
// signal is 1 Mi samples).
inline constexpr size_t kMaxSessionSamples=size_t(1)<<22;
// At most this many samples in all series of a session (about 180 MB as a
// file): Save and autosave refuse more, reading refuses more.
inline constexpr size_t kMaxSessionTotalSamples=size_t(1)<<23;
size_t sessionSamples(const std::vector<RunRecord> &);
QByteArray writeSession(const SessionData &);
// Refuses another format, another version, and anything malformed: missing
// or mistyped fields, columns of unequal or odd length, times that go back,
// out-of-range enums, refs that do not match the names and series.
std::expected<SessionData,QString> readSession(const QByteArray &);

// One run as CSV: '#' lines with the run, the controller, the test signal
// and the gains, then `time_s` (from the trigger) and a column per captured
// signal, "name (unit)".
QString runCsv(const RunRecord &);

// File names: the suffix added when the name has none.
QString sessionFilePath(QString path);
QString csvFilePath(QString path);
} // namespace drivelink::gui
