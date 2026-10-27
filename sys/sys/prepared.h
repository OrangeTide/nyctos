/* prepared.h : prepared programs, a validated batch held in the kernel.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares struct prepared, which holds a copy of an operation batch validated
 * at load time, and the prepared_load, prepared_exec and prepared_free
 * functions. Included by the objref, object, operation and prepared program
 * sources.
 *
 */

#ifndef _SYS_PREPARED_H_
#define _SYS_PREPARED_H_

#include <sys/config.h>
#include <sys/op.h>
#include <sys/pool.h>

struct proc;

struct prepared {
    sys_id_t        pr_id;
    struct proc    *pr_owner;
    uint32_t        pr_count;
    sys_op_t        pr_ops[SYS_BATCH_MAX_ENTRIES];
};

/*
 * Validate a batch and keep it. The entries are read from the caller's
 * memory once and never again.
 */
sys_err_t prepared_load(struct proc *p, const sys_op_t *entries,
  uint32_t count, struct prepared **out);

/* Run it, without validating what was validated at load. */
long prepared_exec(struct prepared *pr);

void prepared_free(struct prepared *pr);

#endif /* _SYS_PREPARED_H_ */
