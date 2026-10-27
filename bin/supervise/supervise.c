/* supervise.c : a userspace supervisor that restarts a program.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It starts the program named in argv[1], waits for it to exit with no
 * deadline on the wait, and starts it again until the number of starts
 * reaches the bound in argv[2]. The child's exit status is read but not acted
 * on. The exit status of supervise is the number of starts it made, so zero
 * means the first start failed or the arguments were invalid.
 *
 */

#include <os/sys.h>
#include <stddef.h>

static int
digits(const char *s, uint32_t *out)
{
    uint32_t v = 0;

    if (*s == '\0') {
        return -1;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        v = v * 10 + (uint32_t)(*s - '0');
    }
    *out = v;

    return 0;
}

int
main(int argc, char **argv)
{
    uint32_t bound;
    uint32_t started = 0;

    if (argc < 3 || digits(argv[2], &bound) != 0 || bound == 0) {
        return 0;
    }

    while (started < bound) {
        sys_handle_t child, done;
        int32_t code;
        uint32_t state;

        if (sys_proc_spawn(argv[1], 20, NULL, 0, NULL, 0, &child) != SYS_OK) {
            break;
        }
        started++;

        if (sys_object_completion(child, &done) != SYS_OK) {
            break;
        }

        /*
         * Without a deadline. The supervisor waits for its
         * child to finish and has nothing useful to do in the
         * meantime, and a deadline here would be a supervisor deciding
         * a child had failed because it was slow.
         */
        if (sys_fence_wait(done, SYS_COMPLETION_EXITED,
          SYS_DEADLINE_NEVER) != SYS_OK) {
            break;
        }
        /*
         * What it exited with. A real supervisor would decide
         * something from this; this one only has to have asked, so
         * that the wait above is a wait for an answer rather than a
         * wait for a while.
         */
        (void)sys_proc_status(child, &code, &state);

        (void)sys_handle_close(done);
        (void)sys_handle_close(child);
    }

    /*
     * Out of starts. The status says how many were made, so a caller
     * can tell "it kept dying" from "it ran once and stopped".
     */
    return (int)started;
}
