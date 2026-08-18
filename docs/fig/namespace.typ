// Section 20.4: the namespace is handed over at launch.
#import "lib/fit.typ": fit
#import "lib/block.typ": *


#fit(cetz.canvas(length: 1cm, {
  panel((0, 0), (7.4, -4.20), "PARENT")
  content((0.35, -1.05), box(width: 6.7cm, text(font: mono, size: 7pt, fill: ink)[
    sys_spawn(attr) \
    \
    ns\[\] = \{ \
    #h(0.6em) \{ "/dev", dir handle \}, \
    #h(0.6em) \{ "/lib", dir handle \}, \
    \} \
    argv\[\], envp\[\]
  ]), anchor: "north-west")

  panel((10.4, 0), (17.8, -4.20), "CHILD")
  content((10.75, -1.05), box(width: 6.7cm, text(font: mono, size: 7pt, fill: ink)[
    \_start(sys_startup_t \*) \
    \
    libc builds its mount table \
    #h(0.6em) from startup->ns\[\] \
    \
    ... then calls main()
  ]), anchor: "north-west")

  arr((7.4, -2.10), (10.4, -2.10))
  content((8.9, -1.72), tm("handles moved"))
  content((8.9, -2.50), tm("record placed"))

  content((0.05, -4.75),
    tm("a child given no directory handles can name nothing: there is no ambient root to fall back on"),
    anchor: "west")
}))
