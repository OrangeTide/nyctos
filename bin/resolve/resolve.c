/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The resolver, checked against a table of paths and answers.
 *
 * This is the one thing in the system that can be checked without a
 * machine doing anything: resolution sends no message, asks the kernel
 * nothing, and is a function of a path and the namespace this process
 * was launched with. So it is checked the way a function is, by naming
 * what it should say and comparing.
 *
 * Whoever starts it mounts a root at `/` and something else at `/dev`,
 * with a working directory at `/`. The cases below are written against
 * that and would be wrong under any other namespace, which is the point:
 * the answers are properties of the table, not of the library.
 *
 * The exit status is the number of the first case that failed, or zero.
 */

#include <os/sys.h>
#include <ns.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

/* Which handle a case should land on, in terms this program can check. */
enum where {
    AT_ROOT,
    AT_DEV,
    NOWHERE,
};

struct probe {
    const char     *path;
    enum where      where;
    const char     *rest;
};

static const struct probe probes[] = {
    /* The ordinary cases. */
    { "/motd",              AT_ROOT,    "motd" },
    { "/dev/console",       AT_DEV,     "console" },
    { "/dev",               AT_DEV,     "" },
    { "/",                  AT_ROOT,    "" },

    /*
     * The case section 20.3 says forces the order. Selecting the entry
     * first would match `/dev` and send `../motd` to a server that
     * cannot see the boundary above itself; reducing first sends `motd`
     * to the handle mounted at `/`, which is where the file is.
     */
    { "/dev/../motd",       AT_ROOT,    "motd" },
    { "/dev/../dev/console", AT_DEV,    "console" },

    /* Matching by component and not by character. */
    { "/development/x",     AT_ROOT,    "development/x" },
    { "/dev.old",           AT_ROOT,    "dev.old" },

    /* Noise a path may contain and mean nothing by. */
    { "//dev//console",     AT_DEV,     "console" },
    { "/./motd",            AT_ROOT,    "motd" },
    { "/dev/./console",     AT_DEV,     "console" },
    { "/motd/..",           AT_ROOT,    "" },

    /*
     * Above everything. A `..` with nothing over it fails rather than
     * being dropped: dropping it would let a path escape the namespace
     * it was written in and land on whatever is mounted highest, which
     * is authority manufactured out of a name.
     */
    { "/..",                NOWHERE,    NULL },
    { "/../..",             NOWHERE,    NULL },
    { "/dev/../..",         NOWHERE,    NULL },

    /* Relative, against a working directory of `/`. */
    { "motd",               AT_ROOT,    "motd" },
    { "dev/console",        AT_DEV,     "console" },
    { "../motd",            NOWHERE,    NULL },
};

#define NPROBES (int)(sizeof(probes) / sizeof(probes[0]))


/*
 * Two answers that are not "it named nothing", checked apart from the
 * table above because they are about the call rather than about the
 * namespace. A caller that cannot tell a path it may not use from a path
 * that reaches nothing will report the wrong one to whoever wrote it.
 */
static int
awkward_cases(void)
{
    char rest[NS_PATH_MAX];
    char shout[NS_PATH_MAX * 2];
    sys_handle_t got;
    unsigned int i;

    /* A name that is no name at all. */
    if (ns_resolve("", &got, rest, sizeof(rest)) != SYS_ERR_INVALID_ARG) {
        printf("resolve: the empty path reached something\n");
        return -2;
    }

    /* Longer than the library will hold, which is not "no such name". */
    shout[0] = '/';
    for (i = 1; i < sizeof(shout) - 1; i++) {
        shout[i] = 'x';
    }
    shout[sizeof(shout) - 1] = '\0';
    if (ns_resolve(shout, &got, rest, sizeof(rest)) != SYS_ERR_INVALID_ARG) {
        printf("resolve: an unusable path answered as a missing one\n");
        return -3;
    }

    /*
     * And a remainder with nowhere to go. The handle is resolved by
     * then, so what matters is that the caller is told and that what it
     * would have sent is not left as whatever was on the stack.
     */
    rest[0] = 'z';
    if (ns_resolve("/motd", &got, rest, 2) != SYS_ERR_TRUNCATED ||
      rest[0] != '\0') {
        printf("resolve: a remainder that did not fit was left behind\n");
        return -4;
    }

    return 0;
}

int
main(void)
{
    sys_handle_t root = ns_lookup(SYS_NS_ROOT);
    sys_handle_t dev = ns_lookup(SYS_NS_CONSOLE);
    int i;

    if (root == SYS_HANDLE_INVALID || dev == SYS_HANDLE_INVALID) {
        printf("resolve: launched without the namespace to check\n");
        return -1;
    }

    if (awkward_cases() != 0) {
        return -2;
    }

    for (i = 0; i < NPROBES; i++) {
        const struct probe *p = &probes[i];
        sys_handle_t got = SYS_HANDLE_INVALID;
        char rest[NS_PATH_MAX];
        sys_err_t err;

        err = ns_resolve(p->path, &got, rest, sizeof(rest));

        if (p->where == NOWHERE) {
            if (err == SYS_OK) {
                printf("resolve: %s reached something\n", p->path);
                return i + 1;
            }
            continue;
        }

        if (err != SYS_OK) {
            printf("resolve: %s reached nothing\n", p->path);
            return i + 1;
        }
        if (got != (p->where == AT_ROOT ? root : dev)) {
            printf("resolve: %s landed on the wrong handle\n", p->path);
            return i + 1;
        }
        if (strncmp(rest, p->rest, NS_PATH_MAX) != 0) {
            printf("resolve: %s left '%s', not '%s'\n", p->path, rest,
              p->rest);
            return i + 1;
        }
    }

    return 0;
}
