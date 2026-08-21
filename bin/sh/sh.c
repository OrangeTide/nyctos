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
 * Split a line into words in place. Everything else a shell does to a line
 * is quoting, expansion and substitution, and none of it exists here.
 */
static int
split(char *s, char **out, int max)
{
    int n = 0;

    while (*s != '\0' && n < max) {
        while (*s == ' ' || *s == '\t') {
            *s++ = '\0';
        }
        if (*s == '\0') {
            break;
        }
        out[n++] = s;
        while (*s != '\0' && *s != ' ' && *s != '\t') {
            s++;
        }
    }

    return n;
}

/*
 * Hand a child a copy of one of this shell's handles, attenuated to what a
 * child needs: it may send on the connection and hand it on, and it may
 * not mint another or duplicate it further than that.
 */
static sys_handle_t
share(sys_handle_t h)
{
    sys_handle_t copy;

    if (h == SYS_HANDLE_INVALID) {
        return SYS_HANDLE_INVALID;
    }
    if (sys_handle_dup(h, SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER,
      &copy) != SYS_OK) {
        return SYS_HANDLE_INVALID;
    }

    return copy;
}

static sys_err_t
run(int argc, char **argv, int32_t *status)
{
    sys_ns_spec_t ns[2];
    sys_handle_t child, done;
    uint32_t nns = 0;
    uint32_t state;
    sys_err_t err;

    ns[nns].id = SYS_NS_CONSOLE;
    ns[nns].handle = share(ns_lookup(SYS_NS_CONSOLE));
    if (ns[nns].handle != SYS_HANDLE_INVALID) {
        nns++;
    }
    ns[nns].id = SYS_NS_ROOT;
    ns[nns].handle = share(ns_lookup(SYS_NS_ROOT));
    if (ns[nns].handle != SYS_HANDLE_INVALID) {
        nns++;
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
        return SYS_ERR_NO_MEMORY;
    }
    if (sys_fence_wait(done, SYS_COMPLETION_EXITED,
      SYS_DEADLINE_NEVER) != SYS_OK) {
        return SYS_ERR_INTERRUPTED;
    }
    if (sys_proc_status(child, status, &state) != SYS_OK) {
        return SYS_ERR_STALE;
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

        argc = split(line, word, SYS_MAX_ARGS);
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
