# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Recurses into SUBDIR, in the order listed. The order is load-bearing:
# libc must exist before a program links against it, and the userland
# program must exist before the kernel embeds it.

.PHONY: all clean

all clean:
	@for d in $(SUBDIR); do $(MAKE) -C $$d $@ || exit $$?; done
