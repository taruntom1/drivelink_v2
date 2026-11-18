import QtQuick
import DriveLink.Style
import "calibration.js" as Cal

// The banner across the top of the calibration panel: what the live data is
// doing, or that a local file is loaded and not applied. Informational only.
// DLBanner has no warning tone (amber means Armed), so every state is Info,
// as in the Figma frames; the icon says warning or done.
DLBanner {
    id: banner
    objectName: "statusBanner"

    property var panel

    readonly property string sensorState: panel ? panel.sensor.state : "unavailable"
    // An Apply sequence is the only operation with steps; its success ends
    // "not applied".
    readonly property bool applied: !!panel && panel.operation !== null && panel.operation.state === "ok"
                                    && panel.operation.steps.length > 0
    readonly property bool pendingFile: !!panel && panel.imported !== null && !applied
    readonly property bool calibrated: !!panel && sensorState === "live" && Cal.fullyCalibrated(panel.sensor)

    // [title, message, icon]
    readonly property var content: {
        if (!panel) return ["", "", "info"]
        if (sensorState === "unavailable")
            return [qsTr("IMU data unavailable"), panel.sensor.note || qsTr("The IMU is not running or sends no telemetry."), "triangle-alert"]
        if (sensorState === "waiting")
            return [qsTr("Waiting for the first sample"), qsTr("Levels and orientation appear with the first valid sample."), "info"]
        if (sensorState === "stale")
            return [qsTr("Stale data: the last pose is frozen"), qsTr("Samples stopped or the sensor reports invalid data. Device operations need live data."), "triangle-alert"]
        if (pendingFile)
            return [qsTr("File loaded locally · not applied"), qsTr("Importing has sent nothing to the device."), "info"]
        if (calibrated)
            return [qsTr("Fully calibrated"), qsTr("All four levels are 3 / 3. Calibration does not indicate whether the robot is safe to arm."), "circle-check"]
        return [qsTr("Calibration in progress"), qsTr("Live levels come from the IMU. Calibration progress does not indicate whether the robot is safe to arm."), "info"]
    }

    tone: DLBanner.Info
    title: content[0]
    message: content[1]
    iconName: content[2]
}
