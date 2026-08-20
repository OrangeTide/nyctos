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
