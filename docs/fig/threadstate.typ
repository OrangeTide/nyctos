// Section 10.6: thread state transitions.
//
// BLOCKED is deliberately drawn as one state rather than several. A thread is
// blocked on at most one object, so there is no family of blocked substates
// and every entry and exit can be enumerated on a single pair of edges.

#import "lib/fit.typ": fit
#import "lib/block.typ": *


// Elbow arrow through an explicit list of points.
#let path(pts, kind: "call") = {
  let paint = if kind == "kill" { warn } else { accent }
  let dash = if kind == "kill" { "dashed" } else { none }
  line(..pts,
       stroke: (thickness: 0.9pt, paint: paint, dash: dash),
       mark: (end: "straight", scale: 0.5, fill: paint))
}

#fit(cetz.canvas(length: 1cm, {
  // --- states ----------------------------------------------------------
  let ready_x = 3.9
  let run_x   = 9.6
  let exit_x  = 14.9
  let blk_y   = -3.2

  circle((0.6, 0), radius: 0.11, fill: dark, stroke: none)

  bx(ready_x, 0, 2.6, 1.0, tx("READY", weight: "semibold"))
  bx(run_x, 0, 2.8, 1.0, tx("RUNNING", weight: "semibold"))
  bx(run_x, blk_y, 5.6, 1.2, [
    #tx("BLOCKED", weight: "semibold") \
    #tm("on exactly one object")
  ])
  bx(exit_x, 0, 2.6, 1.0, tx("EXITED", fill: white, weight: "semibold"), night: true)

  // --- creation --------------------------------------------------------
  path(((0.78, 0), (2.6, 0)))
  content((1.69, 0.70), tm("sys_thread_create()"))

  // --- ready / running -------------------------------------------------
  path(((5.2, 0.22), (8.2, 0.22)))
  content((6.7, 0.52), tm("dispatched"))
  path(((8.2, -0.22), (5.2, -0.22)))
  content((6.7, -0.54), tm("preempted · quantum expiry"))

  // --- blocking --------------------------------------------------------
  path(((run_x, -0.50), (run_x, blk_y + 0.60)))
  content((run_x + 0.22, -1.85), box(width: 5.2cm, tm(
    "msg_send · msg_recv · timeline_wait · futex · page fault")), anchor: "west")

  // --- unblocking ------------------------------------------------------
  path(((run_x - 2.8, blk_y), (ready_x, blk_y), (ready_x, -0.50)))
  content((ready_x + 0.55, blk_y - 0.95), box(width: 7.4cm, tm(
    "reply · message delivered · timeline reaches value · timeline fails · futex wake · deadline expires · cancelled")),
    anchor: "north-west")

  // --- exit ------------------------------------------------------------
  path(((11.0, 0), (13.6, 0)))
  content((12.3, 0.70), tm("sys_thread_exit()"))

  path(((run_x + 2.8, blk_y), (exit_x, blk_y), (exit_x, -0.50)), kind: "kill")
  content((exit_x + 1.4, blk_y - 0.95), tm("sys_proc_kill(), from any state", fill: warn),
          anchor: "north-east")

  // --- what EXITED is not ----------------------------------------------
  content((exit_x + 1.4, 2.20), box(width: 5.4cm, tm(
    "not reaped here: the completion timeline advances, the exit code stays readable, and the object is released when the last handle closes")),
    anchor: "north-east")
}))
