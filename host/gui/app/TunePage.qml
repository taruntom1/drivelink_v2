import QtQuick
import QtQuick.Templates as T
import DriveLink.Style

// G5 Tune (H2e-1): Controller and gains on the left (the controller, its
// tune-flagged enums and gains, Revert gains to saved), the toolbar, the runs
// overlaid and their metrics in the centre, Test signal, Capture and Runs on
// the right. Run (F5) needs this window's ARM: while disarmed it is disabled
// with "Arm first". Gain history, A/B, Auto re-run, plant identification and
// session files follow in H2e-2 and H2e-3.
Item {
    id: page
    objectName: "tunePage"

    property var backend
    readonly property var tune: backend.tune
    readonly property var run: tune.run || ({})
    readonly property var testSignal: tune.testSignal || ({})
    readonly property var capture: tune.capture || ({})
    readonly property var plots: tune.plots || []
    readonly property real spanUs: Math.max(1, tune.plotEndUs - tune.plotStartUs)
    // Two gains per row where they fit (G5); one column at 1080p.
    readonly property bool wide: page.width >= 2100
    property bool blocked: false
    // Collapsed gain groups the user opened (prefix -> true).
    property var opened: ({})

    function colorOf(index) { return Theme.seriesColors[Math.max(0, index) % Theme.seriesColors.length] }
    function seriesColor(s) {
        const c = colorOf(s.colour)
        // The setpoint and the older runs are drawn fainter (G5).
        const alpha = (s.faded ? 0.45 : 1.0) * (s.target ? 0.5 : 1.0)
        return Qt.rgba(c.r, c.g, c.b, alpha)
    }
    function timeText(us) {
        const s = us / 1e6
        return Number(s).toLocaleString(Qt.locale("C"), "f", page.spanUs < 2e6 ? 2 : 1) + " s"
    }

    Shortcut {
        sequence: "F5"
        enabled: page.visible && !page.blocked
        onActivated: page.tune.startRun()
    }

    // ---- Controller and gains ----------------------------------------------
    Flickable {
        id: left
        objectName: "tuneLeft"
        x: Theme.space24; y: Theme.space24
        width: page.wide ? 2 * 344 + Theme.space16 + 2 * Theme.space24 : 344 + 2 * Theme.space24
        height: page.height - 2 * Theme.space24
        contentHeight: gainsCard.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Card {
            id: gainsCard
            width: left.width
            title: qsTr("Controller and gains")
            contentSpacing: Theme.space12

            Text { text: qsTr("Controller"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
            DLComboBox {
                objectName: "tuneController"
                width: parent.width
                model: page.tune.controllers
                textRole: "text"
                currentIndex: page.tune.controllers.findIndex(c => c.key === page.tune.controller)
                displayText: currentIndex >= 0 ? page.tune.controllers[currentIndex].text : qsTr("No instance has a test-signal input")
                enabled: page.tune.controllers.length > 0 && !page.run.active
                onActivated: function(index) { page.tune.selectController(page.tune.controllers[index].key) }
            }
            Text {
                text: qsTr("Instances with test-signal inputs only.")
                font: Theme.fontCaption; color: Theme.textSecondary
            }
            Repeater {
                model: page.tune.enums
                delegate: Row {
                    id: enumRow
                    required property var modelData
                    spacing: Theme.space8
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 80; text: enumRow.modelData.name; elide: Text.ElideRight
                        font: Theme.fontBodyDefault; color: Theme.textSecondary
                    }
                    DLComboBox {
                        objectName: "tuneEnum_" + enumRow.modelData.name
                        width: 200
                        model: enumRow.modelData.options
                        currentIndex: enumRow.modelData.index
                        enabled: !enumRow.modelData.sending && !page.run.active
                        onActivated: function(index) { page.tune.setEnum(enumRow.modelData.key, index) }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, gainsCard.width - 2 * Theme.space24 - 296)
                        wrapMode: Text.Wrap
                        text: enumRow.modelData.problem
                        font: Theme.fontCaption; color: Theme.stateFault
                    }
                }
            }
            Flow {
                objectName: "tuneGains"
                width: parent.width
                spacing: Theme.space16
                Repeater {
                    model: page.tune.gains
                    // A header row starts each group on its own line; gains
                    // follow two per row where they fit (G5).
                    delegate: Item {
                        id: gainCell
                        required property string key
                        required property bool header
                        required property string name
                        required property string description
                        required property string unit
                        required property string text
                        required property real position
                        required property var windowLo
                        required property var windowHi
                        required property var saved
                        required property bool hasSaved
                        required property var ticks
                        required property bool zero
                        required property bool sending
                        required property string problem
                        required property string group
                        required property string groupTitle
                        required property bool groupOpen
                        required property string groupNote
                        readonly property bool open: groupOpen || page.opened[group] === true
                        visible: header || open
                        width: header ? parent.width : 344
                        height: !visible ? 0 : header ? Theme.sizeControlSm : control.implicitHeight
                        Item {
                            visible: gainCell.header
                            anchors.fill: parent
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: gainCell.groupTitle; font: Theme.fontLabelCaps; color: Theme.textSecondary
                            }
                            DLButton {
                                objectName: "tuneGroup_" + gainCell.group
                                anchors.right: parent.right
                                height: Theme.sizeControlSm
                                visible: !gainCell.groupOpen
                                kind: DLButton.Ghost
                                text: gainCell.open ? qsTr("Hide") : qsTr("Show")
                                toolTipText: gainCell.groupNote
                                onClicked: { const o = Object.assign({}, page.opened); o[gainCell.group] = !gainCell.open; page.opened = o }
                            }
                        }
                        DLGainControl {
                            id: control
                            objectName: "gain_" + gainCell.name
                            visible: !gainCell.header
                            width: 344
                            // The run records the gains it started with.
                            enabled: !page.run.active
                            name: gainCell.name
                            description: gainCell.description
                            unit: gainCell.unit
                            valueText: gainCell.text
                            position: gainCell.position
                            lowText: gainCell.windowLo === undefined ? "" : page.tune.formatGain(gainCell.windowLo)
                            midText: gainCell.zero ? "" : gainCell.text
                            highText: gainCell.windowHi === undefined ? "" : page.tune.formatGain(gainCell.windowHi)
                            ticks: (gainCell.ticks || []).map(t => ({position: t.position,
                                color: t.colour < 0 ? Theme.textPrimary : Qt.rgba(page.colorOf(t.colour).r, page.colorOf(t.colour).g, page.colorOf(t.colour).b, t.faded ? 0.5 : 1)}))
                            savedText: gainCell.hasSaved ? page.tune.formatGain(gainCell.saved) : ""
                            zero: gainCell.zero
                            sending: gainCell.sending
                            problem: gainCell.problem
                            onSliderPressed: page.tune.gainPress(gainCell.key)
                            onSliderMoved: function(position) { page.tune.gainDrag(gainCell.key, position) }
                            onSliderReleased: page.tune.gainRelease(gainCell.key)
                            onValueTyped: function(text) { page.tune.gainText(gainCell.key, text) }
                            onStepped: function(factor) { page.tune.gainStep(gainCell.key, factor) }
                            onKeyStepped: function(up, shift, ctrl) { page.tune.gainKey(gainCell.key, up, shift, ctrl) }
                            onWindowTyped: function(low, high) { page.tune.setWindow(gainCell.key, low, high) }
                        }
                    }
                }
            }
            Text {
                visible: page.tune.controller.length > 0 && page.tune.gainCount === 0
                width: parent.width; wrapMode: Text.Wrap
                text: qsTr("This instance marks no params for Tune (the definitions' tune flag).")
                font: Theme.fontCaption; color: Theme.textSecondary
            }
            DLReasonHover {
                reason: (page.tune.revert || {}).reason || ""
                DLButton {
                    objectName: "tuneRevertGains"
                    iconName: "undo-2"
                    text: qsTr("Revert gains to saved")
                    enabled: (page.tune.revert || {}).enabled === true
                    onClicked: page.tune.revertGains()
                }
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: qsTr("Gains apply live: Enter, a step, a key or a slider drag. Save and Revert are in the status bar.")
                font: Theme.fontCaption; color: Theme.textSecondary
            }
        }
    }

    // ---- Right column: test signal, capture, runs -----------------------------
    Flickable {
        id: right
        objectName: "tuneRight"
        anchors.right: parent.right; anchors.rightMargin: Theme.space24
        y: Theme.space24
        width: Math.min(460, Math.max(360, page.width * 0.2))
        height: page.height - 2 * Theme.space24
        contentHeight: rightColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: rightColumn
            width: right.width
            spacing: Theme.space24

            Card {
                objectName: "tuneSignal"
                width: parent.width
                title: qsTr("Test signal")
                contentSpacing: Theme.space12
                Item {
                    width: parent.width; height: Theme.sizeControl
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Target"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
                    DLComboBox {
                        objectName: "tuneTarget"
                        anchors.right: parent.right; width: parent.width * 0.62
                        model: page.tune.targets
                        textRole: "name"
                        currentIndex: page.tune.targets.findIndex(t => t.key === page.testSignal.target)
                        displayText: currentIndex >= 0 ? page.tune.targets[currentIndex].name + (page.tune.targets[currentIndex].unit.length > 0 ? " (" + page.tune.targets[currentIndex].unit + ")" : "") : ""
                        enabled: !page.run.active
                        onActivated: function(index) { page.tune.setTarget(page.tune.targets[index].key) }
                    }
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space4
                    Repeater {
                        model: page.tune.waveforms
                        delegate: DLTabButton {
                            required property string modelData
                            required property int index
                            objectName: "tuneWaveform_" + index
                            text: modelData
                            checkable: true
                            checked: page.testSignal.waveform === index
                            enabled: !page.run.active
                            onClicked: page.tune.setWaveform(index)
                        }
                    }
                }
                Repeater {
                    model: [
                        {field: "offset", label: qsTr("Offset"), unit: page.testSignal.unit, shown: true},
                        {field: "amplitude", label: qsTr("Amplitude"), unit: page.testSignal.unit, shown: true},
                        {field: "f0", label: page.testSignal.chirp ? qsTr("Start frequency") : qsTr("Frequency"), unit: "Hz", shown: page.testSignal.usesFrequency === true},
                        {field: "f1", label: qsTr("End frequency"), unit: "Hz", shown: page.testSignal.chirp === true},
                        {field: "duration", label: qsTr("Duration"), unit: "s", shown: true},
                        {field: "repeat", label: qsTr("Repeat"), unit: "×", shown: true}]
                    delegate: Item {
                        id: fieldRow
                        required property var modelData
                        visible: modelData.shown
                        width: parent.width; height: visible ? Theme.sizeControl : 0
                        Text { anchors.verticalCenter: parent.verticalCenter; text: fieldRow.modelData.label; font: Theme.fontBodyDefault; color: Theme.textSecondary }
                        DLNumberField {
                            id: signalField
                            objectName: "tuneSignal_" + fieldRow.modelData.field
                            anchors.right: parent.right; width: 160
                            unit: fieldRow.modelData.unit
                            readOnly: page.run.active
                            readonly property string current: String(page.testSignal[fieldRow.modelData.field] === undefined ? "" : page.testSignal[fieldRow.modelData.field])
                            onAccepted: page.tune.setSignalField(fieldRow.modelData.field, text)
                            onEditingFinished: Qt.callLater(() => text = current)
                        }
                        Binding { target: signalField; property: "text"; value: signalField.current; when: !signalField.activeFocus }
                    }
                }
                Text {
                    objectName: "tuneSignalProblem"
                    width: parent.width; wrapMode: Text.Wrap
                    visible: text.length > 0
                    text: page.testSignal.problem || ""
                    font: Theme.fontCaption; color: Theme.stateFault
                }
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    text: page.testSignal.holds || ""
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
            }

            Card {
                objectName: "tuneCapture"
                width: parent.width
                title: qsTr("Capture")
                contentSpacing: Theme.space12
                Flow {
                    width: parent.width
                    spacing: Theme.space8
                    Repeater {
                        model: page.capture.refs || []
                        delegate: Rectangle {
                            id: refChip
                            required property var modelData
                            height: Theme.sizeControlSm; width: refRow.implicitWidth + Theme.space8
                            radius: Theme.radiusSm; color: Theme.bgSecondary
                            Row {
                                id: refRow
                                x: Theme.space8; height: parent.height; spacing: Theme.space4
                                Text { anchors.verticalCenter: parent.verticalCenter; text: refChip.modelData.name; font: Theme.fontMonoSmall; color: Theme.textPrimary }
                                DLIconButton {
                                    anchors.verticalCenter: parent.verticalCenter
                                    size: DLIconButton.Sm; iconName: "x"; enabled: !page.run.active
                                    toolTipText: qsTr("Do not capture")
                                    onClicked: page.tune.removeCaptureRef(refChip.modelData.key)
                                }
                            }
                        }
                    }
                    DLComboBox {
                        objectName: "tuneCaptureAdd"
                        width: 160; height: Theme.sizeControlSm
                        readonly property var choices: (page.capture.candidates || []).filter(c => c.enabled)
                        model: choices.map(c => c.owner + "." + c.name)
                        displayText: qsTr("+ add")
                        currentIndex: -1
                        enabled: choices.length > 0 && !page.run.active
                        onActivated: function(index) { page.tune.addCaptureRef(choices[index].key) }
                    }
                }
                Item {
                    width: parent.width; height: Theme.sizeControl
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Rate"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
                    DLComboBox {
                        objectName: "tuneCaptureRate"
                        anchors.right: parent.right; width: parent.width * 0.62
                        model: page.capture.rates || []
                        textRole: "text"
                        currentIndex: page.capture.rateIndex === undefined ? -1 : page.capture.rateIndex
                        enabled: !page.run.active
                        onActivated: function(index) { page.tune.setDivider(page.capture.rates[index].divider) }
                    }
                }
                Item {
                    width: parent.width; height: Theme.sizeControl
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Length"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
                    Row {
                        anchors.right: parent.right
                        spacing: Theme.space8
                        DLNumberField {
                            id: samplesField
                            objectName: "tuneCaptureSamples"
                            width: 130; unit: qsTr("samples"); readOnly: page.run.active
                            onAccepted: page.tune.setSamples(text, preField.text)
                            onEditingFinished: Qt.callLater(() => text = String(page.capture.samples))
                        }
                        DLNumberField {
                            id: preField
                            objectName: "tuneCapturePre"
                            width: 130; unit: qsTr("before"); readOnly: page.run.active
                            onAccepted: page.tune.setSamples(samplesField.text, text)
                            onEditingFinished: Qt.callLater(() => text = String(page.capture.pre))
                        }
                    }
                    Binding { target: samplesField; property: "text"; value: String(page.capture.samples); when: !samplesField.activeFocus }
                    Binding { target: preField; property: "text"; value: String(page.capture.pre); when: !preField.activeFocus }
                }
                Item {
                    width: parent.width; height: Theme.sizeControlSm
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Trigger"); font: Theme.fontBodyDefault; color: Theme.textSecondary }
                    Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: qsTr("test signal start"); font: Theme.fontBodyDefault; color: Theme.textPrimary }
                }
                Text {
                    objectName: "tuneCaptureUsage"
                    width: parent.width; wrapMode: Text.Wrap
                    text: [page.capture.usage || "", page.capture.note || ""].filter(t => t.length > 0).join(" · ")
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                Text {
                    objectName: "tuneCaptureProblem"
                    width: parent.width; wrapMode: Text.Wrap
                    visible: text.length > 0
                    text: page.capture.problem || ""
                    font: Theme.fontCaption; color: Theme.stateFault
                }
            }

            Card {
                objectName: "tuneRuns"
                width: parent.width
                title: qsTr("Runs")
                contentSpacing: Theme.space8
                Text {
                    visible: page.tune.runCount === 0
                    text: qsTr("No runs yet")
                    font: Theme.fontBodyDefault; color: Theme.textTertiary
                }
                Repeater {
                    model: page.tune.runs
                    delegate: Item {
                        id: runRow
                        required property int number
                        required property int colour
                        required property string time
                        required property string changed
                        required property string end
                        required property bool pinned
                        required property bool shown
                        required property bool faded
                        required property bool irregular
                        objectName: "tuneRunRow_" + number
                        width: parent.width; height: Theme.sizeControl
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.space8
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 10; height: 10; radius: 2
                                color: page.colorOf(runRow.colour)
                                opacity: runRow.shown ? (runRow.faded ? 0.5 : 1) : 0.2
                            }
                            Text { anchors.verticalCenter: parent.verticalCenter; width: 52; text: qsTr("run %1").arg(runRow.number); font: Theme.fontBodyDefault; color: Theme.textPrimary }
                            Text { anchors.verticalCenter: parent.verticalCenter; width: 64; text: runRow.time; font: Theme.fontBodyNumeric; color: Theme.textSecondary }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: runRow.width - 52 - 64 - 10 - 4 * Theme.space8 - menuButton.width
                                elide: Text.ElideRight
                                // Irregular: some tick came late, so the samples' spacing may not hold.
                                text: (runRow.pinned ? qsTr("pinned · ") : "") + runRow.changed
                                      + (runRow.end.length > 0 ? " · " + runRow.end : "") + (runRow.irregular ? qsTr(" · irregular timing") : "")
                                font: Theme.fontBodyDefault
                                color: runRow.end.length > 0 || runRow.irregular ? Theme.stateArmed : Theme.textPrimary
                            }
                        }
                        DLIconButton {
                            id: menuButton
                            objectName: "tuneRunMenu_" + runRow.number
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            size: DLIconButton.Sm; iconName: "ellipsis"
                            toolTipText: qsTr("Pin or remove")
                            onClicked: runMenu.open()
                            DLMenu {
                                id: runMenu
                                y: menuButton.height + Theme.space4
                                DLMenuItem {
                                    objectName: "tunePin_" + runRow.number
                                    text: runRow.pinned ? qsTr("Unpin") : qsTr("Pin: keep it on the plot")
                                    onTriggered: page.tune.pinRun(runRow.number, !runRow.pinned)
                                }
                                DLMenuItem {
                                    objectName: "tuneRemove_" + runRow.number
                                    text: qsTr("Remove")
                                    onTriggered: page.tune.removeRun(runRow.number)
                                }
                            }
                        }
                    }
                }
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    text: qsTr("Overlay: the newest run in full, the two before it faded, plus pinned runs. Runs stay until the device changes.")
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
            }
        }
    }

    // ---- Centre: toolbar, plots, metrics ------------------------------------
    Item {
        id: centre
        objectName: "tuneCentre"
        anchors.left: left.right; anchors.leftMargin: Theme.space24
        anchors.right: right.left; anchors.rightMargin: Theme.space24
        y: Theme.space24
        height: page.height - 2 * Theme.space24

        Flow {
            id: toolbar
            objectName: "tuneToolbar"
            width: parent.width
            spacing: Theme.space8
            DLReasonHover {
                reason: page.run.blocked || ""
                DLButton {
                    objectName: "tuneRun"
                    kind: DLButton.Primary
                    iconName: "play"
                    text: qsTr("Run  F5")
                    enabled: page.run.canRun === true
                    onClicked: page.tune.startRun()
                }
            }
            DLButton {
                objectName: "tuneStop"
                iconName: "square"
                text: page.run.stopping ? qsTr("Stopping") : qsTr("Stop")
                enabled: page.run.active === true && !page.run.stopping
                onClicked: page.tune.stopRun()
            }
            Text {
                objectName: "tuneRunStatus"
                height: Theme.sizeControl; verticalAlignment: Text.AlignVCenter
                text: page.run.active ? page.run.text
                    : page.tune.runCount === 0 ? (page.run.canRun ? qsTr("Ready: set the test signal and press Run") : page.run.blocked)
                    : qsTr("%1 runs").arg(page.tune.runCount)
                font: Theme.fontBodyDefault
                color: page.run.active ? Theme.stateArmed : Theme.textSecondary
            }
            DLProgressBar {
                y: (Theme.sizeControl - height) / 2
                width: 120
                visible: page.run.active === true && page.run.progress >= 0
                value: Math.max(0, page.run.progress || 0)
            }
            Repeater {
                model: page.tune.chips
                delegate: DLFilterChip {
                    required property var modelData
                    objectName: "tuneChip_" + modelData.name
                    y: (Theme.sizeControl - height) / 2
                    text: modelData.name
                    checked: modelData.checked
                    onClicked: page.tune.toggleChip(modelData.key)
                }
            }
        }

        Column {
            id: plotStack
            objectName: "tunePlots"
            anchors.top: toolbar.bottom; anchors.topMargin: Theme.space12
            anchors.bottom: metrics.top; anchors.bottomMargin: Theme.space24
            width: parent.width
            spacing: Theme.space8

            Rectangle {
                visible: page.plots.length === 0
                width: parent.width; height: parent.height
                color: Theme.bgSurface; radius: Theme.radiusMd
                border.width: Theme.borderWidth; border.color: Theme.borderDefault
                Text {
                    anchors.centerIn: parent
                    width: parent.width * 0.6
                    horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                    text: page.tune.runCount === 0 ? qsTr("No runs yet. Arm, set the test signal and press Run (F5).")
                                                   : qsTr("Check a captured signal above to plot it")
                    font: Theme.fontBodyDefault; color: Theme.textTertiary
                }
            }
            Repeater {
                model: page.plots
                delegate: Rectangle {
                    id: plotFrame
                    required property var modelData
                    required property int index
                    objectName: "tunePlot_" + index
                    width: plotStack.width
                    height: (plotStack.height - plotStack.spacing * (page.plots.length - 1) - timeAxis.height) / Math.max(1, page.plots.length)
                    color: Theme.bgSurface; radius: Theme.radiusMd
                    border.width: Theme.borderWidth; border.color: Theme.borderDefault

                    Flow {
                        id: legend
                        x: Theme.space12; y: Theme.space8
                        width: parent.width - 2 * Theme.space12
                        spacing: Theme.space8
                        Text {
                            height: Theme.sizeControlSm; verticalAlignment: Text.AlignVCenter
                            text: plotFrame.modelData.title + (plotFrame.modelData.unit.length > 0 ? " (" + plotFrame.modelData.unit + ")" : "")
                            font: Theme.fontBodyStrong; color: Theme.textPrimary
                        }
                        Repeater {
                            model: plotFrame.modelData.legend
                            delegate: Rectangle {
                                id: legendChip
                                required property var modelData
                                height: Theme.sizeControlSm; width: legendRow.implicitWidth + 2 * Theme.space8
                                radius: Theme.radiusSm; color: Theme.bgSecondary
                                Row {
                                    id: legendRow
                                    x: Theme.space8; height: parent.height; spacing: Theme.space8
                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter; width: 10; height: 10; radius: 2
                                        color: page.colorOf(legendChip.modelData.colour); opacity: legendChip.modelData.faded ? 0.5 : 1
                                    }
                                    Text { anchors.verticalCenter: parent.verticalCenter; text: legendChip.modelData.text; font: Theme.fontMonoSmall; color: Theme.textPrimary }
                                }
                            }
                        }
                    }
                    Rectangle {
                        id: area
                        anchors.left: parent.left; anchors.leftMargin: 64
                        anchors.right: parent.right; anchors.rightMargin: Theme.space12
                        anchors.top: legend.bottom; anchors.topMargin: Theme.space8
                        anchors.bottom: parent.bottom; anchors.bottomMargin: Theme.space12
                        color: Theme.plotBg
                        border.width: Theme.borderWidth; border.color: Theme.plotAxis
                        clip: true
                        Repeater {
                            model: plot.ticks
                            delegate: Rectangle { required property var modelData; y: modelData.y; width: area.width; height: 1; color: Theme.plotGrid }
                        }
                        Repeater {
                            model: 4
                            delegate: Rectangle { required property int index; x: area.width * (index + 1) / 5; width: 1; height: area.height; color: Theme.plotGrid }
                        }
                        // The trigger (the signal's start).
                        Rectangle {
                            x: (0 - page.tune.plotStartUs) / page.spanUs * area.width
                            width: 1; height: area.height
                            color: Theme.plotAxis
                        }
                        PlotItem {
                            id: plot
                            objectName: "tunePlotItem_" + plotFrame.index
                            anchors.fill: parent
                            source: page.tune
                            keys: plotFrame.modelData.series.map(s => s.key)
                            colors: plotFrame.modelData.series.map(s => page.seriesColor(s))
                        }
                        // Where a Stop or a disarm cut a run.
                        Repeater {
                            model: plotFrame.modelData.markers
                            delegate: Item {
                                id: marker
                                required property var modelData
                                x: (modelData.us - page.tune.plotStartUs) / page.spanUs * area.width
                                height: area.height
                                Rectangle { width: 2; height: parent.height; color: Theme.stateArmed }
                                Text {
                                    x: Theme.space4; y: Theme.space4
                                    text: marker.modelData.text
                                    font: Theme.fontLabelCaps; color: Theme.stateArmed
                                }
                            }
                        }
                    }
                    Repeater {
                        model: plot.ticks
                        delegate: Text {
                            required property var modelData
                            x: 0; width: area.x - Theme.space8
                            y: area.y + modelData.y - height / 2
                            visible: modelData.y >= 0 && modelData.y <= area.height
                            horizontalAlignment: Text.AlignRight
                            text: modelData.text
                            font: Theme.fontMonoSmall; color: Theme.textSecondary
                        }
                    }
                }
            }
            Item {
                id: timeAxis
                objectName: "tuneTimeAxis"
                visible: page.plots.length > 0
                x: 64; width: parent.width - 64 - Theme.space12
                height: visible ? Theme.sizeControlSm : 0
                Repeater {
                    model: 6
                    delegate: Text {
                        required property int index
                        x: timeAxis.width * index / 5 - (index === 5 ? width : index === 0 ? 0 : width / 2)
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.timeText(page.tune.plotStartUs + page.spanUs * index / 5)
                        font: Theme.fontMonoSmall; color: Theme.textSecondary
                    }
                }
            }
        }

        Card {
            id: metrics
            objectName: "tuneMetrics"
            anchors.bottom: parent.bottom
            width: parent.width
            title: qsTr("Run metrics")
            contentSpacing: Theme.space8
            readonly property var columns: [qsTr("Run"), qsTr("Rise"), qsTr("Overshoot"), qsTr("Settling 2 %"), qsTr("Steady error"), qsTr("Response")]
            Row {
                width: parent.width
                Repeater {
                    model: metrics.columns
                    delegate: Text {
                        required property string modelData
                        required property int index
                        width: metrics.width / 6 - Theme.space8
                        horizontalAlignment: index === 0 ? Text.AlignLeft : Text.AlignRight
                        text: modelData; font: Theme.fontLabelCaps; color: Theme.textSecondary
                    }
                }
            }
            Text {
                visible: page.tune.runCount === 0
                text: qsTr("Metrics appear for step runs: rise 10–90 %, overshoot, settling within 2 %, steady-state error.")
                width: parent.width; wrapMode: Text.Wrap
                font: Theme.fontCaption; color: Theme.textTertiary
            }
            Repeater {
                model: page.tune.runs
                delegate: Row {
                    id: metricRow
                    required property int number
                    required property int colour
                    required property bool shown
                    required property bool faded
                    required property string rise
                    required property string overshoot
                    required property string settling
                    required property string steadyError
                    required property string response
                    required property string metricsNote
                    visible: shown
                    objectName: "tuneMetricRow_" + number
                    width: parent.width; height: visible ? Theme.sizeControlSm : 0
                    Repeater {
                        model: [qsTr("run %1").arg(metricRow.number), metricRow.rise, metricRow.overshoot, metricRow.settling, metricRow.steadyError, metricRow.response]
                        delegate: Text {
                            required property string modelData
                            required property int index
                            width: metrics.width / 6 - Theme.space8
                            horizontalAlignment: index === 0 ? Text.AlignLeft : Text.AlignRight
                            text: modelData
                            elide: Text.ElideRight
                            font: index === 0 ? Theme.fontBodyDefault : Theme.fontBodyNumeric
                            color: index === 0 ? page.colorOf(metricRow.colour) : Theme.textPrimary
                            opacity: metricRow.faded ? 0.7 : 1
                            DLToolTip { visible: index === 1 && metricRow.metricsNote.length > 0 && hover.hovered; text: metricRow.metricsNote }
                            HoverHandler { id: hover }
                        }
                    }
                }
            }
        }
    }
}
