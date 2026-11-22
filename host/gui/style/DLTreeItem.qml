import QtQuick
import QtQuick.Templates as T

// Figma TreeItem. A loop header (52 px, two lines: NAME · RATE, budget; a
// neutral Real-time badge and its state) or a module row (32 px: label and
// type, indented by depth). Pending add / remove / label / change, failed and
// offline are shown in the row; selection uses the accent fill.
T.ItemDelegate {
    id: control

    property bool isLoop: false
    property int depth: 0
    property string label
    property string typeName
    property string pending        // "add", "remove", "label", "change" or ""
    property int health: 0         // 0 running, 1 failed, 2 invalid, 3 dependency missing, 4 offline, 255 pending
    property string healthText
    property int rateHz: 0
    property bool realtime: false
    property real budget: 0
    property bool hasBudget: false
    property bool selected: false

    readonly property bool failed: health >= 1 && health <= 3
    readonly property string pendingText: pending === "add" ? qsTr("Pending add")
                                        : pending === "remove" ? qsTr("Pending remove")
                                        : pending === "label" ? qsTr("Label change pending")
                                        : pending === "change" ? qsTr("Pending change") : ""
    readonly property string stateText: pendingText.length > 0 ? pendingText
                                      : health === 0 ? (isLoop ? qsTr("Running") : "")
                                      : healthText.length > 0 ? healthText.charAt(0).toUpperCase() + healthText.slice(1) : ""
    function rateText(hz) { return hz >= 1000 && hz % 100 === 0 ? (hz / 1000) + " kHz" : hz + " Hz" }

    implicitWidth: 320
    implicitHeight: isLoop ? 52 : Theme.sizeControl
    leftPadding: Theme.space8 + (isLoop ? 0 : Theme.space16 * Math.max(0, depth - 1))
    rightPadding: Theme.space8
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        radius: Theme.radiusSm
        color: control.selected ? Theme.bgSelected
             : control.pending.length > 0 ? Theme.statePendingSubtle
             : control.down ? Theme.bgPressed : control.hovered ? Theme.bgHover : "transparent"
        border.width: control.visualFocus ? Theme.borderFocusWidth : 0
        border.color: Theme.borderFocus
    }

    contentItem: Item {
        implicitHeight: control.isLoop ? 52 : Theme.sizeControl
        // Loop header.
        Column {
            visible: control.isLoop
            anchors.left: parent.left; anchors.right: loopRight.left; anchors.rightMargin: Theme.space8
            anchors.verticalCenter: parent.verticalCenter
            Text {
                width: parent.width; elide: Text.ElideRight
                text: (control.label + " · " + control.rateText(control.rateHz)).toUpperCase()
                font: Theme.fontLabelCaps
                color: control.pending === "remove" ? Theme.textTertiary : Theme.textPrimary
            }
            Text {
                text: control.hasBudget ? qsTr("Budget %1%").arg((control.budget * 100).toFixed(control.budget < 0.1 ? 2 : 1))
                                        : control.health === 0 ? qsTr("Budget —") : ""
                visible: text.length > 0
                font: Theme.fontCaption
                color: Theme.textSecondary
            }
        }
        Column {
            id: loopRight
            visible: control.isLoop
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space2
            DLBadge {
                anchors.right: parent.right
                visible: control.realtime
                size: DLBadge.Sm
                tone: DLBadge.Neutral
                text: qsTr("Real-time")
            }
            Text {
                anchors.right: parent.right
                text: control.stateText
                font: Theme.fontCaption
                color: control.pending.length > 0 ? Theme.statePending : control.failed ? Theme.stateFault : Theme.textSecondary
            }
        }
        // Module row.
        Row {
            visible: !control.isLoop
            anchors.left: parent.left; anchors.right: moduleState.left; anchors.rightMargin: Theme.space8
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space8
            clip: true
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.label
                font: Theme.fontBodyDefault
                color: control.pending === "remove" ? Theme.textTertiary : control.failed ? Theme.stateFault : Theme.textPrimary
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.typeName
                font: Theme.fontMonoSmall
                color: Theme.textTertiary
            }
        }
        Text {
            id: moduleState
            visible: !control.isLoop
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
            text: control.stateText
            font: Theme.fontCaption
            color: control.pending.length > 0 ? Theme.statePending : control.failed ? Theme.stateFault : Theme.textSecondary
        }
    }
}
