import QtQuick
import DriveLink.Style

// The Apply review (G7, "Confirm apply"): the target, the file and its
// warnings, then what applying does. Accepting calls the panel's
// confirmApply(); closing without accepting sends nothing.
DLDialog {
    id: dialog
    objectName: "applyDialog"

    property var panel

    readonly property var target: panel ? panel.target : ({})
    readonly property var imported: panel && panel.imported ? panel.imported : null
    readonly property var source: imported ? imported.source : null
    function joined(parts) { return parts.filter(function(t) { return t && t.length > 0 }).join(" · ") }

    component Line: Text {
        property string label
        property string value
        width: parent.width
        text: label + ": " + value
        font: Theme.fontBodyDefault
        color: Theme.textPrimary
        wrapMode: Text.Wrap
    }

    width: 560
    title: qsTr("Apply calibration to the IMU?")
    acceptText: qsTr("Apply to IMU")
    onAccepted: panel.confirmApply()

    extra: Column {
        width: parent.width
        spacing: Theme.space12

        Column {
            width: parent.width
            spacing: Theme.space4
            Line {
                objectName: "applyTargetDevice"
                label: qsTr("Target device")
                value: dialog.joined([dialog.target.deviceName, dialog.target.deviceId])
            }
            Line {
                objectName: "applyTargetInstance"
                label: qsTr("Instance")
                value: dialog.joined([dialog.target.instance, dialog.target.type])
            }
            Line {
                objectName: "applyFile"
                label: qsTr("File")
                value: dialog.imported ? dialog.imported.fileName : ""
            }
            Text {
                objectName: "applySource"
                width: parent.width
                visible: dialog.source !== null
                text: dialog.source ? qsTr("Read from %1 at %2").arg(dialog.joined([dialog.source.deviceName, dialog.source.instance, dialog.source.deviceId])).arg(dialog.source.createdUtc) : ""
                font: Theme.fontCaption
                color: Theme.textSecondary
                wrapMode: Text.Wrap
            }
        }
        Column {
            width: parent.width
            spacing: Theme.space4
            visible: dialog.imported !== null && dialog.imported.warnings.length > 0
            Repeater {
                model: dialog.imported ? dialog.imported.warnings : []
                Text {
                    required property string modelData
                    objectName: "applyWarning"
                    width: parent.width
                    text: modelData
                    font: Theme.fontBodyDefault
                    color: Theme.stateUnsaved
                    wrapMode: Text.Wrap
                }
            }
        }
        Text {
            width: parent.width
            text: qsTr("The profile must match this physical sensor and its mounting.")
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            text: qsTr("Writing pauses the sensor loop: its signals and any consumer see stale data for a moment. The calibration is staged on the device (Unsaved); Save keeps it after a reboot.")
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }
    }
}
