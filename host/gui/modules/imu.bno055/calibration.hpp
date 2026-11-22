#pragma once
// The imu.bno055 calibration panel's logic (H2f), shared by imu.mock: the F6
// action blob, the PC calibration file, the import review, the sensor state
// read from Layout's live signals, guidance, the orientation projection and
// the write → apply sequence. Pure functions, no device calls; the QML side
// is imu_calibration_qml.hpp. Tested by gui_panel_tests (calibrationLogic).
//
// Blob (docs/protocol/imu.md): 42 bytes, little-endian,
// version u8 = 1 | has_sic u8 (0/1) | 20 × i16: accel x, y, z, radius;
// gyro x, y, z; mag x, y, z, radius; SIC 0..8 (row-major, Q14).
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace drivelink::gui::imu_calibration {

inline constexpr int kBlobVersion=1;
inline constexpr int kBlobBytes=42;
inline constexpr int kFileVersion=1;
inline constexpr const char *kFileFormat="drivelink.imu-calibration";
inline constexpr const char *kFileSuffix=".imucal.json";
// The F6 action names.
inline constexpr const char *kActionRead="calibration_read";
inline constexpr const char *kActionWrite="calibration_write";
inline constexpr const char *kActionApply="calibration_apply";
// sample_age_us before any sample.
inline constexpr double kNoSample=4294967295.0;
// After the apply reply, how long to wait for new samples before saying none
// arrived (the sequence keeps watching).
inline constexpr qint64 kConfirmMs=3000;

// ---- Blob and calibration values ------------------------------------------------

inline constexpr int kValues=20;
// Blob order; the radii are 0..32767, everything else i16.
inline constexpr std::array<const char *,kValues> kValueNames{
    "accel.x","accel.y","accel.z","accel.radius","gyro.x","gyro.y","gyro.z",
    "mag.x","mag.y","mag.z","mag.radius",
    "sic[0]","sic[1]","sic[2]","sic[3]","sic[4]","sic[5]","sic[6]","sic[7]","sic[8]"};
bool isRadius(int index);

struct Calibration {
    bool has_sic=false;
    std::array<int,kValues> values{}; // in blob order (kValueNames)
    bool operator==(const Calibration &) const=default;
};
// Why the values cannot be encoded ("" when they can).
QString problem(const Calibration &);
// Hex text with spaces (what Connection.runAction parses).
QString encodeBlob(const Calibration &);
std::expected<Calibration,QString> decodeBlob(const QString &hex);

// ---- The PC file ------------------------------------------------------------------

// Calibration levels in the order sys, gyro, accel, mag; -1 = not shown.
using Levels=std::array<int,4>;
inline constexpr std::array<const char *,4> kLevelNames{"sys","gyro","accel","mag"};
inline constexpr std::array<const char *,4> kLevelTitles{"System","Gyroscope","Accelerometer","Magnetometer"};

// Where a calibration came from, captured when it was read.
struct Provenance {
    QString type, created_utc, device_id, device_name, instance, firmware, gui;
    Levels levels{-1,-1,-1,-1};
    int actual_mode=-1;
    QString note;
    bool operator==(const Provenance &) const=default;
};
// "2026-11-18T09:05:03Z".
QString utcText(const QDateTime &);
// Version-1 file text (indented JSON, decoded fields only: the blob is
// rebuilt from them, so the two can never disagree).
QString buildFile(const Calibration &, const Provenance &read, const QString &note);
// <device>-<instance>-<yyyymmdd-hhmmss>.imucal.json.
QString suggestedName(const Provenance &read);

struct Target {
    QString type, device_id, instance;
};
struct ParsedFile {
    Calibration calibration;
    Provenance source;
    QStringList warnings;
};
// Refusals (the error): not JSON, another format or version, no type,
// another module type than the target's, a missing or out-of-range value.
// Warnings: no device id, another device, another instance label, levels
// below 3 when read. Unknown keys are ignored.
std::expected<ParsedFile,QString> parseFile(const QString &text, const Target &);

// ---- Live sensor state ------------------------------------------------------------

enum class SensorKind { Unavailable, Waiting, Stale, Live };
struct Euler { double roll=0, pitch=0, yaw=0; }; // degrees
struct Quaternion { double w=1, x=0, y=0, z=0; };
struct SensorState {
    SensorKind state=SensorKind::Unavailable;
    QString note;
    Levels levels{-1,-1,-1,-1};
    std::optional<Euler> euler;
    std::optional<Quaternion> quat;
    bool valid=false;
    double sample_count=-1, age_us=-1;
    int mode=-1, available=0;
};
// From Connection.live ({note, telemetry, signals: [{name, number}]}):
// unavailable (not running, no telemetry), waiting (no sample yet), stale
// (samples stopped, or `valid` is 0: the last pose stays), live. Levels only
// while live and in a mode with calibration status (`available` bit 8).
// Euler in degrees as the device reports it (BNO055: heading clockwise).
SensorState sensorState(const QVariantMap &live);
bool fullyCalibrated(const SensorState &);
// Why Read from IMU is disabled ("" when enabled); `blocked` is the
// backend's reason for the action.
QString readBlock(const SensorState &, const QString &blocked);
QString levelText(int level);
struct GuidanceRow {
    QString name, title, text;
    int level=-1;
    bool done=false;
};
// Bosch BNO055 datasheet rev 1.8, 3.11: gyro, accel, mag, then sys.
std::vector<GuidanceRow> guidance(const SensorState &);
QString degText(std::optional<double>);
QString quatText(std::optional<double>);
QString ageText(double us);

// ---- Orientation preview ----------------------------------------------------------

// v rotated by q (normalised); nullopt for a degenerate quaternion.
std::optional<std::array<double,3>> rotate(const Quaternion &, const std::array<double,3> &);
struct Axis {
    QString axis;
    double x=0, y=0, depth=0; // screen x right, y down, within [-1, 1]
};
// The sensor's X, Y and Z axes in a fixed isometric view, farthest first;
// empty for no or a degenerate quaternion.
std::vector<Axis> axes(const std::optional<Quaternion> &);

// ---- One action step and the write → apply sequence ---------------------------------

enum class StepState { Pending, Running, Ok, Rebooted, Refused, Uncertain };
struct Outcome {
    StepState state=StepState::Pending;
    QString text, blob_hex;
};
// A step's outcome from Connection.actionResults / actionRunning.
Outcome stepOutcome(const QString &key, const QVariantMap &results, const QString &running, bool linked);
// Right after Connection.runAction(key): sent (running), refused locally, or
// not sent at all (an earlier outcome for the key is not this one's).
Outcome sentOutcome(const QString &key, const QVariantMap &results, const QString &running);

// write and apply wait for their replies, confirming waits for new samples;
// the rest are ends (noSample still watches).
enum class Phase { Write, Apply, Confirming, Applied, NoSample, Failed, Uncertain, Ended, Abandoned };
struct Step {
    QString title;
    StepState state=StepState::Pending;
    QString text;
    bool operator==(const Step &) const=default;
};
struct Sequence {
    QString node_key, write_key, apply_key, blob_hex, label;
    Phase phase=Phase::Write;
    QString note;
    std::array<Step,2> steps;
    double confirm_count=-1;
    int confirm_seen=0;
    qint64 confirm_since=0;
    bool operator==(const Sequence &) const=default;
};
// Nothing is sent by this.
Sequence newApply(const QString &nodeKey, const QString &writeKey, const QString &applyKey,
                  const QString &blobHex, const QString &label);
bool busy(const Sequence &);
// After the panel called runAction for the current step.
Sequence markSent(Sequence, const Outcome &);
struct View {
    QVariantMap results;
    QString running;
    bool linked=true;
    QString node_key;
    SensorState sensor;
    qint64 now=0;
};
struct Advance {
    Sequence seq;
    bool send_apply=false; // the panel must now send calibration_apply, then markSent
};
// One observation of the backend. "Applied" needs the apply reply and then
// two new sample counts while live: a sample published before the action
// paused the loop can still arrive after the reply, but not two.
Advance advance(Sequence, const View &);

QString stateName(StepState);
StepState stepStateFrom(const QString &);
QString phaseName(Phase);
Phase phaseFrom(const QString &);

} // namespace drivelink::gui::imu_calibration
