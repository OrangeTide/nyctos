// Section 15.3: a page fault is a message the faulting thread sends.
#import "lib/fit.typ": fit
#import "lib/seq.typ": *


#fit(msc(
  actors: ("Faulting thread", "Kernel", "Pager"),
  colw: 4.0,
  events: (
    note("Faulting thread", "executes from unmapped text"),
    msg("Faulting thread", "Kernel", "page fault trap"),
    msg("Kernel", "Kernel", "synthesize message as if sent by the thread"),
    note("Faulting thread", "BLOCKED_SENDING"),
    msg("Kernel", "Pager", "sys_msg_recv() returns the fault"),
    band("priority donated: pager runs at the faulting thread’s priority"),
    note("Pager", "locates the page"),
    ret("Pager", "Kernel", "sys_msg_reply() with a read-only grant"),
    ret("Kernel", "Faulting thread", "resume"),
    note("Faulting thread", "retries the instruction"),
  ),
))
