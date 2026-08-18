// Section 9.1: timeline semantics.
//
// One picture for four rules that are otherwise scattered through the prose:
// the counter is monotonic, it may advance by more than one, waiting on a
// value already passed succeeds immediately, and the error state satisfies
// every waiter at once including those that arrive afterwards.
//
// Proportions are kept close to the printed measure so the figure sits near
// 1:1 on the page. A wider canvas scales down and takes the labels with it.

#import "lib/fit.typ": fit
#import "lib/block.typ": *


#fit(cetz.canvas(length: 1cm, {
  // --- geometry --------------------------------------------------------
  let t0 = 1.4      // axis origin
  let t1 = 3.0      // signal -> 1
  let t2 = 4.5      // signal -> 2
  let t3 = 6.3      // signal -> 5, a jump
  let te = 8.2      // enters error state
  let tn = 10.0     // axis end
  let tl = 10.35    // description column

  let v0 = 0.0
  let v1 = 0.70
  let v2 = 1.40
  let v5 = 2.10
  let ve = 2.85

  // --- axes ------------------------------------------------------------
  line((t0, -0.25), (t0, ve + 0.50), stroke: (thickness: 0.5pt, paint: soft))
  line((t0, v0), (tn, v0), stroke: (thickness: 0.5pt, paint: soft))
  content((t0 - 0.12, ve + 0.50), tm("counter"), anchor: "south-east")
  content((tn, -0.40), tm("time"), anchor: "east")

  for (v, lbl) in ((v0, "0"), (v1, "1"), (v2, "2"), (v5, "5")) {
    content((t0 - 0.16, v), tm(lbl), anchor: "east")
    line((t0 - 0.06, v), (t0 + 0.06, v), stroke: (thickness: 0.5pt, paint: soft))
  }
  content((t0 - 0.16, ve), tm("error", fill: warn), anchor: "east")

  // --- guides at each signal ------------------------------------------
  for t in (t1, t2, t3, te) {
    line((t, -4.25), (t, ve + 0.10),
         stroke: (thickness: 0.4pt, paint: hair, dash: "dotted"))
  }

  // --- the counter itself ----------------------------------------------
  line((t0, v0), (t1, v0), (t1, v1), (t2, v1), (t2, v2), (t3, v2), (t3, v5), (te, v5),
       stroke: (thickness: 1.1pt, paint: accent))
  line((te, v5), (te, ve), (tn, ve),
       stroke: (thickness: 1.1pt, paint: warn, dash: "dashed"))

  content((t1, v1 + 0.26), tm("signal 1"))
  content((t2, v2 + 0.26), tm("signal 2"))
  content((t3, v5 + 0.26), tm("signal 5"))
  content((te + 0.14, ve + 0.26), tm("fails", fill: warn), anchor: "west")

  // --- waiters ---------------------------------------------------------
  // Each lane: issue tick, bar until satisfied, terminator, description.
  let lane(y, tag, from, to, label, kind: "ok", immediate: false) = {
    let paint = if kind == "err" { warn } else { accent }
    line((from, y + 0.15), (from, y - 0.15), stroke: (thickness: 0.9pt, paint: paint))
    let stop = if immediate { from + 0.38 } else { to }
    line((from, y), (stop, y),
         stroke: (thickness: 0.9pt, paint: paint),
         mark: (end: "straight", scale: 0.45, fill: paint))
    content((t0 - 0.16, y), tm(tag), anchor: "east")
    content((tl, y), box(width: 6.0cm,
      text(font: mono, size: 7pt, fill: if kind == "err" { warn } else { ink }, label)),
      anchor: "west")
  }

  lane(-1.10, "A", 1.85, t2, "blocked on 2, released when the counter reaches it")
  lane(-2.00, "B", 5.35, 5.35, "asks for 1 when the counter is already 2: returns at once",
       immediate: true)
  lane(-2.90, "C", 6.85, te, "blocked on 9, which never arrives: the failure releases it",
       kind: "err")
  lane(-3.80, "D", 9.10, 9.10, "asks for 9 after the failure: receives the error at once",
       kind: "err", immediate: true)

  line((t0, -0.58), (tn, -0.58), stroke: (thickness: 0.4pt, paint: rule))
  content((t0 - 0.16, -0.58), tm("waiters"), anchor: "east")
}))
