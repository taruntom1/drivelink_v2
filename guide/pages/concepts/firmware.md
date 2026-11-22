# Firmware and serial

If you’re about to flash or connect an ESP32-S3, this page explains what goes on the board and what travels over the cable.

**Firmware** is the program stored in the ESP’s flash memory. It starts when the board powers up. It handles the motor and sensor timing even when your computer is busy. The DriveLink desktop app runs on your computer and lets you see and configure the running board.

**Serial** is a stream of bytes. The ESP and app have to agree on the speed, called the **baud rate**, and on the message format. The reference board uses a USB-to-UART bridge, a chip that converts your computer’s USB connection to the ESP’s serial connection. Windows gives the bridge a name like `COM3`. Ubuntu can show a path like `/dev/ttyUSB0`.

**First flash** writes a complete image to the board and clears its saved settings. Later, **Firmware update** in the app installs the application image and keeps the saved layout.

[Espressif’s board guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/) shows the physical ports. Continue to [flash your board]({{base}}flash/) or [connect the app]({{base}}pc/connect/).
