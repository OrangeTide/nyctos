/* brief.c : a program that starts and exits without doing any work.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * main returns 0 at once. Repeated runs measure the cost of a spawn and a
 * reap: loading the program's frames, building its address space, and
 * taking both down again.
 *
 */

int
main(void)
{
    return 0;
}
