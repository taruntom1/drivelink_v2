#pragma once
// The QML singleton `ImuCalibration` (import DriveLink.Modules.ImuBno055): the
// panel's view of calibration.hpp. Values cross as plain maps with the field
// names the panel's QML reads; every rule is in calibration.cpp. One instance
// per engine, which also keeps the panel's per-instance memory (imported file,
// last read, last operation) while the GUI runs, across panel reloads and
// reconnects.
#include "calibration.hpp"
#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace drivelink::gui::imu_calibration {

// Map shapes (see CalibrationPanel.qml): calibration {hasSic, accel {x, y, z,
// radius}, gyro {x, y, z}, mag {x, y, z, radius}, sic [9]}; provenance {type,
// createdUtc, deviceId, deviceName, instance, firmware, gui, levels {sys,
// gyro, accel, mag}, actualMode, note}; sensor {state, note, levels, euler
// {roll, pitch, yaw} | null, quat {w, x, y, z} | null, valid, sampleCount,
// ageUs, mode, available}; sequence {nodeKey, writeKey, applyKey, blobHex,
// label, phase, note, steps [{title, state, text}], confirmCount,
// confirmSeen, confirmSince}.
QVariantMap toVariant(const Calibration &);
Calibration calibrationFrom(const QVariantMap &);
QVariantMap toVariant(const Provenance &);
Provenance provenanceFrom(const QVariantMap &);
QVariantMap toVariant(const SensorState &);
SensorState sensorFrom(const QVariantMap &);
QVariantMap toVariant(const Sequence &);
Sequence sequenceFrom(const QVariantMap &);
QVariantMap toVariant(const Outcome &);
Outcome outcomeFrom(const QVariantMap &);

class ImuCalibration final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString actionRead READ actionRead CONSTANT)
    Q_PROPERTY(QString actionWrite READ actionWrite CONSTANT)
    Q_PROPERTY(QString actionApply READ actionApply CONSTANT)
    Q_PROPERTY(QString fileSuffix READ fileSuffix CONSTANT)
    // ["sys", "gyro", "accel", "mag"] and their titles.
    Q_PROPERTY(QStringList levels READ levels CONSTANT)
    Q_PROPERTY(QVariantMap levelTitles READ levelTitles CONSTANT)
public:
    using QObject::QObject;
    QString actionRead() const {return QString::fromLatin1(kActionRead);}
    QString actionWrite() const {return QString::fromLatin1(kActionWrite);}
    QString actionApply() const {return QString::fromLatin1(kActionApply);}
    QString fileSuffix() const {return QString::fromLatin1(kFileSuffix);}
    QStringList levels() const;
    QVariantMap levelTitles() const;

    // The blob as hex text for runAction; {ok, cal | error}.
    Q_INVOKABLE QString encodeBlob(const QVariantMap &cal) const;
    Q_INVOKABLE QVariantMap decodeBlob(const QString &hex) const;
    // The file text, a suggested file name, the UTC time text.
    Q_INVOKABLE QString buildFile(const QVariantMap &cal,const QVariantMap &read,const QString &note) const;
    Q_INVOKABLE QString suggestedName(const QVariantMap &read) const;
    Q_INVOKABLE QString nowUtcText() const;
    // target {type, deviceId, instance}: {ok, cal, source, warnings | error}.
    Q_INVOKABLE QVariantMap parseFile(const QString &text,const QVariantMap &target) const;

    // A null or undefined argument (before the backend is set) reads as empty.
    Q_INVOKABLE QVariantMap sensorState(const QVariant &live) const;
    Q_INVOKABLE bool fullyCalibrated(const QVariant &sensor) const;
    Q_INVOKABLE QString readBlock(const QVariant &sensor,const QString &blocked) const;
    Q_INVOKABLE QString levelText(int level) const;
    // [{name, title, level, done, text}].
    Q_INVOKABLE QVariantList guidance(const QVariant &sensor) const;
    // "2.4°" / "0.7312" / "8 ms", "—" for none.
    Q_INVOKABLE QString degText(const QVariant &value) const;
    Q_INVOKABLE QString quatText(const QVariant &value) const;
    Q_INVOKABLE QString ageText(double us) const;
    // [{axis, x, y, depth}], farthest first; [] for no quaternion.
    Q_INVOKABLE QVariantList axes(const QVariant &quat) const;

    // {state: running | ok | rebooted | refused | uncertain, text, blobHex}.
    Q_INVOKABLE QVariantMap stepOutcome(const QString &key,const QVariantMap &results,const QString &running,bool linked) const;
    Q_INVOKABLE QVariantMap sentOutcome(const QString &key,const QVariantMap &results,const QString &running) const;
    Q_INVOKABLE QVariantMap newApply(const QString &nodeKey,const QString &writeKey,const QString &applyKey,
                                     const QString &blobHex,const QString &label) const;
    // false for null.
    Q_INVOKABLE bool busy(const QVariant &sequence) const;
    Q_INVOKABLE QVariantMap markSent(const QVariantMap &sequence,const QVariantMap &outcome) const;
    // view {results, running, linked, nodeKey, sensor, now}: {seq, send: "" | "apply"}.
    Q_INVOKABLE QVariantMap advance(const QVariantMap &sequence,const QVariantMap &view) const;

    // The per-instance memory: copies in and out.
    Q_INVOKABLE QString storeKey(const QString &deviceId,const QString &nodeKey) const {return deviceId+QLatin1Char('|')+nodeKey;}
    Q_INVOKABLE QVariantMap recall(const QString &key) const {return m_store.value(key);}
    // A null or undefined value forgets the field.
    Q_INVOKABLE void remember(const QString &key,const QString &field,const QVariant &value);
    Q_INVOKABLE void forgetAll() {m_store.clear();}
private:
    QHash<QString,QVariantMap> m_store;
};

} // namespace drivelink::gui::imu_calibration
