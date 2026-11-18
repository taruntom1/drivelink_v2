import QtQuick
import DriveLink.Style
import DriveLink.Shell
import "calibration.js" as Cal

// Calibration guidance: what each sensor needs, with a Calibrated tag once
// its level is 3.
Card {
    id: card
    objectName: "guidanceCard"

    property var panel

    readonly property var rows: Cal.guidance(panel.sensor)

    title: qsTr("Calibration guidance")

    Text {
        width: parent.width
        text: qsTr("Levels 0–2: calibration is incomplete. Level 3: calibrated. Values may change as the sensor observes its surroundings.")
        font: Theme.fontBodyDefault
        color: Theme.textSecondary
        wrapMode: Text.Wrap
    }
    Column {
        width: parent.width
        spacing: Theme.space12
        Repeater {
            model: card.rows.length
            Item {
                id: guide
                required property int index
                readonly property var row: card.rows[index]
                width: parent.width
                height: Math.max(rowTitle.implicitHeight, rowText.implicitHeight, tag.height)
                Text {
                    id: rowTitle
                    objectName: "guidanceTitle"
                    width: 140
                    text: guide.row.title
                    font: Theme.fontBodyStrong
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                }
                Text {
                    id: rowText
                    objectName: "guidanceText"
                    anchors.left: rowTitle.right
                    anchors.leftMargin: Theme.space16
                    anchors.right: tag.visible ? tag.left : parent.right
                    anchors.rightMargin: tag.visible ? Theme.space12 : 0
                    text: guide.row.text
                    font: Theme.fontBodyDefault
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                }
                DLBadge {
                    id: tag
                    objectName: "guidanceDone"
                    anchors.right: parent.right
                    visible: guide.row.done
                    tone: DLBadge.Ok
                    size: DLBadge.Sm
                    text: qsTr("Calibrated")
                    iconName: "check"
                }
            }
        }
    }
    Text {
        width: parent.width
        text: qsTr("Secure the rig and keep motors stopped while handling the sensor.")
        font: Theme.fontCaption
        color: Theme.textTertiary
        wrapMode: Text.Wrap
    }
}
