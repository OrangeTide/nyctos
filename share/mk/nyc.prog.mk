# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Builds a userland program from SRCS: an ELF image for debugging, plus the
# flat binary the kernel actually loads.

include $(TOPDIR)/share/mk/nyc.obj.mk

PROG_ELF := $(OBJDIR)/$(PROG)
PROG_BIN := $(PROG_ELF).bin

all: $(PROG_ELF) $(PROG_BIN)

$(PROG_ELF): $(OBJS) $(CRT1) $(LIBC) $(USER_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -T $(USER_LDSCRIPT) -o $@ $(CRT1) $(OBJS) $(LIBC)

$(PROG_BIN): $(PROG_ELF)
	$(OBJCOPY) -O binary $< $@
