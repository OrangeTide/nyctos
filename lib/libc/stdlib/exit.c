/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <stdlib.h>
#include <unistd.h>

void
exit(int status)
{
    _exit(status);
}
