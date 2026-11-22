# Monitor

Connect to a board with a saved layout. You can use **Monitor** while disarmed. You’ll plot a device [signal]({{base}}concepts/modules/) and pause the display to inspect it.

1. Open **Monitor**. In the signal list, find **left_drive** and select `velocity`. You should see a trace with a scale in radians per second. A stopped wheel should read close to zero.
2. Select `duty` under the same instance to see the controller output. Look for its own scale, since duty and velocity have different units. With the board disarmed, motor duty should stay at zero.
3. Use **Window** to choose how much time you can see. Use **Pause** or press **Space** while focused on the plot area to freeze the display. The control should change to **Resume**. Resume to return to live data.
4. If adding signals exceeds the link budget, lower a loop’s plot rate in **Plot rates** or deselect signals. The plot rate controls what the app receives. The device loop still runs at its configured rate.

{{figure:monitor}}

[Tune and identify]({{base}}pc/tune/) once you’ve completed [First spin]({{base}}pc/first-spin/).
