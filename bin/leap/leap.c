/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Jump into its own stack.
 *
 * M14 stage three gives a program's writable memory the permissions
 * writable memory should have: read and write, and not execute. What
 * proves it is a program that tries to execute some. Nothing else here
 * can: every other program jumps only to addresses the linker chose,
 * which are all in text.
 *
 * The bytes on the stack are never meant to be a valid instruction, and
 * it does not matter what they are. A machine that refuses the fetch
 * refuses it before decoding anything, and a machine that permits the
 * fetch is the one this is here to catch, whatever it then makes of
 * what it found.
 */

#include <os/sys.h>
#include <stdio.h>
#include <stdint.h>

int
main(void)
{
    volatile uint32_t code[8];
    void (*fn)(void);
    uintptr_t at;
    uint32_t i;

    for (i = 0; i < 8; i++) {
        code[i] = 0;
    }

    at = (uintptr_t)(void *)code;

    printf("leap: jumping to 0x%x\n", (unsigned int)at);

    /*
     * Through an integer, because a pointer to an object and a pointer
     * to a function are different kinds of thing and a compiler is
     * entitled to say so. The whole point of this program is to make
     * the hardware answer a question the language does not have.
     */
    fn = (void (*)(void))at;
    fn();

    /*
     * Reaching here means the stack was executable and whatever was on
     * it returned, which on these machines means the zeroes decoded to
     * something harmless. Either way the fetch was permitted, which is
     * the failure.
     */
    return 3;
}
