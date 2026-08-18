# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Builds a static library from SRCS and stages it in LIBDIR as lib$(LIB).a.

include $(TOPDIR)/share/mk/nyc.obj.mk

LIB_A := $(LIBDIR)/lib$(LIB).a

all: $(LIB_A)

$(LIB_A): $(OBJS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $(OBJS)
