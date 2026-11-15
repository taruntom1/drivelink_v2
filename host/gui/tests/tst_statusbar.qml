import QtQuick
import QtTest
import DriveLink.Style

TestCase {
    id: testCase
    name: "StatusBar"
    width: 2560
    height: 200
    visible: true
    when: windowShown

    Component {
        id: barComponent
        DLStatusBar {
            width: 1920
            deviceName: "bench-rig"
            deviceId: "a4cf123e9b01"
            connection: "COM5 · 921600"
        }
    }
    Component { id: spyComponent; SignalSpy {} }

    function spy(target, signalName) {
        return createTemporaryObject(spyComponent, testCase, { target: target, signalName: signalName })
    }

    function centreOffset(bar) {
        const centre = findChild(bar, "centreZone")
        return centre.x + centre.width / 2 - bar.width / 2
    }

    function test_centre_stays_centred() {
        const bar = createTemporaryObject(barComponent, testCase, { mode: DLStatusBar.Disarmed })
        for (const width of [1920, 2560, 1280]) {
            bar.width = width
            for (const reason of ["", "last: watchdog timeout · 18:20:11"]) {
                bar.reason = reason
                bar.unsaved = reason.length > 0
                fuzzyCompare(centreOffset(bar), 0, 1)
            }
        }
        bar.deviceName = "a-very-long-device-name-that-would-push-a-row-layout"
        fuzzyCompare(centreOffset(bar), 0, 1)
        const left = findChild(bar, "leftZone")
        verify(left.x + left.width <= findChild(bar, "centreZone").x, "left zone never overlaps the centre")
    }

    function test_disarm_enabled_whenever_there_is_a_link() {
        const bar = createTemporaryObject(barComponent, testCase)
        const disarm = findChild(bar, "disarmButton")
        const requests = spy(bar, "disarmRequested")
        for (let mode = DLStatusBar.NoDevice; mode <= DLStatusBar.LinkLost; ++mode) {
            bar.mode = mode
            const linked = mode !== DLStatusBar.NoDevice && mode !== DLStatusBar.LinkLost
            compare(disarm.enabled, linked, "mode " + mode)
            compare(disarm.mode, DLArmButton.Disarm)
            const before = requests.count
            mouseClick(disarm)
            compare(requests.count, before + (linked ? 1 : 0), "mode " + mode)
        }
    }

    function test_arm_button_per_mode() {
        const bar = createTemporaryObject(barComponent, testCase)
        const arm = findChild(bar, "armButton")
        const expected = [
            [DLStatusBar.NoDevice, false],
            [DLStatusBar.Loading, false],
            [DLStatusBar.Disarmed, true, DLArmButton.Arm, true],
            [DLStatusBar.Arming, true, DLArmButton.Arming, true],
            [DLStatusBar.Armed, false],
            [DLStatusBar.Fault, true, DLArmButton.AckFault, true],
            [DLStatusBar.SafeMode, true, DLArmButton.Arm, false],
            [DLStatusBar.LinkLost, false]
        ]
        for (const row of expected) {
            bar.mode = row[0]
            compare(arm.parent.visible, row[1], "mode " + row[0])
            if (row[1]) {
                compare(arm.mode, row[2], "mode " + row[0])
                compare(arm.enabled, row[3], "mode " + row[0])
            }
        }
        bar.mode = DLStatusBar.Disarmed
        bar.armEnabled = false
        verify(!arm.enabled)
        compare(arm.iconName, "lock")
    }

    function test_requests_are_forwarded() {
        const bar = createTemporaryObject(barComponent, testCase, { mode: DLStatusBar.Disarmed, unsaved: true })
        const arm = spy(bar, "armRequested")
        const save = spy(bar, "saveRequested")
        const revert = spy(bar, "revertRequested")
        const ack = spy(bar, "ackFaultRequested")
        const faults = spy(bar, "faultsRequested")
        const device = spy(bar, "deviceRequested")
        mouseClick(findChild(bar, "armButton"))
        compare(arm.count, 1)
        mouseClick(findChild(bar, "saveButton"))
        compare(save.count, 1)
        mouseClick(findChild(bar, "revertButton"))
        compare(revert.count, 1)
        mouseClick(findChild(bar, "deviceChip"))
        compare(device.count, 1)
        bar.mode = DLStatusBar.Fault
        bar.faultsVisible = true
        bar.faultsText = "1 active"
        mouseClick(findChild(bar, "armButton"))
        compare(ack.count, 1)
        mouseClick(findChild(bar, "stateChip"))
        mouseClick(findChild(bar, "faultsBadge"))
        compare(faults.count, 2)
        compare(arm.count, 1)
    }

    function test_save_and_revert_need_a_link_and_permission() {
        const bar = createTemporaryObject(barComponent, testCase, { mode: DLStatusBar.Armed, unsaved: true })
        const save = findChild(bar, "saveButton")
        const revert = findChild(bar, "revertButton")
        verify(findChild(bar, "unsavedGroup").visible)
        verify(save.enabled)
        bar.saveEnabled = false
        verify(!save.enabled)
        verify(revert.enabled)
        bar.mode = DLStatusBar.LinkLost
        verify(!revert.enabled)
        bar.unsaved = false
        verify(!findChild(bar, "unsavedGroup").visible)
    }

    function test_flags_and_left_zone() {
        const bar = createTemporaryObject(barComponent, testCase, { mode: DLStatusBar.NoDevice })
        verify(!findChild(bar, "deviceChip").visible)
        bar.pendingReboot = true
        bar.faultsVisible = true
        bar.faultsText = "2 faults"
        verify(!findChild(bar, "pendingBadge").visible, "no flags without a device")
        verify(!findChild(bar, "faultsBadge").visible)
        bar.mode = DLStatusBar.Disarmed
        verify(findChild(bar, "deviceChip").visible)
        verify(findChild(bar, "pendingBadge").visible)
        const faults = findChild(bar, "faultsBadge")
        verify(faults.visible)
        compare(faults.tone, DLBadge.Neutral)
        bar.faultsActive = true
        compare(faults.tone, DLBadge.Fault)
        compare(bar.height, Theme.sizeStatusbar)
    }
}
