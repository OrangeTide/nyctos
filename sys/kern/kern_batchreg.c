/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The register file of section 17.4.
 *
 * Small enough that the interesting part is the refusals rather than
 * the reads: what this exists to do is say no to a batch that tries to
 * use a number as a handle, and to a batch that reads a register
 * nothing has written.
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

uint8_t
regs_tag(const struct batch_regs *r, unsigned int i)
{
    if (i >= SYS_BATCH_REGS) {
        return REG_EMPTY;
    }

    return r->br_tag[i];
}
