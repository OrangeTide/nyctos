// Section 17: cohort zero-copy data path.
#import "lib/fit.typ": fit
#import "lib/block.typ": *


#fit(cetz.canvas(length: 1cm, {
  let W = 15.0

  panel((0, 0), (W, -3.30), "DRIVER COHORT: STORAGE", fill: deepbg)
  content((0.30, -0.90), box(width: 14.4cm,
    tm("one protection domain · one recovery unit · one scheduling node · memory-locked on the paging path")),
    anchor: "west")

  bx(4.0, -2.25, 6.4, 1.35, [
    #tx("ext4 file system", weight: "semibold") \
    #tm("1. allocate from the cohort pool") \
    #tm("2. build sg_list")
  ])
  bx(11.0, -2.25, 6.4, 1.35, [
    #tx("NVMe block driver", weight: "semibold") \
    #tm("3. receive physical addresses") \
    #tm("4. program DMA, return a timeline")
  ])

  arr((7.25, -2.25), (7.75, -2.25))
  content((7.5, -3.10), tm("direct call · no protection boundary crossed"))

  // shared pool
  rect((0, -4.55), (W, -5.55), fill: dark, stroke: none, radius: 0.08)
  content((W / 2, -5.05), tx("Shared cohort DMA buffer pool", fill: white, weight: "semibold"))
  content((0.05, -5.90), tm("SYSTEM PHYSICAL RAM"), anchor: "west")

  arr((4.0, -3.30), (4.0, -4.55), label: "allocate")
  arr((11.0, -3.30), (11.0, -4.55), label: "DMA")
}))
