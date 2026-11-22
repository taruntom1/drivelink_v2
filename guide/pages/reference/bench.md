# Reference build

Use this list when choosing parts for the PC bench or a later Pi robot. Start with the board and cable. A single motor and encoder are enough for the first spin. Add the second wheel, IMU, and Pi when you need them.

| Part | What to check |
| --- | --- |
| ESP32-S3-DevKitC-1 | Choose a variant with 16 MB flash, such as N16R8. Confirm your board has the pins in the [wiring tables]({{base}}pc/wire/). The bench uses its USB-to-UART port and a USB data cable. [Espressif board guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/) |
| Pololu G2 High-Power Motor Driver 18v17 | Use one current-revision board (marked md31c) per brushed DC motor. Check the driver and connector ratings against the motor’s running and stall current. Include a supply capacitor as specified by Pololu. [Pololu specifications and pinout](https://www.pololu.com/product/2991) |
| 12 V JGB37-520-class gear motor with encoder | Check the exact model’s rated and stall current, pinout, encoder supply and output levels, counts per motor turn, and gear ratio. Check the output shaft and mounting before choosing wheels. |
| Fused 12 V motor supply | Use a current-limited supply. Choose the fuse and wire size for your actual parts. Follow the driver vendor’s guidance for supply voltage spikes and motor braking. |
| Adafruit BNO055 breakout | Optional for the bench. Use the breakout's I²C connections and mount it so you know which way its axes point. [Adafruit pinouts](https://learn.adafruit.com/adafruit-bno055-absolute-orientation-sensor/pinouts) |
| Raspberry Pi 5 and buck converter | The Pi is for the later robot. A buck converter steps the 12 V rail down for compatible loads. For the Pi, use its recommended USB-C power supply on the bench or a vehicle power solution that meets its USB-C requirements. [Raspberry Pi power supply requirements](https://www.raspberrypi.com/documentation/computers/getting-started.html#recommended-power-supply) |
| Bench hardware | Secure the motor or chassis, support the wheels clear of the bench, and put the motor power switch within reach. Use motor and power wiring rated for the current. |

## What else works

- A substitute driver needs **PWM + DIR** inputs for sign-magnitude control at 3.3 V logic and must support the motor’s voltage and current. It must accept the released firmware’s 25 kHz PWM. For example, Cytron lists a 20 kHz maximum for the MDD10A. [Cytron specifications](https://www.cytron.io/p-10amp-5v-30v-dc-motor-driver-2-channels)
- Use an encoder with two quadrature channels. Keep ESP inputs at 3.3 V logic. If the encoder outputs 5 V, use suitable level translation. Open-drain outputs need pull-ups to a compatible rail. Check the datasheet before connecting anything.
- Use free [GPIOs]({{base}}concepts/glossary/) that can perform the intended function. Keep the UART pins free for the USB-to-UART link, and avoid the board’s boot-strapping, USB, flash, PSRAM, and on-board LED pins. The ESP configuration permits up to six PWM actuator instances and four quadrature feedback instances, subject to available pins and device limits.

When you have the parts, [install the app]({{base}}pc/install/) or open the [wiring page]({{base}}pc/wire/) to check the connections.
