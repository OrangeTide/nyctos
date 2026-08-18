// Section 21: the blocking relation is a chain, so donation walks one way.
#import "lib/fit.typ": fit
#import "lib/block.typ": *


#fit(cetz.canvas(length: 1cm, {
  let y = 0
  let xs = (2.4, 8.0, 13.6)
  let labels = (
    ([Thread A], [own priority 40]),
    ([Thread B], [own priority 10]),
    ([Thread C], [own priority 5, holds SIGNAL right]),
  )

  for (i, x) in xs.enumerate() {
    let (name, sub) = labels.at(i)
    bx(x, y, 4.2, 1.25, [
      #tx(name, weight: "semibold") \
      #tm(sub)
    ])
    // effective priority badge beneath each thread
    rect((x - 1.35, y - 1.05), (x + 1.35, y - 1.62),
         fill: dark, stroke: none, radius: 0.06)
    content((x, y - 1.335), tm("effective 40", fill: white))
  }

  arr((xs.at(0) + 2.1, y), (xs.at(1) - 2.1, y))
  content(((xs.at(0) + xs.at(1)) / 2, y + 0.95), tm("blocked sending"))

  arr((xs.at(1) + 2.1, y), (xs.at(2) - 2.1, y))
  content(((xs.at(1) + xs.at(2)) / 2, y + 0.95), tm("blocked on timeline"))

  content((xs.at(0) - 2.1, y - 2.25),
          tm("a thread blocks on exactly one object, so the relation is a chain, not a graph"),
          anchor: "west")
}))
