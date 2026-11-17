import QtQuick
import QtQuick.Templates as T
import DriveLink.Style

// G5 Monitor: the Signals card (browser by loop and instance, plot rates per
// loop, the link budget), the toolbar (Pause and Space, window, cursors,
// snapshot CSV, add plot), stacked plots on one device-time axis with A–B
// cursors while paused, and the Manual command card (H2d-2). A refused add
// shows its reason and, when one fits, the lower rate in one click; nothing
// changes by itself.
Item {
    id: page
    objectName: "monitorPage"

    property var backend
    readonly property var monitor: backend.monitor
    readonly property var link: monitor.link || ({})
    readonly property var refusal: monitor.refusal || ({})
    readonly property var plots: monitor.plots || []
    readonly property real spanUs: monitor.windowSeconds * 1000000
    readonly property real startUs: monitor.nowUs - spanUs
    property string filter: ""

    function colorOf(index) { return Theme.seriesColors[Math.max(0, index) % Theme.seriesColors.length] }
    function xOf(us, width) { return (us - page.startUs) / page.spanUs * width }
    function togglePause() { page.monitor.setPaused(!page.monitor.paused) }

    // Space pauses and resumes, also after a click on a row or button
    // (the shortcut takes Space before the focused control). A text field
    // keeps it for typing, and nothing under a shell dialog, overlay,
    // drawer or popover reacts.
    property bool blocked: false
    Shortcut {
        sequence: "Space"
        enabled: page.visible && !page.blocked && !(page.Window.activeFocusItem && "cursorPosition" in page.Window.activeFocusItem)
        onActivated: page.togglePause()
    }

    // ---- Signals card ------------------------------------------------------
    Rectangle {
        id: signalsCard
        objectName: "monitorSignals"
        x: Theme.space24; y: Theme.space24
        width: Math.min(360, Math.max(280, page.width * 0.2))
        height: page.height - 2 * Theme.space24
        color: Theme.bgSurface
        radius: Theme.radiusLg
        border.width: Theme.borderWidth
        border.color: Theme.borderDefault

        Column {
            id: signalsHead
            x: Theme.space16; y: Theme.space16
            width: parent.width - 2 * Theme.space16
            spacing: Theme.space12
            Item {
                width: parent.width; height: Theme.sizeControlSm
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Signals"); font: Theme.fontTitleSection; color: Theme.textPrimary
                }
                DLBadge {
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    size: DLBadge.Sm; tone: page.monitor.plotted > 0 ? DLBadge.Accent : DLBadge.Neutral
                    text: qsTr("%1 plotted").arg(page.monitor.plotted)
                }
            }
            DLTextField {
                objectName: "monitorSearch"
                width: parent.width
                placeholderText: qsTr("Search signals")
                onTextChanged: page.filter = text.toLowerCase()
                Keys.onEscapePressed: { text = ""; focus = false }
            }
        }

        Flickable {
            id: browserScroll
            objectName: "monitorBrowser"
            anchors.top: signalsHead.bottom; anchors.topMargin: Theme.space12
            anchors.bottom: budget.top; anchors.bottomMargin: Theme.space12
            x: Theme.space16; width: parent.width - 2 * Theme.space16
            clip: true
            contentHeight: browserColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: browserColumn
                width: browserScroll.width
                spacing: Theme.space4
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    visible: (page.monitor.browser || []).length === 0
                    text: qsTr("No loop is running, so there is nothing to plot.")
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                Repeater {
                    model: page.monitor.browser
                    delegate: Column {
                        id: section
                        required property var modelData
                        readonly property var rows: modelData.rows.filter(r => page.filter.length === 0
                            || r.name.toLowerCase().indexOf(page.filter) >= 0
                            || section.modelData.title.toLowerCase().indexOf(page.filter) >= 0)
                        width: browserColumn.width
                        visible: rows.length > 0
                        spacing: Theme.space2
                        Item {
                            width: parent.width; height: Theme.sizeControlSm
                            Text {
                                id: sectionTitle
                                anchors.verticalCenter: parent.verticalCenter
                                text: section.modelData.title
                                font: section.modelData.isLoop ? Theme.fontBodyStrong : Theme.fontBodyDefault
                                color: Theme.textPrimary
                                leftPadding: section.modelData.isLoop ? 0 : Theme.space12
                            }
                            Text {
                                anchors.left: sectionTitle.right; anchors.leftMargin: Theme.space8
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideRight
                                text: section.modelData.subtitle
                                font: Theme.fontCaption; color: Theme.textTertiary
                            }
                        }
                        Repeater {
                            model: section.rows
                            delegate: T.AbstractButton {
                                id: signalRow
                                required property var modelData
                                objectName: "monitorSignal_" + modelData.key
                                width: section.width
                                implicitHeight: Theme.sizeControlSm
                                leftPadding: section.modelData.isLoop ? Theme.space8 : Theme.space24
                                hoverEnabled: true
                                onClicked: page.monitor.toggleSignal(modelData.key)
                                background: Rectangle {
                                    radius: Theme.radiusSm
                                    color: signalRow.down ? Theme.bgPressed : signalRow.hovered ? Theme.bgHover : "transparent"
                                }
                                contentItem: Item {
                                    Rectangle {
                                        id: box
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 14; height: 14; radius: 3
                                        color: signalRow.modelData.checked ? page.colorOf(signalRow.modelData.color) : "transparent"
                                        border.width: signalRow.modelData.checked ? 0 : Theme.borderWidth
                                        border.color: Theme.borderStrong
                                        DLIcon {
                                            anchors.centerIn: parent
                                            visible: signalRow.modelData.checked
                                            name: "check"; size: 12; color: Theme.plotBg
                                        }
                                    }
                                    Text {
                                        anchors.left: box.right; anchors.leftMargin: Theme.space8
                                        anchors.right: value.left; anchors.rightMargin: Theme.space8
                                        anchors.verticalCenter: parent.verticalCenter
                                        elide: Text.ElideRight
                                        text: signalRow.modelData.name + (signalRow.modelData.input ? qsTr(" (input)") : "")
                                        font: Theme.fontMonoSmall
                                        color: Theme.textPrimary
                                    }
                                    Text {
                                        id: value
                                        objectName: "value"
                                        anchors.right: parent.right; anchors.rightMargin: Theme.space4
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: page.monitor.values[signalRow.modelData.key] || (signalRow.modelData.unit || "")
                                        font: Theme.fontBodyNumeric
                                        color: page.monitor.values[signalRow.modelData.key] ? Theme.textSecondary : Theme.textTertiary
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Column {
            id: budget
            objectName: "monitorBudget"
            x: Theme.space16
            anchors.bottom: parent.bottom; anchors.bottomMargin: Theme.space16
            width: parent.width - 2 * Theme.space16
            spacing: Theme.space8

            // A refused add or rate: the reason and, when one fits, a lower rate.
            Rectangle {
                objectName: "monitorRefusal"
                width: parent.width
                height: refusalColumn.implicitHeight + 2 * Theme.space12
                visible: (page.refusal.reason || "").length > 0
                radius: Theme.radiusMd
                color: Theme.stateFaultSubtle
                Column {
                    id: refusalColumn
                    x: Theme.space12; y: Theme.space12
                    width: parent.width - 2 * Theme.space12
                    spacing: Theme.space8
                    Item {
                        width: parent.width; height: refusalText.implicitHeight
                        Text {
                            id: refusalText
                            objectName: "monitorRefusalText"
                            width: parent.width - Theme.sizeControlSm - Theme.space8
                            wrapMode: Text.Wrap
                            text: page.refusal.reason || ""
                            font: Theme.fontBodyDefault; color: Theme.textPrimary
                        }
                        DLIconButton {
                            anchors.right: parent.right; anchors.top: parent.top
                            size: DLIconButton.Sm; iconName: "x"; toolTipText: qsTr("Dismiss")
                            onClicked: page.monitor.dismissRefusal()
                        }
                    }
                    DLButton {
                        objectName: "monitorSuggestion"
                        visible: (page.refusal.suggestion || "").length > 0
                        text: page.refusal.suggestion || ""
                        onClicked: page.monitor.applySuggestion()
                    }
                }
            }

            Text { text: qsTr("PLOT RATES"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
            Repeater {
                model: page.monitor.rates
                delegate: Item {
                    id: rateRow
                    required property var modelData
                    width: budget.width; height: Theme.sizeControl
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * 0.4; elide: Text.ElideRight
                        text: rateRow.modelData.name
                        font: Theme.fontBodyDefault
                        color: rateRow.modelData.plotted > 0 ? Theme.textPrimary : Theme.textSecondary
                    }
                    DLComboBox {
                        objectName: "monitorRate_" + rateRow.modelData.loop
                        anchors.right: parent.right
                        width: parent.width * 0.6
                        model: rateRow.modelData.choices
                        textRole: "text"
                        currentIndex: rateRow.modelData.index
                        onActivated: function(index) { page.monitor.setRate(rateRow.modelData.loop, rateRow.modelData.choices[index].divider) }
                    }
                }
            }

            Text { text: qsTr("LINK"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
            DLProgressBar {
                objectName: "monitorBudgetBar"
                width: parent.width
                from: 0; to: 1
                value: Math.min(1, page.link.load || 0)
            }
            Text {
                objectName: "monitorBudgetText"
                width: parent.width; wrapMode: Text.Wrap
                text: page.link.text || ""
                font: Theme.fontBodyNumeric
                color: (page.link.load || 0) > 0.8 ? Theme.statePending : Theme.textPrimary
            }
            Text {
                objectName: "monitorGroups"
                text: page.link.total > 0 ? qsTr("Groups %1 of %2 in use").arg(page.link.groups).arg(page.link.total)
                                          : qsTr("Groups: reading the device's list")
                font: Theme.fontCaption; color: Theme.textSecondary
            }
            Text {
                objectName: "monitorLost"
                text: qsTr("Lost frames %1").arg(page.link.lost || 0)
                font: Theme.fontCaption
                color: (page.link.lost || 0) > 0 ? Theme.statePending : Theme.textSecondary
            }
            Text {
                objectName: "monitorNote"
                width: parent.width; wrapMode: Text.Wrap
                visible: text.length > 0
                text: page.link.note || ""
                font: Theme.fontCaption; color: Theme.stateFault
            }
        }
    }

    // ---- Manual command (H2d-2) -------------------------------------------
    // Sets inputs through this window's value groups while it has armed the
    // device. A released slider ramps to 0 (dead-man); Latch keeps the value;
    // Remove and Hold all return inputs to 0. Esc and DISARM stay reachable.
    readonly property var command: backend.command
    property bool adding: false
    property var problems: ({})
    function setProblem(key, text) { const p = Object.assign({}, problems); p[key] = text; problems = p }
    Rectangle {
        id: commandCard
        objectName: "monitorCommand"
        anchors.right: parent.right; anchors.rightMargin: Theme.space24
        y: Theme.space24
        width: 344 + 2 * Theme.space16
        height: Math.min(commandColumn.implicitHeight + 2 * Theme.space16, page.height - 2 * Theme.space24)
        color: Theme.bgSurface
        radius: Theme.radiusLg
        border.width: Theme.borderWidth
        border.color: Theme.borderDefault
        clip: true
        Flickable {
            anchors.fill: parent
            anchors.margins: Theme.space16
            contentHeight: commandColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            Column {
                id: commandColumn
                width: parent.width
                spacing: Theme.space16
                Text { text: qsTr("Manual command"); font: Theme.fontTitleSection; color: Theme.textPrimary }
                Text {
                    width: parent.width; wrapMode: Text.Wrap
                    text: qsTr("Sets inputs through this window's command groups while armed. A released slider ramps back to 0; Latch keeps the value. Remove returns an input to 0.")
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                Text {
                    objectName: "commandBlocked"
                    width: parent.width; wrapMode: Text.Wrap
                    visible: !page.command.enabled && text.length > 0
                    text: page.command.blocked
                    font: Theme.fontCaption; color: Theme.textSecondary
                }
                Repeater {
                    model: page.command.rows
                    // Role values through `model`: the control's own
                    // properties carry the same names.
                    delegate: DLCommandControl {
                        id: commandControl
                        readonly property string key: model.key
                        objectName: "command:" + key
                        width: commandColumn.width
                        name: model.name
                        owner: model.owner
                        unit: model.unit
                        from: model.from
                        to: model.to
                        limit: model.limit
                        bound: model.bound
                        value: model.value
                        stepSize: model.step
                        decimals: model.decimals
                        latched: model.latched
                        held: model.held
                        ramping: model.ramping
                        writing: model.writing
                        commandEnabled: page.command.enabled
                        problem: page.problems[key] || ""
                        onSliderPressed: { page.setProblem(key, ""); page.command.press(key) }
                        onSliderMoved: (v) => page.command.drag(key, v)
                        onSliderReleased: page.command.release(key)
                        onLatchToggled: (on) => page.command.setLatched(key, on)
                        onValueTyped: (text) => page.setProblem(key, page.command.typeValue(key, text))
                        onLimitTyped: (text) => page.setProblem(key, page.command.setLimitText(key, text))
                        onRemoveRequested: { page.setProblem(key, ""); page.command.remove(key) }
                    }
                }
                DLButton {
                    objectName: "commandAdd"
                    kind: DLButton.Ghost
                    iconName: page.adding ? "chevron-up" : "plus"
                    text: qsTr("Command another input")
                    onClicked: page.adding = !page.adding
                }
                Column {
                    objectName: "commandCandidates"
                    visible: page.adding
                    width: parent.width
                    spacing: Theme.space4
                    Text {
                        visible: (page.command.candidates || []).length === 0
                        text: qsTr("No running instance has inputs")
                        font: Theme.fontCaption; color: Theme.textSecondary
                    }
                    Repeater {
                        model: page.command.candidates || []
                        delegate: Rectangle {
                            required property var modelData
                            objectName: "candidate:" + modelData.key
                            width: commandColumn.width
                            height: candidateColumn.implicitHeight + 2 * Theme.space4
                            radius: Theme.radiusSm
                            color: candidateHover.hovered && modelData.enabled ? Theme.bgHover : "transparent"
                            Column {
                                id: candidateColumn
                                x: Theme.space8; y: Theme.space4
                                width: parent.width - 2 * Theme.space8
                                Text {
                                    width: parent.width; elide: Text.ElideRight
                                    text: modelData.name + "  ·  " + modelData.owner + (modelData.unit ? "  (" + modelData.unit + ")" : "")
                                    font: Theme.fontBodyDefault
                                    color: modelData.enabled ? Theme.textPrimary : Theme.textDisabled
                                }
                                Text {
                                    visible: !modelData.enabled
                                    width: parent.width; wrapMode: Text.Wrap
                                    text: modelData.reason
                                    font: Theme.fontCaption; color: Theme.textSecondary
                                }
                            }
                            HoverHandler { id: candidateHover }
                            TapHandler {
                                enabled: modelData.enabled
                                // add() rebuilds this list (and this delegate): last.
                                onTapped: { const key = modelData.key; page.adding = false; page.command.add(key) }
                            }
                        }
                    }
                }
                DLReasonHover {
                    reason: !page.command.enabled ? page.command.blocked : qsTr("Every input is at 0")
                    DLButton {
                        objectName: "commandHoldAll"
                        iconName: "hand"
                        text: qsTr("Hold all")
                        enabled: page.command.enabled && page.command.commanding
                        onClicked: page.command.holdAll()
                    }
                }
            }
        }
    }

    // ---- Plots ---------------------------------------------------------------
    Item {
        id: center
        anchors.left: signalsCard.right; anchors.leftMargin: Theme.space16
        anchors.right: commandCard.left; anchors.rightMargin: Theme.space16
        anchors.top: parent.top; anchors.topMargin: Theme.space24
        anchors.bottom: parent.bottom; anchors.bottomMargin: Theme.space24

        Flow {
            id: toolbar
            objectName: "monitorToolbar"
            width: parent.width
            spacing: Theme.space8
            DLButton {
                objectName: "monitorPause"
                kind: page.monitor.paused ? DLButton.Primary : DLButton.Secondary
                iconName: page.monitor.paused ? "play" : "pause"
                text: page.monitor.paused ? qsTr("Resume  Space") : qsTr("Pause  Space")
                onClicked: page.togglePause()
            }
            DLComboBox {
                objectName: "monitorWindow"
                width: 160
                model: page.monitor.windowChoices
                displayText: qsTr("Window %1 s").arg(page.monitor.windowSeconds)
                currentIndex: page.monitor.windowChoices.indexOf(page.monitor.windowSeconds)
                onActivated: function(index) { page.monitor.setWindowSeconds(page.monitor.windowChoices[index]) }
            }
            DLButton {
                objectName: "monitorCursors"
                enabled: page.monitor.paused
                kind: page.monitor.cursors ? DLButton.Primary : DLButton.Secondary
                iconName: "crosshair"
                text: qsTr("Cursors")
                toolTipText: page.monitor.paused ? "" : qsTr("Pause to place cursors")
                onClicked: page.monitor.setCursors(!page.monitor.cursors)
            }
            DLButton {
                objectName: "monitorSnapshot"
                enabled: page.monitor.plotted > 0
                iconName: "download"
                text: qsTr("Snapshot CSV")
                onClicked: page.monitor.snapshot(page.backend.deviceId)
            }
            DLButton {
                objectName: "monitorAddPlot"
                kind: DLButton.Ghost
                enabled: page.plots.length < 8
                iconName: "plus"
                text: qsTr("Add plot")
                onClicked: page.monitor.addPlot()
            }
        }

        // The cursor readout, under the toolbar while the cursors are shown.
        Rectangle {
            id: readout
            objectName: "monitorReadout"
            anchors.top: toolbar.bottom; anchors.topMargin: visible ? Theme.space12 : 0
            width: parent.width
            height: visible ? readoutColumn.implicitHeight + 2 * Theme.space12 : 0
            visible: page.monitor.cursors
            color: Theme.bgSurface
            radius: Theme.radiusMd
            border.width: Theme.borderWidth
            border.color: Theme.borderDefault
            Column {
                id: readoutColumn
                x: Theme.space12; y: Theme.space12
                width: parent.width - 2 * Theme.space12
                spacing: Theme.space4
                Row {
                    width: parent.width
                    Text { width: parent.width * 0.4; text: qsTr("SIGNAL"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
                    Text { width: parent.width * 0.2; horizontalAlignment: Text.AlignRight; text: "A"; font: Theme.fontLabelCaps; color: Theme.textSecondary }
                    Text { width: parent.width * 0.2; horizontalAlignment: Text.AlignRight; text: "B"; font: Theme.fontLabelCaps; color: Theme.textSecondary }
                    Text {
                        objectName: "monitorDt"
                        width: parent.width * 0.2; horizontalAlignment: Text.AlignRight
                        text: qsTr("Δ  (Δt %1)").arg((page.monitor.readout || {}).dt || "")
                        font: Theme.fontLabelCaps; color: Theme.textSecondary
                    }
                }
                Repeater {
                    model: (page.monitor.readout || {}).rows || []
                    delegate: Row {
                        id: readoutRow
                        required property var modelData
                        width: readoutColumn.width
                        Row {
                            width: parent.width * 0.4; spacing: Theme.space8
                            Rectangle { width: 8; height: 8; radius: 4; anchors.verticalCenter: parent.verticalCenter; color: page.colorOf(readoutRow.modelData.color) }
                            Text { text: readoutRow.modelData.name; font: Theme.fontMonoSmall; color: Theme.textPrimary }
                        }
                        Text { width: parent.width * 0.2; horizontalAlignment: Text.AlignRight; text: readoutRow.modelData.a; font: Theme.fontBodyNumeric; color: Theme.textPrimary }
                        Text { width: parent.width * 0.2; horizontalAlignment: Text.AlignRight; text: readoutRow.modelData.b; font: Theme.fontBodyNumeric; color: Theme.textPrimary }
                        Text { width: parent.width * 0.2; horizontalAlignment: Text.AlignRight; text: readoutRow.modelData.delta; font: Theme.fontBodyNumeric; color: Theme.textPrimary }
                    }
                }
            }
        }

        Column {
            id: plotStack
            anchors.top: readout.bottom; anchors.topMargin: Theme.space12
            anchors.bottom: timeAxis.top
            width: parent.width
            spacing: Theme.space12
            Repeater {
                model: page.plots
                delegate: Rectangle {
                    id: plotFrame
                    required property var modelData
                    required property int index
                    readonly property var series: modelData.series
                    objectName: "monitorPlot_" + index
                    width: plotStack.width
                    height: (plotStack.height - plotStack.spacing * (page.plots.length - 1)) / Math.max(1, page.plots.length)
                    color: Theme.bgSurface
                    radius: Theme.radiusMd
                    border.width: Theme.borderWidth
                    border.color: Theme.borderDefault

                    // Legend: one chip per series with its live value.
                    Flow {
                        id: legend
                        x: Theme.space12; y: Theme.space8
                        width: parent.width - 2 * Theme.space12 - removePlot.width
                        spacing: Theme.space8
                        Text {
                            height: Theme.sizeControlSm; verticalAlignment: Text.AlignVCenter
                            visible: plotFrame.modelData.unit.length > 0
                            text: plotFrame.modelData.unit
                            font: Theme.fontLabelCaps; color: Theme.textSecondary
                        }
                        Repeater {
                            model: plotFrame.series
                            delegate: Rectangle {
                                id: chip
                                required property var modelData
                                objectName: "monitorChip_" + modelData.key
                                height: Theme.sizeControlSm
                                width: chipRow.implicitWidth + Theme.space8
                                radius: Theme.radiusSm
                                color: Theme.bgSecondary
                                Row {
                                    id: chipRow
                                    x: Theme.space8; height: parent.height
                                    spacing: Theme.space8
                                    Rectangle { width: 10; height: 3; radius: 1; anchors.verticalCenter: parent.verticalCenter; color: page.colorOf(chip.modelData.color) }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: chip.modelData.owner + "." + chip.modelData.name
                                        font: Theme.fontMonoSmall; color: Theme.textPrimary
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: page.monitor.values[chip.modelData.key] || "—"
                                        font: Theme.fontBodyNumeric; color: Theme.textSecondary
                                    }
                                    DLIconButton {
                                        anchors.verticalCenter: parent.verticalCenter
                                        size: DLIconButton.Sm; iconName: "x"; toolTipText: qsTr("Remove from the plot")
                                        onClicked: page.monitor.removeSignal(chip.modelData.key)
                                    }
                                }
                            }
                        }
                    }
                    DLIconButton {
                        id: removePlot
                        anchors.right: parent.right; anchors.rightMargin: Theme.space8; y: Theme.space8
                        visible: page.plots.length > 1
                        size: DLIconButton.Sm; iconName: "trash-2"; toolTipText: qsTr("Remove this plot")
                        onClicked: page.monitor.removePlot(plotFrame.index)
                    }

                    Rectangle {
                        id: area
                        anchors.left: parent.left; anchors.leftMargin: 64
                        anchors.right: parent.right; anchors.rightMargin: Theme.space12
                        anchors.top: legend.bottom; anchors.topMargin: Theme.space8
                        anchors.bottom: parent.bottom; anchors.bottomMargin: Theme.space12
                        color: Theme.plotBg
                        border.width: Theme.borderWidth
                        border.color: Theme.plotAxis
                        clip: true

                        // Grid: the Y ticks and the time axis divisions.
                        Repeater {
                            model: plot.ticks
                            delegate: Rectangle {
                                required property var modelData
                                y: modelData.y; width: area.width; height: 1
                                color: Theme.plotGrid
                            }
                        }
                        Repeater {
                            model: 4
                            delegate: Rectangle {
                                required property int index
                                x: area.width * (index + 1) / 5; width: 1; height: area.height
                                color: Theme.plotGrid
                            }
                        }
                        PlotItem {
                            id: plot
                            objectName: "monitorPlotItem_" + plotFrame.index
                            anchors.fill: parent
                            monitor: page.monitor
                            keys: plotFrame.series.map(s => s.key)
                            colors: plotFrame.series.map(s => page.colorOf(s.color))
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: plotFrame.series.length === 0
                            text: qsTr("Check a signal to plot it here")
                            font: Theme.fontBodyDefault; color: Theme.textTertiary
                        }
                        // A–B cursors, dragged along the window (paused only).
                        Repeater {
                            model: page.monitor.cursors ? 2 : 0
                            delegate: Item {
                                id: cursor
                                required property int index
                                readonly property real at: index === 0 ? page.monitor.cursorA : page.monitor.cursorB
                                x: page.xOf(at, area.width) - width / 2
                                width: 11; height: area.height
                                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 1; height: parent.height; color: Theme.plotCursor }
                                Text {
                                    x: parent.width / 2 + Theme.space4; y: Theme.space4
                                    text: cursor.index === 0 ? "A" : "B"
                                    font: Theme.fontLabelCaps; color: Theme.plotCursor
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.SizeHorCursor
                                    onPositionChanged: function(mouse) {
                                        if (!pressed) return
                                        const x = cursor.mapToItem(area, mouse.x, 0).x
                                        page.monitor.setCursor(cursor.index, page.startUs + x / area.width * page.spanUs)
                                    }
                                }
                            }
                        }
                    }
                    // Y labels beside the area.
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
        }

        // One time axis for every plot: device time, now at the right.
        Item {
            id: timeAxis
            objectName: "monitorTimeAxis"
            anchors.bottom: parent.bottom
            x: 64; width: parent.width - 64 - Theme.space12
            height: Theme.sizeControlSm
            Repeater {
                model: 6
                delegate: Text {
                    required property int index
                    x: timeAxis.width * index / 5 - (index === 5 ? width : index === 0 ? 0 : width / 2)
                    anchors.verticalCenter: parent.verticalCenter
                    text: index === 5 ? (page.monitor.paused ? qsTr("paused") : qsTr("now"))
                                      : qsTr("−%1 s").arg(Number(page.monitor.windowSeconds * (5 - index) / 5).toLocaleString(Qt.locale("C"), "f", page.monitor.windowSeconds < 5 ? 1 : 0))
                    font: Theme.fontMonoSmall; color: Theme.textSecondary
                }
            }
        }
    }
}
