# Tune and identify

You need a saved [first-spin setup]({{base}}pc/first-spin/) with confirmed motor direction and encoder sign. Identify a starting set of velocity gains, then check them with a small speed step.

> **Safety.** Lift the wheels clear of the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch. For an unexpected response, use **Stop**, **DISARM**, or **Esc**. Cut motor power if the app cannot stop movement.

## Identify a starting point

**Plant identification** measures how the motor responds to a change in duty. It uses that response to suggest starting values for [controller gains]({{base}}concepts/control/).

1. On **Tune**, select **left_drive** and open **Plant identification**. Confirm the actuator can move freely. In **Run the step**, select `duty_sp` and `open_loop` if needed. Set **From** to zero and **To** to a small positive duty already tested in **First spin**. Check that it stays within your duty cap.
2. With the wheel clear, turn on the motor supply. Select **ARM**, then **Run step**. You should see a captured response once the test finishes and the command goes back to zero. Disarm before reviewing or editing settings.
3. Under **Model and suggested gains**, check that the velocity response goes in the expected direction and has a usable fit. If the fit is poor or there are no suggestions, check encoder polarity, binding, and whether the wheel actually turned. **Apply suggested gains** changes the values immediately. You should see the new values and **Unsaved**. Treat them as starting values.

{{figure:tune-identify}}

## Check a velocity step

> **Safety.** Lift the wheels clear of the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch. Start with a low target speed and keep **Auto re-run** off so changing a value cannot start another run.

1. While disarmed, set **left_drive**’s mode to `velocity` and open **Step runs**. In **Test signal**, choose `velocity_sp`, a step, zero offset, a small positive amplitude in radians per second, and a short duration. Check that the setup shows the speed you mean to test.
2. Under **Capture**, include the measured `velocity`. Select **ARM**, then **Run**. When the run finishes, you should see the command and response on the plot. Select **DISARM**.
3. Compare the measured speed with the target. A large overshoot or repeated oscillation means the response needs work. Change one gain at a time and repeat the same small test. When the response is stable, select **Save** while disarmed to keep the settings, or use **Revert** to return to your last saved configuration. Check that **Unsaved** clears after saving.

{{figure:tune}}

Switch off the motor supply. You can now [add the IMU]({{base}}pc/imu/) or skip to [firmware updates]({{base}}pc/update/).
