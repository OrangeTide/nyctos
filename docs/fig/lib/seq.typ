// Message sequence chart helper for Nyctos diagrams.
//
// Drawn in Typst rather than generated, so the text is real text in the
// document's own font and needs no SVG font resolution.
//
//   #import "seq.typ": msc, msg, ret, note, gap, band
//   #msc(actors: ("Client", "Kernel", "Server"), events: (
//     msg("Client", "Kernel", `sys_msg_send()`),
//     note("Kernel", "blocked"),
//   ))

#import "@preview/cetz:0.4.2"
#import cetz.draw: *

#let ink    = rgb("#12161c")
#let soft   = rgb("#4a5462")
#let hair   = rgb("#c3cad4")
#let accent = rgb("#1c5d99")
#let warn   = rgb("#a33a2e")
#let panel  = rgb("#f5f6f8")

#let sans = "TeX Gyre Pagella"
#let mono = "IBM Plex Mono"

// --- event constructors ------------------------------------------------

#let msg(from, to, label, kind: "call") = (
  t: "msg", from: from, to: to, label: label, kind: kind,
)
#let ret(from, to, label) = msg(from, to, label, kind: "return")
#let err(from, to, label) = msg(from, to, label, kind: "error")
#let note(actor, label)   = (t: "note", actor: actor, label: label)
#let band(label)          = (t: "band", label: label)
#let gap()                = (t: "gap")

// --- renderer ----------------------------------------------------------

#let msc(actors: (), events: (), colw: 3.6, step: 0.92, pad: 1.5) = {
  let idx(a) = actors.position(x => x == a)
  let xof(a) = idx(a) * colw

  // y position of each event row
  let rows = ()
  let y = -1.35
  for e in events {
    if e.t == "gap" { y -= step * 0.45 } else {
      rows.push(y)
      y -= step
    }
  }
  let bottom = y - 0.15

  cetz.canvas(length: 1cm, {
    set-style(stroke: (thickness: 0.5pt, paint: soft))

    // actor heads and lifelines
    for a in actors {
      let x = xof(a)
      rect((x - colw / 2 + 0.22, 0.0), (x + colw / 2 - 0.22, -0.62),
           fill: panel, stroke: (thickness: 0.6pt, paint: soft), radius: 0.06)
      content((x, -0.31), text(font: sans, size: 8pt, weight: "semibold", fill: ink, a))
      line((x, -0.62), (x, bottom),
           stroke: (thickness: 0.5pt, paint: hair, dash: "dotted"))
    }

    // events
    let r = 0
    for e in events {
      if e.t == "gap" { continue }
      let yy = rows.at(r)
      r += 1

      if e.t == "band" {
        line((-colw / 2 + 0.22, yy + 0.30), (xof(actors.last()) + colw / 2 - 0.22, yy + 0.30),
             stroke: (thickness: 0.5pt, paint: hair))
        content((-colw / 2 + 0.30, yy + 0.02),
                text(font: mono, size: 7pt, fill: soft, e.label), anchor: "west")
      } else if e.t == "note" {
        let x = xof(e.actor)
        content((x, yy),
                box(fill: panel, inset: (x: 4pt, y: 2.5pt), radius: 2pt,
                    stroke: (thickness: 0.4pt, paint: hair),
                    text(font: mono, size: 7pt, fill: soft, e.label)))
      } else {
        let paint = if e.kind == "return" { soft }
                    else if e.kind == "error" { warn }
                    else { accent }
        let dash = if e.kind == "return" { "dashed" } else { none }
        let x1 = xof(e.from)
        let x2 = xof(e.to)

        if e.from == e.to {
          // self-message: small loop to the right of the lifeline
          let w = 0.55
          line((x1, yy + 0.12), (x1 + w, yy + 0.12), (x1 + w, yy - 0.12), (x1, yy - 0.12),
               stroke: (thickness: 0.7pt, paint: paint, dash: dash),
               mark: (end: "straight", scale: 0.5, fill: paint))
          content((x1 + w + 0.12, yy),
                  text(font: mono, size: 7pt, fill: ink, e.label), anchor: "west")
        } else {
          line((x1, yy), (x2, yy),
               stroke: (thickness: 0.7pt, paint: paint, dash: dash),
               mark: (end: "straight", scale: 0.5, fill: paint))
          content(((x1 + x2) / 2, yy + 0.19),
                  text(font: mono, size: 7pt, fill: ink, e.label))
        }
      }
    }
  })
}
