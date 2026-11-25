# Device settings

Use this page to change how much the board can hold, for example to allow more loops than the default 4. You need the app connected to a disarmed board and a release whose **Device** page has a **Device settings** card. If you don’t see the card, [update the firmware]({{base}}pc/update/) and the app first. At the end you’ll see a new limit in effect after a restart.

The card lists the board’s own settings. Most are **capacity limits**: the maximum number of loops, instances, groups and so on. The board sizes its memory from them when it starts, so a new limit takes effect only after a restart. The other settings apply at once.

## Change a capacity limit

1. Open **Device** and find **Device settings**. Rows marked **reboot** are the limits. The **Limits** card on the right, below, shows the limits in effect now. It should show `loops` as 4.
2. In the `limits.loops` row, type 6 and press **Enter**. The row should show **Current 4 → Pending 6 · after reboot**, and the card should show **1 pending**. A banner at the top says **Changes require a reboot**. The **Limits** card still shows 4 because nothing has restarted.

{{figure:device-settings}}

3. Select **Save and reboot** in the banner. The board restarts and the app should reconnect by itself.
4. Check **Limits**. It should now show `loops` as 6, and the banner and pending note should be gone. On [Layout]({{base}}pc/layout/), the count above the loop list should now read **of 6 loops**.

While a limit is pending the app won’t let you arm. Finish the restart first.

If you type a number outside the allowed range, the app uses the nearest allowed value and says so, for example **Clamped to 100**.

## Settings that apply at once

`status.rate_hz` (how often the board reports its status), the `diag.*` settings (how often diagnostics are sent) and `watchdog_ms` change as soon as you press **Enter**. The row shows **Applied** and the card shows **Unsaved**. Select **Save** to keep the value for the next start. Without **Save**, the board returns to the saved value when it restarts.

> **Safety.** `watchdog_ms` decides how soon the motors stop after commands stop arriving. A longer time means a wheel can keep running longer when the link drops. Change it only with the wheels off the ground, a current-limited supply, a low duty cap and a hand on the power switch. The board allows 20 to 1,000 ms and 200 ms by default. You can’t change it while the board is armed. See [Arming and faults]({{base}}concepts/safety/).

PWM frequency and the encoder glitch filter belong to a module, so you set them on [Layout]({{base}}pc/layout/), not here.

## If a limit is refused

Nothing is staged when the board refuses a value. The row shows the reason:

| Message | What to do |
| --- | --- |
| **The pending layout uses more than this** | Your layout, including its pending changes, already needs more than this limit. Remove loops or instances in **Layout**, or choose a higher limit. |
| **The device has not enough memory for this** | Choose a lower value, or lower another limit first. |

On **Layout**, the counts above the loop list use the pending limits, so you can add what a pending limit allows before you restart.

## Go back

- **Before you save:** use **Discard all** in the banner to drop the staged values. Live values you already changed stay unsaved until you use **Revert**.
- **After you save:** **Discard all** does not undo the save. The banner then says **Saved settings are waiting for a reboot.** and the next start loads the saved values. To go back, make sure the rows show the old values, then use **Save and reboot**.

If the board is in safe mode, the reboot rows run at their defaults and the card says so. Your saved values are kept and load again after a normal start.

If another client holds control, as when you [share the device]({{base}}pi/control/), reboot rows say **Needs the control lease**. Live rows ask you to confirm each change.

## Change the serial link (baud rate or pins)

The **LINK** rows (`link.baud`, `link.tx_pin`, and `link.rx_pin`) set the speed and UART pins the board uses for DriveLink traffic. The defaults are **921600** baud on **TX 43 / RX 44**. Changing them starts a timed **trial** so a bad speed or wiring mistake does not lock you out:

- A **baud rate** change runs on a **30 s** trial.
- A **pin** change runs on a **120 s** trial so you have time to move the UART wires.

If the new link is not confirmed before the timer runs out—or if the board resets or loses power—the board restarts on its previous confirmed link.

1. Make sure the board is disarmed and no firmware update is pending. In **Device settings**, change `link.baud` (for example to `2000000`), or change `link.tx_pin` and `link.rx_pin`. An amber note appears inside the card showing the old and new link and the trial time.
2. Select **Save and reboot**. A confirmation dialog appears describing the trial window (and the new TX/RX pins if pins changed). **DISARM** remains live in the status bar. Select **Save and reboot** in the dialog.
3. The app shows a reconnect countdown while the board restarts on the new link. If you changed pins, move the UART wires to the new TX and RX pins now.
4. Once the board answers on the new link (directly with **Auto** baud, or through `drivelink-agent`), the app confirms the trial automatically. **Device settings** shows a **Serial link confirmed** banner and **Device** shows the new **Link rate**.

If the trial cannot be confirmed in time, the board reverts automatically and **Device settings** shows **Serial link reverted**. If neither the new nor the old link answers before the wait window ends, the app shows **Device not reachable after link change** with recovery steps and **Retry (Auto baud)**.

While a trial is active—for example if you connect to a board that another tool put into a trial—**Device settings** shows a **Serial link on trial** banner with the remaining time and a **Confirm link** button. Arming and firmware updates are paused until the trial is confirmed or reverts. Through an agent, confirming needs the control lease if another client holds it.

> **Warning.** If the board does not answer during a trial, press **Reset** or power-cycle it once so the unconfirmed trial reverts to the previous link. If a **confirmed** link becomes unreachable (for example, after confirming pins you no longer have wired), erase the board’s NVS partition over USB with `esptool` to return to **921600** on **TX 43 / RX 44**. Erasing NVS clears **all** saved settings on the board—layout, parameters, device name, and link settings. Firmware older than core v12 ignores stored link settings and always starts at **921600** on **GPIO 43/44**.

## The same change from a terminal

The command-line tool is installed with the app on Ubuntu and defaults to `--baud auto`. Replace the port with yours. Setting 0:0:16 is `limits.loops`, and 0:0:48 to 0:0:50 are `link.baud`, `link.tx_pin`, and `link.rx_pin`.

~~~sh
drivelink-cli --endpoint serial:///dev/ttyUSB0 set 0:0:16=6
drivelink-cli --endpoint serial:///dev/ttyUSB0 save
drivelink-cli --endpoint serial:///dev/ttyUSB0 reboot
drivelink-cli --endpoint serial:///dev/ttyUSB0 get 0:0:16
~~~

The first command should report `pending_reboot: true`, and the last should report 6. For a serial link change, `reboot` stays a plain reboot and prints the pending trial window and the `link-confirm` command to run after the board restarts:

~~~sh
drivelink-cli --endpoint serial:///dev/ttyUSB0 set 0:0:48=2000000
drivelink-cli --endpoint serial:///dev/ttyUSB0 save
drivelink-cli --endpoint serial:///dev/ttyUSB0 reboot
drivelink-cli --endpoint serial:///dev/ttyUSB0 link-confirm
~~~

Run `drivelink-cli --endpoint serial:///dev/ttyUSB0 items 0` to list every setting and its range. Settings 0:0:16 to 0:0:23 are the limits, and 0:0:48 to 0:0:50 are the serial link settings.

Next, add what the new capacity allows on [Layout]({{base}}pc/layout/), or go to [First spin]({{base}}pc/first-spin/).
