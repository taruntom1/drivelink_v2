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
}
