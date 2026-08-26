/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

#include <sys/bootarch.h>
#include <sys/cohort.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/vm.h>

/*
 * The membership table, which is the whole of section 18.1.
 *
 * Editing this file and rebuilding is how a cohort changes, and there is
 * deliberately no other way. The alternative was an operation that took
 * a name, and the reason not to have one is not that it would be hard:
 * a process that could ask to join would be a process that could ask for
 * every member's memory.
 */
static const char *const storage_members[] = {
    "fs_server",
    "blk_server",
};

static const char *const peers_members[] = {
    "peer_a",
    "peer_b",
    "peer_c",
};

static struct cohort cohorts[] = {
    {
        .c_name = "storage",
        .c_members = storage_members,
        .c_nmembers = 2,
    },
    {
        .c_name = "peers",
        .c_members = peers_members,
        .c_nmembers = 3,
    },
};

#define NCOHORTS (sizeof(cohorts) / sizeof(cohorts[0]))

/*
 * Membership is decided by the archive lookup rather than by a string
 * compare of its own. The kernel has no strcmp, and it should not grow
 * one for this: the question here is whether a spawn of this image is a
 * spawn of that member, and the only comparison entitled to answer it is
 * the one the loader will use a moment later. Two rules for one question
 * is how a member ends up almost in a cohort.
 */
struct cohort *
cohort_of_image(const char *image)
{
    const struct boot_member *m;
    uint32_t i, j;

    if (image == NULL) {
        return NULL;
    }

    m = boot_lookup(image);
    if (m == NULL) {
        return NULL;
    }

    for (i = 0; i < NCOHORTS; i++) {
        for (j = 0; j < cohorts[i].c_nmembers; j++) {
            if (boot_lookup(cohorts[i].c_members[j]) == m) {
                return &cohorts[i];
            }
        }
    }

    return NULL;
}

const char *
cohort_name(const struct cohort *c)
{
    return c == NULL ? "none" : c->c_name;
}

/*
 * Which member of this cohort an image is, or c_nmembers if it is not
 * one. Through boot_lookup for the reason cohort_of_image gives.
 */
static uint32_t
member_index(const struct cohort *c, const char *image)
{
    const struct boot_member *m = boot_lookup(image);
    uint32_t j;

    for (j = 0; m != NULL && j < c->c_nmembers; j++) {
        if (boot_lookup(c->c_members[j]) == m) {
            return j;
        }
    }

    return c->c_nmembers;
}

sys_err_t
cohort_place(struct cohort *c, const char *image, struct proc *p)
{
    uint32_t j = member_index(c, image);

    if (j >= c->c_nmembers) {
        panic("cohort %s: %s is a member and then is not", c->c_name,
          image);
    }

    /*
     * A place a dead process left is free. Nothing is reaped before M7,
     * so today this only ever sees NULL or something still running, and
     * the exited case is here because the rule is about what is running
     * and not about what has ever run.
     *
     * M7 has to clear this pointer when it reclaims a process. Until
     * something reuses a process slot the pointer cannot go stale, and
     * the day reaping arrives it can, so the teardown of section 18.2
     * owns this array as much as spawn does.
     */
    if (c->c_running[j] != NULL && !c->c_running[j]->p_exited) {
        return SYS_ERR_WOULDBLOCK;
    }

#if SYS_TIER == 2
    if (c->c_placed == 0) {
        sys_err_t err = vm_as_create(&c->c_as);

        if (err != SYS_OK) {
            return err;
        }
    }
#endif

    c->c_running[j] = p;
    c->c_placed++;

    return SYS_OK;
}

void
cohort_release(struct cohort *c, struct proc *p)
{
    uint32_t j;

    for (j = 0; j < c->c_nmembers; j++) {
        if (c->c_running[j] == p) {
            c->c_running[j] = NULL;
            return;
        }
    }
}

