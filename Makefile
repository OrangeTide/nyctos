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
# banner to the shell's exit is compared against
# share/check/expected.txt, and that is where the comparison starts
# because everything before it carries numbers that differ between runs
# and between boards. A check that compared those would fail for the
# wrong reason.
#
# Then burst, which is how a pipe delivers a session: the whole thing
# arrives before anything has asked for any of it. The transcript
# differs, because the console echoes each line when it arrives rather
# than when it is read, so what is checked there is that the machine
# reaches the end without a panic.
#
# The resource report after the session is compared separately, against
# a file per tier. The session is the same on both and that is the
# milestone's claim; what a tier costs is not the same, because Tier 2
# runs two processes where Tier 0 refuses the second, and holding both
# to one number would mean giving up an exact count of what the run
# used.
#
# The carriage returns go. A terminal needs one before every line feed
# and the driver writes them, but a file a person reads and edits
# should not carry them: the first attempt to break this check on
# purpose failed to break it, because the line being edited ended in a
# carriage return and the edit matched nothing.
check: all
	@mkdir -p $(OBJTOP)/check $(dir $(DISK_IMG))
	@$(MKDISK) $(DISK_IMG)
	@echo "check: $(ARCH), paced"
	@$(TOPDIR)/scripts/session.sh 2 "$(QEMU)" "$(QEMU_FLAGS)" \
	    $(KERNEL_ELF) > $(OBJTOP)/check/paced.out
	@sed -n '/^nyctos shell/,/^    the shell exited/p' $(OBJTOP)/check/paced.out \
	    | tr -d '\r' > $(OBJTOP)/check/paced.session
	@diff -u $(TOPDIR)/share/check/expected.txt \
	    $(OBJTOP)/check/paced.session
	@sed -n '/^used at most:/,$$p' $(OBJTOP)/check/paced.out \
	    | tr -d '\r' > $(OBJTOP)/check/paced.usage
	@diff -u $(TOPDIR)/share/check/usage-tier$(TIER).txt \
	    $(OBJTOP)/check/paced.usage
	@echo "check: $(ARCH), burst"
	@$(TOPDIR)/scripts/session.sh 0 "$(QEMU)" "$(QEMU_FLAGS)" \
	    $(KERNEL_ELF) > $(OBJTOP)/check/burst.out
	@grep -q '^stage 9 complete' $(OBJTOP)/check/burst.out \
	    || { echo 'check: the burst session did not finish'; false; }
	@! grep -q '^panic:' $(OBJTOP)/check/burst.out \
	    || { echo 'check: the burst session panicked'; false; }
	@grep -q '^persist: the marker survived' $(OBJTOP)/check/burst.out \
	    || { echo 'check: the write did not survive to the second boot'; \
	         false; }
	@grep -q '^blk: interrupts told apart' $(OBJTOP)/check/burst.out \
	    || { echo 'check: the block driver did not tell its interrupts apart'; \
	         false; }
	@grep -q '^blk: metadata restored' $(OBJTOP)/check/burst.out \
	    || { echo 'check: the store metadata did not survive to the second boot'; \
	         false; }
	@echo "check: $(ARCH), symbols"
	@$(TOPDIR)/scripts/symcheck.sh $(NYCSYM) $(KERNEL_ELF)
	@$(TOPDIR)/scripts/klinecheck.sh $(MKLINE) $(KLINE_BLOB) $(KERNEL_ELF)
	@echo "check: $(ARCH) ok"

run: all
	@mkdir -p $(dir $(DISK_IMG))
	@$(MKDISK) $(DISK_IMG)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

# Removes every architecture's build products, not just this one's.
cleandir:
	rm -rf $(TOPDIR)/_out

# Builds docs/site (see docs/Makefile) and publishes it to the gh-pages
# branch; see scripts/deploy-gh-pages.sh for what "publishes" means.
gh-pages:
	$(TOPDIR)/scripts/deploy-gh-pages.sh
