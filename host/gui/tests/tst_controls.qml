import QtQuick
import QtTest
import DriveLink.Style

TestCase {
    id: testCase
    name: "Controls"
    width: 800
    height: 600
    visible: true
    when: windowShown

    Component { id: buttonComponent; DLButton { text: "Button" } }
    Component { id: iconButtonComponent; DLIconButton { iconName: "settings"; toolTipText: "Settings" } }
    Component { id: armComponent; DLArmButton {} }
    Component { id: chipComponent; DLStateChip {} }
    Component { id: badgeComponent; DLBadge { text: "badge" } }
    Component { id: deviceComponent; DLDeviceChip {} }
    Component { id: leaseComponent; DLLeaseChip {} }
    Component { id: navItemComponent; DLNavItem { text: "Device"; iconName: "cpu" } }
    Component {
        id: railComponent
        DLNavRail {
            height: 500
            model: [{ text: "Device", icon: "cpu" }, { text: "Layout", icon: "workflow" }]
            footerModel: [{ text: "Settings", icon: "settings" }]
        }
    }
    Component { id: bannerComponent; DLBanner { width: 1200; title: "Title"; message: "Message" } }
    Component { id: toastComponent; DLToast { text: "Saved to flash." } }
    Component {
        id: dialogComponent
        DLDialog { title: "Save to flash?"; text: "Writes NVS."; acceptText: "Save" }
    }
    Component {
        id: reasonComponent
        DLReasonHover {
            reason: "Disarm to save"
            DLButton { text: "Save"; enabled: false }
        }
    }
    Component { id: textFieldComponent; DLTextField { width: 300 } }
    Component { id: comboComponent; DLComboBox { width: 200; model: [921600, 460800, 115200] } }
    Component { id: checkComponent; DLCheckItem { width: 300; label: "Command keepalives are ready" } }
    Component { id: progressComponent; DLProgressBar { width: 200 } }
    Component { id: levelComponent; DLCalibrationLevel { width: 240; title: "Sensor"; caption: "Calibrating" } }
    Component { id: previewComponent; DLOrientationPreview { width: 320; height: 200; caption: "Sensor axes" } }
    Component {
        id: extraDialogComponent
        DLDialog {
            title: "Rename device"
            acceptText: "Rename"
            extra: DLTextField { objectName: "extraField"; width: 400 }
        }
    }
    Component { id: sliderComponent; DLSlider { width: 320; from: -2.5; to: 2.5; stepSize: 0.01 } }
    Component {
        id: commandComponent
        DLCommandControl { name: "velocity_sp"; owner: "left_drive"; unit: "rad/s"; from: -2.5; to: 2.5; limit: 2.5; bound: 1000 }
    }
    Component {
        id: gainComponent
        DLGainControl { name: "vel.kp"; description: "proportional"; valueText: "0.85"; position: 0.5
                        lowText: "0.21"; midText: "0.85"; highText: "3.40" }
    }
    Component { id: logSliderComponent; DLSlider { width: 320; scaleType: DLSlider.Log; from: 0; to: 1 } }
    Component { id: spyComponent; SignalSpy {} }

    function spy(target, signalName) {
        return createTemporaryObject(spyComponent, testCase, { target: target, signalName: signalName })
    }

    function cleanup() {
        Theme.dark = true
    }

    // Button ----------------------------------------------------------------

    function test_button_kinds() {
        const primary = createTemporaryObject(buttonComponent, testCase, { kind: DLButton.Primary })
        compare(primary.height, Theme.sizeControl)
        compare(primary.background.color, Theme.accentDefault)
        compare(primary.foreground, Theme.textOnAccent)
        const secondary = createTemporaryObject(buttonComponent, testCase, { kind: DLButton.Secondary })
        compare(secondary.background.color, Theme.bgSecondary)
        compare(secondary.background.border.width, 1)
        const ghost = createTemporaryObject(buttonComponent, testCase, { kind: DLButton.Ghost })
        compare(ghost.background.color, Qt.rgba(0, 0, 0, 0))
        const danger = createTemporaryObject(buttonComponent, testCase, { kind: DLButton.Danger })
        compare(danger.background.border.color, Theme.stateFault)
        compare(danger.foreground, Theme.stateFault)
        danger.down = true
        compare(danger.background.border.width, 2)
        compare(danger.background.color, Theme.stateFaultSubtle)
    }

    function test_button_pressed_and_theme_switch() {
        const button = createTemporaryObject(buttonComponent, testCase, { kind: DLButton.Primary })
        button.down = true
        compare(button.background.color, Theme.accentPressed)
        Theme.dark = false
        compare(button.background.color.toString(), "#1f4e9e")
    }

    function test_button_disabled() {
        const button = createTemporaryObject(buttonComponent, testCase)
        const clicked = spy(button, "clicked")
        button.enabled = false
        compare(button.opacity, 0.4)
        mouseClick(button)
        compare(clicked.count, 0)
        button.enabled = true
        compare(button.opacity, 1)
        mouseClick(button)
        compare(clicked.count, 1)
    }

    function test_button_padding_and_font() {
        const button = createTemporaryObject(buttonComponent, testCase, { iconName: "plus" })
        compare(button.leftPadding, 12)
        compare(button.rightPadding, 12)
        compare(button.font.weight, Font.Medium)
        verify(button.implicitWidth > 12 + 16 + 8 + 12)
    }

    // IconButton --------------------------------------------------------------

    function test_icon_button_sizes_and_name() {
        const md = createTemporaryObject(iconButtonComponent, testCase)
        compare(md.width, 32)
        compare(md.height, 32)
        compare(md.Accessible.name, "Settings")
        const sm = createTemporaryObject(iconButtonComponent, testCase, { size: DLIconButton.Sm })
        compare(sm.width, 24)
        const clicked = spy(sm, "clicked")
        mouseClick(sm)
        compare(clicked.count, 1)
    }

    // ArmButton ---------------------------------------------------------------

    function test_arm_button_requests_follow_mode() {
        const button = createTemporaryObject(armComponent, testCase, { mode: DLArmButton.Arm })
        const arm = spy(button, "armRequested")
        const disarm = spy(button, "disarmRequested")
        const ack = spy(button, "ackFaultRequested")
        compare(button.width, 184)
        compare(button.height, Theme.sizeDisarm)
        compare(button.text, "ARM")
        mouseClick(button)
        compare(arm.count, 1)

        button.mode = DLArmButton.Arming
        compare(button.text, "ARMING…")
        mouseClick(button)
        compare(arm.count, 1)
        compare(disarm.count + ack.count, 0)

        button.mode = DLArmButton.AckFault
        compare(button.text, "ACK FAULT")
        mouseClick(button)
        compare(ack.count, 1)

        button.mode = DLArmButton.Disarm
        compare(button.text, "DISARM")
        mouseClick(button)
        compare(disarm.count, 1)
        compare(arm.count, 1)
    }

    function test_arm_disabled_and_disarm_disabled() {
        const arm = createTemporaryObject(armComponent, testCase, { mode: DLArmButton.Arm, enabled: false })
        compare(arm.iconName, "lock")
        compare(arm.foreground, Theme.textDisabled)
        compare(arm.opacity, 1)
        compare(arm.background.border.color, Theme.borderDefault)
        const armSpy = spy(arm, "armRequested")
        mouseClick(arm)
        compare(armSpy.count, 0)

        const disarm = createTemporaryObject(armComponent, testCase, { mode: DLArmButton.Disarm, enabled: false })
        compare(disarm.opacity, 0.4)
        compare(disarm.background.color, Theme.disarmBg)
        const disarmSpy = spy(disarm, "disarmRequested")
        mouseClick(disarm)
        compare(disarmSpy.count, 0)
    }

    function test_disarm_is_red_and_shows_esc() {
        const disarm = createTemporaryObject(armComponent, testCase, { mode: DLArmButton.Disarm })
        compare(disarm.background.color, Theme.disarmBg)
        compare(disarm.foreground, Theme.textOnAccent)
        compare(disarm.Accessible.name, "DISARM (Esc)")
        verify(findChild(disarm, "escHint").visible)
        disarm.down = true
        compare(disarm.background.color, Theme.disarmPressed)
        Theme.dark = false
        compare(disarm.background.color, Theme.disarmPressed)
        const arm = createTemporaryObject(armComponent, testCase, { mode: DLArmButton.Arm })
        verify(!findChild(arm, "escHint").visible)
    }

    // StateChip ---------------------------------------------------------------

    function test_state_chip_states() {
        const chip = createTemporaryObject(chipComponent, testCase)
        const expected = ["Offline", "Connecting…", "Disarmed", "Arming…", "ARMED", "Fault", "Safe mode"]
        for (let s = 0; s < expected.length; ++s) {
            chip.status = s
            compare(chip.text, expected[s])
            compare(chip.height, 32)
        }
        chip.status = DLStateChip.Armed
        compare(chip.background.color, Theme.stateArmed)
        chip.status = DLStateChip.Fault
        compare(chip.background.border.color, Theme.stateFault)
        compare(chip.background.border.width, 1)
        chip.status = DLStateChip.Disarmed
        compare(chip.background.border.width, 0)
    }

    function test_state_chip_opens_faults_only_in_fault_and_safe_mode() {
        const chip = createTemporaryObject(chipComponent, testCase, { status: DLStateChip.Disarmed })
        const faults = spy(chip, "faultsRequested")
        mouseClick(chip)
        compare(faults.count, 0)
        chip.status = DLStateChip.Fault
        mouseClick(chip)
        compare(faults.count, 1)
        chip.status = DLStateChip.SafeMode
        mouseClick(chip)
        compare(faults.count, 2)
    }

    // Badge -------------------------------------------------------------------

    function test_badge_tones_and_sizes() {
        const badge = createTemporaryObject(badgeComponent, testCase)
        compare(badge.height, 24)
        compare(badge.leftPadding, 8)
        badge.tone = DLBadge.Pending
        compare(badge.background.color, Theme.statePendingSubtle)
        compare(badge.foreground, Theme.statePending)
        badge.tone = DLBadge.Unsaved
        compare(badge.foreground, Theme.stateUnsaved)
        badge.tone = DLBadge.Fault
        compare(badge.background.color, Theme.stateFaultSubtle)
        badge.size = DLBadge.Sm
        compare(badge.height, 20)
        compare(badge.leftPadding, 4)
    }

    // DeviceChip --------------------------------------------------------------

    function test_lease_chip_variants() {
        const chip = createTemporaryObject(leaseComponent, testCase)
        const expected = [
            [DLLeaseChip.Available, "Lease: available", "lock-open", false],
            [DLLeaseChip.Mine, "Lease: this GUI", "key-round", true],
            [DLLeaseChip.Ros, "Lease: ROS", "lock", false],
            [DLLeaseChip.Other, "Lease: other GUI", "users", false],
            [DLLeaseChip.Requesting, "Lease: requesting…", "loader-circle", true],
            [DLLeaseChip.Revoked, "Lease: revoked", "key-round", false]
        ]
        const icon = findChild(chip, "leaseIcon")
        const label = findChild(chip, "leaseLabel")
        for (const row of expected) {
            chip.variant = row[0]
            compare(chip.text, row[1], "variant " + row[0])
            compare(label.text, row[1])
            compare(icon.name, row[2])
            compare(label.color, row[3] ? Theme.accentDefault : Theme.textSecondary, "variant " + row[0])
            verify(chip.width > 0 && chip.height > 0, "variant " + row[0])
            compare(chip.height, 16 + 2 * Theme.space4 + 2 * Theme.borderWidth)
        }
        compare(chip.Accessible.name, "Lease: revoked")
    }

    function test_lease_chip_holder_name() {
        const chip = createTemporaryObject(leaseComponent, testCase, { holderName: "bench-laptop" })
        chip.variant = DLLeaseChip.Other
        compare(chip.text, "Lease: bench-laptop")
        chip.variant = DLLeaseChip.Ros
        compare(chip.text, "Lease: bench-laptop")
        chip.variant = DLLeaseChip.Mine
        compare(chip.text, "Lease: this GUI")
        chip.variant = DLLeaseChip.Available
        compare(chip.text, "Lease: available")
    }

    function test_lease_chip_activation() {
        const chip = createTemporaryObject(leaseComponent, testCase, { variant: DLLeaseChip.Mine })
        const clicks = createTemporaryObject(spyComponent, testCase, { target: chip, signalName: "clicked" })
        mouseClick(chip)
        compare(clicks.count, 1)
        chip.forceActiveFocus()
        verify(chip.activeFocus)
        keyClick(Qt.Key_Space)
        compare(clicks.count, 2)
        chip.enabled = false
        mouseClick(chip)
        compare(clicks.count, 2)
    }

    function test_lease_chip_spinner_only_while_requesting_and_visible() {
        const chip = createTemporaryObject(leaseComponent, testCase)
        const icon = findChild(chip, "leaseIcon")
        verify(!icon.spinning)
        chip.variant = DLLeaseChip.Requesting
        verify(icon.spinning)
        chip.variant = DLLeaseChip.Revoked
        verify(!icon.spinning)
        chip.variant = DLLeaseChip.Requesting
        chip.visible = false
        verify(!icon.visible, "an invisible chip hides its icon, which stops the animation")
        chip.visible = true
        verify(icon.spinning)
    }

    function test_device_chip_identity() {
        const chip = createTemporaryObject(deviceComponent, testCase,
                                           { name: "bench-rig", deviceId: "A4CF123E9B01", connection: "COM5 · 921600" })
        compare(chip.formattedId, "a4:cf:12:3e:9b:01")
        compare(chip.text, "bench-rig")
        verify(findChild(chip, "deviceId").visible)
        compare(chip.Accessible.name, "bench-rig, COM5 · 921600")
        chip.name = ""
        compare(chip.text, "a4:cf:12:3e:9b:01")
        verify(!findChild(chip, "deviceId").visible)
        chip.deviceId = "not-a-mac"
        compare(chip.formattedId, "not-a-mac")
    }

    // NavItem and NavRail -----------------------------------------------------

    function test_nav_item_selected() {
        const item = createTemporaryObject(navItemComponent, testCase)
        compare(item.width, 72)
        compare(item.height, 64)
        verify(!findChild(item, "indicator").visible)
        item.checked = true
        compare(item.background.color, Theme.bgSelected)
        verify(findChild(item, "indicator").visible)
        mouseClick(item)
        verify(item.checked, "a nav item never toggles itself")
    }

    function test_nav_rail_is_controlled() {
        const rail = createTemporaryObject(railComponent, testCase)
        const activated = spy(rail, "activated")
        compare(rail.width, 72)
        mouseClick(rail, 36, 8 + 64 + 4 + 32)
        compare(activated.count, 1)
        compare(activated.signalArguments[0][0], 1)
        compare(rail.currentIndex, 0)
        mouseClick(rail, 36, rail.height - 8 - 32)
        compare(activated.signalArguments[1][0], 2)
        rail.currentIndex = 2
        compare(rail.currentIndex, 2)
    }

    function test_nav_rail_sections_enable_separately() {
        const rail = createTemporaryObject(railComponent, testCase, { pagesEnabled: false })
        const activated = spy(rail, "activated")
        mouseClick(rail, 36, 8 + 32)
        mouseClick(rail, 36, 8 + 64 + 4 + 32)
        compare(activated.count, 0, "disabled page items do not activate")
        mouseClick(rail, 36, rail.height - 8 - 32)
        compare(activated.count, 1)
        compare(activated.signalArguments[0][0], 2)
        rail.footerEnabled = false
        mouseClick(rail, 36, rail.height - 8 - 32)
        compare(activated.count, 1, "a disabled footer item does not activate")
        rail.pagesEnabled = true
        mouseClick(rail, 36, 8 + 32)
        compare(activated.count, 2)
        compare(activated.signalArguments[1][0], 0)
    }

    // Banner ------------------------------------------------------------------

    function test_banner_actions() {
        const banner = createTemporaryObject(bannerComponent, testCase,
                                             { tone: DLBanner.Pending, action1Text: "Review", action2Text: "Save and reboot" })
        compare(banner.height, 48)
        compare(banner.color, Theme.statePendingSubtle)
        compare(banner.iconName, "rotate-ccw")
        const triggered = spy(banner, "actionTriggered")
        const buttons = []
        for (const child of banner.children[0].children) {
            if (child.kind !== undefined && child.visible)
                buttons.push(child)
        }
        compare(buttons.length, 2)
        compare(buttons[0].kind, DLButton.Secondary)
        compare(buttons[1].kind, DLButton.Primary)
        mouseClick(buttons[1])
        compare(triggered.count, 1)
        compare(triggered.signalArguments[0][0], 1)
    }

    // Toast -------------------------------------------------------------------

    function test_toast_auto_dismiss_except_fault() {
        const info = createTemporaryObject(toastComponent, testCase, { tone: DLToast.Ok, timeout: 50 })
        const infoSpy = spy(info, "dismissed")
        infoSpy.wait(1000)
        const fault = createTemporaryObject(toastComponent, testCase, { tone: DLToast.Fault, timeout: 50 })
        const faultSpy = spy(fault, "dismissed")
        wait(200)
        compare(faultSpy.count, 0)
        mouseClick(findChild(fault, "closeButton"))
        compare(faultSpy.count, 1)
        compare(fault.width, 380)
    }

    // Dialog ------------------------------------------------------------------

    function test_dialog_enter_accepts_default_only() {
        const dialog = createTemporaryObject(dialogComponent, testCase)
        verify(!dialog.modal, "a Qt modal would block DISARM in the status bar")
        verify(!dialog.dim)
        const accepted = spy(dialog, "accepted")
        dialog.open()
        tryCompare(dialog, "opened", true)
        compare(findChild(dialog.contentItem, "acceptButton").kind, DLButton.Primary)
        keyClick(Qt.Key_Return)
        compare(accepted.count, 1)

        const destructive = createTemporaryObject(dialogComponent, testCase, { destructive: true })
        const destroyAccepted = spy(destructive, "accepted")
        const rejected = spy(destructive, "rejected")
        destructive.open()
        tryCompare(destructive, "opened", true)
        compare(findChild(destructive.contentItem, "acceptButton").kind, DLButton.Danger)
        keyClick(Qt.Key_Return)
        compare(destroyAccepted.count, 0)
        keyClick(Qt.Key_Escape)
        tryCompare(rejected, "count", 1)
    }

    function test_dialog_confirmation() {
        const dialog = createTemporaryObject(dialogComponent, testCase)
        verify(!findChild(dialog.contentItem, "confirmation").visible)
        dialog.confirmationText = "Don't ask again for this connection"
        dialog.open()
        tryCompare(dialog, "opened", true)
        // Click where the Column puts the box, not where it was before layout.
        waitForItemPolished(dialog.contentItem)
        const box = findChild(dialog.contentItem, "confirmation")
        verify(box.visible)
        verify(!dialog.confirmed)
        mouseClick(box)
        verify(dialog.confirmed)
        compare(dialog.width, 480)
        dialog.close()
    }

    function test_dialog_extra_content_and_accept_gate() {
        const dialog = createTemporaryObject(extraDialogComponent, testCase, { acceptEnabled: false })
        const accepted = spy(dialog, "accepted")
        dialog.open()
        tryCompare(dialog, "opened", true)
        const field = findChild(dialog.contentItem, "extraField")
        verify(field !== null, "extra content sits inside the dialog")
        verify(findChild(dialog.contentItem, "extra").visible)
        verify(!findChild(dialog.contentItem, "acceptButton").enabled)
        keyClick(Qt.Key_Return)
        compare(accepted.count, 0, "Enter cannot bypass a disabled accept")
        dialog.acceptEnabled = true
        verify(findChild(dialog.contentItem, "acceptButton").enabled)
        keyClick(Qt.Key_Return)
        compare(accepted.count, 1)
    }

    // Inputs and feedback (H2b) -----------------------------------------------

    function test_text_field_states() {
        const field = createTemporaryObject(textFieldComponent, testCase)
        compare(field.height, Theme.sizeControl)
        compare(field.background.border.color, Theme.borderDefault)
        field.forceActiveFocus()
        compare(field.background.border.color, Theme.borderFocus)
        field.error = true
        compare(field.background.border.color, Theme.stateFault)
        field.enabled = false
        compare(field.opacity, 0.4)
    }

    function test_combo_box_is_not_modal() {
        const box = createTemporaryObject(comboComponent, testCase, { x: 20, y: 20 })
        compare(box.displayText, "921600")
        verify(!box.popup.modal, "a modal popup would block DISARM")
        box.currentIndex = 2
        compare(box.displayText, "115200")
    }

    function test_check_item_states() {
        const item = createTemporaryObject(checkComponent, testCase)
        compare(item.status, DLCheckItem.Pending)
        item.status = DLCheckItem.Fail
        item.detail = "Press DISARM first"
        compare(findChild(item, "detail").color, Theme.stateFault)
        verify(findChild(item, "detail").visible)
        item.status = DLCheckItem.Pass
        compare(item.colors[item.status], Theme.stateOk)
        verify(!findChild(item, "tag").visible, "no tag by default")
        item.status = DLCheckItem.Warning
        item.tag = "warning"
        compare(item.icons[item.status], "triangle-alert")
        compare(item.colors[item.status], Theme.textSecondary)
        verify(findChild(item, "tag").visible)
        compare(findChild(item, "tag").text, "warning")
        verify(findChild(item, "tag").x > findChild(item, "label").width, "the tag sits right of the label")
    }

    function test_progress_bar_value() {
        const bar = createTemporaryObject(progressComponent, testCase, { value: 0.25 })
        compare(bar.visualPosition, 0.25)
        bar.indeterminate = true
        verify(bar.indeterminate)
    }

    // DLReasonHover -----------------------------------------------------------

    function test_reason_hover_explains_disabled_control() {
        const wrapper = createTemporaryObject(reasonComponent, testCase, { x: 100, y: 100 })
        const button = wrapper.control
        verify(button !== null)
        compare(wrapper.width, button.implicitWidth)
        verify(!wrapper.showing)
        mouseMove(wrapper, 10, 10)
        tryCompare(wrapper, "showing", true)
        button.enabled = true
        verify(!wrapper.showing, "an enabled control needs no reason")
    }

    // DLSlider / DLCommandControl (H2d-2) -------------------------------------

    function test_slider_symmetric_format_and_zero() {
        const slider = createTemporaryObject(sliderComponent, testCase)
        compare(slider.zeroPosition, 0.5)
        compare(slider.format(1.6), "+1.60")
        compare(slider.format(-0.25), "−0.25")
        compare(slider.format(0), "0.00")
        slider.from = 0
        compare(slider.zeroPosition, 0)
    }

    function test_command_control_emits_and_follows_value() {
        const control = createTemporaryObject(commandComponent, testCase, { x: 20, y: 20 })
        const slider = findChild(control, "commandSlider")
        const field = findChild(control, "commandValue")
        const note = findChild(control, "commandNote")
        // Disabled: the slider takes nothing and the field is read-only.
        verify(!slider.enabled)
        verify(field.readOnly)
        control.commandEnabled = true
        const pressed = spy(control, "sliderPressed")
        const moved = spy(control, "sliderMoved")
        const released = spy(control, "sliderReleased")
        mousePress(slider, slider.width / 2, 22)
        mouseMove(slider, slider.width * 0.8, 22)
        compare(pressed.count, 1)
        verify(moved.count >= 1)
        verify(moved.signalArguments[moved.count - 1][0] > 0)
        mouseRelease(slider, slider.width * 0.8, 22)
        compare(released.count, 1)
        // The shown value follows the owner (a ramp) once released.
        control.value = 0.4
        compare(slider.value, 0.4)
        compare(field.text, "+0.40")
        // Latch: the field becomes editable; a non-zero latched value is amber.
        verify(field.readOnly)
        control.latched = true
        verify(!field.readOnly)
        compare(control.border.color, Theme.stateArmed)
        verify(note.text.indexOf("+0.40 rad/s") >= 0)
        const typed = spy(control, "valueTyped")
        field.forceActiveFocus()
        field.text = "1.2"
        keyClick(Qt.Key_Return)
        compare(typed.count, 1)
        compare(typed.signalArguments[0][0], "1.2")
        // Leaving the field without Enter sends nothing and shows the value.
        field.forceActiveFocus()
        field.text = "0.9"
        field.focus = false
        compare(typed.count, 1)
        tryCompare(field, "text", "+0.40")
        // A write in flight: held at 0, nothing to drag, latch or type.
        control.writing = true
        verify(!slider.enabled)
        verify(field.readOnly)
        verify(note.text.indexOf("param write") >= 0)
        control.writing = false
        control.problem = "Not a number"
        compare(note.text, "Not a number")
        compare(field.status, DLNumberField.Rejected)
    }

    // DLSlider Log / DLGainControl (H2e-1) ------------------------------------

    function test_log_slider_labels_and_ticks() {
        const slider = createTemporaryObject(logSliderComponent, testCase,
            { lowText: "0.21", midText: "0.85", highText: "3.40", value: 0.5,
              ticks: [{position: 0.25, color: "red"}, {position: 0.9, color: "blue"}] })
        verify(slider.log)
        // No 0 mark on a log scale; the owner's labels and one mark per tick.
        const texts = []
        const marks = []
        for (const child of slider.background.children) {
            if (child.text !== undefined && child.visible) texts.push(child.text)
            if (child.width === 2 && child.height === 8) marks.push(child)
        }
        verify(texts.indexOf("0.21") >= 0 && texts.indexOf("0.85") >= 0 && texts.indexOf("3.40") >= 0)
        compare(marks.length, 2)
        fuzzyCompare(marks[0].x + 1, 0.25 * slider.availableWidth, 0.5)
    }

    function test_gain_control_emits_and_zero_disables() {
        const control = createTemporaryObject(gainComponent, testCase, { x: 20, y: 20 })
        const slider = findChild(control, "gainSlider")
        const field = findChild(control, "gainValue")
        const note = findChild(control, "gainNote")
        compare(control.width, 344)
        compare(field.text, "0.85")
        const pressed = spy(control, "sliderPressed")
        const moved = spy(control, "sliderMoved")
        const released = spy(control, "sliderReleased")
        mousePress(slider, slider.width / 2, 22)
        mouseMove(slider, slider.width * 0.8, 22)
        compare(pressed.count, 1)
        verify(moved.count >= 1)
        verify(moved.signalArguments[moved.count - 1][0] > 0.5)
        verify(note.text.indexOf("50 ms") >= 0)
        mouseRelease(slider, slider.width * 0.8, 22)
        compare(released.count, 1)
        // While not pressed the slider shows the owner's position.
        control.position = 0.5
        compare(slider.value, 0.5)
        // Keys: 1 %, Shift 10 %, Ctrl 0.1 % (not the slider's own steps).
        const keys = spy(control, "keyStepped")
        slider.forceActiveFocus()
        keyClick(Qt.Key_Up)
        keyClick(Qt.Key_Down, Qt.ShiftModifier)
        compare(keys.count, 2)
        compare(keys.signalArguments[0][0], true)
        compare(keys.signalArguments[1][0], false)
        compare(keys.signalArguments[1][1], true)
        compare(slider.value, 0.5)
        // % steps.
        const steps = spy(control, "stepped")
        mouseClick(findChild(control, "gainStep_2"))
        compare(steps.count, 1)
        compare(steps.signalArguments[0][0], 2)
        // Enter sends the typed value; leaving the field drops it.
        const typed = spy(control, "valueTyped")
        field.forceActiveFocus()
        field.text = "1.2"
        keyClick(Qt.Key_Return)
        compare(typed.signalArguments[0][0], "1.2")
        // A gain of 0: no slider or steps until a value is typed.
        control.zero = true
        verify(!slider.enabled)
        verify(!findChild(control, "gainStep_2").enabled)
        verify(note.text.indexOf("non-zero") >= 0)
        control.problem = "Locked while armed"
        compare(note.text, "Locked while armed")
        compare(field.status, DLNumberField.Rejected)
    }

    function filledSegments(level) {
        let n = 0
        for (let i = 0; i < 3; ++i)
            if (findChild(level, "levelSegment" + i).color.toString() !== Theme.bgSecondary.toString()) ++n
        return n
    }

    function test_calibration_level() {
        const level = createTemporaryObject(levelComponent, testCase, { level: 2 })
        compare(findChild(level, "levelValue").text, "2 / 3")
        compare(findChild(level, "levelTitle").text, "Sensor")
        compare(filledSegments(level), 2)
        compare(findChild(level, "levelSegment0").color, Theme.accentDefault)
        level.level = 3
        compare(findChild(level, "levelValue").text, "3 / 3")
        compare(filledSegments(level), 3)
        compare(findChild(level, "levelSegment2").color, Theme.stateOk)
        level.level = 0
        compare(filledSegments(level), 0)
        level.level = -1
        level.caption = "No live data"
        compare(findChild(level, "levelValue").text, "— / 3")
        compare(filledSegments(level), 0)
        compare(findChild(level, "levelCaption").text, "No live data")
        compare(findChild(level, "levelCaption").color, Theme.textTertiary)
        verify(level.height === level.implicitHeight, "a fixed height")
    }

    readonly property var sampleAxes: [{ axis: "Y", x: -0.6, y: -0.35, depth: -0.7 },
                                       { axis: "X", x: -0.55, y: 0.5, depth: -0.1 },
                                       { axis: "Z", x: 0.02, y: -0.8, depth: 0.8 }]

    function test_orientation_preview() {
        const preview = createTemporaryObject(previewComponent, testCase, { axes: sampleAxes })
        // Unavailable by default: no drawing, only the empty text.
        verify(!findChild(preview, "previewDrawing").visible)
        verify(findChild(preview, "previewEmpty").visible)
        compare(findChild(preview, "previewEmpty").text, "No orientation data")
        verify(!findChild(preview, "previewTag").visible)
        preview.state = "live"
        verify(findChild(preview, "previewDrawing").visible)
        verify(!findChild(preview, "previewEmpty").visible)
        compare(findChild(preview, "previewTag").text, "LIVE")
        compare(findChild(preview, "previewTag").color, Theme.accentText)
        let count = 0
        for (let c of findChild(preview, "previewDrawing").children)
            if (c.objectName === "axisLabel") ++count
        compare(count, 3)
        preview.state = "stale"
        compare(findChild(preview, "previewTag").text, "STALE")
        compare(findChild(preview, "previewTag").color, Theme.stateUnsaved)
        verify(findChild(preview, "previewDrawing").visible)
        preview.axes = []
        verify(findChild(preview, "previewEmpty").visible, "no axes: the empty text")
        verify(!findChild(preview, "previewDrawing").visible)
        compare(findChild(preview, "previewCaption").text, "Sensor axes")
    }
}
