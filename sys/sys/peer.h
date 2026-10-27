/* peer.h : request and reply records for the cohort check.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares peer_request_t and peer_reply_t, the records the two cohort programs
 * exchange. A request carries an address and the value to write there, and a
 * reply carries the value that was there before. Also declares PEER_NS_OUTSIDER,
 * a private namespace identifier. Included by bin/peer_a/peer_a.c,
 * bin/peer_b/peer_b.c and sys/kern/init_main.c.
 *
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
