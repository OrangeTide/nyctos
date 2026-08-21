/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A program that talks to a person, and holds no ambient authority at all.
 *
 * Every other program here prints through the debug console of section
 * 22.4, which takes no handle and reaches the device because it exists.
 * This one is built without it: DEBUG_CONSOLE is off, so the macros expand
 * to nothing and there is no call to the backdoor anywhere in the image.
 * What it has instead is a connection to the console service, handed to it
 * at spawn under SYS_NS_CONSOLE.
 *
 * That is the whole of the check. If this program says anything at all,
 * output reached a person through a handle rather than through a hole.
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
