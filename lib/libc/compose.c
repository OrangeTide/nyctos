/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Composing a namespace for a child, per section 20.4.
 *
 * The section says composition is trivial and this file is the claim
 * being cashed: `NS_SHARE`, `NS_CLONE` and `NS_NEW` are choices about
 * what array to pass, not mechanisms. There is no kernel involvement
 * because there is nothing for a kernel to do: a parent decides which of
 * the handles it holds to hand over and under what names, and spawn
 * moves them.
 *
 * The handles are duplicated rather than passed, because spawn moves
 * what it is given and a parent that handed over its own root would
 * cease to have one. A caller whose spawn fails owns what this built and
 * has to close it, which is the same rule as everywhere else here: what
 * you hold is what you were given and did not give away.
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

    if (out == NULL || nout == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    *nout = 0;
    memset(out, 0, (size_t)max * sizeof(*out));

    /*
     * Nothing at all, which is not an empty case to be skipped over. A
     * process given no entries can name nothing, per section 20.3, and
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

        if (sys_handle_dup(h, rights, &copy) != SYS_OK) {
            while (n > 0) {
                (void)sys_handle_close(out[--n].handle);
            }
            return SYS_ERR_NO_MEMORY;
        }

        out[n].id = id;
        out[n].handle = copy;
        copy_name(out[n].name, name);
        n++;
    }

    *nout = n;

    return SYS_OK;
}
