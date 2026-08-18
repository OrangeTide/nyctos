/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The first userland program: it prints the project banner and exits.
 * Everything it does goes through libc and, from there, through the
 * system call trap into the kernel. It also reads /etc/motd off the
 * initramfs to prove out open(2)/read(2)/close(2) against the VFS.
 */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* Kept identical to docs/banner.txt. */
static const char *banner[] = {
    "    \033[0;1;33;93m_\033[0m   \033[0;1;36;96m__\033[0m           \033[0;1;36;96m_\033[0;1;34;94m_\033[0m            ",
    "   \033[0;1;33;93m/\033[0m \033[0;1;32;92m|\033[0m \033[0;1;36;96m/\033[0m \033[0;1;34;94m/\033[0;1;35;95m_\033[0m  \033[0;1;31;91m_\033[0;1;33;93m__\033[0;1;32;92m__\033[0;1;36;96m__\033[0;1;34;94m/\033[0m \033[0;1;35;95m/_\033[0;1;31;91m__\033[0;1;33;93m__\033[0m  \033[0;1;36;96m__\033[0;1;34;94m__\033[0;1;35;95m_\033[0m",
    "  \033[0;1;32;92m/\033[0m  \033[0;1;36;96m|\033[0;1;34;94m/\033[0m \033[0;1;35;95m/\033[0m \033[0;1;31;91m/\033[0m \033[0;1;33;93m/\033[0m \033[0;1;32;92m/\033[0m \033[0;1;36;96m__\033[0;1;34;94m_/\033[0m \033[0;1;35;95m_\033[0;1;31;91m_/\033[0m \033[0;1;33;93m_\033[0;1;32;92m_\033[0m \033[0;1;36;96m\\/\033[0m \033[0;1;34;94m_\033[0;1;35;95m__\033[0;1;31;91m/\033[0m",
    " \033[0;1;32;92m/\033[0m \033[0;1;36;96m/\033[0;1;34;94m|\033[0m  \033[0;1;35;95m/\033[0m \033[0;1;31;91m/\033[0;1;33;93m_/\033[0m \033[0;1;32;92m/\033[0m \033[0;1;36;96m/\033[0;1;34;94m__\033[0;1;35;95m/\033[0m \033[0;1;31;91m/_\033[0;1;33;93m/\033[0m \033[0;1;32;92m/_\033[0;1;36;96m/\033[0m \033[0;1;34;94m(_\033[0;1;35;95m_\033[0m  \033[0;1;31;91m)\033[0m ",
    "\033[0;1;36;96m/_\033[0;1;34;94m/\033[0m \033[0;1;35;95m|_\033[0;1;31;91m/\\\033[0;1;33;93m__\033[0;1;32;92m,\033[0m \033[0;1;36;96m/\\\033[0;1;34;94m__\033[0;1;35;95m_/\033[0;1;31;91m\\_\033[0;1;33;93m_/\033[0;1;32;92m\\_\033[0;1;36;96m__\033[0;1;34;94m_/\033[0;1;35;95m__\033[0;1;31;91m__\033[0;1;33;93m/\033[0m  ",
    "      \033[0;1;33;93m/_\033[0;1;32;92m__\033[0;1;36;96m_/\033[0m                       \n"
};

int
main(void)
{
    unsigned int i;
    int fd;
    char buf[128];
    ssize_t n;

    for (i = 0; i < sizeof(banner) / sizeof(banner[0]); i++) {
        puts(banner[i]);
    }
    puts("");
    puts("Hello, world from userspace on " ARCH_NAME "!");

    puts("");
    fd = open("/etc/motd", O_RDONLY);
    if (fd >= 0) {
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(STDOUT_FILENO, buf, (size_t)n);
        }
        close(fd);
    } else {
        puts("(could not open /etc/motd)");
    }

    return 0;
}
