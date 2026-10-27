---
title: Nyctos
subtitle: Backlog
revision: "1"
---

# Backlog

Work that is wanted but not scheduled. `ROADMAP.md` holds what is being built
and in what order, `design.md` what the system is, and `PROPOSALS.md` ideas that
are not being built. This file holds the middle ground: concrete, agreed,
unscheduled.

Each item states what it is, why it is wanted, and where a reference
implementation already exists. Paths beginning outside this tree point into
other repositories of mine and are pointers for the work, not dependencies of
the build. They can be stripped before this file is published.

An item carries a rough priority. HIGH is cheap and earns its place at once. MED
is worth doing and waits on a trigger named in the item.

---

## HIGH

### Bounds-checked wire cursor for message payloads (done, built as M24)

Done, and built as M24 ("The Payload a Server Reads Without Trusting It").
`sys/sys/wire.h` holds the append-and-consume cursor, and `fs_server`,
`fs_client`, `blk_server`, the console service and `ramfs` encode and decode
through it. The compound parser validates its entries through the consume
cursor, and a crafted entry whose name runs past the payload is refused
`SYS_ERR_INVALID_ARG` rather than followed off the end. The original
description is kept below.

The filesystem client and server encode a request by casting a struct onto
`message.inline_data` and decode a reply the same way, and the compound path
does its own bounds arithmetic by hand (`bin/fs_server/fs_server.c`, the batch
parse). Hand-rolled offset math on a message a client controls is where a parser
walks off the end of a payload.

Adopt a small append-and-consume cursor over the inline payload: a writer that
refuses to overrun and a reader that refuses to over-read, both carrying a
sticky failure flag the caller checks once at the end. Replace the casts in
`fs_server`, `fs_client`, `blk_server` and the console service with it.

A freestanding little-endian cursor of this exact shape already exists and is
header-only with no libc dependency, in `~/Vibe/vmm/abi/jotun_9p.h` (`p9_out`
and `p9_in`, the `p9o_*` and `p9i_*` helpers). Strip the protocol-specific
parts and keep the cursor. Carry across the discipline it failed to follow in
one place: a caller must check the sticky flag, and the reference server's
directory path does not, which is the bug to not reproduce.

### Resource-exactness soak tests

The system asserts exact reclaim of statically sized pools, and the roadmap has
several milestones about a pool giving everything back. A soak test makes that
property fail loudly rather than drift. Churn spawn and reap, connect and close,
and open and close in a loop, then assert that every pool count returns to its
pre-loop baseline. The static pools make this sharper here than in a system with
a heap, because the baseline is exact rather than approximate.

Reference programs that do this against free memory rather than pool counts
exist in `~/DEVEL/OS/jon-os/cmd/soaktest`, with companions `semtest`, `rendtest`,
`mnttest` and `ftest`. Rewrite the calls against the nyctos libc and the pool
accounting, and assert the counts directly.

---

## MED

### Offline, then in-kernel, panic symbolization

A panic reports an address, and nothing turns it into a function and a line. An
offline symbolizer that reads the archive and prints a backtrace from a panic
record is the first step, and an in-kernel one that carries a symbol blob is the
second. This is why Proposal 5 ("ELF in the Boot Archive") is wanted: a flat
image with a sidecar has no symbols to read.

A DWARF reader that builds sorted line and function tables with binary-search
lookup and no per-lookup allocation exists in `~/Vibe/skjegg/dwarf/dwread.c`
with `dwarf.c`. It is ELF32 today with four-byte addresses, and its frame walk
assumes the RV32 layout. The port needs ELF64, LP64 addresses, and a frame walk
parameterized for the AArch64 and riscv64 layout (`[fp]` and `[fp+8]`). Moderate
work. Trigger: Proposal 5 lands, so there is a symbol table to read.

### Adopt a host-side ELF reader in the boot-archive tool

Implements Proposal 5. The archive tool reads each member's `PT_LOAD` headers
instead of a sidecar, honours `p_flags` for the read-execute against read-write
split, and keeps the ELF container so a reader downstream has the symbol and
debug sections. A portable reader with its own little-endian decoders and no
`<elf.h>` dependency, which matters because the tools build on more than one
host, exists in `~/Vibe/vmm/host/elf.c`. Fix its missing check that a segment's
file size does not exceed its memory size before the copy. Trigger: deciding to
act on Proposal 5.

