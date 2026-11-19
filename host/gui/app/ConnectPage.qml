import QtQuick
import QtQuick.Layouts
import DriveLink.Style

// G3 Connect: serial port, TCP (vdev) or a remote agent (G6, AgentTab), recent
// devices by id, model loading steps and the protocol-mismatch refusal.
Item {
    id: page
    objectName: "connectPage"

    required property var backend
    property int tab: 0
    property string selectedPort
    property var ports: []
    readonly property var bauds: [921600, 460800, 230400, 115200]
    property alias tcpField: tcp
    property alias baudBox: baud

    function refreshPorts() {
        ports = backend.serialPorts()
        if (selectedPort.length === 0 || !ports.some(p => p.endpoint === selectedPort))
            selectedPort = ports.length > 0 ? ports[0].endpoint : ""
    }
    function connectSelected() {
        if (tab === 0 && selectedPort.length > 0) backend.connectTo(selectedPort, bauds[baud.currentIndex])
        else if (tab === 1) backend.connectTo(tcp.text)
    }
    function connectRecent(row) {
        const index = bauds.indexOf(row.baud)
        backend.connectTo(row.endpoint, index >= 0 ? row.baud : 921600)
    }
    function formatId(id) { return id.length === 12 ? id.match(/../g).join(":") : id }

    Component.onCompleted: refreshPorts()
    onVisibleChanged: if (visible) refreshPorts()

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(Theme.space48, (parent.height - implicitHeight) / 3)
        width: Math.min(1120, parent.width - 2 * Theme.space48)
        spacing: Theme.space24

        Text {
            text: page.backend.loading ? qsTr("Loading the device") : qsTr("Connect to a device")
            font: Theme.fontTitlePage
            color: Theme.textPrimary
        }

        // Model loading (G3 "Loading the device model").
        Card {
            objectName: "loadingCard"
            width: parent.width
            visible: page.backend.loading
            title: page.backend.endpoint
            DLProgressBar {
                width: parent.width
                value: page.backend.loadStep / 3
            }
            Repeater {
                model: [qsTr("Open the link"), qsTr("Read identity and diagnostics"),
                        qsTr("Load descriptors, layout and status")]
                DLCheckItem {
                    required property string modelData
                    required property int index
                    width: parent.width
                    label: modelData
                    status: page.backend.loadStep > index ? DLCheckItem.Pass
                          : page.backend.loadStep === index ? DLCheckItem.Active : DLCheckItem.Pending
                }
            }
            DLButton {
                objectName: "cancelConnect"
                text: qsTr("Cancel")
                enabled: !page.backend.closing
                onClicked: page.backend.disconnectDevice()
            }
        }

        // The refused connect with both versions (G3 "Protocol mismatch").
        Card {
            objectName: "mismatchCard"
            width: parent.width
            visible: !page.backend.loading && page.backend.mismatch.device !== undefined
            title: qsTr("Protocol mismatch")
            DLBanner {
                width: parent.width
                tone: DLBanner.Fault
                title: qsTr("This device speaks core protocol v%1; this DriveLink speaks v%2")
                    .arg(page.backend.mismatch.device).arg(page.backend.mismatch.host)
                message: qsTr("Nothing was changed on the device. Update its firmware with a matching build, or use a DriveLink release for protocol v%1.")
                    .arg(page.backend.mismatch.device)
            }
            InfoRow { label: qsTr("Endpoint"); value: page.backend.mismatch.endpoint || ""; mono: true }
        }

        RowLayout {
            width: parent.width
            spacing: Theme.space24
            visible: !page.backend.loading

            Card {
                Layout.fillWidth: true
                Layout.preferredWidth: 3
                Layout.alignment: Qt.AlignTop
                title: qsTr("Connection")

                Row {
                    spacing: Theme.space8
                    Repeater {
                        model: [qsTr("Serial"), qsTr("TCP (vdev)"), qsTr("Agent (remote)")]
                        DLButton {
                            required property string modelData
                            required property int index
                            objectName: "tab" + index
                            text: modelData
                            kind: page.tab === index ? DLButton.Secondary : DLButton.Ghost
                            onClicked: page.tab = index
                        }
                    }
                }

                // Serial ports, with the device last seen on each one.
                Item {
                    width: parent.width
                    height: Theme.sizeControl
                    visible: page.tab === 0
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.ports.length === 0 ? qsTr("No serial ports found") : qsTr("Serial ports")
                        font: Theme.fontLabelCaps
                        color: Theme.textSecondary
                    }
                    DLIconButton {
                        objectName: "refreshPorts"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "refresh-cw"
                        toolTipText: qsTr("Refresh ports")
                        onClicked: page.refreshPorts()
                    }
                }
                Column {
                    width: parent.width
                    visible: page.tab === 0
                    spacing: Theme.space4
                    Repeater {
                        model: page.ports
                        Rectangle {
                            id: portRow
                            required property var modelData
                            objectName: "port_" + modelData.name
                            width: parent.width
                            height: 56
                            radius: Theme.radiusMd
                            color: page.selectedPort === modelData.endpoint ? Theme.bgSelected
                                 : portMouse.containsMouse ? Theme.bgHover : "transparent"
                            border.width: page.selectedPort === modelData.endpoint ? Theme.borderWidth : 0
                            border.color: Theme.borderFocus
                            DLIcon {
                                id: portIcon
                                x: Theme.space12
                                anchors.verticalCenter: parent.verticalCenter
                                name: "usb"
                            }
                            Column {
                                anchors.left: portIcon.right; anchors.leftMargin: Theme.space12
                                anchors.right: portBadge.left; anchors.rightMargin: Theme.space12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.space2
                                Text {
                                    width: parent.width
                                    text: portRow.modelData.name + (portRow.modelData.description ? "  ·  " + portRow.modelData.description : "")
                                    font: Theme.fontBodyStrong; color: Theme.textPrimary; elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width
                                    visible: text.length > 0
                                    text: portRow.modelData.deviceId
                                        ? qsTr("Last seen: %1").arg(portRow.modelData.deviceName || page.formatId(portRow.modelData.deviceId))
                                        : (portRow.modelData.usbId ? qsTr("USB %1").arg(portRow.modelData.usbId) : "")
                                    font: Theme.fontCaption; color: Theme.textSecondary; elide: Text.ElideRight
                                }
                            }
                            DLBadge {
                                id: portBadge
                                anchors.right: parent.right; anchors.rightMargin: Theme.space12
                                anchors.verticalCenter: parent.verticalCenter
                                visible: portRow.modelData.inUse
                                text: qsTr("In use here")
                                size: DLBadge.Sm
                            }
                            MouseArea {
                                id: portMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: page.selectedPort = portRow.modelData.endpoint
                                onDoubleClicked: { page.selectedPort = portRow.modelData.endpoint; page.connectSelected() }
                            }
                        }
                    }
                }
                Row {
                    visible: page.tab === 0
                    spacing: Theme.space12
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Baud")
                        font: Theme.fontBodyDefault; color: Theme.textSecondary
                    }
                    DLComboBox {
                        id: baud
                        objectName: "baud"
                        width: 160
                        model: page.bauds
                    }
                }
                Column {
                    width: parent.width
                    visible: page.tab === 1
                    spacing: Theme.space8
                    Text { text: qsTr("Address"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
                    DLTextField {
                        id: tcp
                        objectName: "endpoint"
                        width: parent.width
                        monospace: true
                        text: "tcp://127.0.0.1:8765"
                        placeholderText: "tcp://127.0.0.1:8765"
                        enabled: !page.backend.closing
                        onAccepted: page.connectSelected()
                    }
                }
                AgentTab {
                    objectName: "agentTab"
                    width: parent.width
                    visible: page.tab === 2
                    backend: page.backend
                }
                DLButton {
                    objectName: "connectButton"
                    visible: page.tab < 2
                    kind: DLButton.Primary
                    text: qsTr("Connect")
                    enabled: !page.backend.closing && (page.tab === 1 ? tcp.text.length > 0 : page.selectedPort.length > 0)
                    onClicked: page.connectSelected()
                }
                Text {
                    objectName: "connectError"
                    width: parent.width
                    visible: page.tab < 2 && text.length > 0 && page.backend.mismatch.device === undefined
                    wrapMode: Text.Wrap
                    // Win32 ERROR_ACCESS_DENIED, Linux EBUSY, from the failed open.
                    text: page.backend.lastError.osError === (Qt.platform.os === "windows" ? 5 : 16) && page.backend.endpoint.startsWith("serial://")
                        ? qsTr("%1 is in use by another program. Close it there, then connect again.").arg(page.backend.endpoint.slice(9))
                        : (page.backend.lastError.text || "")
                    color: Theme.stateFault
                    font: Theme.fontBodyDefault
                }
            }

            Card {
                objectName: "recentCard"
                Layout.fillWidth: true
                Layout.preferredWidth: 2
                Layout.alignment: Qt.AlignTop
                title: qsTr("Recent devices")
                Text {
                    visible: page.backend.recentDevices.length === 0
                    text: qsTr("Devices you connect to appear here, by device id.")
                    font: Theme.fontBodyDefault; color: Theme.textSecondary
                    width: parent.width; wrapMode: Text.Wrap
                }
                Repeater {
                    model: page.backend.recentDevices
                    Rectangle {
                        id: recentRow
                        required property var modelData
                        objectName: "recent_" + modelData.id
                        width: parent.width
                        height: 64
                        radius: Theme.radiusMd
                        color: recentMouse.containsMouse ? Theme.bgHover : "transparent"
                        Column {
                            x: Theme.space12
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 2 * Theme.space12 - forget.width
                            spacing: Theme.space2
                            Text {
                                width: parent.width
                                text: recentRow.modelData.name || page.formatId(recentRow.modelData.id)
                                font: Theme.fontBodyStrong; color: Theme.textPrimary; elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: (recentRow.modelData.name ? page.formatId(recentRow.modelData.id) + "  ·  " : "")
                                    + recentRow.modelData.endpoint + "  ·  " + recentRow.modelData.firmware
                                font: Theme.fontCaption; color: Theme.textSecondary; elide: Text.ElideRight
                            }
                        }
                        MouseArea {
                            id: recentMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: !page.backend.closing
                            onClicked: page.connectRecent(recentRow.modelData)
                        }
                        DLIconButton {
                            id: forget
                            anchors.right: parent.right; anchors.rightMargin: Theme.space8
                            anchors.verticalCenter: parent.verticalCenter
                            iconName: "x"
                            size: DLIconButton.Sm
                            toolTipText: qsTr("Forget this device")
                            onClicked: page.backend.forgetRecent(recentRow.modelData.id)
                        }
                    }
                }
            }
        }
    }
}
