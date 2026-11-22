import QtQuick
import DriveLink.Style
import DriveLink.Modules.ImuBno055

// The four calibration levels across the panel (System, Gyroscope,
// Accelerometer, Magnetometer), from the live signals.
Row {
    id: row
    objectName: "levelsRow"

    property var panel

    readonly property var objectNames: ({ sys: "levelSys", gyro: "levelGyro", accel: "levelAccel", mag: "levelMag" })

    spacing: Theme.space16

    Repeater {
        model: ImuCalibration.levels
        DLCalibrationLevel {
            required property string modelData
            objectName: row.objectNames[modelData]
            width: (row.width - 3 * row.spacing) / 4
            title: ImuCalibration.levelTitles[modelData]
            level: row.panel ? row.panel.sensor.levels[modelData] : -1
            caption: ImuCalibration.levelText(level)
        }
    }
}
