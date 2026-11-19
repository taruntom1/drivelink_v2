import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import DriveLink.Style

// G6 Connect > Agent (remote): the agent's address and token, the device list
// the agent serves, the failure states (unreachable, version mismatch, refused
// token, unknown or offline device) and the recent agents. A connection made
// here observes first; the control lease is taken explicitly on Device.
// Everything shown comes from the backend; nothing is decided here.
Column {
    id: tab
    objectName: "agentTab"

    required property var backend
    property alias addressField: address
    property alias tokenField: token
    property bool rememberToken: true
    // The last device a Connect named: Retry after a failed connect names it
    // again (the backend keeps no agent target after a failed open).
    property string lastAddress
    property string lastDevice

    readonly property var listing: backend.agentListing || ({})
    readonly property bool listingBusy: listing.state === "listing"
    readonly property var devices: listing.state === "ok" ? (listing.devices || []) : []
    // A failed connect wins over a failed listing; either is shown the same way.
    readonly property bool connectFailed: backend.agentFailure !== undefined && backend.agentFailure.state !== undefined
    readonly property var failure: connectFailed ? backend.agentFailure
        : listing.state === "failed" && listing.failure ? listing.failure : ({})
    readonly property string failureState: failure.state || ""
    readonly property bool offlineFailure: failureState === "offline"
    readonly property string failureTitle: {
        switch (failureState) {
        case "unreachable": return qsTr("Agent unreachable")
        case "version": return qsTr("Version mismatch")
        case "token": return qsTr("The agent refused the token")
        case "unknownDevice": return qsTr("The agent does not know this device")
        case "offline": return qsTr("Device is offline")
        case "capacity": return qsTr("The agent has no free client slot")
        default: return qsTr("Could not connect to the agent")
        }
    }
    readonly property bool canList: address.text.trim().length > 0 && !listingBusy && !backend.closing

    spacing: Theme.space16

    function formatId(id) { return id.length === 12 ? id.match(/../g).join(":") : id }
    function leaseText(row) {
        return row.holderId ? qsTr("Lease: %1 (%2)").arg(row.holderName || row.holderKindText).arg(row.holderKindText)
                            : qsTr("Lease: available")
    }
    // The listed devices follow the agent (online, clients, lease) every 2 s
    // while the tab shows them (G6).
    Timer {
        interval: 2000
        repeat: true
        running: tab.visible && tab.listing.state === "ok" && !tab.backend.linked && !tab.backend.loading && !tab.backend.closing
        onTriggered: tab.backend.refreshAgentList()
    }
    function clientsText(count) { return count === 1 ? qsTr("1 client") : qsTr("%1 clients").arg(count) }
    function listDevices() {
        if (canList) backend.listAgent(address.text.trim(), token.text)
    }
    // Retry the failed step: the connect that failed, else the listing.
    function retry() {
        if (connectFailed && lastDevice.length > 0) connectDevice(lastAddress, lastDevice)
        else listDevices()
    }
    function connectDevice(agentAddress, device) {
        lastAddress = agentAddress
        lastDevice = device
        backend.connectAgent(agentAddress, token.text, device, rememberToken)
    }
    function useRecent(row) {
        address.text = row.address
        token.text = row.token || ""
        rememberToken = row.rememberToken !== false
        listDevices()
    }

    Component.onCompleted: {
        const recent = backend.recentAgents
        if (address.text.length === 0 && recent && recent.length > 0) {
            address.text = recent[0].address
            token.text = recent[0].token || ""
            rememberToken = recent[0].rememberToken !== false
        }
    }

    Text {
        width: parent.width
        wrapMode: Text.Wrap
        text: qsTr("Connect to observe first. Take the control lease explicitly when you are ready to tune. Every connected client can DISARM.")
        font: Theme.fontBodyDefault
        color: Theme.textSecondary
    }

    Column {
        width: parent.width
        spacing: Theme.space8
        Text { text: qsTr("Agent address"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
        DLTextField {
            id: address
            objectName: "agentAddress"
            width: parent.width
            monospace: true
            placeholderText: qsTr("host:port")
            enabled: !tab.listingBusy && !tab.backend.closing
            onAccepted: tab.listDevices()
        }
        Text { text: qsTr("Token"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
        DLTextField {
            id: token
            objectName: "agentToken"
            width: parent.width
            monospace: true
            echoMode: TextInput.Password
            placeholderText: qsTr("Agent token")
            enabled: !tab.listingBusy && !tab.backend.closing
            error: tab.failureState === "token"
            onAccepted: tab.listDevices()
        }
        T.CheckBox {
            id: remember
            objectName: "rememberToken"
            text: qsTr("Remember token")
            checked: tab.rememberToken
            onToggled: tab.rememberToken = checked
            spacing: Theme.space8
            padding: 0
            leftPadding: indicator.width + spacing
            implicitWidth: implicitContentWidth + leftPadding
            implicitHeight: 20
            indicator: Rectangle {
                y: (remember.height - height) / 2
                implicitWidth: Theme.sizeIconSm; implicitHeight: Theme.sizeIconSm
                radius: Theme.radiusSm
                color: remember.checked ? Theme.accentDefault : Theme.bgField
                border.width: remember.checked ? 0 : Theme.borderWidth
                border.color: Theme.borderStrong
                DLIcon { anchors.centerIn: parent; visible: remember.checked; name: "check"; size: 12; color: Theme.textOnAccent }
            }
            contentItem: Text {
                verticalAlignment: Text.AlignVCenter
                text: remember.text
                font: Theme.fontBodyDefault; color: Theme.textPrimary
            }
        }
    }

    DLButton {
        objectName: "agentList"
        kind: DLButton.Primary
        text: tab.listingBusy ? qsTr("Listing…") : qsTr("List devices")
        iconName: tab.listingBusy ? "loader-circle" : ""
        enabled: tab.canList
        onClicked: tab.listDevices()
    }

    // Unreachable, version mismatch, refused token, unknown or offline device
    // (G6 137:1115, 137:1255, 137:1406).
    Rectangle {
        id: failureBox
        objectName: "agentFailure"
        width: parent.width
        visible: tab.failureState.length > 0
        height: failureColumn.implicitHeight + 2 * Theme.space16
        radius: Theme.radiusMd
        color: tab.offlineFailure ? Theme.statePendingSubtle : Theme.stateFaultSubtle
        border.width: Theme.borderWidth
        border.color: tab.offlineFailure ? Theme.statePending : Theme.stateFault
        Column {
            id: failureColumn
            x: Theme.space16; y: Theme.space16
            width: parent.width - 2 * Theme.space16
            spacing: Theme.space8
            Row {
                spacing: Theme.space8
                DLIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: tab.offlineFailure ? "unplug" : tab.failureState === "version" ? "shield-alert" : "triangle-alert"
                    size: Theme.sizeIconMd
                    color: tab.offlineFailure ? Theme.statePending : Theme.stateFault
                }
                Text {
                    objectName: "agentFailureTitle"
                    anchors.verticalCenter: parent.verticalCenter
                    text: tab.failureTitle
                    font: Theme.fontBodyStrong
                    color: Theme.textPrimary
                }
            }
            Text {
                objectName: "agentFailureText"
                width: parent.width
                wrapMode: Text.Wrap
                visible: text.length > 0
                text: tab.failure.text || ""
                font: Theme.fontBodyDefault
                color: Theme.textSecondary
            }
            Text {
                objectName: "agentFailureAddress"
                width: parent.width
                wrapMode: Text.Wrap
                visible: tab.failureState === "unreachable"
                text: qsTr("Address: %1").arg(address.text.trim() || (tab.listing.address || ""))
                font: Theme.fontMonoDefault
                color: Theme.textSecondary
            }
            Column {
                objectName: "agentVersions"
                width: parent.width
                visible: tab.failureState === "version"
                spacing: Theme.space4
                InfoRow { label: qsTr("Agent"); value: String(tab.failure.agentVersion ?? "—"); labelWidth: 120 }
                InfoRow { label: qsTr("This DriveLink"); value: String(tab.failure.coreVersion ?? "—"); labelWidth: 120 }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Nothing was changed on the agent or the device. Use a DriveLink release that matches the agent, or update the agent.")
                    font: Theme.fontBodyDefault
                    color: Theme.textSecondary
                }
            }
            Row {
                spacing: Theme.space8
                visible: tab.failureState !== "version" && tab.failureState !== "token"
                DLButton {
                    objectName: tab.offlineFailure ? "agentRetryDevice" : "agentRetry"
                    text: tab.offlineFailure ? qsTr("Retry device") : qsTr("Retry")
                    iconName: "refresh-cw"
                    enabled: !tab.backend.closing && (tab.connectFailed || tab.canList)
                    onClicked: tab.retry()
                }
                DLButton {
                    objectName: "agentDisconnect"
                    visible: tab.offlineFailure
                    text: qsTr("Disconnect")
                    onClicked: tab.backend.disconnectDevice()
                }
            }
        }
    }

    // The agent's devices.
    Column {
        objectName: "agentDevices"
        width: parent.width
        visible: tab.listing.state === "ok"
        spacing: Theme.space4
        Text {
            text: qsTr("Devices on %1").arg(tab.listing.address || "")
            font: Theme.fontLabelCaps
            color: Theme.textSecondary
        }
        Text {
            objectName: "agentNoDevices"
            width: parent.width
            wrapMode: Text.Wrap
            visible: tab.devices.length === 0
            text: qsTr("The agent has no devices.")
            font: Theme.fontBodyDefault
            color: Theme.textSecondary
        }
        Repeater {
            model: tab.devices
            Rectangle {
                id: deviceRow
                required property var modelData
                objectName: "agentDevice_" + modelData.alias
                width: parent.width
                height: 72
                radius: Theme.radiusMd
                color: "transparent"
                border.width: Theme.borderWidth
                border.color: Theme.borderDefault
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space12
                    anchors.rightMargin: Theme.space12
                    spacing: Theme.space12
                    Column {
                        Layout.fillWidth: true
                        spacing: Theme.space2
                        opacity: deviceRow.modelData.online ? 1 : 0.6
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: deviceRow.modelData.alias + "  ·  " + (deviceRow.modelData.name || "—")
                            font: Theme.fontBodyStrong
                            color: Theme.textPrimary
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: tab.formatId(deviceRow.modelData.id || "")
                            font: Theme.fontMonoSmall
                            color: Theme.textSecondary
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: tab.leaseText(deviceRow.modelData) + "  ·  " + tab.clientsText(deviceRow.modelData.clients)
                            font: Theme.fontCaption
                            color: Theme.textSecondary
                        }
                    }
                    DLBadge {
                        objectName: "agentOnline_" + deviceRow.modelData.alias
                        tone: deviceRow.modelData.online ? DLBadge.Ok : DLBadge.Neutral
                        text: deviceRow.modelData.online ? qsTr("Online") : qsTr("Offline")
                    }
                    DLReasonHover {
                        reason: qsTr("Offline")
                        DLButton {
                            objectName: "agentConnect_" + deviceRow.modelData.alias
                            kind: DLButton.Primary
                            text: qsTr("Connect")
                            enabled: deviceRow.modelData.online && !tab.backend.closing
                            onClicked: tab.connectDevice(tab.listing.address || address.text.trim(), deviceRow.modelData.alias)
                        }
                    }
                }
            }
        }
    }

    // Recent agents (G6 137:978): click to fill the address (and the token when
    // it was remembered) and list its devices.
    Column {
        objectName: "recentAgents"
        width: parent.width
        spacing: Theme.space4
        visible: tab.backend.recentAgents.length > 0
        Text {
            text: qsTr("Recent agents")
            font: Theme.fontLabelCaps
            color: Theme.textSecondary
        }
        Repeater {
            model: tab.backend.recentAgents
            Rectangle {
                id: recentRow
                required property var modelData
                objectName: "recentAgent_" + modelData.address
                width: parent.width
                height: 56
                radius: Theme.radiusMd
                color: recentMouse.containsMouse ? Theme.bgHover : "transparent"
                Column {
                    x: Theme.space12
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 2 * Theme.space12 - forgetButton.width - Theme.space8
                    spacing: Theme.space2
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: recentRow.modelData.address
                        font: Theme.fontMonoDefault
                        color: Theme.textPrimary
                    }
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: (recentRow.modelData.deviceName || recentRow.modelData.device || "—")
                            + "  ·  " + tab.formatId(recentRow.modelData.deviceId || "")
                            + "  ·  " + qsTr("Last connected %1").arg(Qt.formatDateTime(recentRow.modelData.lastSeen, "yyyy-MM-dd HH:mm"))
                        font: Theme.fontCaption
                        color: Theme.textSecondary
                    }
                }
                MouseArea {
                    id: recentMouse
                    objectName: "useRecentAgent_" + recentRow.modelData.address
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !tab.listingBusy && !tab.backend.closing
                    onClicked: tab.useRecent(recentRow.modelData)
                }
                DLIconButton {
                    id: forgetButton
                    objectName: "forgetAgent_" + recentRow.modelData.address
                    anchors.right: parent.right; anchors.rightMargin: Theme.space8
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "x"
                    size: DLIconButton.Sm
                    toolTipText: qsTr("Forget this agent")
                    onClicked: tab.backend.forgetAgent(recentRow.modelData.address)
                }
            }
        }
    }
}
