# Actuators, encoders, and controllers

If you’re setting up a wheel, this page will help you choose what to command and what to measure.

An **actuator** drives the motor through a motor driver. In the reference build, **PWM** makes the driver switch power rapidly. The **duty** is the fraction of time the pulse is on. A separate **direction** signal selects which way it runs.

An **encoder** measures turning. A **quadrature encoder** uses two signals, A and B. Which signal changes first tells the board which way the shaft is turning. The controller can use the encoder counts and gear ratio to turn those changes into position and velocity.

| Mode | What you ask for | What the controller does |
| --- | --- | --- |
| Open loop | A signed duty | Applies duty within its limit. The actual speed depends on the motor and load. |
| Velocity | A speed | Adjusts duty using the difference between measured and requested speed. |
| Position | An angle | Uses the measured angle to bring the shaft to the requested position. |

A **PID** controller adjusts its output using the error now (proportional), accumulated error (integral), and how fast the error changes (derivative). Those three weights are called **gains**. Confirm encoder polarity in open loop before using feedback.

Next, [check the first spin]({{base}}pc/first-spin/) or learn how to [tune a controller]({{base}}pc/tune/).
