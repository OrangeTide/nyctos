/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A shell: read a line, start what it names, wait for it, say how it went.
 *
 * That is the whole loop and it is the milestone's goal. Everything under
 * it was built for this: a console to read from, an operation to spawn
 * with, a completion timeline to wait on and a status to read afterwards.
 *
 * What a child is given is what this hands it, and nothing else. A
 * duplicate of the console, so it can talk to the person; a duplicate of
 * whatever this shell has as a root, so it can reach files; and its
 * arguments. Section 7.4 makes a handle move rather than copy, so the
 * duplicate is what leaves and the original stays, and section 20.1 is why
 * a program started here can reach nothing this shell did not name.
 *
 * It blocks on reading a line and it blocks on joining a child, one after
 * the other and never both at once, which is the blocking invariant of
 * section 5 holding without anything having to arrange it. The first shell
 * that wants a keystroke while a child runs is the one that needs a second
 * thread.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LINE_BYTES  128

static char line[LINE_BYTES];
static char *word[SYS_MAX_ARGS];

/*
 * Split a line into words in place, and refuse a line with more words
 * than a command can carry. Silently merging the ninth word into the
 * eighth is the worst of the three options: the line looks accepted,
 * and echo hides it by joining its arguments with the spaces that were
 * lost, while any other program receives one argument that is three.
 *
 * Everything else a shell does to a line
 * is quoting, expansion and substitution, and none of it exists here.
 */
static int
split(char *s, char **out, int max)
{
    int n = 0;

    for (;;) {
        while (*s == ' ' || *s == '\t') {
            *s++ = '\0';
        }
        if (*s == '\0') {
            break;
        }
        if (n == max) {
            return -1;      /* more words than a command can carry */
        }
        out[n++] = s;
        while (*s != '\0' && *s != ' ' && *s != '\t') {
            s++;
        }
    }

    return n;
}


/*
 * The name of a fault kind. Section 16.1's list, in the words a person
 * reading a shell reads.
 */
static const char *
fault_name(uint32_t kind)
{
    switch (kind) {
    case SYS_FAULT_PAGE:        return "page";
    case SYS_FAULT_ACCESS:      return "access";
    case SYS_FAULT_ALIGN:       return "alignment";
    case SYS_FAULT_INSTRUCTION: return "instruction";
    case SYS_FAULT_ARITH:       return "arithmetic";
    case SYS_FAULT_DEBUG:       return "debug";
    case SYS_FAULT_EXTERNAL:    return "external abort";
    default:                    return "unknown";
    }
}



static sys_err_t
run(int argc, char **argv, int32_t *status)
{
    sys_ns_spec_t ns[SYS_NS_MAX];
    const char *only[1];
    ns_how_t how = NS_SHARE;
    sys_handle_t child, done;
    unsigned int nns = 0;
    uint32_t state;
    sys_err_t err;

    /*
     * Two words in front of a command choose what it is launched with.
     * Section 20.4 says composition is a choice about what array to
     * pass, and this is a shell making that choice out loud: `alone`
     * hands over nothing, `only <name>` hands over one entry, and a bare
     * command hands over everything this shell holds.
     *
     * They are not builtins in any deeper sense. Nothing in the kernel
     * knows they exist and nothing in the library does either; they
     * decide which of three arrays is built.
     */
    if (argc >= 2 && strncmp(argv[0], "alone", 6) == 0) {
        how = NS_NEW;
        argv++;
        argc--;
    } else if (argc >= 3 && strncmp(argv[0], "only", 5) == 0) {
        how = NS_CLONE;
        only[0] = argv[1];
        argv += 2;
        argc -= 2;
    }

    /*
     * Attenuated on the way down, per section 7: a child may send on
     * these connections and hand them to its own children, and may
     * not mint another. What it cannot do is duplicate one, which
     * means it cannot compose a namespace of its own from them.
     */
    err = ns_compose(how, only, 1, SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER,
      ns, SYS_NS_MAX, &nns);
    if (err != SYS_OK) {
        return err;
    }

    err = sys_proc_spawn(argv[0], SYS_PRIO_INHERIT, ns, nns,
      (const char *const *)argv, (uint32_t)argc, &child);
    if (err != SYS_OK) {
        /*
         * The duplicates were not moved, because nothing was built. They
         * would otherwise accumulate one per failed command until the
         * table filled, which is a shell that stops working after sixty
         * typing mistakes.
         */
        while (nns > 0) {
            (void)sys_handle_close(ns[--nns].handle);
        }
        return err;
    }

    if (sys_object_completion(child, &done) != SYS_OK) {
        (void)sys_handle_close(child);
        return SYS_ERR_NO_MEMORY;
    }
    if (sys_fence_wait(done, SYS_COMPLETION_EXITED,
      SYS_DEADLINE_NEVER) != SYS_OK) {
        (void)sys_handle_close(done);
        (void)sys_handle_close(child);
        return SYS_ERR_INTERRUPTED;
    }
    if (sys_proc_status(child, status, &state) != SYS_OK) {
        (void)sys_handle_close(done);
        (void)sys_handle_close(child);
        return SYS_ERR_STALE;
    }

    /*
     * A program the machine stopped, per section 16.2. The status alone
     * says a number a program never returns, which is true and useless;
     * what a person needs is which fault, where, and on what
     * instruction. Printed here rather than left to the caller because
     * the caller has the number and this has the reason.
     */
    {
        sys_fault_t fault;

        if (sys_proc_fault(child, &fault) == SYS_OK &&
          fault.kind != SYS_FAULT_NONE) {
            printf("sh: %s: %s fault at 0x%x, pc 0x%x\n", argv[0],
              fault_name(fault.kind), (unsigned int)fault.addr,
              (unsigned int)fault.pc);
        }
    }
    (void)sys_handle_close(done);
    (void)sys_handle_close(child);

    return SYS_OK;
}

int
main(void)
{
    int32_t status = 0;
    int argc;
    int n;

    printf("nyctos shell. exit leaves, last reports a status.\n");

    for (;;) {
        printf("$ ");

        n = readline(line, sizeof(line));
        if (n < 0) {
            printf("sh: the console is gone\n");
            return -1;
        }

        /*
         * A line that filled the buffer did not end. What is left of
         * it arrives as further reads, and running those as commands
         * is what this did before: one long line became six, five of
         * them nonsense that the shell reported as if a person had
         * typed them.
         */
        if (n == (int)sizeof(line) - 1) {
            while (n == (int)sizeof(line) - 1) {
                n = readline(line, sizeof(line));
                if (n < 0) {
                    return -1;
                }
            }
            printf("sh: line too long\n");
            continue;
        }

        argc = split(line, word, SYS_MAX_ARGS);
        if (argc < 0) {
            printf("sh: too many arguments\n");
            continue;
        }
        if (argc == 0) {
            continue;
        }

        if (strncmp(word[0], "exit", 5) == 0) {
            return 0;
        }
        if (strncmp(word[0], "last", 5) == 0) {
            printf("%d\n", status);
            continue;
        }

        if (run(argc, word, &status) != SYS_OK) {
            printf("sh: %s: cannot run it\n", word[0]);
            status = -1;
            continue;
        }
        if (status != 0) {
            printf("[%d]\n", status);
        }
    }
}
