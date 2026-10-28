# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Builds a userland program from SRCS: an ELF image for debugging, plus the
# debug-stripped ELF the kernel loads out of the boot archive.
#
# PROG_LOAD_SLOT places the image. There is no MMU at Tier 0, so every
# program that can be live at the same time as another needs its own slot,
# and a repeated slot is two programs at one address. Slots are assigned by
# hand for now, which is the honest form of a decision a loader will make
# once section 15.2 exists.

include $(TOPDIR)/share/mk/nyc.obj.mk

PROG_LOAD_SLOT ?= 0
PROG_LOAD_ADDR := $(shell printf '0x%x' \
    $$(( $(USER_LOAD_ADDR) + $(PROG_LOAD_SLOT) * $(USER_LOAD_STRIDE) )))

PROG_ELF := $(OBJDIR)/$(PROG)
PROG_STRIP := $(PROG_ELF).strip

all: $(PROG_ELF) $(PROG_STRIP)

# max-page-size is 4 KiB so the ELF's loadable segments are packed on page
# boundaries rather than the linker's 64 KiB default. The archive carries
# the ELF, and the default would pad tens of kilobytes of zeroes ahead of
# each program's text, which the kernel-embedded archive should not hold.
$(PROG_ELF): $(OBJS) $(CRT1) $(LIBC) $(USER_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -z max-page-size=0x1000 \
	    --defsym=USER_LOAD_ADDR=$(PROG_LOAD_ADDR) \
	    -T $(USER_LDSCRIPT) -o $@ $(CRT1) $(OBJS) $(LIBC)

# The archive carries a debug-stripped ELF: a valid container with the
# program's loadable segments and the global symbols a backtrace resolves,
# without the line-number debug sections and the local symbols. The locals
# are dropped because riscv relaxation leaves thousands of unnamed ones per
# program, which carry nothing a reader could print and would three times
# over the size of the kernel-embedded archive. The cost is that a static
# function is named in a backtrace by the global symbol before it rather
# than by its own name.
$(PROG_STRIP): $(PROG_ELF)
	$(OBJCOPY) --strip-debug --discard-all $< $@

# What the loader needs, recorded as offsets from the load address: where
# writable memory begins and where the program's memory ends. The archive
# tool reads these out of the program headers now; this sidecar is kept
# only so the tool can cross-check what it read against the link that
# produced it, and goes away once that check has earned its retirement.
PROG_LAYOUT := $(PROG_STRIP).layout

all: $(PROG_LAYOUT)

$(PROG_LAYOUT): $(PROG_ELF)
	@mkdir -p $(@D)
	@rw=$$($(NM) $< | awk '$$3 == "_rw_start" { print $$1 }'); \
	 me=$$($(NM) $< | awk '$$3 == "_mem_end" { print $$1 }'); \
	 test -n "$$rw" -a -n "$$me" || \
	     { echo "$@: the link named no _rw_start or _mem_end" >&2; \
	       exit 1; }; \
	 printf '0x%x 0x%x\n' \
	     $$(( 0x$$rw - $(PROG_LOAD_ADDR) )) \
	     $$(( 0x$$me - $(PROG_LOAD_ADDR) )) > $@
