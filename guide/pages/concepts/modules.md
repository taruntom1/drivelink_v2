# Modules, instances, and loops

If you’ve connected a board, this page helps you read the **Layout** screen and decide where each part belongs.

A **module type** describes a function the board knows how to run, such as a quadrature encoder. An **instance** is one use of that type, such as the encoder for your left wheel. Give each instance a label you can recognize on **Monitor** and **Tune**.

A **loop** runs its instances on a schedule. A loop set to 1,000 Hz runs 1,000 times per second. A real-time control loop serves timing-sensitive motor work. A slower sensor loop can read the [IMU]({{base}}concepts/imu/) without delaying the motor loop.

The board tells the app what types and settings it supports. A **parameter** is a setting you can change, such as a pin. A **signal** is a value you can read, such as measured speed. On **Layout**, a controller’s wiring choices connect it to other instances. The motor, encoder, and controller for one wheel belong in the same control loop.

[Build your layout]({{base}}pc/layout/) to see how those pieces fit.
