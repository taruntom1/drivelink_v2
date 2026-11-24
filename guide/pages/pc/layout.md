# Build the layout

You need the app connected to a disarmed board and your [wiring notes]({{base}}pc/wire/). Keep the motor supply off. You’ll add the motor, encoder, and controller to a shared [loop]({{base}}concepts/modules/) and keep that configuration across a reboot.

## Add the instances

1. Open **Layout**. A fresh board shows the real-time control loop at 1,000 Hz and the sensor loop at 100 Hz. Use the control loop for the motor, encoder, and controller. If you are starting with a layout with no compatible loop, use **Add → Loop**, give the loop a label, enable real-time, and set its rate to 1,000 Hz. The loop should appear as pending.
2. Use **Add → Instance** for each row below. Pick the type, enter the label, and choose the control loop. You should see the labeled instances staged under the same loop.

   | Type | Label |
   | --- | --- |
   | `actuator.pwm_dir` | `left_pwm` |
   | `feedback.quadrature` | `left_enc` |
   | `controller.cascade_pid` | `left_drive` |

{{figure:layout-add-instance}}

## Set pins and limits

1. Select **left_pwm** and open **Params**. Set `pwm_pin` to 4 and `dir_pin` to 5 for the reference wiring. Set `duty_limit` to 0.2 or lower for the first run. Keep `pwm_freq_hz` at 25000 unless your driver’s datasheet gives a lower maximum PWM frequency. Press Enter after typing each value. Check that each pin shows its pending value and the duty limit shows the cap you set.
2. Select **left_enc**. Set `pin_a` to 6 and `pin_b` to 7 for the reference wiring. Set `counts_per_rev` to the counts per motor revolution **after 4× quadrature decoding**. If the datasheet gives complete A-channel cycles per motor revolution, multiply by four. If it already counts all four edges, enter its count directly. Set `gear_ratio` to the number of motor turns per output turn. Compare the pending values with the exact motor and encoder data.
3. Select **left_drive**. In **Wiring**, choose **left_pwm** as the actuator and **left_enc** as the feedback. Set `mode` to `open_loop` and `duty_limit` to 0.2 or lower. You should see the correct wiring targets and both duty limits should be set before any power is applied to the motors.
4. For a second motor, repeat with `right_pwm`, `right_enc`, and `right_drive` and the second channel from the pin table. Check each controller’s wiring targets against its wheel label.

## Save and check

1. Use **Review** on the pending banner. Check the staged instances, pins, and wiring before selecting **Save and reboot**. The app should reconnect after the board restarts.
2. Return to **Layout**. Check that the pending banner is gone and the expected labels, pins, and wiring are active. Confirm the board is still disarmed.

{{figure:layout-review}}

See [Saving and reverting]({{base}}concepts/saving/) if you need to undo a change. When the active layout matches the wiring, go to [First spin]({{base}}pc/first-spin/).
