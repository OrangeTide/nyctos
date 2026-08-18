// Scale a diagram down to the available measure, never up.
//
// Diagrams are drawn at natural size in centimetres. Inlined into the
// document they must fit the text width, but a diagram narrower than the
// measure should be left alone rather than stretched.

#let fit(body) = layout(size => {
  let m = measure(body)
  let s = if m.width > size.width { size.width / m.width } else { 1.0 }
  scale(x: s * 100%, y: s * 100%, reflow: true, body)
})
