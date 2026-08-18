// Pandoc Typst template for the Nyctos architecture white paper.
// Rendered via: pandoc design.md -t typst --template=template.typ
//
// Body-rendering blocks (terms, tables, figures, highlighting) are carried
// over from pandoc's default typst template so the writer's output keeps
// working across pandoc versions.

// Explicit fallback rather than whatever Typst picks on its own. TeX Gyre
// Pagella covers the symbols this document uses, but a declared fallback keeps
// any future gap from being filled silently. DejaVu Sans Mono is one of
// Typst's built-in faces, so this adds no system font dependency and the
// build stays reproducible under --ignore-system-fonts.
#let sans  = ("TeX Gyre Pagella", "DejaVu Sans Mono")
#let mono  = ("IBM Plex Mono", "DejaVu Sans Mono")

#let ink       = rgb("#12161c")
#let ink-soft  = rgb("#4a5462")
#let rule      = rgb("#d7dce3")
#let link-col  = rgb("#1c5d99")
#let code-bg   = rgb("#f5f6f8")
#let cover-bg  = rgb("#0d1117")

// ---------------------------------------------------------------- page setup

#set document(
  title: "$title$ — $subtitle$",
  author: "$for(author)$$author$$sep$, $endfor$",
)

#set page(
  paper: "a4",
  margin: (top: 24mm, bottom: 22mm, inside: 26mm, outside: 22mm),
  numbering: none,
)

#set text(font: sans, size: 10pt, fill: ink, lang: "en")
#set par(justify: true, leading: 0.68em, spacing: 1.15em)

// Manual section numbers are already written into the prose ("## 1. Scope"),
// so heading numbering stays off to avoid doubling them.
#set heading(numbering: none)

// Level 1 is a Part and opens its own page. Level 2 is a section, 3 a
// subsection, 4 anything below that.
#show heading.where(level: 1): it => {
  pagebreak(weak: true)
  block(above: 0em, below: 1.6em)[
    #set text(size: 26pt, weight: "bold")
    #v(2.2em)
    #it.body
    #v(-0.3em)
    #line(length: 38%, stroke: 1.4pt + ink)
  ]
}
#show heading.where(level: 2): it => block(above: 2.0em, below: 0.9em)[
  #set text(size: 16pt, weight: "bold")
  #it.body
  #v(-0.45em)
  #line(length: 100%, stroke: 0.6pt + rule)
]
#show heading.where(level: 3): it => block(above: 1.5em, below: 0.6em)[
  #set text(size: 12pt, weight: "semibold")
  #it.body
]
#show heading.where(level: 4): it => block(above: 1.2em, below: 0.5em)[
  #set text(size: 10.5pt, weight: "semibold", fill: ink-soft)
  #it.body
]

#show link: set text(fill: link-col)

// Explicit list markers. Typst's default second-level marker is U+2023, which
// Pagella lacks, and it silently pulled in a fallback face for one page.
#set list(marker: ([•], [◦], [–]))

// Code: mono family, tinted block, and wide listings scroll-free by shrinking
// rather than overflowing the measure.
#show raw.where(block: false): it => box(
  fill: code-bg, inset: (x: 3pt, y: 0pt), outset: (y: 3pt), radius: 2pt,
  text(font: mono, size: 0.88em, it),
)
#show raw.where(block: true): it => block(
  fill: code-bg, inset: 8pt, radius: 3pt, width: 100%, breakable: true,
  text(font: mono, size: 7.6pt, it),
)

// Tables: the decision log and prior-art tables are wide, so they get a
// compact face and header emphasis rather than being allowed to overflow.
#set table(inset: 5pt, stroke: (x, y) => if y == 0 { (bottom: 0.6pt + ink) } else { (bottom: 0.3pt + rule) })
#show table: set text(size: 8.4pt)
// Pandoc wraps tables in align(center) and the cells inherit it, which reads
// badly for reference tables. Justification inside narrow cells is worse.
#show table: set align(left + horizon)
#show table: set par(justify: false, leading: 0.55em)

#let horizontalrule = block(above: 1.4em, below: 1.4em, line(length: 100%, stroke: 0.4pt + rule))

// ----------------------------------------------------------------- the index
//
// tools/filter.lua drops a #nyc-mark ahead of the first block of every section
// a term earns, and puts a single #nyc-index() call where the entries belong.
// The marks carry no ink; they exist so that Typst, which is the only part of
// this pipeline that knows what a page is, can report where each one landed.
//
// This is why a page-number index needs the filter at all: markdown has
// nowhere to put an anchor, and pandoc has no way to pass one through.

#let nyc-mark(name, primary: false) = [
  #metadata((name: name, primary: primary))<nyc-idx>
]

