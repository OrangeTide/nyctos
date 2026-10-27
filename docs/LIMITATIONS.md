# Known limitations

What the system does not do yet, and the paths its checks do not exercise. The
roadmap and backlog say which of these are scheduled.

- One `make` invocation builds one architecture and one tier. Building all four
  is four commands.
- A spawn that fails partway leaks the frames it had allocated. Nothing records
  what a spawn allocated at the point it failed, and every failure there is a
  panic today.
- A cohort restart loses the previous member's memory. A place becomes free when
  its occupant exits rather than when it is reaped, so the replacement loads over
  the old member's mappings and those frames become unreachable. The boot's
  frame ledger counts them.
- There is no cross-core TLB maintenance on riscv64, whose fences reach only the
  issuing core. Reclaiming a cohort member's memory while a peer runs on another
  core is not safe there, and the checks only reap processes in no cohort.
  aarch64, whose fences are inner-shareable, does not have this limit.
- There is no calendar time. A program makes its own deadlines from the counter
  and the system page, but the page carries no UTC offset, so a program knows how
  long it has waited and not what day it is.
- At Tier 0 a program can write the shared system page, because there is one
  frame and flat memory has nothing to refuse the store. Only the MMU prevents
  it.
- A process that creates a thread and keeps the handle can never be reaped: the
  handle references the thread, the thread references its process, and the
  process closes its handle table only when reaped. Closing the handle lets it
  be reaped normally.
- No server here receives on one thread and replies from a pool of workers,
  though the kernel can name several replies a thread owes at once. The most any
  thread owes in a boot is two, which the console does.
- Address space identifiers are assigned on both ports, but buy nothing
  measurable under QEMU, which discards every translation when the root register
  is written. The kernel is correct and the saving is not visible here.
- Several guarded paths are unexercised by the checks: killing a thread
  interrupted mid-operation, a thread arriving at the kernel already marked for
  death, and a first dispatch that finds the kernel lock held. Each guard is
  present because the contract requires it, and removing it fails no check.
