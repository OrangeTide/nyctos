/* kern_batchreg.c : tagged register file for batched system calls.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Each batch register holds a value and a tag that marks it as empty, a
 * scalar or a handle. Put and get functions check the index and the tag, so
 * a batch cannot read a handle as a number or read a register that nothing
 * has written. Refused accesses return SYS_ERR_INVALID_ARG.
 *
 */

#include <sys/batchreg.h>
#include <sys/systm.h>

void
regs_init(struct batch_regs *r)
{
    unsigned int i;

    for (i = 0; i < SYS_BATCH_REGS; i++) {
        r->br_val[i] = 0;
        r->br_tag[i] = REG_EMPTY;
    }
}

sys_err_t
regs_put_handle(struct batch_regs *r, unsigned int i, sys_handle_t h)
{
    if (i >= SYS_BATCH_REGS) {
        return SYS_ERR_INVALID_ARG;
    }

    r->br_val[i] = (uint64_t)h;
    r->br_tag[i] = REG_HANDLE;

    return SYS_OK;
}

sys_err_t
regs_put_scalar(struct batch_regs *r, unsigned int i, uint64_t v)
{
    if (i >= SYS_BATCH_REGS) {
        return SYS_ERR_INVALID_ARG;
    }

    r->br_val[i] = v;
    r->br_tag[i] = REG_SCALAR;

    return SYS_OK;
}

sys_err_t
regs_get_handle(const struct batch_regs *r, unsigned int i, sys_handle_t *out)
{
    if (i >= SYS_BATCH_REGS || r->br_tag[i] != REG_HANDLE) {
        return SYS_ERR_INVALID_ARG;
    }

    *out = (sys_handle_t)r->br_val[i];

    return SYS_OK;
}

sys_err_t
regs_get_scalar(const struct batch_regs *r, unsigned int i, uint64_t *out)
{
    if (i >= SYS_BATCH_REGS || r->br_tag[i] != REG_SCALAR) {
        return SYS_ERR_INVALID_ARG;
    }

    *out = r->br_val[i];

    return SYS_OK;
}

sys_err_t
regs_peek(const struct batch_regs *r, unsigned int i, uint8_t *tag)
{
    if (i >= SYS_BATCH_REGS) {
        return SYS_ERR_INVALID_ARG;
    }

    *tag = r->br_tag[i];

    return SYS_OK;
}
