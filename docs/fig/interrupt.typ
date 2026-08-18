// Section 13.2: the interrupt path is one timeline advance.
#import "lib/fit.typ": fit
#import "lib/seq.typ": *


#fit(msc(
  actors: ("Device", "ISR trampoline", "Timeline", "Driver thread"),
  colw: 3.6,
  events: (
    note("Driver thread", "blocked in FENCE_WAIT(tl, N)"),
    msg("Device", "ISR trampoline", "IRQ"),
    note("ISR trampoline", "acknowledge and mask at the controller"),
    msg("ISR trampoline", "Timeline", "advance by increment"),
    band("the ISR runs no driver logic, takes no thread-held lock, allocates nothing"),
    ret("ISR trampoline", "Device", "return from interrupt"),
    msg("Timeline", "Driver thread", "value reaches N: thread runnable"),
    note("Driver thread", "preempts if its priority warrants"),
  ),
))
