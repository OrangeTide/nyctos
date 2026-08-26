/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * What two cohort members say to each other, per section 18.
 *
 * This is not a service and nothing but the cohort check uses it. It is
 * here rather than in one of the two programs because both of them have
 * to agree on it, and a structure written out twice is a structure that
 * will disagree with itself eventually.
 *
 * The whole protocol is an address and a value. That is the point: what
 * crosses is a number, and the receiver dereferences it directly. No
 * buffer descriptor, no grant, and nothing for the kernel to map. In a
 * cohort that works because the sender's memory is already in the
 * receiver's address space. Outside one it faults, which is the other
 * half of the check.
 */

#ifndef _SYS_PEER_H_
#define _SYS_PEER_H_

#include <sys/startup.h>

#include <stdint.h>

/*
 * A private identifier, per section 20.4: a number a parent and a child
 * agree on and nothing else reads. The connection under it goes to a
 * server outside the cohort, and there is no system role that means
 * that. Borrowing one that means something else would be a check
 * program teaching a wrong use of a name.
 */
#define PEER_NS_OUTSIDER    (SYS_NS_PRIVATE + 0)

typedef struct peer_request {
    uint64_t    pr_addr;        /* a word in the sender's own slot */
    uint32_t    pr_value;       /* what the receiver should write there */
    uint32_t    pr_pad;
} peer_request_t;

typedef struct peer_reply {
    uint32_t    pr_status;
    uint32_t    pr_was;         /* what stood there before the write */
} peer_reply_t;

#endif /* _SYS_PEER_H_ */
