# Flash in the browser

Install DriveLink on a new ESP32-S3 with 16 MB of flash, or reset an existing board. You need a USB data connection to the port you normally use for programming. Use Edge or Chrome on your computer, or follow the command-line fallback on this page.

> **Warning.** First flash erases the board’s saved layout and settings. Disconnect any motor drivers before you start. To update an existing DriveLink installation and keep its settings, use **Firmware update** on the desktop app’s **Device** page.

## Flash with Edge or Chrome

1. Close any app using the board’s port and plug the board into your computer. The browser should list the board or its USB serial adapter when you open the port picker.
2. Select **Connect** below. Choose the board’s port in the browser’s picker and select **Connect** again.

{{flasher}}

3. Select **Install DriveLink**, then select **Install** to confirm. Keep the board connected until the installer shows **Installation complete!**
4. Select **Next**, then close the installer dialog so the desktop app can use the port. Open the [DriveLink desktop app]({{base}}downloads/#desktop-app). On **Connect**, select the same serial port and connect. The app should show the device details.

## Flash with esptool

Use this fallback if your browser cannot access the serial port. Use the same USB port as above. On Windows, install Python and make it available in PowerShell. On Ubuntu, you need Python 3, the python3-venv package and permission to open the serial port.

1. {{firmware}}. Extract it and open a terminal in the extracted folder. You should see drivelink-merged.bin and flash_args.json there.
2. Choose your computer below. Create a Python environment in this folder and install Espressif’s esptool. The terminal should report that the install succeeded.

<div class="command-tabs" data-choice-group>
<div data-choice="Windows">

~~~powershell
python -m venv .flash-tools
.\.flash-tools\Scripts\python.exe -m pip install esptool==4.11.0
~~~

### Write the image on Windows

Replace PORT with your board’s serial port, such as COM3. The terminal should report that it verified the data and reset the board.

~~~powershell
.\.flash-tools\Scripts\python.exe {{esptool}}
~~~

</div>
<div data-choice="Ubuntu">

~~~sh
python3 -m venv .flash-tools
./.flash-tools/bin/python -m pip install esptool==4.11.0
~~~

### Write the image on Ubuntu

Replace PORT with your board’s serial port, such as /dev/ttyUSB0. The terminal should report that it verified the data and reset the board.

~~~sh
./.flash-tools/bin/python {{esptool}}
~~~

</div>
</div>

Open DriveLink and connect to the same port. You should see the device details.

Continue to [connect and tour the app]({{base}}pc/connect/) to check your first connection.
