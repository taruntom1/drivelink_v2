# Arming and faults

Read this before sending any motor command. You’ll know when the board allows motion and what to do when it stops.

| State or feature | What it means |
| --- | --- |
| Disarmed | The board keeps the outputs disabled. Start here for wiring, configuration, and updates. |
| Armed | The board can act on valid commands. Check that the area is clear before selecting **ARM**. |
| Watchdog | If command updates stop arriving in time, the board forces the commands to safe values. |
| Fault | The board reports a problem. Some faults disarm it or block arming. Open **Faults** and fix the reported cause before acknowledging it. |
| Safe mode | The board starts with motion disabled after a boot problem. Use the Device screen to investigate the cause before exiting safe mode. |

> **Safety.** Before a step that can move a motor, lift the wheels clear of the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch. If anything moves unexpectedly, select **DISARM** or press **Esc** in the app. Cut motor power if you cannot stop it from the app.

If **ARM** is refused, read the failed checks before trying again. After a stop, confirm the app shows the board as disarmed. Resume with the [first-spin checks]({{base}}pc/first-spin/).
