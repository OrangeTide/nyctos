# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Builds a userland program from SRCS: an ELF image for debugging, plus the
# flat binary the kernel actually loads.
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
PROG_BIN := $(PROG_ELF).bin

all: $(PROG_ELF) $(PROG_BIN)

$(PROG_ELF): $(OBJS) $(CRT1) $(LIBC) $(USER_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) --defsym=USER_LOAD_ADDR=$(PROG_LOAD_ADDR) \
	    -T $(USER_LDSCRIPT) -o $@ $(CRT1) $(OBJS) $(LIBC)

$(PROG_BIN): $(PROG_ELF)
	$(OBJCOPY) -O binary $< $@

# What the loader needs and the flat binary cannot say: where writable
# memory begins and where the program's memory ends. Both are addresses
# the linker script names; this records them as offsets from the load
# address, because that is what the archive carries and what a loader
# placing an image elsewhere would still be able to use.
#
# Written beside the binary rather than passed on a command line, so
# that the archive rule does not have to know how to read symbols and
# the numbers cannot drift from the link that produced them.
PROG_LAYOUT := $(PROG_BIN).layout

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
