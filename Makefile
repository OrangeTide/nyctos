# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Builds one architecture per invocation, selected via ARCH (default in
# share/mk/nyc.own.mk). To build a different architecture, re-invoke make:
#   make ARCH=riscv64
#   make ARCH=riscv64 run
#   make ARCH=riscv64 clean

TOPDIR := $(CURDIR)
export TOPDIR

include $(TOPDIR)/share/mk/nyc.own.mk

# Order matters: libc and crt1 before the programs that link against them,
# and the programs before the kernel that will load them.
SUBDIR := lib bin tools sys

.PHONY: all check clean cleandir run gh-pages

all clean:
	@for d in $(SUBDIR); do $(MAKE) -C $$d $@ || exit $$?; done

# The recorded session of milestone 3 stage 8, run twice.
#
# Paced, which is how a person types: every line arrives while the
# machine is already waiting for it. The transcript from the shell's
# banner onward is compared against share/check/expected.txt, and that
# is where the comparison starts because everything before it carries
# numbers that differ between runs and between boards. A check that
# compared those would fail for the wrong reason.
#
# Then burst, which is how a pipe delivers a session: the whole thing
# arrives before anything has asked for any of it. The transcript
# differs, because the console echoes each line when it arrives rather
# than when it is read, so what is checked there is that the machine
#
# The carriage returns go. A terminal needs one before every line feed
# and the driver writes them, but a file a person reads and edits
# should not carry them: the first attempt to break this check on
# purpose failed to break it, because the line being edited ended in a
# carriage return and the edit matched nothing.
# reaches the end without a panic.
check: all
	@mkdir -p $(OBJTOP)/check
	@echo "check: $(ARCH), paced"
	@$(TOPDIR)/scripts/session.sh 2 "$(QEMU)" "$(QEMU_FLAGS)" \
	    $(KERNEL_ELF) > $(OBJTOP)/check/paced.out
	@sed -n '/^nyctos shell/,$$p' $(OBJTOP)/check/paced.out \
	    | tr -d '\r' > $(OBJTOP)/check/paced.session
	@diff -u $(TOPDIR)/share/check/expected.txt \
	    $(OBJTOP)/check/paced.session
	@echo "check: $(ARCH), burst"
	@$(TOPDIR)/scripts/session.sh 0 "$(QEMU)" "$(QEMU_FLAGS)" \
	    $(KERNEL_ELF) > $(OBJTOP)/check/burst.out
	@grep -q '^stage 9 complete' $(OBJTOP)/check/burst.out \
	    || { echo 'check: the burst session did not finish'; false; }
	@! grep -q '^panic:' $(OBJTOP)/check/burst.out \
	    || { echo 'check: the burst session panicked'; false; }
	@echo "check: $(ARCH) ok"

run: all
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

# Removes every architecture's build products, not just this one's.
cleandir:
	rm -rf $(TOPDIR)/_out

# Builds docs/site (see docs/Makefile) and publishes it to the gh-pages
# branch; see scripts/deploy-gh-pages.sh for what "publishes" means.
gh-pages:
	$(TOPDIR)/scripts/deploy-gh-pages.sh
