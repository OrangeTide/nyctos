# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Compilation rules shared by libraries, programs and the kernel. A leaf
# Makefile sets SRCS, CFLAGS and CPPFLAGS, then includes nyc.lib.mk or
# nyc.prog.mk, which include this file.

OBJDIR := $(OBJTOP)/$(patsubst $(TOPDIR)/%,%,$(CURDIR))
OBJS   := $(patsubst %,$(OBJDIR)/%.o,$(basename $(SRCS)))
DEPS   := $(OBJS:.o=.d)

# Objects depend on the Makefile that sets their flags. Without this a
# change to CFLAGS leaves every object looking up to date, so toggling a
# -D flag appears to do nothing and the build is silently the old one.
$(OBJDIR)/%.o: %.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.o: %.S Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

.PHONY: all clean

clean:
	rm -rf $(OBJDIR)

-include $(DEPS)
