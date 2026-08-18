// Section 7.4: handle transfer by move.
//
// Three things are easy to get wrong from the prose alone: the value changes
// across the boundary because the tables have independent index spaces, the
// sender's slot is invalidated rather than shared, and attenuation happens by
// duplicating first and transferring the duplicate.

#import "lib/fit.typ": fit
#import "lib/block.typ": *


// One table row: index cell, rights cell, and an optional strike for a slot
// that has been moved out.
#let row(x0, x1, y, idx, rights, dead: false) = {
  let fg = if dead { soft } else { ink }
  let fg2 = if dead { soft } else { soft }
  line((x0, y + 0.32), (x1, y + 0.32), stroke: (thickness: 0.4pt, paint: rule))
  content((x0 + 0.22, y), tm(idx, fill: fg), anchor: "west")
  content((x0 + 1.85, y), tm(rights, fill: fg2), anchor: "west")
  if dead {
    line((x0 + 0.14, y), (x0 + 1.62, y), stroke: (thickness: 0.7pt, paint: warn))
  }
}

#fit(cetz.canvas(length: 1cm, {
  let sx0 = 0.0
  let sx1 = 6.4
  let rx0 = 9.2
  let rx1 = 15.6

  // --- the two tables --------------------------------------------------
  panel((sx0, 0), (sx1, -2.85), "SENDER handle table")
  row(sx0, sx1, -1.35, "0x0003", "READ WRITE TRANSFER DUP")
  row(sx0, sx1, -2.25, "0x0009", "READ", dead: true)
  content((sx1 - 0.22, -2.25), tm("moved out", fill: warn), anchor: "east")

  panel((rx0, 0), (rx1, -2.85), "RECEIVER handle table")
  row(rx0, rx1, -1.35, "0x0102", "READ")

  // --- the object ------------------------------------------------------
  rect((4.4, -4.45), (11.2, -5.35), fill: dark, stroke: none, radius: 0.08)
  content((7.8, -4.90), tx("one kernel object", fill: white, weight: "semibold"))

  arr((1.6, -1.60), (5.6, -4.45))
  arr((13.6, -1.60), (10.2, -4.45))
  line((1.6, -2.50), (6.3, -4.45),
       stroke: (thickness: 0.7pt, paint: rule, dash: "dashed"))

  // --- the transfer ----------------------------------------------------
  arr((sx1, -2.25), (rx0, -1.35))
  content(((sx1 + rx0) / 2, -1.28), box(width: 3.4cm, align(center,
    tm("moved in sys_msg_send()"))), anchor: "south")

}))
