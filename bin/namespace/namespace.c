/* namespace.c : prints the namespace it was launched with.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It prints one line for each entry in its namespace, using the name when
 * the entry has one. The exit status is the number of named entries, so a
 * caller can check the result without reading the console. The output
 * depends on what the parent mounted, so the same image prints different
 * tables under different parents.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stdio.h>
#include <stddef.h>

int
main(void)
{
    unsigned int slots = ns_slots();
    unsigned int i;
    int named = 0;

    for (i = 0; i < slots; i++) {
        sys_ns_id_t id;
        sys_handle_t h;
        const char *name;

        ns_slot(i, &id, &h);
        if (id == SYS_NS_NONE) {
            continue;
        }

        name = ns_name(i);
        if (name[0] != '\0') {
            named++;
            printf("namespace: %s is id %u\n", name, (unsigned int)id);
        } else {
            printf("namespace: id %u has no name\n", (unsigned int)id);
        }
    }


    /*
     * And what handing this namespace on would do. Section 7.2
     * refuses a duplication asking for a right the source lacks,
     * and refuses one at all where the right to duplicate was not
     * given, so a composer meets two different walls with one
     * answer behind them. Reported rather than worked around: what
     * a process may pass on is its parent's decision, and a
     * program that could route around it would be a program its
     * parent did not confine.
     */
    {
        sys_ns_spec_t out[SYS_NS_MAX];
        unsigned int n = 0;
        sys_err_t err;

        err = ns_compose(NS_SHARE, NULL, 0,
          SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER, out, SYS_NS_MAX, &n);
        if (err == SYS_OK) {
            printf("namespace: composed %u to hand on\n", n);
            while (n > 0) {
                (void)sys_handle_close(out[--n].handle);
            }
        } else if (err == SYS_ERR_PERM_DENIED) {
            /*
             * Two situations answer the same way and this program
             * cannot tell them apart: an entry that is not the kind
             * of thing those rights describe, and an entry this
             * process may not duplicate at all. Both are section
             * 7.2 refusing to hand on more than is held, so the
             * message says that rather than guessing which.
             */
            printf("namespace: cannot hand these on with those "
              "rights\n");
        } else {
            printf("namespace: composing failed some other way\n");
            return -1;
        }
    }

    if (named == 0) {
        printf("namespace: nothing is mounted anywhere\n");
    }

    return named;
}
