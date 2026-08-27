/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A supervisor, in userspace, because the kernel holds no restart
 * policy and section 18.3 says it must not.
 *
 * The mechanism the kernel provides is the one from section 10.4: wait
 * on a child's completion timeline and start another. Everything else
 * here is policy, and this program is one policy rather than the
 * policy: how many restarts before giving up, and what giving up means,
 * are exactly the things section 18.3 says vary per deployment and do
 * not belong in a kernel trying to stay small enough to analyse.
 *
 * The bound is the interesting part. A child that dies immediately
 * every time is a configuration mistake, and a supervisor that
 * relaunched it for ever would turn a mistake into a system that looks
 * busy and never works. So the count is finite and running out is
 * reported rather than being the same silence as success.
 *
 * Its exit status is how many starts it made. Zero means it could not
 * start the child at all.
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
         * Without a deadline. Section 24.3's supervisor waits for its
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
