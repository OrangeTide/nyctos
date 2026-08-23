/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Resolving a path against the namespace a process was given.
 *
 * This is the whole of what section 20 puts in the C library, and the
 * thing to notice is what it does not do: it sends no message, holds no
 * cache, and asks the kernel nothing. A path is reduced to canonical
 * form, matched against the table of named handles the process was
 * launched with, and what comes out is one of those handles and the part
 * of the path that is relative to it. Section 20.2's claim that
 * resolution costs no syscall is this file being sixty lines of string
 * handling.
 *
 * The order is forced, per section 20.3. Dot-dot is reduced lexically
 * *before* an entry is selected, because a process holding `/` and `/dev`
 * from two different servers and opening `/dev/../motd` must reach the
 * first: matching `/dev` first would send `../motd` to a server that
 * cannot see the boundary above it and can only refuse.
 *
 * Confinement does not depend on any of this being right. A canonical
 * path that lands outside every entry matches no handle, so there is
 * nothing to send a message to, and a program that skips the library
 * gains nothing because it still holds only the handles it was given.
 * What the library owes is correctness, not safety.
 */

#include <ns.h>

#include <stddef.h>
#include <string.h>

/*
 * One component of a path, or the end of it. Returns where the next one
 * starts, having written the length of this one.
 */
static const char *
component(const char *p, size_t *len)
{
    size_t n = 0;

    while (*p == '/') {
        p++;
    }
    while (p[n] != '\0' && p[n] != '/') {
        n++;
    }
    *len = n;

    return p;
}

/*
 * Reduce a path to canonical form: leading slash, single separators, no
 * `.`, and no `..` left in it.
 *
 * A `..` with nothing above it is the interesting case and it fails
 * rather than being dropped. Dropping it is the tempting mistake and it
 * manufactures authority out of a name: `/../..` would become `/`, which
 * is a path escaping the namespace it was written in and landing on
 * whatever is mounted at the top of it.
 */
static int
reduce(const char *path, char *out, size_t out_len)
{
    size_t used = 1;

    if (out_len < 2) {
        return -1;
    }
    out[0] = '/';

    for (;;) {
        size_t n;

        path = component(path, &n);
        if (n == 0) {
            break;
        }

        if (n == 1 && path[0] == '.') {
            path += n;
            continue;
        }

        if (n == 2 && path[0] == '.' && path[1] == '.') {
            /* Back up over the last component, or fail at the top. */
            if (used == 1) {
                return -1;
            }
            used--;                     /* the separator before it */
            while (used > 1 && out[used - 1] != '/') {
                used--;
            }
            if (used > 1) {
                used--;
            }
            path += n;
            continue;
        }

        if (used > 1) {
            if (used + 1 >= out_len) {
                return -1;
            }
            out[used++] = '/';
        }
        if (used + n >= out_len) {
            return -1;
        }
        memcpy(&out[used], path, n);
        used += n;
        path += n;
    }

    out[used] = '\0';

    return 0;
}

/*
 * Whether a canonical path is inside a mount point, and where the part
 * below it starts.
 *
 * The comparison is by component and not by character, which is the
 * whole of the difference between a table holding `/dev` matching
 * `/dev/console` and the same table matching `/development`.
 */
static int
under(const char *path, const char *mount, size_t *rest_at)
{
    size_t n = strlen(mount);

    if (n == 0 || strncmp(path, mount, n) != 0) {
        return 0;
    }

    /* A mount of `/` is above everything and ends in its own separator. */
    if (n == 1 && mount[0] == '/') {
        *rest_at = 1;
        return 1;
    }

    if (path[n] == '\0') {
        *rest_at = n;
        return 1;
    }
    if (path[n] == '/') {
        *rest_at = n + 1;
        return 1;
    }

    return 0;
}

sys_err_t
ns_resolve(const char *path, sys_handle_t *out_dir, char *rest,
  size_t rest_len)
{
    char canon[NS_PATH_MAX];
    unsigned int slots = ns_slots();
    unsigned int i, best = 0;
    size_t best_len = 0, best_at = 0;
    int found = 0;

    if (path == NULL || out_dir == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * A relative path is relative to what this process calls its working
     * directory, which is an entry like any other. A process without one
     * cannot write a relative path, and there is no root to fall back on:
     * section 20.3 removed the ambient root, and falling back to the
     * shortest entry would put it back.
     */
    if (path[0] != '/') {
        const char *cwd = ns_name_of(SYS_NS_CWD);
        char joined[NS_PATH_MAX];
        size_t n;

        if (cwd[0] == '\0') {
            return SYS_ERR_NO_ENT;
        }
        n = strlen(cwd);
        if (n + 1 + strlen(path) + 1 > sizeof(joined)) {
            return SYS_ERR_INVALID_ARG;
        }
        memcpy(joined, cwd, n);
        joined[n] = '/';
        memcpy(&joined[n + 1], path, strlen(path) + 1);

        if (reduce(joined, canon, sizeof(canon)) != 0) {
            return SYS_ERR_NO_ENT;
        }
    } else if (reduce(path, canon, sizeof(canon)) != 0) {
        return SYS_ERR_NO_ENT;
    }

    for (i = 0; i < slots; i++) {
        const char *mount = ns_name(i);
        size_t at, n = strlen(mount);

        /*
         * Strictly longer wins, so two entries at the same name
         * keep the order the parent wrote them in. That order is
         * what a union directory means, and picking the last would
         * make the answer depend on where in the table a name
         * happens to sit.
         */
        if ((found && n <= best_len) || !under(canon, mount, &at)) {
            continue;
        }
        best = i;
        best_len = n;
        best_at = at;
        found = 1;
    }

    if (!found) {
        return SYS_ERR_NO_ENT;
    }

    ns_slot(best, NULL, out_dir);

    if (rest != NULL) {
        size_t n = strlen(&canon[best_at]);

        if (n + 1 > rest_len) {
            return SYS_ERR_TRUNCATED;
        }
        memcpy(rest, &canon[best_at], n + 1);
    }

    return SYS_OK;
}
