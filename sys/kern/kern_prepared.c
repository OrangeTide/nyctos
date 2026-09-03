/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

#include <sys/cpu.h>
#include <sys/measure.h>
#include <sys/object.h>
#include <sys/prepared.h>
#include <sys/proc.h>
#include <sys/systm.h>


sys_err_t
prepared_load(struct proc *p, const sys_op_t *entries, uint32_t count,
  struct prepared **out)
{
    struct prepared *pr;
    sys_id_t id;
    uint32_t i;
    sys_err_t err;

    if (count == 0 || count > SYS_BATCH_MAX_ENTRIES) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * The same validation a submission gets, done here so that it does
     * not have to be done again. Counted the same way, because the
     * question this stage asks is how many checks N executions cost
     * and the answer has to include the ones the loading paid for.
     */
    cpu_self()->cp_checks++;
    if (!proc_reaches(p, (uintptr_t)entries, count * sizeof(*entries))) {
        return SYS_ERR_FAULT;
    }

    for (i = 0; i < count; i++) {
        cpu_self()->cp_checks++;

        err = entry_static_check(p, &entries[i]);
        if (err != SYS_OK) {
            return err;
        }
    }

    pr = pool_alloc(prepared_pool(), &id);
    if (pr == NULL) {
        return SYS_ERR_NO_MEMORY;
    }

    pr->pr_id = id;
    pr->pr_owner = p;
    pr->pr_count = count;

    /*
     * Copied, which is the whole of why the validation lasts. Left in
     * the caller's memory it could be rewritten between executions and
     * every execution would have to check it again.
     */
    memcpy(pr->pr_ops, entries, count * sizeof(*entries));

    *out = pr;

    return SYS_OK;
}

long
prepared_exec(struct prepared *pr)
{
    /*
     * By whoever loaded it and nobody else. The entries name addresses
     * in the loader's memory, and a handle can be moved: executing in
     * another process would re-check that those addresses are
     * reachable there, so it is safe, and they would mean something
     * else, so it is wrong. Refusing is the answer that needs no
     * argument.
     */
    if (pr->pr_owner != proc_current()) {
        return SYS_ERR_PERM_DENIED;
    }

    return batch_run(pr->pr_ops, pr->pr_count);
}

void
prepared_free(struct prepared *pr)
{
    (void)pool_free(prepared_pool(), pr->pr_id);
}
