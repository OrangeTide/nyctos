// Section 2.8: system architecture.
#import "lib/fit.typ": fit
#import "lib/block.typ": *


#fit(cetz.canvas(length: 1cm, {
  let W = 19.0

  // ---- user space -----------------------------------------------------
  panel((0, 0), (W, -2.25), "USER SPACE")
  bx(3.9, -1.35, 6.4, 1.20, [
    #tx("Application", weight: "semibold") \
    #tm("process & threads · libc holds the mount table")
  ])
  bx(11.3, -1.35, 5.0, 1.20, [
    #tx("Pager / library server", weight: "semibold") \
    #tm("memory-locked")
  ])
  bx(16.4, -1.35, 4.6, 1.20, [
    #tx("Supervisor / loader", weight: "semibold") \
    #tm("restart policy")
  ])

  // ---- cohort: occupies only its own column, leaving clear channels ----
  panel((0, -3.55), (7.8, -6.45), "DRIVER COHORT: STORAGE", fill: deepbg)
  content((0.30, -4.45), box(width: 7.1cm, tm("one protection domain · one recovery unit · one scheduling node")), anchor: "west")
  bx(2.0, -5.55, 3.4, 1.15, [
    #tx("ext4 file system", weight: "semibold") \
    #tm("builds sg_list")
  ])
  bx(5.8, -5.55, 3.4, 1.15, [
    #tx("NVMe driver", weight: "semibold") \
    #tm("programs DMA")
  ])
  arr2((3.72, -5.55), (4.08, -5.55))
  content((3.9, -4.92), tm("zero-copy"))

  // ---- kernel ---------------------------------------------------------
  panel((0, -7.30), (W, -9.75), "KERNEL", fill: dark, border: dark, title-fill: rgb("#8a97a8"))
  let ky = -8.80
  let kw = 4.30
  bx(2.50, ky, kw, 1.25, [
    #tx("Partitioned RT scheduler", fill: white, weight: "semibold")
  ], night: true)
  bx(7.10, ky, kw, 1.25, [
    #tx("Synchronous IPC + page faults", fill: white, weight: "semibold")
  ], night: true)
  bx(11.70, ky, kw, 1.25, [
    #tx("Timeline engine", fill: white, weight: "semibold") \
    #tm("I/O · IRQ · timers · exit", fill: rgb("#9fb0c4"))
  ], night: true)
  bx(16.30, ky, kw, 1.25, [
    #tx("Handle table", fill: white, weight: "semibold") \
    #tm("static object pools", fill: rgb("#9fb0c4"))
  ], night: true)

  // ---- flows: labels sit in the gaps, arrows never cross a panel -------
  arr((3.9, -2.25), (3.9, -3.55), label: "batch / direct invoke")
  arr((3.9, -6.45), (3.9, -7.30), label: "syscalls · pulses · timeline signals")
  arr((11.3, -2.25), (11.3, -7.30), label: "fault IPC")
  arr((16.4, -2.25), (16.4, -7.30), label: "death timelines")
}))