#let nyc-index() = context {
  let marks = query(<nyc-idx>)

  // Group the marks by term, keeping the page each one landed on. Two marks
  // can share a page, so the pages are de-duplicated on the way in.
  let entries = (:)
  for mark in marks {
    let value = mark.value
    let page = counter(page).at(mark.location()).first()
    let entry = entries.at(value.name, default: (pages: (), primary: none))
    if not entry.pages.contains(page) {
      entry.pages.push(page)
    }
    if value.primary { entry.primary = page }
    entries.insert(value.name, entry)
  }

  // Split into two columns by hand rather than with columns(). That function
  // is a flow, not a balancer: it fills the first column to the height of its
  // container before starting the second, so a short index sits in one column
  // and looks like a mistake.
  // Sorted case-insensitively. Typst's default string ordering is by
  // codepoint, which puts every capitalised acronym ahead of the lowercase
  // letters: "DMA" landed before "Deadline", and "MMU" before "Mapping". An
  // index is scanned alphabetically or it is not scanned at all.
  let items = ()
  for name in entries.keys().sorted(key: k => lower(k)) {
    let entry = entries.at(name)
    let refs = ()
    for page in entry.pages.sorted() {
      refs.push(if page == entry.primary { strong[#page] } else { [#page] })
    }
    items.push(block(above: 0.35em, below: 0.35em, breakable: false)[
      #strong[#name] #sym.space.thin #refs.join([, ])
    ])
  }

  let half = calc.ceil(items.len() / 2)
  set par(justify: false, leading: 0.55em)
  set text(size: 9pt)
  grid(
    columns: (1fr, 1fr),
    column-gutter: 1.6em,
    items.slice(0, half).join(),
    items.slice(half).join(),
  )
}

#show terms.item: it => block(breakable: false)[
  #text(weight: "bold")[#it.term]
  #block(inset: (left: 1.5em, top: -0.4em))[#it.description]
]

#show figure.where(kind: table): set figure.caption(position: top)
#show figure.where(kind: image): set figure.caption(position: bottom)

// Diagrams are vector and sized to the measure. Figures do not split across
// pages, since half a sequence chart communicates nothing.
#set figure(numbering: "1")
#show figure.where(kind: image): set block(breakable: false, above: 1.4em, below: 1.4em)
#set table(fill: none)
#show figure.where(kind: table): set block(breakable: true)
#show figure.where(kind: image): set image(width: 100%)
#show figure.caption: it => text(size: 8pt, fill: ink-soft, it)

$if(highlighting-definitions)$
$highlighting-definitions$
$endif$

$for(header-includes)$
$header-includes$
$endfor$

// -------------------------------------------------------------------- cover

#page(margin: 0pt, numbering: none)[
  #set text(fill: white)
  #block(width: 100%, height: 100%, fill: cover-bg)[
    #place(top + left, dx: 22mm, dy: 24mm)[
      $if(logo)$
      #image("$logo$", width: 34mm)
      $endif$
    ]
    #place(bottom + left, dx: 22mm, dy: -26mm)[
      #block(width: 150mm)[
        #text(size: 46pt, weight: "bold", tracking: 1.2pt)[$title$]
        #v(-0.2em)
        #line(length: 46mm, stroke: 1.6pt + white)
        #v(0.5em)
        #text(size: 15pt, weight: "semibold")[$subtitle$]
        $if(tagline)$
        #v(0.7em)
        #text(size: 10.5pt, fill: rgb("#9fb0c4"))[$tagline$]
        $endif$
        $if(revision)$
        #v(1.6em)
        #text(size: 9.5pt, fill: rgb("#7d8da0"), font: mono)[Revision $revision$$if(date)$ · $date$$endif$]
        $endif$
      ]
    ]
  ]
]

// ----------------------------------------------------------------- contents

#page(numbering: "i")[
  #counter(page).update(1)
  #text(size: 14pt, weight: "bold")[Contents]
  #v(0.8em)
  #show outline.entry.where(level: 1): it => {
    v(0.55em, weak: true)
    strong(it)
  }
  #outline(title: none, depth: 3, indent: 1.1em)
]

// --------------------------------------------------------------------- body

// Running header naming the current section. The C listing in section 23 runs
// for a dozen pages, and without this a reader has no way to tell where they
// are once the section heading has scrolled off.
#set page(
  numbering: "1",
  header: context {
    // A part-opening page is a divider and carries no running header.
    let opens-part = query(heading.where(level: 1))
      .any(h => h.location().page() == here().page())
    let prior = query(heading.where(level: 2).before(here()))
    if not opens-part and prior.len() > 0 {
      set text(size: 8pt, fill: ink-soft)
      block(width: 100%, below: 0.6em)[
        #prior.last().body
        #v(-0.55em)
        #line(length: 100%, stroke: 0.4pt + rule)
      ]
    }
  },
)
#counter(page).update(1)

$body$
