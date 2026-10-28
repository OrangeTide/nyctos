# Changelog

Notable changes per minor version. Patch bumps between minors move the VERSION
file alone and are not listed here; see the git log for their detail. Each
release's full retrospective lives in docs/ROADMAP.md.

## 0.31.0

ELF in the boot archive. Each member is carried as a debug-stripped ELF, and
the kernel loads a program by walking its `PT_LOAD` headers: each loadable
segment to its virtual address, the memory past the file bytes zeroed, the
read-only and writable regions taken from the segment flags. The archive tool
reads the load address and the bounds out of those headers, so the `.layout`
sidecar and the load addresses once hand-written in the archive rule are gone;
a member is named and pointed at its ELF, and the tool refuses an image whose
memory would overrun its slot. The linker script reserves the stack and the
record page inside the bss section so the writable segment's memory size
reaches the program's end, which is what lets the headers describe the whole
image.

The archived ELF keeps its symbol table, and a reader resolves an address in a
member to the name of the function that contains it, the symbol path panic
symbolization will read next. The ELF is stripped of its debug sections and
local symbols: a backtrace names a static function by the public one before
it, and the line numbers stay in the on-disk ELF, so the kernel-embedded
archive stays near its former size. Userland links with a 4 KiB max page size
for the same reason. A shared `sys/elf.h` gives the kernel and the host tool
one description of the format. No kernel mechanism changed.

## 0.30.0

A copy-on-write block store. The store resolves every read and write through an
in-memory logical-to-physical block map, identity at boot, over a physical
arena larger than the logical store. A write to a block held by more than one
reference forks it: a free block takes a copy of the old bytes, the map is
pointed at the new block, and the write lands there, so the old bytes stay
readable through the other holder. A snapshot captures the live map and raises
a reference on every block it names, so a later write to a shared block forks
and a read through the snapshot still reads the bytes from before the write
while the live view reads after. Three boot self-tests stand behind the map,
the fork, and the snapshot before the store serves.

The map, the reference counts, the free pool, and the snapshot live in memory
and are rebuilt from the fixed layout each boot. A snapshot does not survive a
boot, and on-disk CoW metadata is later work. No kernel mechanism changed.

## 0.29.0

M28, "Every Pool Back to Where It Started," is the milestone of this release.
A soak churns the lifecycles the system reclaims, spawn and reap, connect and
close, and walk and close, interleaved in one loop longer than any of their
pools, under a bracket that records every object pool and the settled frame
count before and asserts them after. The per-subsystem reclaim checks each
prove one pool gives everything back; this proves they compose, that no pool
leaks when the lifecycles churn together. No kernel mechanism changed; the soak
is a boot-harness loop over operations the system already had and an assertion
over counts it already kept.

## 0.28.0

M27, "The Block Device's Interrupt, Not Its Spin," is the milestone of this
release. The user-space virtio-mmio driver waits for the device's interrupt
instead of spinning the used ring: a transfer notifies the queue and blocks on
a timeline the kernel advances from the interrupt handler, the same path the
console uses for the UART. The driver tells a completion from a configuration
change or a spurious line by the interrupt-status register, counts the wakes it
takes for completions, and refuses to serve if that count drifts from the
transfers it issued. The ports name the virtio-blk interrupt from a helper, and
the launcher attaches it to a timeline and hands the store a handle to the
interrupt and a handle to wait on. No kernel mechanism changed.

The forge of a spurious interrupt, which lets the driver show it passes over a
source that is not a completion, works through the GIC on aarch64; QEMU's PLIC
ignores a software write to its pending array, so riscv64 has none to pass over
and runs the identical driver.

This release also cut the README down to reference material and moved the
developer and known-limitation detail into docs/HACKING.md and
docs/LIMITATIONS.md.

## 0.27.0

M26, "A Write the Disk Keeps Across a Boot," is the milestone of this release.
The user-space virtio-mmio store gained a write path: it answers FS_OP_WRITE by
a read-modify-write of the block's sector, so a write smaller than a sector
leaves its neighbours alone, and a marker written in one boot is read back by
the next boot over the same disk image.

The versioning and release process was also established in this release: the
VERSION file as the source of truth, this changelog, annotated tags on minor
bumps, and a local CI run across all four configs after tagging and before any
push for as long as ocelot is the host.

## 0.26.0

Baseline. The version number is anchored here at the project's built history.
The scheme did not exist before this point; the milestones up to it are
recorded in docs/ROADMAP.md.
