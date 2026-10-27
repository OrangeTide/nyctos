/* exit.c : terminates the calling program with a status.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * exit passes its status to _exit and does nothing else. crt1 calls it with
 * the return value of main.
 *
 */

#include <stdlib.h>
#include <unistd.h>

void
exit(int status)
{
    _exit(status);
}
