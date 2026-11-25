"""Run by GDB (tools/jtag.py tasks): one line per FreeRTOS task.

OpenOCD's thread ids are the TCB addresses. FreeRTOS fills a new stack with
0xA5, so the untouched bytes above pxStack are the smallest free stack so far.
"""
import gdb

STACK_FILL = 0xA5
CHUNK = 256


def untouched(address):
    free = 0
    while True:
        data = bytes(gdb.selected_inferior().read_memory(address + free, CHUNK))
        used = len(data) - len(data.lstrip(bytes([STACK_FILL])))
        free += used
        if used < CHUNK:
            return free


tcb_type = gdb.lookup_type('struct tskTaskControlBlock').pointer()
print(f"{'task':<16} {'prio':>4} {'core':>4} {'free min':>8}  now")
for thread in sorted(gdb.selected_inferior().threads(), key=lambda t: t.num):
    tcb = gdb.Value(thread.ptid[1] or thread.ptid[2]).cast(tcb_type)
    free = untouched(int(tcb['pxStack']))
    core = int(tcb['xCoreID'])
    thread.switch()
    frame = gdb.newest_frame()
    print(f"{tcb['pcTaskName'].string():<16} {int(tcb['uxPriority']):>4} "
          f"{core if core in (0, 1) else 'any':>4} {free:>8}  {frame.name() or hex(frame.pc())}")
