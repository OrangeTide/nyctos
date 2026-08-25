/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

#include <sys/bootarch.h>
#include <sys/cohort.h>
#include <sys/systm.h>
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
static const char *const peers_members[] = {
    "peer_a",
    "peer_b",
};

static struct cohort cohorts[] = {
    {
        .c_name = "peers",
        .c_members = peers_members,
        .c_nmembers = 2,
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

#if SYS_TIER == 2

sys_err_t
cohort_place(struct cohort *c, struct addrspace **out)
{
    if (c->c_placed == 0) {
        sys_err_t err = vm_as_create(&c->c_as);

        if (err != SYS_OK) {
            return err;
        }
    }

    c->c_placed++;
    *out = &c->c_as;

    return SYS_OK;
}

#endif /* SYS_TIER == 2 */

void
cohort_report(void)
{
    uint32_t i, j;

    cnprintf("cohorts: %u, membership fixed by the build\n",
      (unsigned int)NCOHORTS);

    for (i = 0; i < NCOHORTS; i++) {
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
