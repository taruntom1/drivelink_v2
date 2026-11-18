# imu.bno055 calibration panel

The GUI's custom panel for `imu.bno055` (H2f, Figma G7): the **Calibration**
tab next to Params on Layout. `imu.mock` loads the same panel
(`../imu.mock/Panel.qml`), since it has the same F6 calibration contract
([docs/protocol/imu.md](../../../../docs/protocol/imu.md)). Layout finds a
panel by the type name the device reports
(`DriveLink/Modules/<type name>/Panel.qml`, compiled in by
`../CMakeLists.txt`); module-specific GUI code lives only under `modules/`.

| File | What |
|---|---|
| `calibration.hpp/.cpp` | Typed C++ logic (Qt Core): the 42-byte blob, the PC file, the import review, the sensor state from the live signals, guidance, the orientation projection, the write → apply sequence. Tested by `gui_panel_tests` (`calibrationBlob`, `calibrationFileFormat`, `calibrationSensor`, `calibrationAxes`, `calibrationSequence`) |
| `calibration_qml.hpp/.cpp` | The QML singleton `ImuCalibration` (`import DriveLink.Modules.ImuBno055`): converts maps at the boundary and keeps the per-instance memory (one per engine). Registered by `../register.cpp` (`calibrationQml`) |
| `CalibrationPanel.qml` | State and every device call through the backend; its header is the cards' contract |
| `StatusBanner`, `LevelsRow`, `OrientationCard`, `GuidanceCard`, `FileCard`, `OperationCard`, `ApplyDialog` | Drawing only |

## What it does

- **Live.** The four levels (System, Gyroscope, Accelerometer, Magnetometer),
  roll/pitch/yaw in degrees as the device reports them (BNO055 Windows
  convention: heading increases clockwise), the quaternion, and a preview of
  the sensor's X/Y/Z axes rotated by the quaternion in a fixed isometric
  view. The data comes from Layout's live group (about 10 Hz). States: live;
  waiting (no sample yet: no pose, dashes); stale (samples stopped or `valid`
  is 0: the last pose frozen and marked, levels Unavailable); unavailable
  (not running, no telemetry). Levels are shown only in a mode with
  calibration status (`available` bit 8).
- **Read from IMU** (`calibration_read`) needs live data and all four levels
  at 3 (datasheet 3.11). It also stages the profile on the device (Unsaved).
  **Export** writes the last read, with the provenance captured when it was
  read, to a file on this PC.
- **Import** validates a file locally and sends nothing.
- **Apply** opens a review (target device and instance, the file and where
  it came from, warnings, the mounting caution, the loop pause), then sends
  `calibration_write` with the blob and, only after its reply,
  `calibration_apply`. Each is sent once, never retried; each step's outcome
  is kept. A refused or unanswered write never sends apply. "Applied" needs
  the apply reply and then two new valid sample counts (a sample published
  before the action paused the loop can still arrive after the reply); after
  3 s without them it says so and keeps watching. Save keeps the calibration
  after a reboot. Arm state does not gate it; the generic action rules do (a
  paused action on a real-time loop is blocked while armed; the IMU runs in
  a non-real-time loop).
- **Cancel and reconnect.** Cancel exists only before sending. Selecting
  another instance, a link loss or anything else that unloads the panel
  mid-operation records it as interrupted ("may have run; nothing further
  was sent"). The imported file, the last read and the last operation are
  kept per device id and instance while the GUI runs, so they survive
  reselection and reconnects; nothing is ever re-sent from them.
- Files use the panel's own non-modal native dialogs (allowed while armed;
  DISARM stays clickable).

## Calibration file (version 1)

UTF-8 JSON, suggested name `<device>-<instance>-<yyyymmdd-hhmmss>.imucal.json`,
at most 64 KiB:

```json
{
  "format": "drivelink.imu-calibration",
  "version": 1,
  "type": "imu.bno055",
  "created_utc": "2026-11-18T09:05:03Z",
  "source": {"device_id": "1cdbd446ff38", "device_name": "bench-rig", "instance": "imu",
             "firmware": "2.0.0", "gui": "drivelink-gui"},
  "levels": {"sys": 3, "gyro": 3, "accel": 3, "mag": 3},
  "actual_mode": 12,
  "has_sic": true,
  "accel": {"x": 12, "y": -7, "z": 25, "radius": 1000},
  "gyro": {"x": -2, "y": 1, "z": 0},
  "mag": {"x": 140, "y": -60, "z": 310, "radius": 680},
  "sic": [16384, 0, 0, 0, 16384, 0, 0, 0, 16384],
  "note": "left bracket, cable down"
}
```

- Values are the blob's: offsets and SIC are i16 sensor units (SIC in Q14,
  row-major), radii 0–32767. The file holds the decoded fields only; the
  blob is rebuilt from them, so the two can never disagree.
- `source`, `levels` (at read time), `actual_mode` and `created_utc` are
  provenance. `note` is the operator's text (mounting, robot).
- The file holds no orientation, so no Euler convention applies to it.
- **Refused on import** (nothing can be applied): not JSON, another `format`
  or `version`, a missing or out-of-range value, `sic` not nine values, or
  `type` not the target's type name. Unknown keys are ignored.
- **Warnings** (shown in the review; Apply stays possible): another device id
  (offsets belong to one physical sensor and its mounting), another instance
  label, levels below 3 when read, no device id.
- A new version is a new `version` number; this GUI reads version 1 only.
