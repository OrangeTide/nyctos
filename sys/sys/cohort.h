/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Cohorts: a protection domain shared by more than one process, per
 * section 18.
 *
 * A cohort is a set of images that run in one address space. Membership
 * is a property of the build and not of anything a running program can
 * ask for, which is section 18.1: an operation that joined a cohort
 * would be an operation that granted a program the memory of every
 * member, and that is a privilege escalation primitive rather than a
 * convenience.
 *
 * The table lives in this kernel's source because that is where a
 * build-time decision belongs. A member is named by its image, so
 * spawning that image is what places a process in the domain, whether
 * the boot sequence spawns it or a program does.
 *
 * Two members must be two images. Every program is linked for a slot of
 * its own, so two copies of one image would be two processes at one
 * address, and in a shared space the second would land on top of the
 * first. Section 6.1 already says two copies of one image cannot pass
 * buffers to each other; this is the same fact reaching further.
 *
 * At Tier 0 a cohort carries no address space, because there is one
 * address space and every process is already in it. What is left is the
 * membership list, which is what fault reporting reads. A cohort is
 * therefore not a mechanism that Tier 0 lacks. It is a mechanism whose
 * whole content at Tier 0 is a name.
 */

#ifndef _SYS_COHORT_H_
#define _SYS_COHORT_H_

#include <sys/op.h>
#include <sys/vm.h>

#include <stdint.h>

#define SYS_COHORT_MEMBERS_MAX  8

struct proc;

struct cohort {
    const char         *c_name;

    /* The images that make it up, fixed by the build. */
    const char *const  *c_members;
    uint32_t            c_nmembers;

#if SYS_TIER == 2
    /*
     * The one address space, built when the first member is placed in
     * it rather than at boot. A cohort no one spawns costs nothing,
     * and a page table built for it would be a page table spent on a
     * configuration that is not in use.
     */
    struct addrspace    c_as;
#endif

    /*
     * Which process is running as each member, indexed the same way as
     * c_members. NULL is nobody, and a process that has exited leaves
     * its place free.
     *
     * This is what refuses a second copy of a member. Every program is
     * linked for a slot of its own, so loading one twice into a shared
     * space would write the second over the first while the first was
     * running: not two processes at one address, which is what an
     * address space each is for, but one process having its memory
     * replaced under it by a loader.
     */
    struct proc        *c_running[SYS_COHORT_MEMBERS_MAX];

    /* How many members have ever been placed, for the flush. */
    uint32_t            c_placed;
};

/*
 * The cohort this image belongs to, or NULL for an image that is in
 * none. Looked up by name because the name is what the build-time table
 * holds and what a spawn carries.
 */
struct cohort *cohort_of_image(const char *image);

/* What a cohort is called, for a fault message and for the report. */
const char *cohort_name(const struct cohort *c);

/*
 * Take this image's place in the cohort for this process, and at Tier 2
 * build the shared address space if this is the first member in it.
 *
 * SYS_ERR_WOULDBLOCK if a live process is already running as that
 * member. A second copy would be loaded at the address the first is
 * running at, and in a shared space the loader would write over it.
 *
 * That is the same refusal the loader already makes at Tier 0 for every
 * image, and it returns the same error deliberately. One condition
 * should not have two answers depending on which rule noticed it, and
 * the answer is honest either way: the request succeeds once the
 * running one has exited.
 *
 * SYS_ERR_NO_MEMORY if the space has to be built and there is no frame
 * to build it from.
 *
 * Both tiers, because the rule is about the cohort rather than about
 * translation. Tier 0 has nothing to build and still has one place per
 * member.
 */
sys_err_t cohort_place(struct cohort *c, const char *image, struct proc *p);

/*
 * Give the place back, for a spawn that failed after taking it. Not the
 * teardown of section 18.2, which is M7's and tears down every member
 * together; this is the undo of a placement whose process never ran.
 */
void cohort_release(struct cohort *c, struct proc *p);

/*
 * A member faulted, so the domain is gone, per section 18.2. Says which
 * cohort, which member, and what the teardown that does not exist yet
 * would have to do. Does not return.
 */
void cohort_fault_halt(const struct cohort *c, const struct proc *p);

/*
 * Say what the build was configured with, at boot, beside the other
 * object reports. A cohort is a decision made where nobody can see it,
 * so the running system says which images share memory rather than
 * leaving that to a table in a source file.
 */
void cohort_report(void);

#endif /* _SYS_COHORT_H_ */
