# Nyctos

A microkernel operating system, built to be read.

Nyctos runs the same application interface whether or not the hardware has a
memory management unit. Flat memory and a full MMU are two levels of
enforcement of one set of rules. A correct program behaves the same on both;
what changes is whether the hardware can catch an incorrect one.

Above that sit synchronous message passing with priority inheritance,
per-process namespaces assembled in the C library rather than in the kernel,
and a filesystem that is a protocol rather than a layer. Filesystems, device
drivers, pagers, loaders and supervisors are all ordinary processes.

A thread can also hand the kernel an ordered array of operations whose
dependencies are expressed as fences rather than control flow, run in the
submitting thread's own context at its own priority, with no worker pool. This
submission engine is kept separable because it is experimental.

## Status

Roughly half the kernel exists, with enough userland to use it. `docs/design.md`
describes the intended system and says which half is built; the gap narrows as
work lands.

What runs today:

- Two architectures, aarch64 and riscv64, on the QEMU `virt` board.
- Two protection tiers from one source: Tier 0 is flat memory with a trusted
  contract, Tier 2 is the same source with an MMU enforcing it. Tier 2 runs on
  two cores; Tier 0 keeps one.
- Processes with spawn, exit, join, kill and reaping, and frames reclaimed when
  a process ends.
- Synchronous message passing with priority inheritance, rendezvous with
  deadlines, and per-process namespaces with no global root.
- A user-space virtio-mmio block store that reads and writes a real emulated
  disk. A value written in one boot is read back in the next.
- A filesystem server, a console driver, and a supervisor, all ordinary
  processes.

All four configurations (two architectures by two tiers) pass `make check`. See
`docs/CHANGELOG.md` for what has shipped and `docs/ROADMAP.md` for the plan.

Footprints are asserted, because a count is a property of this kernel.
Durations are reported and not asserted, because every one was taken under an
emulator. Nothing has run on hardware, so every duration here has the right
shape and the wrong magnitude.

## Design aims

Two long-term aims constrain the code without being deliverables, both stated
in `docs/design.md`.

The first is a system that can be read end to end, kernel and userland, in the
tradition of the Lions commentary and xv6. That is why functions are small, why
the whole system matters and not the kernel alone, and why ideas that do not
fit are recorded in `docs/PROPOSALS.md` rather than built.

The second is per-process environment isolation, in the spirit of Qubes OS but
at a fraction of the weight. It adds nothing new to the design: no ambient
authority, no global filesystem root, a memory contract that transfers rather
than shares, explicit protection domains, and a build-time-fixed set of
processes are all present for other reasons and together are most of what that
property means. There is no hypervisor and no promise of one. Nyctos isolates
processes, not machines.

Neither aim is being written up, and neither is scheduled. The system has to
work first.

## Documentation

| File | What it holds |
| --- | --- |
| `docs/design.md` | The intended system, in detail. Builds to `nyctos.pdf`. |
| `docs/ROADMAP.md` | The plan, what each item answered, and what would change it. |
| `docs/BACKLOG.md` | Work considered but not scheduled. |
| `docs/CHANGELOG.md` | What has shipped, per minor version. |
| `docs/PROPOSALS.md` | Deliberately not built, with the reasoning kept. |
| `docs/LIMITATIONS.md` | Known limitations and the paths the checks do not exercise. |
| `docs/HACKING.md` | The source tree, the memory map, and coding style. |
| `docs/CODING_STYLE.md` | C style: a FreeBSD style(9) variant, 4-space indent, no tabs. |
| `docs/TODO.md` | Notes on the document build toolchain. |

## Requirements

