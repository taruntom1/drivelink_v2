# Agent, leases, and tokens

If your ESP is on a robot away from the PC, this page explains who connects to it and who can issue motion commands.

The **agent** is a service that runs on a Linux computer such as a Raspberry Pi. It keeps the serial connection to the ESP open. The desktop app or ROS 2 connects to the agent over a network. A **token** is the shared secret the agent checks before accepting a connection. Share it only with clients you trust.

A new desktop connection starts as an **observer**. An observer can view the device and its signals. A **lease** grants one client at a time the right to arm and send motor commands. If the lease ends, the agent disarms the board before handing control to a new client. Getting a lease does not arm the board. Any connected client can request disarm.

For a direct USB connection from your PC, the app uses the ESP’s serial port. See how the paths fit together in [Start here]({{base}}start/).
