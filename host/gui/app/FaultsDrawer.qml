import QtQuick
import DriveLink.Style

// G3 faults drawer: the device's diag entries with the code names, flags and
// counts it serves; active entries first. Acknowledge says why it is refused.
// A remote connection (G6) adds the agent's history after the device's entries.
Rectangle {
    id: drawer
    objectName: "faultsDrawer"

    required property var backend
    signal closeRequested()

    color: Theme.bgSurface
    border.color: Theme.borderDefault

    Column {
        id: header
        x: Theme.space24; y: Theme.space24
        width: parent.width - 2 * Theme.space24
        spacing: Theme.space16
        Item {
            width: parent.width
            height: Theme.sizeControl
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Faults")
                font: Theme.fontTitleSection; color: Theme.textPrimary
            }
            DLIconButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                iconName: "x"
                toolTipText: qsTr("Close faults")
                onClicked: drawer.closeRequested()
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("%1 active · %2 since boot").arg(drawer.backend.activeFaults).arg(drawer.backend.faultHistory)
            font: Theme.fontBodyDefault; color: Theme.textSecondary
        }
        DLReasonHover {
            visible: drawer.backend.armState === 2
            reason: drawer.backend.ackBlockedReason
            DLButton {
                objectName: "drawerAck"
                kind: DLButton.Primary
                text: qsTr("Acknowledge fault")
                enabled: drawer.backend.ackEnabled
                onClicked: drawer.backend.acknowledge()
            }
        }
    }
    ListView {
        id: list
        objectName: "faultList"
        anchors.top: header.bottom; anchors.topMargin: Theme.space16
        anchors.bottom: parent.bottom; anchors.bottomMargin: Theme.space24
        x: Theme.space24
        width: parent.width - 2 * Theme.space24
        clip: true
        spacing: Theme.space8
        model: drawer.backend.faults
        // Remote: "This connection" labels the device's own entries.
        header: Column {
            width: list.width
            spacing: Theme.space8
            bottomPadding: visible ? Theme.space8 : 0
            visible: list.count === 0 || drawer.backend.remote
            height: visible ? implicitHeight : 0
            Text {
                objectName: "connectionHeader"
                visible: drawer.backend.remote
                text: qsTr("This connection")
                font: Theme.fontLabelCaps; color: Theme.textSecondary
            }
            Text {
                visible: list.count === 0
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("No diagnostic entries since boot")
                font: Theme.fontBodyDefault; color: Theme.textSecondary
            }
        }
        // Remote: the agent's history, newest first, after the device's entries.
        footer: Column {
            objectName: "agentHistory"
            width: list.width
            visible: drawer.backend.remote
            height: visible ? implicitHeight : 0
            spacing: Theme.space8
            topPadding: Theme.space16
            Text {
                text: qsTr("Agent history")
                font: Theme.fontLabelCaps; color: Theme.textSecondary
            }
            Text {
                objectName: "agentHistoryEmpty"
                width: parent.width
                wrapMode: Text.Wrap
                visible: (drawer.backend.agentHistory || []).length === 0
                text: qsTr("The agent has reported no history yet")
                font: Theme.fontBodyDefault; color: Theme.textSecondary
            }
            Repeater {
                model: drawer.backend.agentHistory || []
                Rectangle {
                    id: historyEntry
                    required property var modelData
                    objectName: "agentHistoryEntry_" + modelData.kind
                    width: parent.width
                    height: historyColumn.implicitHeight + 2 * Theme.space12
                    radius: Theme.radiusMd
                    color: Theme.bgSecondary
                    Column {
                        id: historyColumn
                        x: Theme.space12; y: Theme.space12
                        width: parent.width - 2 * Theme.space12
                        spacing: Theme.space4
                        Text {
                            width: parent.width
                            wrapMode: Text.Wrap
                            text: historyEntry.modelData.text
                            font: Theme.fontBodyDefault; color: Theme.textPrimary
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: Qt.formatTime(historyEntry.modelData.time, "HH:mm:ss") + " · " + qsTr("Agent history")
                            font: Theme.fontCaption; color: Theme.textSecondary
                        }
                    }
                }
            }
        }
        delegate: Rectangle {
            id: entry
            required property string name
            required property string sourceName
            required property var instanceId
            required property string instanceLabel
            required property int code
            required property int severity
            required property bool active
            required property bool disarms
            required property bool blocksArm
            required property var count
            width: ListView.view.width
            height: entryColumn.implicitHeight + 2 * Theme.space12
            radius: Theme.radiusMd
            color: entry.active ? Theme.stateFaultSubtle : Theme.bgSecondary
            border.width: entry.active ? Theme.borderWidth : 0
            border.color: Theme.stateFault
            Column {
                id: entryColumn
                x: Theme.space12; y: Theme.space12
                width: parent.width - 2 * Theme.space12
                spacing: Theme.space4
                Item {
                    width: parent.width
                    height: Math.max(nameText.implicitHeight, badges.implicitHeight)
                    Text {
                        id: nameText
                        anchors.left: parent.left; anchors.right: badges.left; anchors.rightMargin: Theme.space8
                        text: entry.name
                        font: Theme.fontBodyStrong
                        color: entry.active ? Theme.stateFault : Theme.textPrimary
                        elide: Text.ElideRight
                    }
                    Row {
                        id: badges
                        anchors.right: parent.right
                        spacing: Theme.space4
                        DLBadge { visible: entry.active; text: qsTr("Active"); tone: DLBadge.Fault; size: DLBadge.Sm }
                        DLBadge { visible: entry.disarms; text: qsTr("Disarms"); tone: DLBadge.Neutral; size: DLBadge.Sm }
                        DLBadge { visible: entry.blocksArm; text: qsTr("Blocks arm"); tone: DLBadge.Neutral; size: DLBadge.Sm }
                    }
                }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: entry.sourceName
                        + (entry.instanceId !== undefined ? " · " + (entry.instanceLabel || qsTr("instance %1").arg(entry.instanceId)) : "")
                        + " · 0x" + entry.code.toString(16).padStart(4, "0")
                        + " · " + (entry.severity === 1 ? qsTr("error") : entry.severity === 2 ? qsTr("warning") : qsTr("severity %1").arg(entry.severity))
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                Text {
                    text: entry.count === 1 ? qsTr("1 occurrence") : qsTr("%1 occurrences").arg(entry.count)
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
            }
        }
    }
}