The build runs on Linux, or on macOS with [Homebrew](https://brew.sh). Windows
with Chocolatey is a plausible third host and is not supported yet.

Four things are needed on any host:

- **Clang/LLVM**, which cross-compiles and assembles for `aarch64-none-elf` and
  `riscv64-none-elf` without a separate GCC cross-toolchain per architecture.
- **LLD** (`ld.lld`), the linker used for both targets.
- **QEMU** (`qemu-system-aarch64`, `qemu-system-riscv64`).
- **GNU Make**.

[share/mk/nyc.own.mk](share/mk/nyc.own.mk) locates the toolchain itself. If
`brew` is present it asks Homebrew where its keg-only LLVM and LLD live;
otherwise it takes `clang`, `llvm-ar`, `llvm-objcopy` and `ld.lld` from `PATH`.
No manual `PATH` or shell profile changes are required on either host.

## Linux setup

Install the toolchain with the system package manager:

```sh
# Debian / Ubuntu
sudo apt install clang lld llvm make qemu-system-arm qemu-system-riscv

# Fedora
sudo dnf install clang lld llvm make qemu-system-aarch64 qemu-system-riscv

# Arch
sudo pacman -S clang lld llvm make qemu-system-aarch64 qemu-system-riscv
```

Verify:

```sh
clang --version && ld.lld --version
qemu-system-aarch64 --version
qemu-system-riscv64 --version
```

For debugging, `gdb-multiarch` (Debian/Ubuntu) or a distribution `gdb` built
with multiarch support understands both targets and can attach to QEMU's
gdbstub.

## macOS setup

1. Install Homebrew, if you don't already have it:

   ```sh
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

2. Install the Xcode Command Line Tools, which provide GNU Make and other basic
   build tools:

   ```sh
   xcode-select --install
   ```

3. Install the cross-compiler toolchain, linker, and emulator:

   ```sh
   brew install llvm lld qemu
   ```

4. Verify everything is in place:

   ```sh
   brew --prefix llvm && brew --prefix lld
   qemu-system-aarch64 --version
   qemu-system-riscv64 --version
   ```

5. Optional, for debugging: install a target-aware GDB for each architecture:

   ```sh
   brew install aarch64-elf-gdb riscv64-elf-gdb
   ```

   These understand the target's registers and calling convention, unlike the
   host's own `lldb`/`gdb`, and can attach to QEMU's gdbstub
   (`qemu-system-aarch64 -s -S`, `qemu-system-riscv64 -s -S`) to step through
   boot code instruction by instruction.

Homebrew keeps LLVM and LLD keg-only (not linked onto `PATH`) because they
would shadow Apple's own clang, which is why the build asks `brew` where they
are rather than trusting `PATH`.

## Project layout

The tree follows a BSD source layout:

- `sys/` is the kernel: machine-independent code in `kern/`, per-architecture
  code in `arch/aarch64/` and `arch/riscv64/`, and headers in `sys/`.
- `lib/` is the C library (`libc/`) and the C runtime startup (`csu/`).
- `bin/` is around forty userland programs: the shell and its utilities, the
  console driver, the filesystem server and the block store beneath it, a
  supervisor, and programs written to break a rule, block, or fault on purpose
  for the checks.
- `include/` is the headers userland programs compile against.
- `tools/` is the host-side build tools, which make the boot archive and the
  disk image.
- `share/` holds the make fragments (`mk/`) and userland linker scripts (`ld/`).
- `docs/` holds the design, the roadmap, and the documents linked above.

[docs/HACKING.md](docs/HACKING.md) walks the source tree file by file and
records the memory map.

## Building

`ARCH` selects the target architecture and defaults to `aarch64`. `TIER`
selects the protection tier and defaults to `0`, flat memory with a trusted
contract; `TIER=2` is the same source with an MMU behind it. Each `make`
invocation builds one configuration, so building all four is four commands:

```sh
make                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64          # the other architecture
make TIER=2                # the other tier
make ARCH=riscv64 TIER=2   # both
```

Outputs land in `_out/<target-triplet>-tier<N>/`, keyed by the clang target
triplet and the tier so the four configurations never collide. The tree below
it mirrors the source tree, with `lib/libc.a` and `lib/crt1.o` staged where
userland programs link against them and the final image at `sys/kernel.elf`.
Header dependencies are tracked with `-MMD -MP` and picked up on the next build,
so editing a shared header rebuilds every file that includes it.

```sh
make clean                 # removes this configuration's output directory only
make ARCH=riscv64 clean    # removes riscv64 Tier 0's output only
make cleandir              # removes _out/ entirely
```

`make clean` only ever removes the current configuration's output directory. It
never touches another architecture's or another tier's.

## Running under QEMU

```sh
make run                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64 TIER=2 run   # the other of each
```

Both run QEMU with `-nographic`, so console output appears in your terminal. The
boot checks run, then the shell:

```
$ echo one two three
one two three
$ cat motd
served from outside the kernel
$ exit
```

`exit` leaves the shell, after which the kernel powers the machine off and QEMU
exits. A panic does the same, having printed why. On riscv64 the exit status
distinguishes them, because the SiFive test finisher carries a code; PSCI
`SYSTEM_OFF` on aarch64 does not, so there the console is the only signal.

Two boot checks want input before the shell does: the console service reads two
lines and `greet` reads one, so a session starts by typing three lines of
anything.

## Running the recorded session

```sh
make check                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64 check          # the other architecture
make TIER=2 check                # the other tier
make ARCH=riscv64 TIER=2 check   # both
```

`make check` feeds [share/check/session.txt](share/check/session.txt) to the
machine twice and takes about half a minute. Once paced at two seconds a line,
which is how a person types, with the transcript from the shell's banner to its
exit compared against [share/check/expected.txt](share/check/expected.txt).
Then once as a burst, which is how a pipe delivers a whole session at once,
where the check is that the machine reaches the end without a panic.

Both run because they catch different things. A console that lets a completed
line block whatever is typed behind it passes the paced session and hangs the
burst one; a receive buffer too small for a burst fails both.

One session file serves every configuration. The resource report after the
session is one file per tier, because Tier 2 runs two processes where Tier 0
refuses the second, and an exact count of what a run used is worth more than a
line both tiers share.

## Licensing

This operating system is dual-licensed under either of:

* Apache License, Version 2.0 ([LICENSE-APACHE](LICENSES/Apache-2.0.txt))
* BSD Zero Clause License ([LICENSE-0BSD](LICENSES/0BSD.txt))

You may choose which license to follow when using, modifying, or distributing
this software. All inbound contributions must be submitted under this
dual-license structure.

Every source file carries an `SPDX-License-Identifier` line and a short
copyright comment near the top, per
[docs/CODING_STYLE.md](docs/CODING_STYLE.md#licensing).

## More information

- [docs/design.md](docs/design.md) describes the intended system in detail.
- [docs/ROADMAP.md](docs/ROADMAP.md) is the plan, and
  [docs/CHANGELOG.md](docs/CHANGELOG.md) is what has shipped.
- [docs/LIMITATIONS.md](docs/LIMITATIONS.md) lists what the system does not do
  yet.
- [docs/HACKING.md](docs/HACKING.md) is for working on the code.
