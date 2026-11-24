# imu.bno055 calibration panel

The GUI's custom **Calibration** tab for `imu.bno055`, shown next to Params on Layout. `imu.mock` loads the same panel through [`../imu.mock/Panel.qml`](../imu.mock/Panel.qml) because it has the same calibration contract. Layout finds a panel by the device's type name (`DriveLink/Modules/<type name>/Panel.qml`, compiled in by [`../CMakeLists.txt`](../CMakeLists.txt)). The panel uses only the generic backend surface; see the [GUI rules](../../CLAUDE.md) for custom panels. The blob, the three calibration actions and persistence are defined in [Calibration actions and persistence](../../../../docs/protocol/imu.md#calibration-actions-and-persistence).

| File | Role |
|---|---|
| `calibration.hpp/.cpp` | Typed logic (Qt Core): the blob, the PC file, the import review, sensor state from live signals, guidance, the orientation projection and the write → apply sequence |
| `calibration_qml.hpp/.cpp` | QML singleton `ImuCalibration` (`import DriveLink.Modules.ImuBno055`): converts maps at the boundary and keeps the per-instance memory, one per engine. Registered by [`../register.cpp`](../register.cpp) (`calibrationQml`) |
| `CalibrationPanel.qml` | Owns state and every device call through the backend; its header is the cards' contract |
| `ApplyDialog.qml` | The Apply review; accepting it calls `CalibrationPanel.confirmApply()` |
| `StatusBanner`, `LevelsRow`, `OrientationCard`, `GuidanceCard`, `FileCard`, `OperationCard` | Drawing only |

CTest `gui-panels` (`gui_panel_tests` in [`../../tests/test_panels.cpp`](../../tests/test_panels.cpp)) covers the logic (`calibrationBlob`, `calibrationFileFormat`, `calibrationSensor`, `calibrationAxes`, `calibrationSequence`), the singleton (`calibrationQml`) and the panel against vdev (`calibrationPanel`).

## Behavior

- **Live.** Layout's live group (about 10 Hz) supplies the four levels, roll/pitch/yaw in degrees as the device reports them (BNO055 Windows convention: heading increases clockwise), the quaternion, and the sensor's X/Y/Z axes rotated by the quaternion in a fixed isometric view. States: live; waiting (no sample yet: no pose, dashes); stale (samples stopped or `valid` is 0: the last pose frozen and marked, levels Unavailable); unavailable (not running, no telemetry). Levels show only in a mode with calibration status (`available` bit 8).
- **Read from IMU** (`calibration_read`) needs live data and all four levels at 3. It also stages the profile on the device, which then shows Unsaved. **Export** writes the last read, with the provenance captured when it was read, to a file on this PC.
- **Import** validates a file locally and sends nothing.
- **Apply** opens a review: target device and instance, the file and its origin, warnings, the mounting caution and the loop pause. It then sends `calibration_write` with the blob and, only after an ok reply, `calibration_apply`. Each is sent once and never retried, and each step's outcome is kept. A refused or unanswered write never sends apply. "Applied" needs the apply reply and then two new valid sample counts, because a sample published before the action paused the loop can still arrive after the reply. After 3 s (`kConfirmMs`) without them the panel says so and keeps watching. Save keeps the calibration across a reboot. Arming does not gate the panel; the generic paused-action rule does, and the IMU runs in a non-real-time loop.
- **Cancel and reconnect.** Cancel exists only before sending. Selecting another instance, a link loss or anything else that unloads the panel mid-operation records the operation as interrupted ("may have run; nothing further was sent"). The imported file, the last read and the last operation are kept per device id and instance while the GUI runs, so they survive reselection and reconnects; nothing is ever re-sent from them.
- **Files** use the panel's own non-modal native dialogs, allowed while armed; DISARM stays clickable. The backend refuses files over 64 KiB (`kPanelFileBytes` in [`connection.hpp`](../../backend/connection.hpp)).

## Calibration file (version 1)

UTF-8 JSON, suggested name `<device>-<instance>-<yyyymmdd-hhmmss>.imucal.json`:

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

- The file holds the blob's decoded fields with the blob's units and ranges; the blob is rebuilt from them, so the two never disagree.
- `source`, `levels` (at read time), `actual_mode` and `created_utc` are provenance. `note` is the operator's text, such as mounting or robot.
- The file holds no orientation, so no Euler convention applies to it.
- **Refused on import**, so nothing can be applied: not JSON, another `format` or `version`, a missing or out-of-range value, `sic` not nine values, or `type` not the target's type name. Unknown keys are ignored.
- **Warnings** in the review, with Apply still possible: another device id (offsets belong to one physical sensor and its mounting), another instance label, levels below 3 when read, or no device id.
- A format change takes a new `version` number; this GUI reads version 1 only.
