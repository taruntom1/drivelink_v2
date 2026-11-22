# Wire the Pi to the ESP

You need the [prepared Pi]({{base}}pi/prepare/), the [reference build]({{base}}reference/bench/), and a board you have [flashed]({{base}}flash/) and [configured on the bench]({{base}}pc/layout/). At the end the Pi and ESP will share a 3.3 V serial link. The bench page covers the [two motor channels]({{base}}pc/wire/#motor-and-encoder-pins) and [IMU wiring]({{base}}pc/wire/#imu-pins).

> **Safety.** Disconnect the Pi, ESP, and motor power before changing wiring. Verify the board pin labels and use 3.3 V logic on both ends.

1. Isolate the USB-to-UART bridge’s TX connection to ESP GPIO 44 before connecting the Pi’s TX. The bridge and the Pi must never drive that pin together. Check your board’s schematic for how to isolate the bridge. If your build uses USB between the Pi and the board, keep the USB-to-UART bridge connected and use its USB port for data.
2. For GPIO UART, cross TX and RX and connect ground as shown. Check each wire end to end before restoring power.

<figure class="wiring">
<svg viewBox="0 0 480 220" role="group" aria-label="Pi 5 to ESP32-S3 UART connections">
  <path d="M136 57H344 M136 110H344 M136 163H344" />
  <rect x="16" y="27" width="120" height="166" rx="8" />
  <rect x="344" y="27" width="120" height="166" rx="8" />
  <text x="76" y="116">Pi 5</text>
  <text x="404" y="116">ESP32-S3</text>
  <a href="#uart-pins" aria-label="Pi pin 8 transmit to ESP GPIO 44 receive">
    <rect x="154" y="36" width="172" height="42" rx="8" />
    <text x="240" y="64">TX → RX</text>
  </a>
  <a href="#uart-pins" aria-label="ESP GPIO 43 transmit to Pi pin 10 receive">
    <rect x="154" y="89" width="172" height="42" rx="8" />
    <text x="240" y="117">RX ← TX</text>
  </a>
  <a href="#uart-pins" aria-label="Pi pin 6 ground to ESP ground">
    <rect x="154" y="142" width="172" height="42" rx="8" />
    <text x="240" y="170">GND</text>
  </a>
</svg>
<figcaption>The serial link carries two data signals and shares ground. Power the Pi and the ESP through their intended power inputs.</figcaption>
</figure>

## UART pins

| Pi 5 header | ESP32-S3 |
| --- | --- |
| Pin 8, GPIO 14 (TX) | GPIO 44 (RX) |
| Pin 10, GPIO 15 (RX) | GPIO 43 (TX) |
| Pin 6, GND | GND |

Leave motor power off and [install the agent]({{base}}pi/agent/) to check the link.
