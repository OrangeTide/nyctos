/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A program that starts a program, waits for it, and reads how it went.
 *
 * Everything here was the kernel's to do until now. Milestone 2 loaded
 * programs from the boot archive by calling into the loader directly, so
 * nothing outside the kernel had ever created a process, and the shell of
 * stage 7 is the first thing that has to. What this checks is not that a
 * child runs, which milestone 2 settled, but that the operation refuses
 * what it should refuse and leaves the caller intact when it does.
 *
 * It reports by exiting: the child's status if everything held, and a
 * distinct negative code otherwise, with a line on the debug console
 * saying which. There is no formatting in the C library yet, which is
 * stage 6, so a number reaches the outside as a status and not as text.
 */

#include <os/sys.h>
#include <debug.h>
#include <stddef.h>

#define ERR_SPAWN       -1
#define ERR_JOIN        -2
#define ERR_STATUS      -3
#define ERR_STALE       -4
#define ERR_TRANSFER    -5
#define ERR_NOENT       -6
#define ERR_PRIO        -7
#define ERR_LIVE        -8
#define ERR_AGAIN       -9

/* Wait for a child to finish and read what it exited with. */
static int
join(sys_handle_t child, int32_t *out_code)
{
    sys_handle_t done;
    uint32_t state;

    if (sys_object_completion(child, &done) != SYS_OK) {
        return ERR_JOIN;
    }
    if (sys_fence_wait(done, SYS_COMPLETION_EXITED,
      SYS_DEADLINE_NEVER) != SYS_OK) {
        return ERR_JOIN;
    }
    if (sys_proc_status(child, out_code, &state) != SYS_OK) {
        return ERR_STATUS;
    }
    if (state != SYS_STATE_EXITED) {
        return ERR_STATUS;
    }

    (void)sys_handle_close(done);

    return 0;
}

/*
 * Start the named image alongside this program and wait for it. The
 * priority is the caller's own, which is what a parent starting a helper
 * wants and what a program can ask for without knowing its own number.
 */
static int
run(const char *image, int32_t *out_code, sys_handle_t *out_proc)
{
    sys_handle_t child;
    int err;

    if (sys_proc_spawn(image, SYS_PRIO_INHERIT, NULL, 0, &child) != SYS_OK) {
        return ERR_SPAWN;
    }

    err = join(child, out_code);
    if (err != 0) {
        return err;
    }

    if (out_proc != NULL) {
        *out_proc = child;
    } else {
        (void)sys_handle_close(child);
    }

    return 0;
}

int
main(void)
{
    sys_ns_spec_t ns[1];
    sys_handle_t child, busy;
    int32_t code = 0;
    int err;

    DBG_PUTS("spawner: starting a program");
    err = run("status", &code, &child);
    if (err != 0) {
        return err;
    }

    /*
     * A handle the caller does not hold. The point is not the error but
     * what is left behind: nothing, so the spawn that follows must still
     * work.
     */
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = (sys_handle_t)0x1234;
    if (sys_proc_spawn("status", SYS_PRIO_INHERIT, ns, 1, NULL)
      != SYS_ERR_STALE) {
        DBG_PUTS("spawner: a handle it does not hold was accepted");
        return ERR_STALE;
    }

    /*
     * A handle it does hold and may not transfer. A process handle carries
     * the right to wait and the right to ask, and section 7.4 makes moving
     * one a right of its own that this was not given.
     */
    ns[0].handle = child;
    if (sys_proc_spawn("status", SYS_PRIO_INHERIT, ns, 1, NULL)
      != SYS_ERR_PERM_DENIED) {
        DBG_PUTS("spawner: a handle it may not transfer was accepted");
        return ERR_TRANSFER;
    }

    if (sys_proc_spawn("nosuch", SYS_PRIO_INHERIT, NULL, 0, NULL)
      != SYS_ERR_NO_ENT) {
        DBG_PUTS("spawner: an image that does not exist was accepted");
        return ERR_NOENT;
    }

    /*
     * Above its own priority. A program cannot hand out time it was not
     * given, and with no quantum anywhere a child that outranks its parent
     * owns the core until it stops.
     */
    if (sys_proc_spawn("status", 31, NULL, 0, NULL) != SYS_ERR_PERM_DENIED) {
        DBG_PUTS("spawner: a child above its own priority was accepted");
        return ERR_PRIO;
    }

    /*
     * An image that is already running. There is one address per image and
     * no MMU, so a second copy is loaded over the first while it runs. The
     * child is spawned and not yet joined here, which is the only state in
     * which the question can be asked: it is runnable and has not started,
     * because it is at this program's own priority and equal priority does
     * not preempt.
     */
    if (sys_proc_spawn("spin", SYS_PRIO_INHERIT, NULL, 0, &busy) != SYS_OK) {
        return ERR_SPAWN;
    }
    if (sys_proc_spawn("spin", SYS_PRIO_INHERIT, NULL, 0, NULL)
      != SYS_ERR_WOULDBLOCK) {
        DBG_PUTS("spawner: a second copy of a live image was accepted");
        return ERR_LIVE;
    }
    err = join(busy, &code);
    if (err != 0) {
        return err;
    }
    (void)sys_handle_close(busy);

    /*
     * The same image again, now that the first has exited, and the one
     * from the start of this program. Five refusals mean nothing if a
     * spawn that failed took a slot, a handle or an address with it.
     */
    if (sys_proc_spawn("spin", SYS_PRIO_INHERIT, NULL, 0, &busy) != SYS_OK) {
        DBG_PUTS("spawner: the address stayed busy after the child exited");
        return ERR_LIVE;
    }
    err = join(busy, &code);
    if (err != 0) {
        return err;
    }

    DBG_PUTS("spawner: five refusals, then the same spawns again");
    err = run("status", &code, NULL);
    if (err != 0) {
        return ERR_AGAIN;
    }

    return code;
}
