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

## The same change from a terminal

The command-line tool is installed with the app on Ubuntu. Replace the port with yours. Setting 0:0:16 is `limits.loops`.

~~~sh
drivelink-cli --endpoint serial:///dev/ttyUSB0 set 0:0:16=6
drivelink-cli --endpoint serial:///dev/ttyUSB0 save
drivelink-cli --endpoint serial:///dev/ttyUSB0 reboot
drivelink-cli --endpoint serial:///dev/ttyUSB0 get 0:0:16
~~~

The first command should report `pending_reboot: true`, and the last should report 6. Run `drivelink-cli --endpoint serial:///dev/ttyUSB0 items 0` to list every setting and its range. Settings 0:0:16 to 0:0:23 are the limits.

Next, add what the new capacity allows on [Layout]({{base}}pc/layout/), or go to [First spin]({{base}}pc/first-spin/).
