# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# A host tool: it runs on the machine doing the build, so it uses HOSTCC
# and not the cross toolchain. It lives under the architecture's output
# directory anyway, because it is rebuilt per configuration and nothing
# else wants it.

include $(TOPDIR)/share/mk/nyc.own.mk

OBJDIR := $(OBJTOP)/$(patsubst $(TOPDIR)/%,%,$(CURDIR))

.PHONY: all clean

all: $(MKBOOTARCH)

$(MKBOOTARCH): mkbootarch.c $(TOPDIR)/sys/sys/bootarch.h
	@mkdir -p $(@D)
	$(HOSTCC) -Wall -Wextra -O2 -I$(TOPDIR)/sys -o $@ $<

clean:
	rm -rf $(OBJDIR)
