// Section 17.3: issuing ops are the only source of concurrency.
#import "lib/fit.typ": fit
#import "lib/seq.typ": *


#fit(msc(
  actors: ("Thread", "Kernel", "Device A", "Device B"),
  colw: 3.5,
  events: (
    msg("Thread", "Kernel", "sys_batch_exec(entries, 7)"),
    band("entries execute in order, in the caller’s context, at its priority"),
    msg("Kernel", "Device A", "0: DMA_ISSUE -> reg0"),
    msg("Kernel", "Device B", "1: DMA_ISSUE -> reg1"),
    msg("Kernel", "Device A", "2: DMA_ISSUE -> reg2"),
    band("three transfers in flight; the kernel never blocked and never spawned a worker"),
    note("Kernel", "3: FENCE_WAIT reg0"),
    ret("Device A", "Kernel", "IRQ advances timeline"),
    note("Kernel", "4: FENCE_WAIT reg1"),
    ret("Device B", "Kernel", "IRQ advances timeline"),
    note("Kernel", "5: FENCE_WAIT reg2"),
    ret("Device A", "Kernel", "IRQ advances timeline"),
    ret("Kernel", "Thread", "6: IPC_SEND, then return"),
  ),
))
