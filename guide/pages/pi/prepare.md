# Prepare the Raspberry Pi

You need a Raspberry Pi 5, its power supply, a microSD card, Raspberry Pi Imager, and a PC to write the card. At the end you’ll have Ubuntu running with a serial port ready for DriveLink.

1. In [Raspberry Pi Imager](https://www.raspberrypi.com/software/), choose Raspberry Pi 5 and Ubuntu Server 26.04 LTS (64-bit). Set a user, password, and network connection in the imager. Ubuntu Server 24.04 also works for the agent. When the card is ready, insert it into the Pi, boot, and sign in.
2. Update the installation. Apt should finish without errors.

   ~~~sh
   sudo apt update && sudo apt full-upgrade
   ~~~

3. Enable the Pi’s GPIO [UART]({{base}}concepts/glossary/), the serial connection on header pins 8 and 10. Edit `/boot/firmware/config.txt` and add this setting under `[all]`.

   ~~~ini
   dtoverlay=uart0-pi5
   ~~~

4. If `config.txt` contains `dtparam=uart0_console`, remove that option. Open `/boot/firmware/cmdline.txt`. Remove `console=ttyAMA0,115200` if present. Keep `console=tty1` and keep the file on one line. Stop the serial login prompt and reboot. Expect the connection to close if you are signed in remotely.

   ~~~sh
   sudo systemctl disable --now serial-getty@ttyAMA0.service
   sudo reboot
   ~~~

5. Sign in again and list the UARTs. Look for `/dev/ttyAMA0`, which uses GPIO 14 and 15. The Pi 5’s separate debug header uses `/dev/ttyAMA10`.

   ~~~sh
   ls -l /dev/ttyAMA*
   ~~~

Next, [wire the Pi to the ESP]({{base}}pi/wire/).

See the [Pi 5 UART overlay reference](https://github.com/raspberrypi/firmware/blob/master/boot/overlays/README) for the GPIO assignment.
