import QtQuick
import DriveLink.Style
import DriveLink.Shell
import "calibration.js" as Cal

// Live orientation: the isometric preview with the Euler angles and the
// quaternion beside it. Nothing here is sent anywhere.
Card {
    id: card
    objectName: "orientationCard"

    property var panel

    readonly property var sensor: panel.sensor
    readonly property string previewState: sensor.state === "live" ? "live" : sensor.state === "stale" ? "stale" : "unavailable"
    readonly property var euler: sensor.euler
    readonly property string age: Cal.ageText(sensor.ageUs)
    readonly property string footer: {
        if (previewState === "live")
            return [qsTr("Live"), panel.loopText, qsTr("sample age %1").arg(age)].filter(function(t) { return t.length > 0 }).join(" · ")
        if (previewState === "stale") return qsTr("Stale · last sample %1 ago").arg(age)
        return sensor.state === "waiting" ? qsTr("Waiting for the first sample") : qsTr("Unavailable · no live orientation")
    }
    readonly property string previewCaption: {
        if (previewState === "stale") return qsTr("Last pose · live updates unavailable")
        if (previewState !== "live" || euler === null) return ""
        return qsTr("Sensor axes · roll %1 · pitch %2 · yaw %3").arg(Cal.degText(euler.roll)).arg(Cal.degText(euler.pitch)).arg(Cal.degText(euler.yaw))
    }

    title: qsTr("Live orientation")

    Item {
        width: parent.width
        height: 224

        DLOrientationPreview {
            id: preview
            objectName: "orientationPreview"
            width: Math.min(400, Math.max(260, parent.width * 0.5))
            height: parent.height
            axes: Cal.axes(card.sensor.quat)
            state: card.previewState
            caption: card.previewCaption
        }
        Column {
            anchors.left: preview.right
            anchors.leftMargin: Theme.space24
            anchors.right: parent.right
            spacing: Theme.space8

            Repeater {
                model: [{ name: qsTr("Roll"), key: "roll" }, { name: qsTr("Pitch"), key: "pitch" }, { name: qsTr("Yaw"), key: "yaw" }]
                Item {
                    required property var modelData
                    width: parent.width
                    height: Theme.lineBodyDefault + Theme.space4
                    Text {
                        text: parent.modelData.name
                        font: Theme.fontBodyDefault
                        color: Theme.textSecondary
                    }
                    Text {
                        objectName: parent.modelData.key + "Value"
                        anchors.right: parent.right
                        text: Cal.degText(card.euler ? card.euler[parent.modelData.key] : null)
                        font: Theme.fontMonoDefault
                        color: card.previewState === "live" ? Theme.textPrimary : Theme.textSecondary
                    }
                }
            }
            Item { width: 1; height: Theme.space4 }
            Text {
                text: qsTr("Quaternion · w, x, y, z")
                font: Theme.fontCaption
                color: Theme.textSecondary
            }
            Grid {
                columns: 2
                columnSpacing: Theme.space24
                rowSpacing: Theme.space4
                Repeater {
                    model: ["w", "x", "y", "z"]
                    Text {
                        required property string modelData
                        objectName: "quat_" + modelData
                        text: Cal.quatText(card.sensor.quat ? card.sensor.quat[modelData] : null)
                        font: Theme.fontMonoDefault
                        color: card.previewState === "live" ? Theme.textPrimary : Theme.textSecondary
                    }
                }
            }
        }
    }
    Text {
        objectName: "orientationFooter"
        width: parent.width
        text: card.footer
        font: Theme.fontCaption
        color: Theme.textSecondary
        wrapMode: Text.Wrap
    }
    Text {
        width: parent.width
        text: qsTr("Euler angles as the BNO055 reports them: heading increases clockwise.")
        font: Theme.fontCaption
        color: Theme.textTertiary
        wrapMode: Text.Wrap
    }
}
