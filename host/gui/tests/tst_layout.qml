import QtQuick
import QtTest
import DriveLink.Style

// The Layout controls (H2c-1): the param row's commit rules, the tree item,
// the ref picker and the number field states.
TestCase {
    id: testCase
    name: "LayoutControls"
    width: 1200
    height: 600
    visible: true
    when: windowShown

    Component {
        id: rowComponent
        DLParamRow {
            width: 900
            name: "vel.kp"; unit: "1/s"; valueText: "1.5"; valueBits: 0; rangeText: "0 … 10"
        }
    }
    Component { id: otherFieldComponent; DLTextField { x: 0; y: 300; width: 200 } }
    Component { id: treeComponent; DLTreeItem { width: 400 } }
    Component { id: pickerComponent; DLRefPicker { width: 300 } }
    Component { id: armComponent; DLArmButton {} }
    Component { id: spyComponent; SignalSpy {} }

    function spy(target, signalName) {
        return createTemporaryObject(spyComponent, testCase, { target: target, signalName: signalName })
    }
    function field(row) { return findChild(row, "paramField") }
    function typeInto(item, text) {
        item.forceActiveFocus(); item.selectAll()
        for (const ch of text) keyClick(ch)
    }

    function test_enter_sends_once_and_focus_loss_does_not_resend() {
        const row = createTemporaryObject(rowComponent, testCase)
        const other = createTemporaryObject(otherFieldComponent, testCase)
        const sent = spy(row, "commitText")
        const f = field(row)
        typeInto(f, "2.5")
        verify(row.edited)
        compare(f.status, DLNumberField.Edited)
        keyClick(Qt.Key_Return)
        compare(sent.count, 1); compare(sent.signalArguments[0][0], "2.5")
        row.editState = "sending"
        compare(f.text, "2.5", "the sent text stays while sending")
        other.forceActiveFocus()
        compare(sent.count, 1, "focus loss after Enter sends nothing more")
        row.editState = "applied"; row.valueText = "2.5"
        compare(f.text, "2.5")
    }

    function test_focus_loss_sends_and_unchanged_text_does_not() {
        const row = createTemporaryObject(rowComponent, testCase)
        const other = createTemporaryObject(otherFieldComponent, testCase)
        const sent = spy(row, "commitText")
        const f = field(row)
        f.forceActiveFocus(); other.forceActiveFocus()
        compare(sent.count, 0, "leaving an unchanged field sends nothing")
        typeInto(f, "3")
        other.forceActiveFocus()
        compare(sent.count, 1); compare(sent.signalArguments[0][0], "3")
    }

    function test_cancel_restores_without_sending() {
        const row = createTemporaryObject(rowComponent, testCase)
        const other = createTemporaryObject(otherFieldComponent, testCase)
        const sent = spy(row, "commitText")
        const f = field(row)
        typeInto(f, "9")
        row.cancel()
        compare(f.text, "1.5")
        other.forceActiveFocus()
        compare(sent.count, 0)
    }

    function test_clamped_value_replaces_the_sent_text() {
        const row = createTemporaryObject(rowComponent, testCase)
        const other = createTemporaryObject(otherFieldComponent, testCase)
        const sent = spy(row, "commitText")
        const f = field(row)
        typeInto(f, "50"); keyClick(Qt.Key_Return)
        row.editState = "sending"
        row.editState = "clamped"; row.editNote = "Clamped to 10"; row.valueText = "10"
        compare(f.text, "10")
        compare(f.status, DLNumberField.Clamped)
        other.forceActiveFocus()
        compare(sent.count, 1, "the clamped value is not sent again")
    }

    function test_refused_value_is_kept_until_edited() {
        const row = createTemporaryObject(rowComponent, testCase)
        const other = createTemporaryObject(otherFieldComponent, testCase)
        const cleared = spy(row, "clearEdit")
        const sent = spy(row, "commitText")
        const f = field(row)
        typeInto(f, "x"); keyClick(Qt.Key_Return)
        row.editText = "x"; row.editNote = "Enter a finite number"; row.editState = "rejected"
        compare(f.text, "x"); compare(f.status, DLNumberField.Rejected)
        other.forceActiveFocus()
        compare(sent.count, 1, "a refused value is not resent on focus loss")
        typeInto(f, "4")
        verify(cleared.count >= 1, "editing drops the kept refusal")
    }

    function test_pending_locked_and_blocked() {
        const row = createTemporaryObject(rowComponent, testCase,
            { isPending: true, pendingText: "14", valueText: "10", rebootRequired: true })
        const f = field(row)
        compare(f.text, "14")
        compare(f.status, DLNumberField.Pending)
        row.locked = true
        compare(f.status, DLNumberField.Locked)
        verify(!f.parent.enabled)
        row.locked = false; row.blockedReason = "The layout is locked while armed; disarm first"
        verify(!f.parent.enabled)
    }

    function test_enum_and_bool_editors_send_values() {
        const row = createTemporaryObject(rowComponent, testCase,
            { editor: "enum", enums: [{ value: 0, name: "open" }, { value: 1, name: "velocity" }], valueBits: 1 })
        const combo = findChild(row, "paramEnum")
        compare(combo.currentIndex, 1)
        const sent = spy(row, "commitValue")
        combo.activated(0)
        compare(sent.count, 1); compare(sent.signalArguments[0][0], 0)
        compare(combo.currentIndex, 1, "the device value stays shown until read back")
        const boolRow = createTemporaryObject(rowComponent, testCase, { editor: "bool", valueBits: 0 })
        const toggle = findChild(boolRow, "paramSwitch")
        const toggled = spy(boolRow, "commitValue")
        verify(!toggle.checked)
        mouseClick(toggle)
        compare(toggled.count, 1); compare(toggled.signalArguments[0][0], 1)
        verify(!toggle.checked)
    }

    function test_tree_item_loop_and_module() {
        const loop = createTemporaryObject(treeComponent, testCase,
            { isLoop: true, label: "control", rateHz: 1000, realtime: true, budget: 0.041, hasBudget: true })
        compare(loop.height, 52)
        compare(loop.rateText(1000), "1 kHz"); compare(loop.rateText(250), "250 Hz")
        compare(loop.stateText, "Running")
        loop.pending = "change"
        compare(loop.stateText, "Pending change")
        const module = createTemporaryObject(treeComponent, testCase,
            { label: "right_enc", typeName: "feedback.mock", depth: 2, health: 1, healthText: "failed" })
        compare(module.height, Theme.sizeControl)
        verify(module.failed); compare(module.stateText, "Failed")
        verify(module.leftPadding > loop.leftPadding)
    }

    function test_ref_picker_lists_disabled_entries_and_sends_enabled_ones() {
        const picker = createTemporaryObject(pickerComponent, testCase, { chipText: "actuator", valueText: "Not wired" })
        const opening = spy(picker, "aboutToOpen")
        const picked = spy(picker, "picked")
        picker.aboutToOpen.connect(function() {
            picker.candidates = [{ value: 0, text: "Not wired", enabled: true },
                                 { value: 28672, text: "left_pwm", detail: "actuator.mock · control", enabled: true },
                                 { value: 28673, text: "sim_motor", enabled: false, reason: "test loop; left_drive runs in control" }]
        })
        picker.popup.open()
        compare(opening.count, 1)
        compare(picker.count, 3)
        picker.activated(2)
        compare(picked.count, 0, "a disabled entry is not sent")
        picker.activated(1)
        compare(picked.count, 1); compare(picked.signalArguments[0][0], 28672)
        compare(picker.displayText, "Not wired", "the device value stays shown until read back")
        picker.popup.close()
    }

    function test_arm_button_click_keeps_editor_focus() {
        const row = createTemporaryObject(rowComponent, testCase)
        const arm = createTemporaryObject(armComponent, testCase, { x: 950, y: 10, mode: DLArmButton.Disarm })
        const sent = spy(row, "commitText")
        const f = field(row)
        typeInto(f, "7")
        mouseClick(arm)
        verify(f.activeFocus, "clicking DISARM does not end the edit, so nothing is sent")
        compare(sent.count, 0)
        compare(arm.focusPolicy, Qt.TabFocus)
    }
}
