# Connect and tour

You need the installed app, an ESP with [DriveLink firmware]({{base}}flash/), and a USB data cable. Keep the motor driver disconnected. At the end you’ll see the firmware version and know where to configure and monitor the board.

1. Plug the cable into the board’s **USB-to-UART** port, the same port you used to flash it. Open **Connect** in the app and choose **Serial**. A new port should appear under **Serial ports**. If you see several ports, unplug and replug the cable and watch which entry disappears and returns.
2. Select that port, leave **Baud** at **921600** for the reference build, and select **Connect**. The app loads the device, then opens **Device**. Check the firmware version and confirm the board is disarmed.
3. Open **Layout**, **Monitor**, and **Tune** in turn. You should see the loops on **Layout**. **Monitor** is where you can add plots and manual commands. **Tune** is where you can run tests after adding a controller.

If the port is missing, check the data cable and close the browser flasher or any serial terminal. If Ubuntu shows the port but denies access and the port is owned by the `dialout` group, run the following command as your normal desktop user. Sign out and back in, reopen DriveLink, and try the port again.

~~~sh
sudo usermod -aG dialout "$USER"
~~~

The status bar shows whether you’re connected and armed. **Faults** opens the device’s fault list. Keep the device [disarmed]({{base}}concepts/safety/) while you wire or edit the layout.

[Wire the bench]({{base}}pc/wire/) when the firmware version and disarmed state are visible.
