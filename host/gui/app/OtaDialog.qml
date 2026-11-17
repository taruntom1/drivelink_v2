import QtQuick
import QtQuick.Templates as T
import DriveLink.Style

// G3 "Update firmware" (writing, waiting to confirm, rolled back). Like
// DLDialog it is not a Qt modal: the shell's scrim blocks only the page, so
// DISARM stays live. Two modes: `choice` (the inspected image, before Install)
// and the running or finished update from `backend.ota`.
Rectangle {
    id: dialog
    objectName: "otaDialog"

    required property var backend
    // The inspected image while choosing ({} otherwise).
    property var choice: ({})
    readonly property var ota: backend.ota
    readonly property bool choosing: choice.fileName !== undefined && ota.visible !== true
    signal chooseAnother()
    signal dismissed()

    visible: choosing || ota.visible === true
    width: 560
    height: column.implicitHeight + 2 * Theme.space24
    color: Theme.bgRaised
    radius: Theme.radiusLg
    border.width: Theme.borderWidth
    border.color: Theme.borderDefault

    function install() {
        backend.startUpdate(!unconfirmed.checked)
        dialog.choice = ({})
    }
    function cancel() { dialog.choice = ({}); dialog.dismissed() }
    onChoosingChanged: if (choosing) unconfirmed.checked = false

    DLShadow { dialog: true; radius: parent.radius }

    Column {
        id: column
        x: Theme.space24; y: Theme.space24
        width: parent.width - 2 * Theme.space24
        spacing: Theme.space16

        Text {
            objectName: "otaTitle"
            width: parent.width
            text: dialog.choosing ? qsTr("Update firmware") : (dialog.ota.title || "")
            font: Theme.fontTitleSection; color: Theme.textPrimary
            wrapMode: Text.Wrap
        }
        // Rolled back / failed / done: the outcome in one line (G3 rolled back).
        Rectangle {
            objectName: "otaHeadline"
            width: parent.width
            height: headline.implicitHeight + 2 * Theme.space12
            visible: !dialog.choosing && (dialog.ota.headline || "").length > 0
            radius: Theme.radiusMd
            readonly property color tone: dialog.ota.tone === "fault" ? Theme.stateFault
                                        : dialog.ota.tone === "ok" ? Theme.stateOk
                                        : dialog.ota.tone === "pending" ? Theme.statePending : Theme.accentDefault
            color: dialog.ota.tone === "fault" ? Theme.stateFaultSubtle
                 : dialog.ota.tone === "ok" ? Theme.stateOkSubtle
                 : dialog.ota.tone === "pending" ? Theme.statePendingSubtle : Theme.bgSelected
            border.width: Theme.borderWidth; border.color: tone
            Row {
                x: Theme.space12; anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space8
                DLIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: dialog.ota.tone === "ok" ? "circle-check" : dialog.ota.tone === "fault" ? "circle-alert" : "info"
                    size: Theme.sizeIconSm; color: parent.parent.tone
                }
                Text {
                    id: headline
                    width: column.width - 2 * Theme.space12 - Theme.sizeIconSm - Theme.space8
                    text: dialog.ota.headline || ""
                    wrapMode: Text.Wrap
                    font: Theme.fontBodyStrong; color: Theme.textPrimary
                }
            }
        }
        Text {
            width: parent.width; wrapMode: Text.Wrap
            visible: text.length > 0
            text: dialog.choosing ? "" : (dialog.ota.explanation || "")
            font: Theme.fontBodyDefault; color: Theme.textPrimary
        }
        Column {
            width: parent.width
            spacing: Theme.space4
            InfoRow {
                label: qsTr("Image")
                value: dialog.choosing ? dialog.choice.fileName + (dialog.choice.size ? " · " + dialog.choice.size : "")
                                       : (dialog.ota.fileName || "") + " · " + (dialog.ota.size || "")
            }
            InfoRow {
                label: qsTr("Versions")
                visible: value.length > 0
                value: dialog.choosing ? (dialog.choice.versions || "") : (dialog.ota.versions || "")
            }
            InfoRow {
                label: qsTr("SHA-256"); mono: true
                visible: value.length > 0
                value: dialog.choosing ? (dialog.choice.sha || "") : (dialog.ota.sha || "")
            }
            InfoRow {
                label: qsTr("Built"); visible: dialog.choosing && value.length > 0
                value: dialog.choosing ? [dialog.choice.project || "", dialog.choice.built || ""].filter(t => t.length > 0).join(" · ") : ""
            }
        }
        // Choosing: what is wrong with the image, and the confirmation choice.
        Text {
            objectName: "otaProblem"
            width: parent.width; wrapMode: Text.Wrap
            visible: dialog.choosing && text.length > 0
            text: !dialog.choosing ? ""
                : dialog.choice.ok !== true ? (dialog.choice.problem || "")
                : (dialog.backend.firmware.updateBlocked || "")
            font: Theme.fontBodyDefault; color: Theme.stateFault
        }
        Text {
            width: parent.width; wrapMode: Text.Wrap
            visible: dialog.choosing && dialog.choice.ok === true
            text: (dialog.choice.sameBuild ? qsTr("This build is already running. ") : "")
                + (dialog.backend.unsaved ? qsTr("Unsaved changes are lost at the reboot. ") : "")
                + qsTr("The device writes the inactive slot, reboots once into it and is confirmed after DriveLink checks the running build.")
            font: Theme.fontBodyDefault; color: Theme.textSecondary
        }
        T.CheckBox {
            id: unconfirmed
            objectName: "otaNoConfirm"
            visible: dialog.choosing && dialog.choice.ok === true
            text: qsTr("Install without confirming (a reset rolls back)")
            spacing: Theme.space8
            padding: 0
            leftPadding: indicator.width + spacing
            implicitWidth: implicitContentWidth + leftPadding
            implicitHeight: 20
            indicator: Rectangle {
                y: (unconfirmed.height - height) / 2
                implicitWidth: Theme.sizeIconSm; implicitHeight: Theme.sizeIconSm
                radius: Theme.radiusSm
                color: unconfirmed.checked ? Theme.accentDefault : Theme.bgField
                border.width: unconfirmed.checked ? 0 : Theme.borderWidth
                border.color: Theme.borderStrong
                DLIcon { anchors.centerIn: parent; visible: unconfirmed.checked; name: "check"; size: 12; color: Theme.textOnAccent }
            }
            contentItem: Text {
                verticalAlignment: Text.AlignVCenter
                text: unconfirmed.text
                font: Theme.fontBodyDefault; color: Theme.textPrimary
            }
        }
        // The update: one row per step (G3 CheckItems).
        Column {
            width: parent.width
            spacing: Theme.space4
            visible: !dialog.choosing
            Repeater {
                model: dialog.choosing ? [] : (dialog.ota.steps || [])
                DLCheckItem {
                    required property var modelData
                    width: parent.width
                    label: modelData.label
                    detail: modelData.detail
                    status: modelData.state === "pass" ? DLCheckItem.Pass
                          : modelData.state === "fail" ? DLCheckItem.Fail
                          : modelData.state === "active" ? DLCheckItem.Active : DLCheckItem.Pending
                }
            }
        }
        Column {
            width: parent.width
            spacing: Theme.space8
            visible: !dialog.choosing && (dialog.ota.progress || -1) >= 0
            Item {
                width: parent.width; height: progressLabel.implicitHeight
                Text {
                    id: progressLabel
                    text: dialog.ota.progressLabel || ""
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                Text {
                    anchors.right: parent.right
                    visible: dialog.ota.phase === "writing"
                    text: (dialog.ota.percent || 0) + " %"
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
            }
            DLProgressBar {
                objectName: "otaProgress"
                width: parent.width
                value: dialog.ota.phase === "writing" ? (dialog.ota.progress || 0) : 0
                indeterminate: dialog.ota.phase !== "writing"
            }
        }
        Text {
            objectName: "otaNote"
            width: parent.width; wrapMode: Text.Wrap
            visible: !dialog.choosing && text.length > 0
            text: dialog.ota.note || ""
            font: Theme.fontCaption; color: Theme.textSecondary
        }
        Item {
            width: parent.width
            implicitHeight: actions.implicitHeight
            Row {
                id: actions
                anchors.right: parent.right
                spacing: Theme.space8
                // Choosing
                DLButton {
                    visible: dialog.choosing
                    text: qsTr("Choose another…"); iconName: "folder-open"; kind: DLButton.Ghost
                    onClicked: dialog.chooseAnother()
                }
                DLButton {
                    objectName: "otaCancel"
                    visible: dialog.choosing
                    text: qsTr("Cancel"); kind: DLButton.Secondary
                    onClicked: dialog.cancel()
                }
                DLReasonHover {
                    visible: dialog.choosing
                    reason: dialog.choice.ok !== true ? (dialog.choice.problem || "") : (dialog.backend.firmware.updateBlocked || "")
                    DLButton {
                        objectName: "otaInstall"
                        text: qsTr("Install"); iconName: "upload"; kind: DLButton.Primary
                        enabled: dialog.choice.ok === true && !dialog.backend.firmware.updateBlocked
                        onClicked: dialog.install()
                    }
                }
                // Running or finished
                DLButton {
                    objectName: "otaStopReconnect"
                    visible: !dialog.choosing && dialog.backend.reconnecting
                    text: qsTr("Stop and disconnect"); kind: DLButton.Secondary
                    onClicked: dialog.backend.disconnectDevice()
                }
                DLButton {
                    objectName: "otaAbort"
                    visible: !dialog.choosing && dialog.ota.canAbort === true
                    text: qsTr("Abort"); iconName: "x"; kind: DLButton.Secondary
                    onClicked: dialog.backend.abortUpdate()
                }
                DLButton {
                    objectName: "otaRollBack"
                    visible: !dialog.choosing && dialog.ota.canRollBack === true
                    text: qsTr("Roll back"); iconName: "undo-2"; kind: DLButton.Secondary
                    onClicked: dialog.backend.rollBackFirmware()
                }
                DLButton {
                    objectName: "otaConfirm"
                    visible: !dialog.choosing && dialog.ota.canConfirm === true
                    text: qsTr("Confirm"); iconName: "check"; kind: DLButton.Primary
                    onClicked: dialog.backend.confirmFirmware()
                }
                DLButton {
                    objectName: "otaCopy"
                    visible: !dialog.choosing && dialog.ota.canCopy === true
                    text: qsTr("Copy details"); iconName: "copy"; kind: DLButton.Secondary
                    onClicked: dialog.backend.copyToClipboard(dialog.ota.details || "")
                }
                DLButton {
                    objectName: "otaClose"
                    visible: !dialog.choosing && !dialog.ota.active
                    text: qsTr("Close")
                    kind: dialog.ota.canConfirm === true ? DLButton.Secondary : DLButton.Primary
                    onClicked: { dialog.backend.closeUpdate(); dialog.dismissed() }
                }
            }
        }
    }
}
