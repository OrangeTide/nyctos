// Section 10.5: prompt cancellation with three parties.
#import "lib/fit.typ": fit
#import "lib/seq.typ": *


#fit(msc(
  actors: ("Supervisor", "Client", "Kernel", "Server"),
  colw: 3.5,
  events: (
    msg("Client", "Kernel", "sys_msg_send()"),
    msg("Kernel", "Server", "request delivered"),
    note("Client", "BLOCKED_SENDING"),
    note("Server", "allocated buffers for the request"),
    gap(),
    msg("Supervisor", "Kernel", "sys_thread_cancel(client)"),
    band("cancellation is prompt: the client does not wait for the server"),
    err("Kernel", "Client", "returns SYS_ERR_INTERRUPTED"),
    err("Kernel", "Server", "SYS_PULSE_DEAD_OWNER, value = SYS_ID_TAG32(client)"),
    note("Server", "releases the request’s resources"),
    gap(),
    band("if the server replies anyway"),
    msg("Server", "Kernel", "sys_msg_reply(client, ...)"),
    err("Kernel", "Server", "returns SYS_ERR_STALE"),
  ),
))
