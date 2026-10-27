/* greet.c : prints and reads a line through the console service.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The program is built without the debug console, so its output reaches a
 * person only through the console handle it receives at spawn. It prints
 * formatted text, reads one line, prints it back, and returns the line length
 * as its exit status.
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(void)
{
    char line[64];
    int n;
    const char *bad;

    printf("greet: %s, %u %s, %d and %04d\n", "formatting works", 42u,
      "arguments", -7, 5);

    /*
     * The cases a formatter gets wrong quietly: a sign inside a
     * zero-padded width, a string that is not there, and a percent
     * with nothing after it that means anything. Through a variable,
     * so that the compiler does not reject the conversion this is
     * here to check the behaviour of.
     */
    bad = "greet: [%04d] [%6d] [%s] [%q]\n";
    printf(bad, -7, 42, (char *)0, 1);

    printf("greet: type something: ");
    n = readline(line, sizeof(line));
    if (n < 0) {
        return -1;
    }

    printf("greet: %d bytes, and they were '%s'\n", n, line);

    return n;
}
