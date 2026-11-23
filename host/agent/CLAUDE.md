# Agent rules

- Agent ([agent/README.md](README.md), contract [agent.md](../../docs/protocol/agent.md)): nothing from a device reaches a connection before its Hello is accepted; refuse, then close.
- One agent thread owns sockets, routing and group maps; Session completions and frames come to it through the dispatcher, guarded by the device's link and map generations. Never close a connection inline while iterating clients.
- DISARM from any client goes out at once on the fast path (it waits, bounded, only when the Session cannot admit it). ARM, CMD_CONFIG/CMD_CLEAR and command frames reach a device only from the lease holder; exclusive operations only from the holder or while nobody holds it and the device is not armed. A lease end stops the old holder at once, DISARMs, clears its command groups (an ARM still in flight gets another DISARM first), then grants a waiting Take; ARM waits while an earlier holder's command slot remains.
- On every device open: GET_INFO, DISARM, clear all groups, then accept clients.
- Keep routing, the group map, LIST, the lease and history pure (`groups.cpp`, `lease.cpp`, `history.cpp`) with unit tests.
- Agent clients ping the agent, never the device, and compose its ClockModel.
- The mDNS responder (`mdns.cpp`, own thread) answers only from fixed settings and never touches a device, a client or the lease; it advertises only on LAN binds, and parsing a received packet never throws.
