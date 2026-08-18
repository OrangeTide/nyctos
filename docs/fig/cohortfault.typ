// Section 18.2: a cohort fault takes down the whole cohort, and the
// supervisor restarts it.
//
// The point of the chart is that member B, which did nothing wrong, is torn
// down alongside A. Sharing a protection domain means A may have corrupted B
// before faulting, and nothing can establish that it did not.

#import "lib/fit.typ": fit
#import "lib/seq.typ": *


#fit(msc(
  actors: ("Client", "Member A", "Member B", "Kernel", "Supervisor"),
  colw: 3.3,
  step: 0.88,
  events: (
    note("Supervisor", "blocked on the completion timeline"),
    msg("Client", "Member A", "sys_msg_send()"),
    note("Client", "BLOCKED_SENDING"),
    note("Member B", "holds cohort state"),
    gap(),
    err("Member A", "Kernel", "faults"),
    band("the cohort is the recovery unit: B is torn down although it did not fault"),
    err("Kernel", "Member B", "terminated"),
    note("Kernel", "revoke every member handle"),
    err("Kernel", "Client", "released: SYS_ERR_OWNER_DEAD"),
    note("Kernel", "fail every member timeline"),
    gap(),
    ret("Kernel", "Supervisor", "completion timeline advances"),
    band("restart policy is the supervisor’s, not the kernel’s"),
    msg("Supervisor", "Member A", "sys_spawn(), fresh members"),
    msg("Supervisor", "Member B", "sys_spawn()"),
    gap(),
    msg("Client", "Member A", "reconnects and retries"),
  ),
))
