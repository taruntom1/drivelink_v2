# Wire the bench

You need the [reference parts]({{base}}reference/bench/) or compatible substitutes, the exact encoder pinout, and a way to check supply polarity and logic voltage. Start with one motor and its encoder. At the end you’ll have checked the wiring and will be ready to enter the matching pins in the app.

> **Safety.** Unplug USB and switch off the motor supply before wiring. Secure the motor or chassis. Before any powered motion, lift the wheels clear of the ground, use a current-limited supply, set a low duty cap, and keep one hand on the power switch.

## Motor and encoder

Select a label in the diagram to jump to its connections. The figure shows the first motor channel. Use the table when adding a second channel.

<figure class="wiring">
<svg viewBox="0 0 480 290" role="group" aria-label="Motor and encoder wiring. Select a component or connection.">
  <path d="M146 68H334 M400 100V188 M80 188V100 M316 147H352V100" />
  <rect x="16" y="40" width="130" height="60" rx="8" />
  <rect x="334" y="188" width="130" height="66" rx="8" />
  <rect x="16" y="188" width="130" height="66" rx="8" />
  <rect x="334" y="40" width="130" height="60" rx="8" />
  <text x="81" y="76">ESP32-S3</text>
  <text x="399" y="76">G2 18v17</text>
  <text x="399" y="214">Motor 1</text>
  <text x="399" y="237">Power wires</text>
  <text x="81" y="214">Encoder A/B</text>
  <text x="81" y="237">and power</text>
  <a href="#motor-and-encoder-pins" aria-label="Motor driver PWM and direction pins">
    <rect x="164" y="47" width="152" height="43" rx="8" />
    <text x="240" y="75">PWM / DIR</text>
  </a>
  <a href="#encoder-power-and-levels" aria-label="Encoder power and logic levels">
    <rect x="16" y="114" width="130" height="61" rx="8" />
    <text x="81" y="151">GPIO 6 / 7</text>
  </a>
  <a href="#motor-power" aria-label="Fused 12 V motor power">
    <rect x="164" y="119" width="152" height="56" rx="8" />
    <text x="240" y="153">Fused 12 V</text>
  </a>
</svg>
<figcaption>The fused supply feeds the driver, which powers the motor. The encoder’s A and B wires go back to the ESP. Share signal ground.</figcaption>
</figure>

### Motor and encoder pins

| Signal | First motor | Optional second motor |
| --- | --- | --- |
| PWM from ESP to driver | GPIO 4 → first driver PWM | GPIO 10 → second driver PWM |
| Direction from ESP to driver | GPIO 5 → first driver DIR | GPIO 11 → second driver DIR |
| Encoder A to ESP | A → GPIO 6 | A → GPIO 12 |
| Encoder B to ESP | B → GPIO 7 | B → GPIO 13 |
| Motor power from driver | First driver OUTA and OUTB → first motor’s two power wires | Second driver OUTA and OUTB → second motor’s two power wires |
| Ground | ESP GND, driver GND, and encoder GND | Same shared ground |

### Motor power

1. With all power off, wire the driver’s **VIN** to the fused and switched 12 V supply and connect supply negative to driver **GND**. Use motor-rated wires. Fit the supply capacitor at the driver as described in the [Pololu wiring instructions](https://www.pololu.com/product/2991#connections). Check the capacitor polarity against the markings.
2. Wire the motor to **OUTA** and **OUTB**. Connect PWM, DIR, and the shared ground as shown in the table. On the md31c revision, **SLP** defaults to enabled. Leave the driver’s **3V3** output disconnected. You should be able to follow separate wires for motor power and encoder signals.
3. Power the ESP from USB. Leave the motor supply off until you’ve configured the layout and reached [First spin]({{base}}pc/first-spin/).

### Encoder power and levels

1. Identify the encoder’s supply, ground, A, and B from its datasheet. Verify the recommended supply and the type of A/B outputs. Wire colours vary by model.
2. Power the encoder from a suitable regulated rail. Connect A and B to the ESP only when their logic is compatible with 3.3 V. Use appropriate level translation for higher-voltage outputs. You should be able to trace encoder ground to ESP ground and each channel to its own GPIO.

## IMU

You can wire the IMU now or return when you’re ready to [add the sensor]({{base}}pc/imu/).

<figure class="wiring">
<svg viewBox="0 0 480 200" role="group" aria-label="ESP32-S3 to BNO055 wiring. Select a connection.">
  <path d="M132 56H348 M132 138H348" />
  <rect x="16" y="28" width="116" height="138" rx="8" />
  <rect x="348" y="28" width="116" height="138" rx="8" />
  <text x="74" y="105">ESP32-S3</text>
  <text x="406" y="105">BNO055</text>
  <a href="#imu-pins" aria-label="IMU data and clock pins">
    <rect x="154" y="32" width="172" height="49" rx="8" />
    <text x="240" y="63">SDA / SCL</text>
  </a>
  <a href="#imu-pins" aria-label="IMU power and ground pins">
    <rect x="154" y="113" width="172" height="49" rx="8" />
    <text x="240" y="144">3.3 V / GND</text>
  </a>
</svg>
<figcaption>The I²C data and clock lines go between the ESP and the Adafruit breakout.</figcaption>
</figure>

### IMU pins

| ESP32-S3 | Adafruit BNO055 breakout |
| --- | --- |
| GPIO 8 | SDA, the data line |
| GPIO 9 | SCL, the clock line |
| 3V3 | VIN |
| GND | GND |

Use the breakout’s default I²C address, `0x28`, for this layout. When you finish, check that the IMU is on the ESP’s low-voltage rail and the motor supply is still off.

[Build the layout]({{base}}pc/layout/) using the GPIO numbers above.
