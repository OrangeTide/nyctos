/* kern_cohort.c : cohort membership and cohort-wide fault handling.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The storage and peers cohorts are fixed in a compiled-in table, with
 * members named by boot image. An image is a member when its boot archive
 * entry matches a member's entry. A fault in any member ends every member of
 * its cohort, and the cohort functions place, release and report processes.
 *
 */

#include <sys/bootarch.h>
#include <sys/cohort.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/vm.h>

/*
 * The membership table.
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
     * A place a dead process left is free. Nothing is reaped yet,
     * so today this only ever sees NULL or something still running, and
     * the exited case is here because the rule is about what is running
     * and not about what has ever run.
     *
     * Reaping has to clear this pointer when it reclaims a process. Until
     * something reuses a process slot the pointer cannot go stale, and
     * the day reaping arrives it can, so the teardown
     * owns this array as much as spawn does.
     */
    if (c->c_running[j] != NULL && !c->c_running[j]->p_exited) {
        return SYS_ERR_WOULDBLOCK;
    }

#if SYS_TIER == 2
    /*
     * Built when there is none, rather than when nothing has ever been
     * placed. The space goes back when the last
     * member does, so a cohort can be on its second space and later
     * ones, and "has anything ever been placed" is not the same
     * question as "is there a space".
     */
    if (c->c_as.as_root == 0) {
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
    bool found = false;

    for (j = 0; j < c->c_nmembers; j++) {
        if (c->c_running[j] == p) {
            c->c_running[j] = NULL;
            found = true;
            break;
        }
    }

    /*
     * A process may hold no place by the time it is reaped. cohort_place
     * gives a place away as soon as its occupant has exited, so a
     * replacement can already be standing where this one was, and the
     * space then belongs to the replacement.
     */
    if (!found) {
        return;
    }

#if SYS_TIER == 2
    /*
     * And the space itself, once nobody is left in it. Read off the
     * array and not off a count, because the count and the array
     * disagree in exactly the case above: a place handed on before its
     * previous holder was reaped is one increment and no decrement.
     *
     * c_placed goes back to zero with the space, which keeps it meaning
     * what proc_load reads it for, that something has already been
     * placed in the space that exists now.
     */
    for (j = 0; j < c->c_nmembers; j++) {
        if (c->c_running[j] != NULL) {
            return;
        }
    }

    vm_as_destroy(&c->c_as);
    c->c_placed = 0;
#endif
}

bool
cohort_holds(const struct cohort *c, const struct proc *p)
{
    uint32_t j;

    for (j = 0; j < c->c_nmembers; j++) {
        if (c->c_running[j] == p) {
            return true;
        }
    }

    return false;
}

/*
 * What every cohort's address space is holding, .
 *
 * Counted here rather than against the members, because a cohort's
 * space is what a cohort is: one space for a set of images, built when
 * the first of them is placed and returned when the last of them is
 * reaped. Charging it to a member would count it once per member and
 * would charge it to nobody in the stretch where a space exists with
 * every place empty.
 */
uint32_t
cohort_frames(void)
{
#if SYS_TIER == 2
    uint32_t total = 0;
    uint32_t i;

    for (i = 0; i < NCOHORTS; i++) {
        total += md_as_frames(&cohorts[i].c_as);
    }

    return total;
#else
    return 0;
#endif
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
    struct cohort *self = (struct cohort *)c;
    uint32_t i;

    /*
     * The cohort is the recovery unit, and per-process
     * recovery is unsound for a member. The members share a protection
     * domain, so one that faults may have written anything into any
     * peer before it did, and nothing in the system can establish that
     * it did not. Restarting only the faulting member would leave the
     * survivors in a state no one can vouch for.
     *
     * So every member goes, including the ones that did nothing wrong,
     * and that is the whole content of the rule rather than a side
     * effect of it.
     */
    cnprintf("cohort %s: member %s faulted, %s at 0x%lx, pc 0x%lx\n",
      c->c_name, who, fault_name(kind), (unsigned long)addr,
      (unsigned long)pc);

    for (i = 0; i < c->c_nmembers; i++) {
        struct proc *m = self->c_running[i];

        if (m == NULL || m->p_exited) {
            continue;
        }

        /*
         * Ended exactly as a supervisor would end it: every
         * thread unblocked as if cancelled, every server holding one
         * of their requests told, and every client blocked on a
         * channel they answered released rather than left waiting.
         * None of that is special to a cohort; what is special is
         * that it happens to a member that did nothing.
         */
        cnprintf("    %s goes with it\n", m->p_name);
        (void)proc_kill(m, SYS_EXIT_COHORT);
    }

    /*
     * And what does not happen here. The places in the table are given
     * back as each member is reaped, so a restart can take them, but
     * nothing restarts anything: restart policy is kept out
     * of the kernel, so what happens next is a supervisor's decision
     * and this has only made it possible.
     */
}
