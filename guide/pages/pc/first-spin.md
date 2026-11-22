# First spin

You need a secured motor and encoder, the [saved open-loop layout]({{base}}pc/layout/), and the app connected. Test one wheel at a time. You’ll verify that a positive command moves it forward and that the encoder reports positive velocity.

> **Safety.** Lift the wheels clear of the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch. Keep clear of moving parts. If the wheel moves unexpectedly, select **DISARM** or press **Esc**. Cut motor power if the app cannot stop it.

1. With the board disarmed, check that both the actuator and controller `duty_limit` are at or below 0.2 and that the controller is in `open_loop`. In **Monitor**, select **left_drive**’s `velocity` signal. A trace and current reading should appear.
2. In **Manual command**, select **Command another input**, then the `duty_sp` for **left_drive**. Use the pencil next to the limit, enter 0.1 or less, and press Enter. Leave **Latch** off. The command should read zero and remain blocked while disarmed.
3. Confirm the wheel is clear, turn on the motor supply, and select **ARM**. Check that the state says armed. If the app refuses, read the failed checks. Resolve the cause before arming again.
4. Drag the command slider a small amount in the positive direction. The wheel should turn in the direction you’ve chosen as forward, and the controller’s velocity should be positive. Release the slider. The command should ramp to zero. Select **DISARM** and check that the status is disarmed.
5. If the wheel direction was wrong, adjust `invert` on the PWM actuator while disarmed. If the wheel turns forward but the measured velocity is negative, adjust `invert` on the encoder while disarmed. Repeat the low-duty check after each change. If velocity stays at zero while the wheel turns, disarm, cut power, and check the encoder wiring and power before trying again.
6. With the board disarmed and both signs correct, select **Save**. Check that **Unsaved** clears. Switch off the motor supply before making wiring changes.

{{figure:monitor-command-disarmed}}

Once the signs match, [monitor the signals]({{base}}pc/monitor/) or continue to [tune the controller]({{base}}pc/tune/).
