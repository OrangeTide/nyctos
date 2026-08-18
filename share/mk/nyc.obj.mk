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

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

.PHONY: all clean

clean:
	rm -rf $(OBJDIR)

-include $(DEPS)
