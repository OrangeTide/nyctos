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

static volatile uint64_t sink;

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
    uint64_t before, first, second, extra = 0, i;

    /*
     * Counted where the reading happens and not worked out from the
     * argument. The check outside asserts this number, and a count
     * derived from what was asked for would say the reads happened
     * whether or not they did: that arrangement was tried, and taking
     * the loop out did not fail it.
     */
    uint64_t taken = 2;

    if (argc < 2 || argc > 3 || hex(argv[1], &before) != 0) {
        return CLOCK_NO_ARG;
    }

    /*
     * How many more times to read the clock, for the check that counts
     * what the reading costs. The number is the argument rather than a
     * constant here so that one program answers both questions: the
     * count of crossings has to come from the same image doing the
     * same work, or the comparison is between two programs.
     */
    if (argc == 3 && hex(argv[2], &extra) != 0) {
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

    /*
     * Into something volatile, so that a compiler that can see this
     * loop discards nothing: the point of the loop is the reading, and
     * a read whose value is thrown away is a read that never happened.
     */
    for (i = 0; i < extra; i++) {
        sink = sys_uptime();
        taken++;
    }

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

    /*
     * Asked how many times to read, this answers with how many times it
     * read. A count, which is the kind of thing a check can assert on
     * any host: the run that is meant to do more work says how much
     * more it did, and a program that ignored the argument says so.
     * Where the readings fell is the other run's question, and that one
     * is asked without a count.
     */
    if (argc == 3) {
        return (int)taken;
    }

    return (int)(second - before);
}
