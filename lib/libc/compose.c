/* compose.c : builds the namespace array passed to a child process.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * ns_compose copies entries from the caller's namespace into an output
 * array, duplicating each handle with the requested rights. With NS_CLONE
 * only the entries whose names appear in the caller's list are copied, with
 * NS_NEW none are, and otherwise every entry is copied. If the array is too
 * small or a duplication fails, the copies made so far are closed and an
 * error is returned.
 *
 */

#include <ns.h>
#include <os/sys.h>

#include <stddef.h>
#include <string.h>

static void
copy_name(char *dst, const char *src)
{
    unsigned int i;

    for (i = 0; i + 1 < SYS_NS_NAME_MAX && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Whether a name is one of the ones the caller asked to keep. */
static int
wanted(const char *name, const char *const *only, unsigned int nonly)
{
    unsigned int i;

    for (i = 0; i < nonly; i++) {
        if (only[i] != NULL && strncmp(name, only[i],
          SYS_NS_NAME_MAX) == 0) {
            return 1;
        }
    }

    return 0;
}

sys_err_t
ns_compose(ns_how_t how, const char *const *only, unsigned int nonly,
  uint32_t rights, sys_ns_spec_t *out, unsigned int max,
  unsigned int *nout)
{
    unsigned int slots = ns_slots();
    unsigned int i, n = 0;
    sys_err_t err;

    if (out == NULL || nout == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    *nout = 0;
    memset(out, 0, (size_t)max * sizeof(*out));

    /*
     * Nothing at all, which is not an empty case to be skipped over. A
     * process given no entries can name nothing, and
     * that is a thing a parent may want to hand out on purpose.
     */
    if (how == NS_NEW) {
        return SYS_OK;
    }

    for (i = 0; i < slots; i++) {
        sys_ns_id_t id;
        sys_handle_t h, copy;
        const char *name;

        ns_slot(i, &id, &h);
        if (id == SYS_NS_NONE || h == SYS_HANDLE_INVALID) {
            continue;
        }

        name = ns_name(i);
        if (how == NS_CLONE && !wanted(name, only, nonly)) {
            continue;
        }

        if (n == max) {
            /*
             * More than the caller made room for. Refused rather than
             * truncated: a child with most of a namespace cannot tell
             * that from a child with all of one, and the difference is
             * what it can reach.
             */
            while (n > 0) {
                (void)sys_handle_close(out[--n].handle);
            }
            return SYS_ERR_TRUNCATED;
        }

        /*
         * A duplication that asks for anything
         * the source lacks is refused rather than clamped, and that is
         * deliberate: a caller asking for more than it holds has
         * made a mistake worth hearing about. It means the rights
         * passed here have to be rights every entry being composed
         * actually carries. A namespace holding two kinds of thing,
         * a connection and a grant say, is composed in two calls.
         *
         * The error says which of the two happened rather than
         * standing in for both. Running out of table is not the
         * same as asking for a right an entry does not have, and
         * the first version reported the second as the first.
         */
        err = sys_handle_dup(h, rights, &copy);
        if (err != SYS_OK) {
            while (n > 0) {
                (void)sys_handle_close(out[--n].handle);
            }
            return err;
        }

        out[n].id = id;
        out[n].handle = copy;
        copy_name(out[n].name, name);
        n++;
    }

    *nout = n;

    return SYS_OK;
}
