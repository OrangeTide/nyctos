/* path.c : resolves a path to a namespace handle and a relative name.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * ns_resolve_at canonicalizes a path and selects the longest namespace entry
 * whose name is a whole-component prefix of it. It returns that entry's
 * handle and the remainder of the path, and relative paths are joined to the
 * current working directory entry first. The nth entry sharing the winning
 * name is chosen on each call, which lets a caller try every handle in a
 * union directory. ns_resolve is ns_resolve_at with nth set to 0.
 *
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
 * `.`, and no `..` left in it. The result is absolute whatever it was
 * given, because everything here has already been made absolute by the
 * caller.
 *
 * A `..` with nothing above it is the interesting case and it fails
 * rather than being dropped. Dropping it is the tempting mistake and it
 * manufactures authority out of a name: `/../..` would become `/`, which
 * is a path escaping the namespace it was written in and landing on
 * whatever is mounted at the top of it.
 *
 * The two ways to fail are not the same and do not answer the same way.
 * A path that climbs above the top names nothing, which is an ordinary
 * result in a system where a name is not authority. A path too long for
 * this library's buffer is the caller asking for something the library
 * cannot do, which is an error about the call rather than about the
 * namespace, and a caller that cannot tell them apart will report the
 * wrong one.
 */
static sys_err_t
reduce(const char *path, char *out, size_t out_len)
{
    size_t used = 1;

    if (out_len < 2) {
        return SYS_ERR_INVALID_ARG;
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
                return SYS_ERR_NO_ENT;
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
                return SYS_ERR_INVALID_ARG;
            }
            out[used++] = '/';
        }
        if (used + n >= out_len) {
            return SYS_ERR_INVALID_ARG;
        }
        memcpy(&out[used], path, n);
        used += n;
        path += n;
    }

    out[used] = '\0';

    return SYS_OK;
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
ns_resolve_at(const char *path, unsigned int nth, sys_handle_t *out_dir,
  char *rest, size_t rest_len)
{
    char canon[NS_PATH_MAX];
    unsigned int slots = ns_slots();
    unsigned int i, best = 0, seen = 0;
    size_t best_len = 0, best_at = 0;
    int found = 0;
    sys_err_t err;

    if (path == NULL || out_dir == NULL || path[0] == '\0') {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * A relative path is relative to what this process calls its working
     * directory, which is an entry like any other. A process without one
     * cannot write a relative path, and there is no root to fall back on:
     * the ambient root is gone, and falling back to the
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

        err = reduce(joined, canon, sizeof(canon));
    } else {
        err = reduce(path, canon, sizeof(canon));
    }
    if (err != SYS_OK) {
        return err;
    }

    /*
     * The longest name that matches, and then the nth entry carrying
     * that name. Several entries at one name is a union directory: they are
     * tried in the order the parent wrote them,
     * and only entries at the *same* name are in it. An entry with a
     * shorter name is a different mount and not a fallback, because
     * falling through to it would let a name reach a place the parent
     * mounted something else over.
     *
     * Two passes, because the longest match is not known until the table
     * has been walked and the first version stopped at the first entry
     * that matched at all. All the matches for one path are prefixes of
     * it and so are ordered by length, which is why a length is enough
     * to identify which of them are the same mount.
     */
    for (i = 0; i < slots; i++) {
        size_t at, n = strlen(ns_name(i));

        if (!under(canon, ns_name(i), &at)) {
            continue;
        }
        if (!found || n > best_len) {
            found = 1;
            best_len = n;
        }
    }
    if (!found) {
        return SYS_ERR_NO_ENT;
    }

    for (i = 0; i < slots; i++) {
        size_t at, n = strlen(ns_name(i));

        if (n != best_len || !under(canon, ns_name(i), &at)) {
            continue;
        }
        if (seen++ != nth) {
            continue;
        }
        best = i;
        best_at = at;
        found = 2;
        break;
    }

    if (found != 2) {
        return SYS_ERR_NO_ENT;
    }

    ns_slot(best, NULL, out_dir);

    if (rest != NULL) {
        size_t n = strlen(&canon[best_at]);

        if (n + 1 > rest_len) {
            /*
             * Terminated even though it did not fit, because the handle
             * is already written and a caller that checks one and not
             * the other would send whatever was on its stack as a path.
             */
            if (rest_len > 0) {
                rest[0] = '\0';
            }
            return SYS_ERR_TRUNCATED;
        }
        memcpy(rest, &canon[best_at], n + 1);
    }

    return SYS_OK;
}

sys_err_t
ns_resolve(const char *path, sys_handle_t *out_dir, char *rest,
  size_t rest_len)
{
    return ns_resolve_at(path, 0, out_dir, rest, rest_len);
}
