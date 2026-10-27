/* batchreg.h : typed registers for batch submissions.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares struct batch_regs, the typed register file a submission uses to
 * pass a handle or a scalar from one entry to the next. Each register is
 * tagged REG_EMPTY, REG_SCALAR, or REG_HANDLE. Reading a register as the
 * wrong kind, or reading an empty one, returns SYS_ERR_INVALID_ARG.
 * Included by the kernel operation, syscall, and init code.
 *
 */

#ifndef _SYS_BATCHREG_H_
#define _SYS_BATCHREG_H_

#include <sys/op.h>

#include <stdint.h>

/*
 * What a register holds. Empty is the state every register starts in,
 * and is distinct from a scalar zero: a register nothing has written is
 * not a register holding the number nothing.
 */
#define REG_EMPTY       0
#define REG_SCALAR      1
#define REG_HANDLE      2

struct batch_regs {
    uint64_t    br_val[SYS_BATCH_REGS];
    uint8_t     br_tag[SYS_BATCH_REGS];
};

/* Every register empty. Called once at the start of an execution. */
void regs_init(struct batch_regs *r);

/*
 * Write one. Refuses an index outside the file rather than clamping it,
 * because a caller naming a register that does not exist has made a
 * mistake this can still tell it about.
 */
sys_err_t regs_put_handle(struct batch_regs *r, unsigned int i,
  sys_handle_t h);
sys_err_t regs_put_scalar(struct batch_regs *r, unsigned int i, uint64_t v);

/*
 * Read one, as the kind asked for. Reading a register that holds the
 * other kind, or holds nothing at all, is SYS_ERR_INVALID_ARG and not a
 * value: this is the refusal the file exists to make.
 */
sys_err_t regs_get_handle(const struct batch_regs *r, unsigned int i,
  sys_handle_t *out);
sys_err_t regs_get_scalar(const struct batch_regs *r, unsigned int i,
  uint64_t *out);

/*
 * What a register holds, for a caller that wants to ask before reading.
 *
 * An error rather than a tag for an index outside the file, because a
 * tag would have to be one of the three and every one of them says the
 * register exists. Answering REG_EMPTY for a register that cannot be
 * written is the one answer that reads as an invitation.
 */
sys_err_t regs_peek(const struct batch_regs *r, unsigned int i,
  uint8_t *tag);

#endif /* _SYS_BATCHREG_H_ */
