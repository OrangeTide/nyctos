# Working on Nyctos

Developer notes: the source tree, the memory map, and where the coding style
lives. For what the system is and how to build and run it, see the
[README](../README.md).

## Source tree

The tree follows a BSD source layout. `sys` is the kernel, `lib` is the
userland libraries, `bin` is the userland programs, `include` is the headers
those programs compile against, `tools` is the host-side build tools, and
`share/mk` holds the make fragments that tie it together.

```
.
├── Makefile                 # top level: recurses into lib, bin, tools, sys
├── VERSION                  # the current version, source of truth
├── LICENSES/                # Apache-2.0 and 0BSD license texts
├── _out/                    # generated: <target-triplet>-tier<N>/ build trees
├── docs/                    # design, roadmap, proposals, these notes
├── scripts/                 # the check session driver and site deploy
├── tools/
│   ├── mkbootarch/          # builds the boot archive of userland images
│   └── mkdisk/              # builds the raw disk image the block store reads
├── share/
│   ├── mk/                  # toolchain config and shared build rules
│   └── ld/                  # userland linker scripts, one per arch
├── include/                 # userland headers
├── lib/
│   ├── csu/<arch>/crt1.S    # C runtime startup
│   └── libc/                # the C library
│       ├── path.c           # resolving a path against a namespace
│       ├── file.c           # opening one by name
│       ├── compose.c        # what to hand a child
│       ├── startup.c        # the startup record a program receives
│       └── stdio/ stdlib/ string/ sys/ arch/
├── bin/                     # around forty userland programs (see below)
└── sys/                     # the kernel
    ├── sys/                 # kernel-internal headers
    │   ├── config.h         # build-time pool sizes and the tier switch
    │   ├── pool.h           # static pools and the identifier layout
    │   ├── object.h         # the kernel object types and their pools
    │   ├── handle.h         # per-process handle tables and rights
    │   ├── channel.h        # send, receive, reply
    │   ├── connection.h     # a connection opened over a channel
    │   ├── proc.h           # processes and the loader
    │   ├── thread.h         # threads and the context switch
    │   ├── sched.h          # the strict-priority scheduler
    │   ├── op.h             # the operation entry point
    │   ├── batchreg.h       # the submission engine's registers
    │   ├── prepared.h       # a batch loaded once and run many times
    │   ├── timeline.h       # completion timelines
    │   ├── time.h           # absolute deadlines and the counter
    │   ├── intr.h           # interrupt sources
    │   ├── grant.h          # authority over a region, held as a handle
    │   ├── cohort.h         # a protection domain shared by several images
    │   ├── vm.h             # address spaces, and what a port provides
    │   ├── syspage.h        # the read-only page every process maps
    │   ├── blk.h            # block size and disk layout
    │   ├── fs.h             # the filesystem protocol
    │   ├── virtio.h         # virtio-mmio registers and virtqueues
    │   ├── wire.h           # the inline message cursor
    │   └── systm.h          # kernel-internal prototypes
    ├── kern/                # machine-independent
    │   ├── init_main.c      # kernel_main and the boot checks
    │   ├── kern_object.c    # the pool instances
    │   ├── kern_syscall.c   # the trap path and what a fault means
    │   ├── kern_op.c        # the operation dispatch
    │   ├── kern_channel.c   # send, receive, reply, and the move contract
    │   ├── kern_connection.c # connections over a channel
    │   ├── kern_proc.c      # processes, spawn, the loader, and reaping
    │   ├── kern_thread.c    # threads and their lifecycle
    │   ├── kern_sched.c     # the scheduler and priority inheritance
    │   ├── kern_pager.c     # a page fault, sent as a message
    │   ├── kern_cohort.c    # membership, and tearing a cohort down
    │   ├── kern_grant.c     # grants over a region
    │   ├── kern_handle.c    # handle tables
    │   ├── kern_intr.c      # interrupt delivery
    │   ├── kern_time.c      # timers and deadlines
    │   ├── kern_timeline.c  # completion timelines
    │   ├── kern_batchreg.c  # the submission engine
    │   ├── subr_page.c      # the frame allocator
    │   ├── subr_vm.c        # the kernel's own address space
    │   ├── subr_pool.c      # the pool mechanism
    │   └── subr_prf.c       # console output, formatting, panic
    └── arch/                # per-architecture; aarch64 and riscv64 share this set
        ├── boot.S           # reset entry and the drop to the kernel's level
        ├── exception.S      # the vector table, system calls, and fault paths
        ├── swtch.S          # the context switch
        ├── machdep.S        # the drop to user mode
        ├── pmap.c           # page tables and TLB maintenance
        ├── fault_md.c       # every exception, mapped to a fault kind
        ├── mem_md.c         # what memory and devices this board has
        ├── intr.c           # the interrupt controller
        ├── cpu_md.c         # per-core setup
        ├── thread_md.c      # machine-dependent thread state
        ├── halt.c           # power off and reset
        ├── uart.c           # the board's serial port
        └── conf/kern.ld     # where the kernel is placed
```

`bin` holds around forty programs: the shell and its utilities, the console
driver, the filesystem server and the block store beneath it, a supervisor, and
programs written to break a rule, block, or fault on purpose for the checks.

## Memory map

| Arch | Kernel load address | Why | UART |
| --- | --- | --- | --- |
| aarch64 | `0x40080000` | QEMU `virt` RAM starts at `0x40000000`; the `-kernel` loader places ELF images at `+0x80000` by default | PL011 @ `0x09000000` |
| riscv64 | `0x80000000` | QEMU `virt` RAM base; with `-bios none` the reset vector jumps straight here | 16550A @ `0x10000000` |

`USER_LOAD_ADDR` in [../share/mk/nyc.own.mk](../share/mk/nyc.own.mk) must agree
with the userland linker script in `share/ld/`. Every program also needs its own
slot above that base, assigned by hand in the program's own Makefile, because at
Tier 0 there is no MMU and a second copy of an image would load over the first.

At Tier 2 the slot is a virtual address and the memory behind it is frames
nobody else has, so the rule that one image has one live process is compiled
out. The slots stay because the loader still uses them to place a program's
memory, and because one source builds both tiers.

## Coding style

All C and assembly follows [CODING_STYLE.md](CODING_STYLE.md), a variant of the
FreeBSD kernel style(9) guide with 4-space indentation, 2-space continuation
lines, and no hard tabs. Every source file carries an `SPDX-License-Identifier`
line and a short copyright comment near the top.
