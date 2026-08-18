// Section 6.1: the uniform memory contract across the three tiers.
//
// A correct program behaves identically on all three, so a correct program
// cannot illustrate the difference. What separates the tiers is what happens
// to a program that violates the contract, and whether anything notices.

#import "lib/fit.typ": fit
#import "lib/block.typ": *


#let tiers = (
  (
    title: "Tier 0 · flat memory",
    revoked: false,
    outcome: "the write succeeds and nothing detects it",
    how: "no hardware to revoke with",
  ),
  (
    title: "Tier 1 · MPU or PMP",
    revoked: true,
    outcome: "the write faults",
    how: "the sender’s region is reprogrammed",
  ),
  (
    title: "Tier 2 · full MMU",
    revoked: true,
    outcome: "the write faults",
    how: "the page is unmapped from the sender",
  ),
)

#fit(cetz.canvas(length: 1cm, {
  for (i, t) in tiers.enumerate() {
    let cx = 2.7 + i * 5.4

    panel((cx - 2.55, 0), (cx + 2.55, -5.55), t.title,
          fill: if t.revoked { panelbg } else { rgb("#fbf4f2") },
          border: if t.revoked { rule } else { rgb("#e8d3ce") })

    bx(cx - 1.25, -1.40, 2.0, 0.72, tm("sender", fill: ink))
    bx(cx + 1.25, -1.40, 2.0, 0.72, tm("receiver", fill: ink))

    rect((cx - 2.05, -3.10), (cx + 2.05, -3.85),
         fill: dark, stroke: none, radius: 0.06)
    content((cx, -3.48), tm("the granted buffer", fill: white))

    // receiver always reaches it
    arr((cx + 1.25, -1.76), (cx + 1.05, -3.10))

    // the sender's access after the send is what differs
    if t.revoked {
      line((cx - 1.25, -1.76), (cx - 1.05, -3.10),
           stroke: (thickness: 0.8pt, paint: hair, dash: "dashed"))
      let mx = cx - 1.15
      let my = -2.43
      line((mx - 0.17, my - 0.17), (mx + 0.17, my + 0.17),
           stroke: (thickness: 1.1pt, paint: warn))
      line((mx - 0.17, my + 0.17), (mx + 0.17, my - 0.17),
           stroke: (thickness: 1.1pt, paint: warn))
    } else {
      arr((cx - 1.25, -1.76), (cx - 1.05, -3.10))
    }

    content((cx, -4.35), box(width: 4.5cm, align(center,
      tm(t.how, fill: soft))))
    content((cx, -4.90), box(width: 4.7cm, align(center,
      tm(t.outcome, fill: if t.revoked { accent } else { warn }))))
  }

}))
