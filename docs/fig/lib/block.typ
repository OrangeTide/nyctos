// Block-diagram helper for Nyctos diagrams.
//
// Placement is explicit rather than solved by a layout engine. These are
// layered stacks and panels whose arrangement is part of what is being
// communicated, so auto-layout works against the diagram rather than for it.

#import "@preview/cetz:0.4.2"
#import cetz.draw: *

#let ink    = rgb("#12161c")
#let soft   = rgb("#4a5462")
#let hair   = rgb("#c3cad4")
#let rule   = rgb("#d7dce3")
#let accent = rgb("#1c5d99")
#let warn   = rgb("#a33a2e")
#let panelbg = rgb("#f5f6f8")
#let deepbg  = rgb("#eef1f5")
#let dark    = rgb("#12161c")
#let darkbox = rgb("#1d232c")

#let sans = "TeX Gyre Pagella"
#let mono = "IBM Plex Mono"

#let tx(body, size: 8pt, fill: ink, weight: "regular") = text(
  font: sans, size: size, fill: fill, weight: weight, body,
)
#let tm(body, size: 7pt, fill: soft) = text(font: mono, size: size, fill: fill, body)

// A titled region. tl and br are (x, y) corners, y increasing upward.
#let panel(tl, br, title, fill: panelbg, border: rule, title-fill: soft) = {
  rect(tl, br, fill: fill, stroke: (thickness: 0.6pt, paint: border), radius: 0.10)
  content((tl.at(0) + 0.22, tl.at(1) - 0.26), tm(title, size: 7pt, fill: title-fill),
          anchor: "west")
}

// A box centred at (cx, cy). Content is constrained to the box width so it
// wraps rather than overflowing, which auto-layout would otherwise hide.
#let bx(cx, cy, w, h, body, night: false, fill: white) = {
  rect((cx - w / 2, cy - h / 2), (cx + w / 2, cy + h / 2),
       fill: if night { darkbox } else { fill },
       stroke: (thickness: 0.7pt, paint: if night { rgb("#3a4451") } else { soft }),
       radius: 0.07)
  content((cx, cy), box(width: (w - 0.34) * 1cm, align(center, body)))
}

// Straight arrow with an optional label placed beside its midpoint.
#let arr(from, to, label: none, kind: "call", label-anchor: "west", label-dx: 0.14) = {
  let paint = if kind == "return" { soft } else if kind == "error" { warn } else { accent }
  let dash = if kind == "return" { "dashed" } else { none }
  line(from, to,
       stroke: (thickness: 0.8pt, paint: paint, dash: dash),
       mark: (end: "straight", scale: 0.5, fill: paint))
  if label != none {
    let mx = (from.at(0) + to.at(0)) / 2
    let my = (from.at(1) + to.at(1)) / 2
    content((mx + label-dx, my), tm(label), anchor: label-anchor)
  }
}

// Double-headed arrow, for peer relationships.
#let arr2(from, to, label: none) = {
  line(from, to,
       stroke: (thickness: 0.8pt, paint: accent),
       mark: (start: "straight", end: "straight", scale: 0.5, fill: accent))
  if label != none {
    let mx = (from.at(0) + to.at(0)) / 2
    let my = (from.at(1) + to.at(1)) / 2
    content((mx, my + 0.20), tm(label))
  }
}

// A caption line under a diagram.
#let caption(x, y, body) = content((x, y), tm(body), anchor: "west")
