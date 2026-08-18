// Section 12.3: synchronous rendezvous with priority inheritance.
#import "lib/fit.typ": fit
#import "lib/seq.typ": *


#fit(msc(
  actors: ("Client (prio 40)", "Kernel", "Server (prio 10)"),
  colw: 4.4,
  events: (
    note("Server (prio 10)", "blocked in sys_msg_recv()"),
    msg("Client (prio 40)", "Kernel", "sys_msg_send(chan, ...)"),
    note("Client (prio 40)", "BLOCKED_SENDING"),
    band("rendezvous: highest-priority sender is dequeued first"),
    msg("Kernel", "Server (prio 10)", "deliver + client thread id"),
    band("server runs at max(10, 40) = 40 for the request’s duration"),
    note("Server (prio 10)", "effective priority 40"),
    msg("Server (prio 10)", "Kernel", "sys_msg_reply(client, ...)"),
    band("donation ends; server reverts to 10"),
    ret("Kernel", "Client (prio 40)", "reply delivered, client runnable"),
  ),
))
