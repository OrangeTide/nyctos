# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# mkfsimg is a host tool: it runs on the build machine, not the target, so
# it is built with the host's own cc rather than the cross toolchain that
# nyc.obj.mk would otherwise select.

include $(TOPDIR)/share/mk/nyc.own.mk

.PHONY: all clean

all: $(MKFSIMG)

$(MKFSIMG): mkfsimg.c $(TOPDIR)/sys/sys/tfs.h
	@mkdir -p $(@D)
	$(HOSTCC) -Wall -Wextra -O2 -I$(TOPDIR)/sys -o $@ mkfsimg.c

clean:
	rm -f $(MKFSIMG)