### Spike the walk and directory-read semantics (done, spike retired)

Done, and carried into M23. The `dirspike_checks` spike proved the Proposal 6
decisions: whole-path walk with `SYS_ERR_WRONG_TYPE` for a non-directory
mid-path component, `SYS_ERR_NO_ENT` for a missing one, `SYS_ERR_PERM_DENIED`
for a `..`, and a key-based opaque directory-read cursor that finds every
surviving entry exactly once across an unlink of an entry already returned, an
unlink of one not yet reached, and a create past the cursor, with a cursor from
another directory refused. It was pure logic over a static tree, crossing no
rendezvous and allocating no kernel object. The spike was retired in M23 stage
four, once the one-appearance property was proved over the real rendezvous,
and `dirspike_checks` no longer exists in `sys/kern/init_main.c`.

M23 ("A Directory That Changes While It Is Read") is built and closed; its
retrospective is in `ROADMAP.md`. It carried `FS_OP_READDIR`, and the create
and unlink a mutable directory needs, into `sys/sys/fs.h` and across the real
protocol, served by the `ramfs` process. A
capability-mount sandbox filesystem of mine answers the readdir-cursor and
backend-split questions in code, in `~/Vibe/skjegg/fs/fs.h` with `ramfs.c` and
`hostfs.c`; see the investigation note below.

### User-space block and storage driver over virtio-mmio

Persistent storage needs a real device driver, and in this system a driver is an
ordinary process. The missing piece is a virtio-mmio transport and virtqueue
handling on the `virt` board for riscv64 and aarch64, driven by an interrupt
landing on a timeline. Interrupt-driven virtqueue code exists as a kernel-
resident, big-endian, m68k reference in `~/DEVEL/OS/jon-os/arch/virt/virtio.c`:
the byte swaps vanish on these little-endian targets, and the code moves into a
user process here. This is the gating work behind a copy-on-write filesystem.
Trigger: wanting storage that survives a boot.

### Cherry-pick shell features

The shell is minimal. A Bourne subset with functions, locals, `break` and
`continue` with a count, arithmetic expansion, field splitting, `case`, and
positional parameters exists in `~/DEVEL/OS/jon-os/cmd/jsh/jsh.c`, and it builds
hosted for testing. It is a menu to pick from, not a port: its `fork`-degraded-
to-spawn model does not match this system, and the feature set is the only part
worth taking. Trigger: wanting more than the current shell runs.

---

## Investigations

### Bounded formatter against the current printf

Compare the sink-based, allocation-free formatter in `~/Vibe/vmm/guest/libos/`
(`fmt.c`, `fmt.h`), which returns the untruncated length, against
`lib/libc/stdio/printf.c`. The current printf defends its own shape in a comment,
so the question is whether the sink design is enough of an improvement to
override that choice, not whether it is cleaner. Decide and record.

### Capability-mount sandbox namespace (done)

A capability-based mount namespace of mine converges with the namespace handed
over at launch, which is evidence the model is right and a source of concrete
answers. Finding, from `~/Vibe/skjegg/fs/fs.h`:

- The launcher fills a flat mount table with capability handles before the
  program runs, and the program never mounts. This is the namespace handoff of
  "The Namespace Is Handed Over at Launch" arrived at independently.
- Paths select a mount by a `name:` prefix and otherwise resolve against a
  current mount, a drive-letter scheme that is well understood and composes
  without an ambient root. Worth weighing against the current pure handle-
  relative form if a human-facing path syntax is ever wanted.
- A backend vtable separates the protocol operations from the storage, the same
  separation a filesystem server wants here.
- `readdir` takes a backend-owned opaque cursor and returns one entry at a time,
  which is the model Proposal 6 adopts. It settles that the opaque-cursor choice
  is workable in code rather than only on paper.

No port is implied. The value is the validation and the answers it hands
Proposal 6.