void
cohort_report(void)
{
    uint32_t i, j;

    cnprintf("cohorts: %u, membership fixed by the build\n",
      (unsigned int)NCOHORTS);

    for (i = 0; i < NCOHORTS; i++) {
        if (cohorts[i].c_nmembers > SYS_COHORT_MEMBERS_MAX) {
            panic("cohort %s: %u members, and there is room for %u",
              cohorts[i].c_name, (unsigned int)cohorts[i].c_nmembers,
              (unsigned int)SYS_COHORT_MEMBERS_MAX);
        }
        cnprintf("    %s:", cohorts[i].c_name);
        for (j = 0; j < cohorts[i].c_nmembers; j++) {
            cnprintf(" %s", cohorts[i].c_members[j]);
        }
#if SYS_TIER == 2
        cnputs(", one address space between them\n");
#else
        cnputs(", a name only: one address space holds everything\n");
#endif
    }

    /*
     * A member named here that no image answers to is a table nobody
     * checked, and what it produces later is a process quietly running
     * alone in a cohort it believes it shares. Two members linked for
     * one address is worse: in a shared space the second would land on
     * top of the first, and the first would find its memory rewritten
     * by a loader rather than by a peer. Both are build mistakes, so
     * both stop the boot rather than being discovered by a program.
     */
    for (i = 0; i < NCOHORTS; i++) {
        for (j = 0; j < cohorts[i].c_nmembers; j++) {
            const struct boot_member *m;
            uint32_t k;

            m = boot_lookup(cohorts[i].c_members[j]);
            if (m == NULL) {
                panic("cohort %s: no image called %s", cohorts[i].c_name,
                  cohorts[i].c_members[j]);
            }

            for (k = 0; k < j; k++) {
                const struct boot_member *o;

                o = boot_lookup(cohorts[i].c_members[k]);
                if (o->bm_load == m->bm_load) {
                    panic("cohort %s: %s and %s are linked for 0x%lx",
                      cohorts[i].c_name, cohorts[i].c_members[k],
                      cohorts[i].c_members[j],
                      (unsigned long)m->bm_load);
                }
            }
        }
    }
}

void
cohort_fault_halt(const struct cohort *c, const char *who, uint32_t kind,
  uintptr_t addr, uintptr_t pc)
{
    uint32_t i;

    /*
     * Section 18.2: the cohort is the recovery unit, and per-process
     * recovery is unsound for a member. The members share a protection
     * domain, so one that faults may have written anything into any
     * peer before it did, and nothing in the system can establish that
     * it did not. The survivors are in a state no one can vouch for.
     *
     * So this halts, and it halts even when something was waiting for
     * the process. A supervisor holding a member's handle can be told
     * that member died; what it cannot do is anything useful about it,
     * because putting the domain right means tearing down every member
     * together and there is nothing here that can. Continuing would
     * mean running peers that may already be wrong, which is the one
     * outcome section 18.2 rules out.
     *
     * The message says what is missing rather than leaving it to a
     * document. What M7 has to build is in section 18.2 and 12.5:
     * revoke every handle every member holds, force-signal their
     * timelines in error state, release clients blocked sending to
     * them with SYS_ERR_OWNER_DEAD, and deliver SYS_PULSE_DEAD_OWNER
     * to every channel with an outstanding rendezvous. Then a
     * supervisor relaunches the cohort, per section 18.3.
     */
    cnprintf("cohort %s: member %s faulted, %s at 0x%lx, pc 0x%lx\n",
      c->c_name, who, fault_name(kind), (unsigned long)addr,
      (unsigned long)pc);

    cnputs("  the whole cohort is the recovery unit, per section 18.2:");
    for (i = 0; i < c->c_nmembers; i++) {
        cnprintf(" %s", c->c_members[i]);
    }
    cnputs("\n");

    cnputs("  tearing them down together and restarting them is M7, "
      "and is not built\n");

    panic("cohort %s cannot be recovered and must not be left running",
      c->c_name);
}
