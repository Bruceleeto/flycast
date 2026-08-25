/* Cleanroom operand cache miss check.
 *
 * Nothing but the walk: no printf inside the loop, no perf counters, no
 * console redraw. That matters because KOS's console draws through the
 * framebuffer, and those accesses land in the same totals as the probe's -
 * an earlier attempt at this measurement had four times as many console
 * accesses as walk accesses, which made the miss RATIO meaningless.
 *
 * Stride 32 with a 256KB span means every access touches a new line in a
 * buffer sixteen times the size of the cache, so the answer is 1.0 misses
 * per access. Hardware confirms exactly that (see ocprobe). So whatever the
 * model reports for `misses / cached accesses` over this run is its error,
 * with no arithmetic left to argue about.
 */
#include <kos.h>
#include <stdio.h>

#define SPAN   (256 * 1024)
#ifndef STRIDE
#define STRIDE 32
#endif
#ifndef PASSES
#define PASSES 2000
#endif
#define PER    (SPAN / STRIDE)   /* accesses per pass: 8192 */

static uint8_t buf[SPAN] __attribute__((aligned(32)));

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("# clean oc walk: %d passes x %d accesses, stride %d, span %d\n",
           PASSES, PER, STRIDE, SPAN);
    printf("# expected: exactly 1.0 miss per access\n");

    volatile uint8_t *p = buf;
    for (unsigned pass = 0; pass < PASSES; pass++)
        for (unsigned off = 0; off < SPAN; off += STRIDE)
            (void)p[off];

    printf("# total walk accesses = %u\n", (unsigned)PASSES * PER);
    printf("# done\n");
    // Halt rather than return: KOS restarts main, and a second run would make
    // the access count ambiguous. Spin so nothing further touches memory.
    while (1) __asm__ __volatile__("");
    return 0;
}
