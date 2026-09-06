/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A program that starts and stops, for M13 stage 5.
 *
 * What that stage asks is whether the system can run indefinitely, and
 * the way to ask it is to spawn and reap a program more times than the
 * machine has frames. Every other program in this archive computes
 * something, talks to a server or faults on purpose, and the cheapest
 * of them spends eighty milliseconds doing it. Multiplied by the number
 * of frames in the machine that is a check nobody would run.
 *
 * So this one does nothing at all. What a cycle of it costs is what a
 * spawn and a reap cost, which is what the stage is measuring: the
 * loader taking a slot's worth of frames, the reap giving them back,
 * and the address space built and taken down around them. A program
 * that also did some work would be reporting how long the work took.
 */

int
main(void)
{
    return 0;
}
