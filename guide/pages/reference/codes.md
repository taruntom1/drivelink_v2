# Status, fault, and diagnostic codes

Use this page when the app’s diagnostics or a ROS log shows a code. A request reports a result and can add a detail. Diagnostic entries describe events or conditions. For a fault, fix the cause before acknowledging it. See [Arm is refused]({{base}}troubleshooting/#arm-is-refused) for common actions.

## Device status

| Arm state | What you can expect |
| --- | --- |
| Disarmed | Motor output is disabled. You can read the device and prepare a command. |
| Armed | The device can act on fresh motion commands. |
| Fault | Motor output has been disabled. Clear the cause and acknowledge the fault before arming. |

The status stream also identifies why the device last disarmed. An explicit disarm can replace an earlier reason. Use the diagnostic record and the ROS log to find the original trigger.

| Last disarm code | Meaning |
| --- | --- |
| 0 | The device booted. |
| 1 | A client or operator requested disarm. |
| 2 | The command watchdog expired. |
| 3 | A fault disarmed the device. |
| 4 | A device loop stalled. |
| 5 | The device shut down. |

## Request result

{{status_reference}}

## Request detail

{{detail_reference}}

## Diagnostic codes

Find both the source and the code. Different sources can use the same number. A record describes an event. A counter reports how often something occurred. The **Clears** column tells you whether the diagnostic can report that its condition has cleared.

{{diagnostic_reference}}
