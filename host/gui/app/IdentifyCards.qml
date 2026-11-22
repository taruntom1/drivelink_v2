import QtQuick
import DriveLink.Style

// G5 Tune · Plant identification (H2e-3): the right column's three cards for
// `connection.tune.identify`: 1 Check (the operator's confirmation and the
// warnings), 2 Run the step (input, mode, from, to, captured seconds, Run and
// Stop) and 3 Model and suggested gains (the fit, the suggestion table, Apply).
// Every word about controllers, inputs and modes comes from the backend.
Column {
    id: cards

    required property var identify
    readonly property var availability: identify.availability || ({})
    readonly property var step: identify.step || ({})
    readonly property var run: identify.run || ({})
    readonly property var result: identify.result || ({})
    readonly property var suggestion: identify.suggestion || ({})
    readonly property var inputs: identify.inputs || []
    readonly property bool available: availability.available === true
    readonly property bool locked: run.active === true

    spacing: Theme.space24

    // A number field that sends on Enter or focus loss and shows the refusal.
    component StepField: Item {
        id: row
        required property var identify
        required property var stepInfo
        required property bool locked
        required property string field
        required property string label
        property string unit
        readonly property string current: String(stepInfo[field] === undefined ? "" : stepInfo[field])
        property string error
        width: parent.width
        height: Theme.sizeControl + (error.length > 0 ? errorText.implicitHeight + Theme.space4 : 0)

        Text {
            y: (Theme.sizeControl - height) / 2
            width: parent.width - fieldInput.width - Theme.space8
            text: row.label; elide: Text.ElideRight; textFormat: Text.PlainText
            font: Theme.fontBodyDefault; color: Theme.textSecondary
        }
        DLNumberField {
            id: fieldInput
            objectName: "identifyField_" + row.field
            anchors.right: parent.right; width: 160
            unit: row.unit
            readOnly: row.locked
            status: row.error.length > 0 ? DLNumberField.Rejected : DLNumberField.Normal
            onEditingFinished: {
                if (row.locked) return
                if (text === row.current && row.error.length === 0) return
                row.error = row.identify.setField(row.field, text)
                if (row.error.length === 0) Qt.callLater(() => text = row.current)
            }
            onTextEdited: row.error = ""
        }
        Binding { target: fieldInput; property: "text"; value: row.current; when: !fieldInput.activeFocus && row.error.length === 0 }
        Text {
            id: errorText
            objectName: "identifyFieldError_" + row.field
            y: Theme.sizeControl + Theme.space4
            width: parent.width; wrapMode: Text.Wrap
            visible: row.error.length > 0
            text: row.error
            font: Theme.fontCaption; color: Theme.stateFault
        }
    }

    // ---- 1 · Check -----------------------------------------------------------
    Card {
        objectName: "identifyCheck"
        width: parent.width
        title: qsTr("1 · Check")
        contentSpacing: Theme.space12
        Text {
            objectName: "identifyUnavailable"
            visible: !cards.available
            width: parent.width; wrapMode: Text.Wrap
            text: cards.availability.reason || ""
            textFormat: Text.PlainText
            font: Theme.fontBodyDefault; color: Theme.textSecondary
        }
        Repeater {
            model: cards.available ? cards.identify.checks : []
            delegate: Column {
                id: checkRow
                required property var modelData
                required property int index
                width: parent.width
                spacing: Theme.space8
                DLCheckItem {
                    objectName: "identifyCheck_" + checkRow.index
                    width: parent.width
                    status: checkRow.modelData.status
                    label: checkRow.modelData.label
                    detail: checkRow.modelData.detail
                    tag: checkRow.modelData.tag
                }
                // The operator's confirmation of the first check.
                DLSwitch {
                    objectName: "identifyConfirm"
                    visible: checkRow.index === 0
                    x: Theme.sizeIconMd + Theme.space8
                    text: qsTr("Confirmed")
                    checked: cards.identify.confirmed === true
                    enabled: !cards.locked
                    onToggled: {
                        cards.identify.confirmed = checked
                        checked = Qt.binding(() => cards.identify.confirmed === true)
                    }
                }
            }
        }
    }

    // ---- 2 · Run the step -----------------------------------------------------
    Card {
        objectName: "identifyStep"
        visible: cards.available
        width: parent.width
        title: qsTr("2 · Run the step")
        contentSpacing: Theme.space12
        actions: [
            DLBadge {
                objectName: "identifyDone"
                visible: cards.run.done === true
                size: DLBadge.Sm; tone: DLBadge.Ok
                text: qsTr("done")
            }
        ]
        Item {
            visible: cards.inputs.length > 1
            width: parent.width; height: visible ? Theme.sizeControl : 0
            Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Step input"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
            DLComboBox {
                objectName: "identifyInput"
                anchors.right: parent.right; width: parent.width * 0.62
                model: cards.inputs
                textRole: "name"
                currentIndex: cards.inputs.findIndex(i => i.key === cards.step.input)
                enabled: !cards.locked
                onActivated: function(index) { cards.identify.setInput(cards.inputs[index].key) }
            }
        }
        Item {
            width: parent.width; height: Theme.sizeControl
            Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Mode during the step"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
            DLComboBox {
                objectName: "identifyMode"
                anchors.right: parent.right; width: parent.width * 0.5
                model: cards.identify.modes
                currentIndex: cards.step.mode === undefined ? -1 : cards.step.mode
                enabled: !cards.locked
                onActivated: function(index) { cards.identify.setMode(index) }
            }
        }
        StepField {
            identify: cards.identify; stepInfo: cards.step; locked: cards.locked
            field: "from"
            label: qsTr("%1 from").arg(cards.step.inputName || "")
            unit: cards.step.inputUnit || ""
        }
        StepField {
            identify: cards.identify; stepInfo: cards.step; locked: cards.locked
            field: "to"
            label: qsTr("%1 to").arg(cards.step.inputName || "")
            unit: cards.step.inputUnit || ""
        }
        StepField {
            identify: cards.identify; stepInfo: cards.step; locked: cards.locked
            field: "seconds"
            label: qsTr("Captured")
            unit: "s"
        }
        Text {
            objectName: "identifyStepProblem"
            visible: text.length > 0
            width: parent.width; wrapMode: Text.Wrap
            text: cards.step.problem || ""
            textFormat: Text.PlainText
            font: Theme.fontCaption; color: Theme.stateFault
        }
        Text {
            objectName: "identifyCapture"
            visible: text.length > 0
            width: parent.width; wrapMode: Text.Wrap
            text: cards.step.capture || ""
            textFormat: Text.PlainText
            font: Theme.fontCaption; color: Theme.textSecondary
        }
        Flow {
            width: parent.width
            spacing: Theme.space8
            DLReasonHover {
                visible: !cards.locked
                reason: cards.run.blocked || ""
                DLButton {
                    objectName: "identifyRun"
                    kind: DLButton.Primary
                    iconName: "play"
                    text: qsTr("Run step")
                    enabled: cards.run.canRun === true
                    onClicked: cards.identify.startStep()
                }
            }
            DLButton {
                objectName: "identifyStop"
                visible: cards.locked
                iconName: "square"
                text: cards.run.stopping ? qsTr("Stopping") : qsTr("Stop")
                enabled: !cards.run.stopping
                onClicked: cards.identify.stopStep()
            }
        }
        Text {
            objectName: "identifyBlocked"
            visible: !cards.locked && cards.run.canRun !== true && text.length > 0
            width: parent.width; wrapMode: Text.Wrap
            text: cards.run.blocked || ""
            textFormat: Text.PlainText
            font: Theme.fontCaption; color: Theme.textSecondary
        }
        Text {
            objectName: "identifyRunText"
            visible: cards.locked
            width: parent.width; wrapMode: Text.Wrap
            text: cards.run.text || ""
            textFormat: Text.PlainText
            font: Theme.fontBodyDefault; color: Theme.stateArmed
        }
        DLProgressBar {
            objectName: "identifyProgress"
            visible: cards.locked
            width: parent.width
            indeterminate: !(cards.run.progress >= 0)
            value: Math.max(0, cards.run.progress || 0)
        }
    }

    // ---- 3 · Model and suggested gains ------------------------------------------
    Card {
        id: modelCard
        objectName: "identifyModel"
        visible: cards.available && (cards.result.available === true || cards.result.fitting === true)
        width: parent.width
        title: qsTr("3 · Model and suggested gains")
        contentSpacing: Theme.space12
        readonly property var responses: cards.result.responses || []
        readonly property var gains: cards.suggestion.gains || []

        Text {
            objectName: "identifyFitting"
            visible: cards.result.fitting === true
            text: qsTr("Fitting…")
            font: Theme.fontBodyDefault; color: Theme.textSecondary
        }
        Item {
            visible: cards.result.fitting !== true && modelCard.responses.length > 1
            width: parent.width; height: visible ? Theme.sizeControl : 0
            Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Response"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
            DLComboBox {
                objectName: "identifyResponse"
                anchors.right: parent.right; width: parent.width * 0.62
                model: modelCard.responses
                currentIndex: cards.result.response === undefined ? -1 : cards.result.response
                onActivated: function(index) { cards.identify.setResponse(index) }
            }
        }
        Column {
            visible: cards.result.fitting !== true
            width: parent.width
            spacing: Theme.space4
            Repeater {
                model: [
                    {label: qsTr("Gain K"), value: ((cards.result.gain || "") + " " + (cards.result.gainUnit || "")).trim(), quality: false},
                    {label: qsTr("Time constant T"), value: cards.result.timeConstant || "", quality: false},
                    {label: qsTr("Dead time L"), value: cards.result.deadTime || "", quality: false},
                    {label: qsTr("Fit error"), value: cards.result.error || "", quality: true}]
                delegate: Item {
                    id: fitRow
                    required property var modelData
                    objectName: "identifyFit_" + modelData.label
                    width: parent.width; height: Theme.sizeControlSm
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * 0.4
                        text: fitRow.modelData.label
                        font: Theme.fontCaption; color: Theme.textSecondary
                    }
                    Row {
                        x: parent.width * 0.4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.space8
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: fitRow.modelData.value
                            textFormat: Text.PlainText
                            font: Theme.fontBodyNumeric; color: Theme.textPrimary
                        }
                        DLBadge {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: fitRow.modelData.quality && (cards.result.qualityText || "").length > 0
                            size: DLBadge.Sm
                            tone: cards.result.quality === 0 ? DLBadge.Ok : cards.result.quality === 1 ? DLBadge.Pending : DLBadge.Fault
                            text: cards.result.qualityText || ""
                        }
                    }
                }
            }
        }
        Text {
            objectName: "identifyFitProblem"
            visible: cards.result.fitting !== true && text.length > 0
            width: parent.width; wrapMode: Text.Wrap
            text: cards.result.problem || ""
            textFormat: Text.PlainText
            font: Theme.fontCaption; color: Theme.stateFault
        }
        Text {
            objectName: "identifyFitEnd"
            visible: cards.result.fitting !== true && text.length > 0
            width: parent.width; wrapMode: Text.Wrap
            text: cards.result.end || ""
            textFormat: Text.PlainText
            font: Theme.fontCaption; color: Theme.stateArmed
        }

        Column {
            visible: cards.result.fitting !== true && cards.result.available === true
            width: parent.width
            spacing: Theme.space12

            Text { text: qsTr("SUGGESTED GAINS"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
            Flow {
                width: parent.width
                spacing: Theme.space4
                Repeater {
                    model: cards.identify.speeds
                    delegate: DLTabButton {
                        required property string modelData
                        required property int index
                        objectName: "identifySpeed_" + index
                        text: modelData
                        checkable: false
                        checked: cards.suggestion.speed === index
                        onClicked: cards.identify.setSpeed(index)
                    }
                }
            }
            Text {
                objectName: "identifyClosedLoop"
                visible: text.length > 0
                width: parent.width; wrapMode: Text.Wrap
                text: cards.suggestion.closedLoop || ""
                textFormat: Text.PlainText
                font: Theme.fontCaption; color: Theme.textSecondary
            }
            Column {
                id: gainTable
                objectName: "identifyGains"
                visible: modelCard.gains.length > 0
                width: parent.width
                spacing: 0
                readonly property real nameWidth: width * 0.34
                readonly property real valueWidth: (width - nameWidth) / 2
                Row {
                    height: Theme.sizeControlSm
                    Text { width: gainTable.nameWidth; height: parent.height; verticalAlignment: Text.AlignVCenter; text: qsTr("Gain"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
                    Text { width: gainTable.valueWidth; height: parent.height; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignRight; text: qsTr("Current"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
                    Text { width: gainTable.valueWidth; height: parent.height; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignRight; text: qsTr("Suggested"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
                }
                Repeater {
                    model: modelCard.gains
                    delegate: Item {
                        id: gainRow
                        required property var modelData
                        objectName: "identifyGain_" + modelData.name
                        width: gainTable.width; height: Theme.sizeControlSm + Theme.space4
                        Rectangle { width: parent.width; height: 1; color: Theme.borderDefault }
                        Text {
                            x: 0; width: gainTable.nameWidth; height: parent.height; verticalAlignment: Text.AlignVCenter
                            text: gainRow.modelData.name; elide: Text.ElideRight; textFormat: Text.PlainText
                            font: Theme.fontMonoSmall; color: Theme.textPrimary
                        }
                        Text {
                            x: gainTable.nameWidth; width: gainTable.valueWidth; height: parent.height
                            verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignRight
                            text: gainRow.modelData.current; textFormat: Text.PlainText
                            font: Theme.fontBodyNumeric; color: Theme.textSecondary
                        }
                        Row {
                            x: gainTable.nameWidth + gainTable.valueWidth
                            width: gainTable.valueWidth; height: parent.height
                            layoutDirection: Qt.RightToLeft
                            spacing: Theme.space8
                            Text {
                                height: parent.height; verticalAlignment: Text.AlignVCenter
                                text: gainRow.modelData.suggested; textFormat: Text.PlainText
                                font: Theme.fontBodyNumeric; color: Theme.textPrimary
                            }
                            Text {
                                objectName: "identifyLimited_" + gainRow.modelData.name
                                visible: gainRow.modelData.clamped === true
                                height: parent.height; verticalAlignment: Text.AlignVCenter
                                text: qsTr("limited")
                                font: Theme.fontCaption; color: Theme.textSecondary
                            }
                        }
                    }
                }
            }
            Text {
                objectName: "identifySuggestionProblem"
                visible: text.length > 0
                width: parent.width; wrapMode: Text.Wrap
                // The fit's own problem is already shown above.
                text: cards.suggestion.problem === cards.result.problem ? "" : (cards.suggestion.problem || "")
                textFormat: Text.PlainText
                font: Theme.fontCaption; color: Theme.stateFault
            }
            Flow {
                width: parent.width
                spacing: Theme.space8
                DLReasonHover {
                    reason: cards.suggestion.canApply === true ? "" : (cards.suggestion.applyReason || "")
                    DLButton {
                        objectName: "identifyApply"
                        kind: DLButton.Primary
                        iconName: "wand-sparkles"
                        text: qsTr("Apply suggested gains")
                        enabled: cards.suggestion.canApply === true
                        onClicked: cards.identify.applySuggested()
                    }
                }
                DLButton {
                    objectName: "identifyBack"
                    text: qsTr("Back to runs")
                    onClicked: cards.identify.shown = false
                }
            }
            Text {
                objectName: "identifyApplyReason"
                visible: cards.suggestion.canApply !== true && text.length > 0
                width: parent.width; wrapMode: Text.Wrap
                // A reason the button's tooltip and the notes above already give.
                text: cards.suggestion.applyReason === cards.suggestion.problem ? "" : (cards.suggestion.applyReason || "")
                textFormat: Text.PlainText
                font: Theme.fontCaption; color: Theme.textSecondary
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: qsTr("Suggested gains are applied live when you press Apply, then show as unsaved like any other edit.")
                font: Theme.fontCaption; color: Theme.textSecondary
            }
        }
    }
}
