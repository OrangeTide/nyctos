# Changelog

Notable changes per minor version. Patch bumps between minors move the VERSION
file alone and are not listed here; see the git log for their detail. Each
release's full retrospective lives in docs/ROADMAP.md.

## 0.35.0

The block store's metadata on the disk. The copy-on-write store kept its
indirection in memory and rebuilt an identity map every boot, so a write that
forked a block and a snapshot that shared one were both lost at power off. The
live map, the reference counts, and the snapshot now ride in a metadata sector,
so the store reads back the shape a previous boot left rather than starting
over.

The sector holds a header over the live map, the reference counts, the snapshot
map, and its live flag, built in three stages. The first wrote the live map and
the reference counts behind a magic, a version, and an epoch, proven by the
epoch counting up across a boot. The second added the snapshot: the store arms a
diverged snapshot on a fresh image, forking the demonstration block onto a
physical block the allocator never hands out so a later boot's self-tests cannot
clobber it, and a later boot restores the snapshot and reads the new bytes live
and the old bytes through it. The third sealed the body with a checksum, so a
sector with the right magic and version but a damaged body is passed over the
way a fresh image is and the store falls back to identity rather than trusting
it.

Each stage shipped behind a check that fails when its piece is removed. The
second boot's console reports the restored epoch, the surviving snapshot, and
the rejected damaged sector, and the burst check greps the second boot for all
three.

## 0.34.0

Source lines in the backtrace. A panic names each frame with the source file and
line its address falls on, not only the function and the offset, and the kernel
carries the line table to do it without the host tool.

The offline `nycsym` reads `.debug_line`, DWARF version 4 on riscv64 and version
5 on aarch64, so it names a recorded address with its file and line against the
kernel ELF. The in-kernel path carries a line table the way it already carries
the symbol table: `mkline` reads the first pass's line program, flattens it to
one row per statement, and writes the rows sorted by address and delta encoded,
which holds the whole table in about forty per cent of the text rather than its
full size. A second two-pass link embeds the blob past the text so its addresses
do not move, and `kline_lookup` walks it to the nearest row at or below an
address. The panic backtrace prints the file and line beside each name, and a
frame the table has no line for keeps just its name.

Checks stand behind each part. A boot probe records its own source line and the
check fails unless the table gives it back, and the backtrace check requires its
known nest of frames to resolve into this file. The symbols step holds the
embedded table against the DWARF across every text address: `mkline --query`
resolves the blob the way the kernel does, and `klinecheck.sh` compares each
address to the line llvm-symbolizer reads, so a blob that disagrees with its own
DWARF fails the build.

## 0.33.0

A larger shell. The shell reads a command, tokenizes it, and interprets it from
the token list, a Bourne subset built in five stages.

Control flow runs over the programs it starts: `if`, `while`, `until`, `for` and
`case`, command lists joined by `;` or newline, and `break` and `continue` with
an optional count that carries through nested loops. A loop body re-runs by
resetting the interpreter to a recorded token position, and a not-taken branch is
walked in a skip mode that moves the cursor without side effects.

Words carry quoting and expansion, resolved when the word is used rather than
when it is read, so a word reused across loop passes expands against the current
values. Single quotes keep their contents literal, double quotes keep the
expansions but hold the spaces inside, and a backslash escapes the next
character. A leading `NAME=VALUE` word sets a shell variable.

`set` fills the positional parameters, and `$1`..`$9`, `${NN}`, `$#`, `$@`, `$*`
and `$0` read them. Expansion feeds a shared engine that produces one or more
fields: an unquoted expansion splits on whitespace, a quoted one keeps its
spaces, `$@` and an unquoted `$*` give a field per parameter, and a quoted `$*`
joins them into one.

`$(( expr ))` evaluates an integer expression with the usual C precedence from
`||` down to unary and parentheses, reading variables and positional parameters
by name, and treating a divide or remainder by zero as zero rather than a fault.

A function is defined with `name() { list }` and called like any command,
running its body over its own stored token array with the call arguments as the
positional parameters. The caller's stream, parameters and loop state are
restored on return. `return` ends a function and sets its status, `local` gives a
variable a value that lasts only for the call, and `{ list; }` runs a list in the
current shell.

The recorded `make check` session grew a stretch for each stage, and each stage
shipped with a check that fails when its one feature is removed: a `break` that
ignores its count, single quotes that do not suppress an expansion, an unquoted
expansion passed on whole, addition binding as tightly as multiplication, and a
call that does not pass its arguments.

## 0.32.0

Panic symbolization. The kernel compiles with frame pointers, and a panic
unwinds the saved frame-pointer chain to the chain of return addresses that
reached it. Each is named as the function it falls in plus the offset, so a
panic reads as a backtrace of function names rather than bare addresses; an
address the table cannot name prints raw. A fault reaches this path through
panic and reads the same way. The unwinder is bounded to one kernel stack, so
a chain that does not terminate cleanly stops at the stack's edge.

Two readers name the addresses. `nycsym` is a host tool that reads the kernel
ELF and names a panic's recorded addresses offline. The in-kernel reader,
`ksym_lookup`, names an address against a table of the kernel's function
symbols the image carries: a two-pass link embeds it, `mksymtab` builds it from
the first pass, and it sits in rodata past the text so the addresses it records
do not move before the second pass links it in. Both name an address by the
function that contains it, the nearest symbol at or below the address. The
kernel keeps no `strcmp`; the boot checks compare names by the pointer the
table returns.

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
