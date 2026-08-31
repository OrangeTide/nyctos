/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Prepared programs, per section 17.7: a batch validated once and
 * executed many times.
 *
 * The entries are copied into the kernel, which is what makes the
 * validation last. A program whose entries stayed in the caller's
 * memory would have to be checked again at every execution, because the
 * caller could have rewritten them, and then nothing would have been
 * validated once.
 *
 * What cannot be validated once is the handles. A handle is a slot in a
 * table and what it names can change or go away between one execution
 * and the next, so every execution looks them up again. The saving is
 * the static half: the array is kernel memory and needs no reachability
 * check, and the opcodes were checked when the program was loaded.
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
