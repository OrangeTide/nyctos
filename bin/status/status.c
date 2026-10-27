/* status.c : a program that exits with status 7.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It returns 7 from main. The value is nonzero because zero is what an
 * unwritten status field holds, so a check that reads zero cannot tell a real
 * exit from an empty field.
 *
 */

#include <os/sys.h>

#define STATUS  7

int
main(void)
{
    return STATUS;
}
