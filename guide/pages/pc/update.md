# Update firmware

Use this page if DriveLink is already installed on your ESP. You need the desktop app, a reliable USB connection, and the [firmware zip]({{base}}downloads/#esp32-s3-firmware) for the release you want. You’ll install the application image, confirm the version, and check that the saved layout is present.

1. Disarm, switch off motor power, and **Save** any settings you want to keep. Check that **Unsaved** clears. Download and extract the firmware zip. You should see `drivelink-app.bin`, the image for updates through the app. Keep the USB cable connected throughout the update.
2. Open **Device → Firmware update → Choose image…**. Select `drivelink-app.bin`. Check the reported version and file details before continuing.
3. Select **Install** with **Install without confirming** left unchecked. The app should write and verify the image, wait for the board to restart, then reconnect and confirm it. Wait for **Confirmed** before closing the dialog.
4. Check the firmware version on **Device** and confirm the board is disarmed. Return to **Layout** and check the saved loops and labels.

{{figure:update-confirmed}}

If the dialog shows **Unconfirmed** or **Rolled back**, read the update message and check the running version before making another change. To set up an empty board, use [Flash in the browser]({{base}}flash/).
