import QtQuick
import QtQuick.Layouts
import DriveLink.Style

// G3 Device: identity with Rename, loops and CPU per core, ARM checks,
// faults, configuration (Save, Revert, Reboot, Save and reboot, Factory
// reset), limits and firmware. Refused-while-armed buttons say why.
Flickable {
    id: page
    objectName: "devicePage"

    required property var backend
    signal renameRequested()
    signal factoryResetRequested()
    signal faultsRequested()
    signal loopRequested(int loopId)

    readonly property bool wide: width >= 1800
    // Loop table column fractions (G7b Device loop table).
    readonly property var loopWidths: [0.24, 0.11, 0.11, 0.13, 0.13, 0.18, 0.10]
    function formatId(id) { return id.length === 12 ? id.match(/../g).join(":") : id }
    function micros(value) { return value === undefined ? "—" : value >= 1000 ? (value / 1000).toFixed(2) + " ms" : value + " µs" }

    contentHeight: grid.implicitHeight + 2 * Theme.space24
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    GridLayout {
        id: grid
        x: Theme.space24; y: Theme.space24
        width: page.width - 2 * Theme.space24
        columns: page.wide ? 3 : 2
        columnSpacing: Theme.space24
        rowSpacing: Theme.space24

        Card {
            objectName: "identityCard"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            title: page.backend.deviceName || page.formatId(page.backend.deviceId)
            actions: DLReasonHover {
                reason: page.backend.actionBlocked.rename || ""
                DLButton {
                    objectName: "renameButton"
                    text: qsTr("Rename")
                    iconName: "pencil"
                    enabled: !page.backend.actionBlocked.rename
                    onClicked: page.renameRequested()
                }
            }
            InfoRow { label: qsTr("Device id"); value: page.formatId(page.backend.deviceId); mono: true }
            InfoRow { label: qsTr("Endpoint"); value: page.backend.endpoint; mono: true }
            InfoRow { label: qsTr("Firmware"); value: page.backend.info.firmware || "" }
            InfoRow { label: qsTr("Build id"); value: page.backend.info.buildId || ""; mono: true }
            InfoRow { label: qsTr("Core protocol"); value: "v" + (page.backend.info.protocol || "") }
            InfoRow { label: qsTr("Last reset"); value: page.backend.info.resetReason || qsTr("Unknown") }
            InfoRow {
                label: qsTr("Configuration")
                value: page.backend.info.layoutInvalid ? qsTr("Saved layout invalid; factory layout running")
                     : page.backend.info.configLoaded ? qsTr("Loaded from storage") : qsTr("Factory defaults")
            }
        }

        Card {
            objectName: "armCard"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            title: qsTr("Arming")
            actions: [
                DLStateChip {
                    status: page.backend.safeMode ? DLStateChip.SafeMode
                          : page.backend.armState === 1 ? DLStateChip.Armed
                          : page.backend.armState === 2 ? DLStateChip.Fault : DLStateChip.Disarmed
                    onFaultsRequested: page.faultsRequested()
                },
                DLReasonHover {
                    visible: page.backend.armState === 2
                    reason: page.backend.ackBlockedReason
                    DLButton {
                        objectName: "ackButton"
                        text: qsTr("Acknowledge fault")
                        enabled: page.backend.ackEnabled
                        onClicked: page.backend.acknowledge()
                    }
                }
            ]
            InfoRow { label: qsTr("Last disarm"); value: page.backend.reason }
            Repeater {
                model: page.backend.armChecks
                DLCheckItem {
                    required property var modelData
                    width: parent.width
                    label: modelData.label
                    detail: modelData.detail
                    status: modelData.state === "pass" ? DLCheckItem.Pass
                          : modelData.state === "fail" ? DLCheckItem.Fail : DLCheckItem.Pending
                }
            }
        }

        Card {
            objectName: "faultsCard"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            title: qsTr("Faults")
            actions: DLButton {
                text: qsTr("Open faults")
                iconName: "bell"
                onClicked: page.faultsRequested()
            }
            Row {
                spacing: Theme.space32
                Column {
                    Text { text: page.backend.activeFaults; font: Theme.fontDisplayValue; color: page.backend.activeFaults > 0 ? Theme.stateFault : Theme.textPrimary }
                    Text { text: qsTr("active"); font: Theme.fontCaption; color: Theme.textSecondary }
                }
                Column {
                    Text { text: page.backend.faultHistory; font: Theme.fontDisplayValue; color: Theme.textPrimary }
                    Text { text: qsTr("since boot"); font: Theme.fontCaption; color: Theme.textSecondary }
                }
            }
        }

        Card {
            objectName: "loopsCard"
            Layout.fillWidth: true
            Layout.columnSpan: 2
            Layout.alignment: Qt.AlignTop
            title: qsTr("Loops")
            contentSpacing: Theme.space8

            Row {
                width: parent.width
                Repeater {
                    model: [qsTr("Loop"), qsTr("Rate"), qsTr("Core"), qsTr("Run time"),
                            qsTr("Longest"), qsTr("Budget"), qsTr("Overruns")]
                    Text {
                        required property string modelData
                        required property int index
                        width: parent.width * page.loopWidths[index]
                        text: modelData
                        font: Theme.fontLabelCaps
                        color: Theme.textSecondary
                    }
                }
            }
            Repeater {
                id: loopRepeater
                model: page.backend.loops
                Rectangle {
                    id: loopRow
                    required property int loopId
                    required property string name
                    required property var rateHz
                    required property bool realtime
                    required property string core
                    required property bool running
                    required property var lastRunUs
                    required property var maxRunUs
                    required property var budget
                    required property var overruns
                    required property string runTimeNote
                    objectName: "loop_" + loopId
                    width: parent.width
                    height: 44
                    color: rowMouse.containsMouse ? Theme.bgHover : "transparent"
                    radius: Theme.radiusSm
                    readonly property var w: page.loopWidths
                    Row {
                        anchors.fill: parent
                        Item {
                            width: parent.width * loopRow.w[0]; height: parent.height
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: (loopRow.name || qsTr("loop")) + "  #" + loopRow.loopId
                                font: Theme.fontBodyStrong
                                color: Theme.accentText
                            }
                        }
                        Text {
                            width: parent.width * loopRow.w[1]; anchors.verticalCenter: parent.verticalCenter
                            text: loopRow.rateHz + " Hz" + (loopRow.realtime ? "  RT" : "")
                            font: Theme.fontBodyNumeric; color: Theme.textPrimary
                        }
                        Text {
                            width: parent.width * loopRow.w[2]; anchors.verticalCenter: parent.verticalCenter
                            text: !loopRow.running ? qsTr("not running") : loopRow.core === "any" ? qsTr("Any core")
                                : loopRow.core.startsWith("core_") ? qsTr("Core %1").arg(loopRow.core.slice(5)) : loopRow.core
                            font: Theme.fontBodyDefault
                            color: loopRow.running ? Theme.textPrimary : Theme.textTertiary
                        }
                        DLReasonHover {
                            width: parent.width * loopRow.w[3]; height: parent.height
                            reason: loopRow.runTimeNote
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                enabled: loopRow.lastRunUs !== undefined
                                text: page.micros(loopRow.lastRunUs)
                                font: Theme.fontBodyNumeric; color: Theme.textPrimary
                            }
                        }
                        Text {
                            width: parent.width * loopRow.w[4]; anchors.verticalCenter: parent.verticalCenter
                            text: page.micros(loopRow.maxRunUs)
                            font: Theme.fontBodyNumeric; color: Theme.textPrimary
                        }
                        Item {
                            width: parent.width * loopRow.w[5]; height: parent.height
                            DLProgressBar {
                                id: budgetBar
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - budgetText.width - Theme.space24
                                visible: loopRow.budget !== undefined
                                value: Math.min(1, loopRow.budget || 0)
                            }
                            Text {
                                id: budgetText
                                anchors.right: parent.right; anchors.rightMargin: Theme.space12
                                anchors.verticalCenter: parent.verticalCenter
                                text: loopRow.budget === undefined ? "—" : Math.round(loopRow.budget * 100) + " %"
                                font: Theme.fontBodyNumeric
                                color: (loopRow.budget || 0) > 0.8 ? Theme.stateFault : Theme.textPrimary
                            }
                        }
                        Text {
                            width: parent.width * loopRow.w[6]; anchors.verticalCenter: parent.verticalCenter
                            text: loopRow.overruns === undefined ? "—" : loopRow.overruns
                            font: Theme.fontBodyNumeric
                            color: (loopRow.overruns || 0) > 0 ? Theme.stateFault : Theme.textPrimary
                        }
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.loopRequested(loopRow.loopId)
                    }
                }
            }
            Text {
                visible: loopRepeater.count === 0
                text: page.backend.safeMode ? qsTr("No loop runs in safe mode") : qsTr("No loops configured")
                font: Theme.fontBodyDefault; color: Theme.textSecondary
            }
            Item { width: 1; height: Theme.space8 }
            Text { text: qsTr("CPU per core"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
            Repeater {
                model: page.backend.cpuLoad
                Row {
                    required property var modelData
                    required property int index
                    width: parent.width
                    spacing: Theme.space12
                    Text { width: 80; text: qsTr("Core %1").arg(index); font: Theme.fontBodyDefault; color: Theme.textSecondary }
                    DLProgressBar {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 80 - 80 - 2 * Theme.space12
                        value: modelData < 0 ? 0 : modelData / 100
                    }
                    Text {
                        width: 80
                        text: modelData < 0 ? qsTr("n/a") : modelData + " %"
                        font: Theme.fontBodyNumeric; color: Theme.textPrimary
                    }
                }
            }
        }

        Card {
            objectName: "configCard"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            title: qsTr("Configuration")
            actions: DLBadge {
                visible: page.backend.unsaved
                text: qsTr("Unsaved")
                tone: DLBadge.Unsaved
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: page.backend.configBusy ? qsTr("Working…")
                    : page.backend.unsaved ? qsTr("Changes apply now and are lost at reboot unless saved.")
                    : qsTr("The device runs its saved configuration.")
                font: Theme.fontBodyDefault; color: Theme.textSecondary
            }
            Repeater {
                model: page.backend.unsavedChanges
                Text {
                    required property string modelData
                    width: parent.width; elide: Text.ElideRight
                    text: "•  " + modelData
                    font: Theme.fontBodyDefault; color: Theme.textPrimary
                }
            }
            Flow {
                width: parent.width
                spacing: Theme.space8
                Repeater {
                    model: [
                        {key: "save", text: qsTr("Save"), icon: "save", kind: DLButton.Primary},
                        {key: "revert", text: qsTr("Revert"), icon: "undo-2", kind: DLButton.Secondary},
                        {key: "reboot", text: qsTr("Reboot"), icon: "rotate-ccw", kind: DLButton.Secondary},
                        {key: "saveAndReboot", text: qsTr("Save and reboot"), icon: "", kind: DLButton.Secondary},
                        {key: "factoryReset", text: qsTr("Factory reset"), icon: "trash-2", kind: DLButton.Danger}
                    ]
                    DLReasonHover {
                        required property var modelData
                        reason: page.backend.actionBlocked[modelData.key] || ""
                        DLButton {
                            objectName: "device_" + parent.modelData.key
                            text: parent.modelData.text
                            iconName: parent.modelData.icon
                            kind: parent.modelData.kind
                            enabled: !page.backend.actionBlocked[parent.modelData.key]
                            onClicked: {
                                const key = parent.modelData.key
                                if (key === "factoryReset") page.factoryResetRequested()
                                else page.backend[key]()
                            }
                        }
                    }
                }
            }
        }

        Card {
            objectName: "limitsCard"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            title: qsTr("Limits")
            contentSpacing: Theme.space8
            Repeater {
                model: page.backend.limits
                InfoRow {
                    required property var modelData
                    label: modelData.name.replace(/_/g, " ")
                    value: modelData.value + (modelData.unit ? " " + modelData.unit : "")
                }
            }
            InfoRow { label: qsTr("max payload"); value: (page.backend.info.maxPayload || 0) + " bytes" }
            InfoRow { label: qsTr("capture memory"); value: Math.round((page.backend.info.captureCapacity || 0) / 1024) + " KiB" }
        }
    }
}
