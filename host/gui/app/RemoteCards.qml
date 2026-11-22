import QtQuick
import QtQuick.Layouts
import DriveLink.Style

// G6 Device page cards of a remote connection (138:10168): the agent and the
// device, what remote access means, the clients and the lease, and the
// agent's recent history. The buttons only emit requests; the shell opens the
// take and release dialogs.
GridLayout {
    id: cards
    objectName: "remoteCards"

    required property var backend
    property alias clientsCard: clients
    signal takeLeaseRequested()
    signal releaseLeaseRequested()

    readonly property var lease: backend.lease || ({})
    readonly property var agent: backend.agent || ({})
    readonly property var clientList: backend.clients || []
    readonly property var history: backend.agentHistory || []
    // Rows of the history card; the drawer holds the rest.
    readonly property int historyLimit: 8
    readonly property var clientWidths: [0.5, 0.2, 0.3]
    function formatId(id) { return id && id.length === 12 ? id.match(/../g).join(":") : (id || "") }
    function dateText(date) { return date && !isNaN(date.getTime()) ? Qt.formatDateTime(date, "yyyy-MM-dd HH:mm:ss") : "—" }

    columns: 2
    columnSpacing: Theme.space24
    rowSpacing: Theme.space24

    Card {
        objectName: "agentCard"
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.alignment: Qt.AlignTop
        title: qsTr("Agent and device")
        InfoRow { label: qsTr("Agent address"); value: cards.agent.address || ""; mono: true }
        InfoRow { label: qsTr("Agent protocol"); value: cards.agent.protocol || "" }
        InfoRow { label: qsTr("Device name"); value: cards.agent.deviceName || cards.backend.deviceName || "—" }
        InfoRow { label: qsTr("Device id"); value: cards.formatId(cards.agent.deviceId || cards.backend.deviceId); mono: true }
        InfoRow { label: qsTr("Serial link"); value: cards.agent.link || "—"; mono: true }
        InfoRow { label: qsTr("Connected since"); value: cards.dateText(cards.agent.since) }
    }

    Card {
        objectName: "remoteAccessCard"
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.alignment: Qt.AlignTop
        title: qsTr("Remote access")
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("This device is reached through drivelink-agent. Every connected client can watch live data and DISARM. Only the lease holder can ARM, send commands, run tests or change the layout and configuration.")
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("The agent disarms the device whenever the lease changes hands. A new holder never arms it automatically.")
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
        }
    }

    Card {
        id: clients
        objectName: "clientsCard"
        Layout.fillWidth: true
        Layout.columnSpan: cards.columns
        Layout.alignment: Qt.AlignTop
        title: qsTr("Clients and lease")
        contentSpacing: Theme.space8
        actions: [
            DLButton {
                objectName: "refreshClients"
                text: qsTr("Refresh clients")
                iconName: "refresh-cw"
                onClicked: cards.backend.refreshClients()
            },
            DLReasonHover {
                visible: !cards.lease.mine
                reason: cards.lease.takeBlocked || ""
                DLButton {
                    objectName: "takeLease"
                    kind: DLButton.Primary
                    text: qsTr("Take lease…")
                    iconName: "key-round"
                    enabled: (cards.lease.takeBlocked || "") === ""
                    onClicked: cards.takeLeaseRequested()
                }
            },
            DLReasonHover {
                visible: cards.lease.mine === true
                reason: cards.lease.releaseBlocked || ""
                DLButton {
                    objectName: "releaseLease"
                    text: qsTr("Release lease")
                    iconName: "lock-open"
                    enabled: (cards.lease.releaseBlocked || "") === ""
                    onClicked: cards.releaseLeaseRequested()
                }
            }
        ]

        Row {
            width: parent.width
            Repeater {
                model: [qsTr("Client"), qsTr("Kind"), qsTr("Lease")]
                Text {
                    required property string modelData
                    required property int index
                    width: parent.width * cards.clientWidths[index]
                    text: modelData
                    font: Theme.fontLabelCaps
                    color: Theme.textSecondary
                }
            }
        }
        Repeater {
            model: cards.clientList
            Rectangle {
                id: clientRow
                required property var modelData
                objectName: "client_" + modelData.id
                width: parent.width
                height: 40
                radius: Theme.radiusSm
                color: modelData.self ? Theme.bgSelected : "transparent"
                Row {
                    anchors.fill: parent
                    Text {
                        width: parent.width * cards.clientWidths[0]
                        anchors.verticalCenter: parent.verticalCenter
                        leftPadding: Theme.space8
                        elide: Text.ElideRight
                        text: (clientRow.modelData.name || qsTr("Client %1").arg(clientRow.modelData.id))
                            + (clientRow.modelData.self ? qsTr(" (this GUI)") : "")
                        font: Theme.fontBodyStrong
                        color: Theme.textPrimary
                    }
                    Text {
                        width: parent.width * cards.clientWidths[1]
                        anchors.verticalCenter: parent.verticalCenter
                        text: clientRow.modelData.kindText
                        font: Theme.fontBodyDefault
                        color: Theme.textPrimary
                    }
                    Item {
                        width: parent.width * cards.clientWidths[2]
                        height: parent.height
                        DLBadge {
                            anchors.verticalCenter: parent.verticalCenter
                            tone: clientRow.modelData.holder ? DLBadge.Accent : DLBadge.Neutral
                            iconName: clientRow.modelData.holder ? "key-round" : "eye"
                            text: clientRow.modelData.holder ? qsTr("Holds lease") : qsTr("Observer")
                        }
                    }
                }
            }
        }
        Text {
            objectName: "noClients"
            visible: cards.clientList.length === 0
            text: qsTr("No clients listed yet")
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            topPadding: Theme.space8
            text: qsTr("Watching and live param edits remain available. Taking the lease disarms the device; it never arms automatically.")
            font: Theme.fontCaption
            color: Theme.textSecondary
        }
    }

    Card {
        objectName: "agentHistoryCard"
        Layout.fillWidth: true
        Layout.columnSpan: cards.columns
        Layout.alignment: Qt.AlignTop
        title: qsTr("Recent agent history")
        contentSpacing: Theme.space8
        Repeater {
            model: cards.history.slice(0, cards.historyLimit)
            Item {
                id: historyRow
                required property var modelData
                objectName: "agentHistory_" + modelData.kind
                width: parent.width
                height: Math.max(32, historyText.implicitHeight)
                Text {
                    id: historyTime
                    width: 96
                    anchors.verticalCenter: parent.verticalCenter
                    text: Qt.formatTime(historyRow.modelData.time, "HH:mm:ss")
                    font: Theme.fontMonoDefault
                    color: Theme.textSecondary
                }
                Text {
                    id: historyText
                    anchors.left: historyTime.right
                    anchors.right: historySource.left
                    anchors.rightMargin: Theme.space12
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.Wrap
                    text: historyRow.modelData.text
                    font: Theme.fontBodyDefault
                    color: Theme.textPrimary
                }
                Text {
                    id: historySource
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Agent history")
                    font: Theme.fontCaption
                    color: Theme.textSecondary
                }
            }
        }
        Text {
            visible: cards.history.length === 0
            text: qsTr("The agent has reported no history yet")
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
        }
    }
}
