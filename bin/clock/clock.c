/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Read the clock twice, and say where the readings fell.
 *
 * The check outside takes the time before this runs and again after it
 * has finished, and this program's business is to put its own readings
 * where that check can compare them with those two. What it returns is
 * the second reading measured from the kernel's first, which is one
 * number the check can bracket: below zero and the program read a clock
 * behind the kernel's, above the kernel's own span and it read one
 * ahead, and a clock that reads a constant or counts in other units
 * fails one end or the other rather than the eye.
 *
 * The two readings of its own are what say the clock moves forwards.
 * The kernel cannot see them, so this checks that part itself and
 * reports the failure as a status the check knows by name.
 */

#include <os/sys.h>
#include <stddef.h>
#include <stdint.h>

/* What the check reads, rather than numbers it would have to guess. */
#define CLOCK_BACKWARDS     (-1)
#define CLOCK_BEHIND        (-2)
#define CLOCK_NO_ARG        (-3)
#define CLOCK_TOO_FAR       (-4)
#define CLOCK_NO_PAGE       (-5)

/* Bare hex, which is what the kernel's hex_into writes. */
static int
hex(const char *s, uint64_t *out)
{
    uint64_t v = 0;

    if (*s == '\0') {
        return -1;
    }
    for (; *s != '\0'; s++) {
        uint64_t d;

        if (*s >= '0' && *s <= '9') {
            d = (uint64_t)(*s - '0');
        } else if (*s >= 'a' && *s <= 'f') {
            d = (uint64_t)(*s - 'a') + 10u;
        } else {
            return -1;
        }
        v = (v << 4) | d;
    }

    *out = v;

    return 0;
}

int
main(int argc, char **argv)
{
    uint64_t before, first, second;

    if (argc != 2 || hex(argv[1], &before) != 0) {
        return CLOCK_NO_ARG;
    }

    /*
     * The frequency as well, because a program that can read the
     * counter and cannot say how fast it runs still cannot build a
     * deadline. Stage four is what uses it; this says it is there.
     */
    if (sys_uptime_hz() == 0) {
        return CLOCK_NO_PAGE;
    }

    first = sys_uptime();
    second = sys_uptime();

    if (second < first) {
        return CLOCK_BACKWARDS;
    }
    if (first < before) {
        return CLOCK_BEHIND;
    }

    /*
     * A status is a signed 32-bit number, so a gap wider than that is
     * reported rather than truncated into a small one that would look
     * like a pass.
     */
    if (second - before > 0x7fffffffu) {
        return CLOCK_TOO_FAR;
    }

    return (int)(second - before);
}
