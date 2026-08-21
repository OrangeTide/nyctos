/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A program that exits, with something to say about how.
 *
 * The status is not zero on purpose. Zero is what a status field holds
 * when nobody has written one, so a check that reads zero and calls it a
 * successful join has tested the field rather than the path, which is the
 * shape of test failure this project has paid for more than once.
 */

#include <os/sys.h>

#define STATUS  7

int
main(void)
{
    return STATUS;
}
