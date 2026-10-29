# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# A host tool: it runs on the machine doing the build with HOSTCC, not the
# cross toolchain, and is rebuilt per configuration under the architecture's
# output directory.

include $(TOPDIR)/share/mk/nyc.own.mk

OBJDIR := $(OBJTOP)/$(patsubst $(TOPDIR)/%,%,$(CURDIR))

.PHONY: all clean

all: $(MKLINE)

$(MKLINE): mkline.c $(TOPDIR)/sys/sys/elf.h $(TOPDIR)/sys/sys/kline.h
	@mkdir -p $(@D)
	$(HOSTCC) -Wall -Wextra -O2 -I$(TOPDIR)/sys -o $@ $<

clean:
	rm -rf $(OBJDIR)
