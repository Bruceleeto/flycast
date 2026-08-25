/* Dirty writeback spacing sweep.
 *
 * The write-back buffer holds exactly one cache line (SH4 manual 4.3.4) and
 * drains on the B-clock behind the refill. So an isolated dirty eviction is
 * hidden and a stream of them is not - the second eviction has nowhere to go
 * until the first has drained. That is the same shape as the store queue:
 *
 *     stall = max(0, drain - gapSincePreviousEviction)
 *
 * The model currently charges a flat 21.9 cycles per writeback, measured with
 * `occost` at ONE spacing - back to back, where every access both misses and
 * evicts. That is the worst case, so the flat charge is right for streaming
 * writes and overstates anything sparser.
 *
 * This sweeps the gap by putting filler between the writes. Same analysis as
 * sqsweep: the gap is measured as (cycles - frz_dc) per access, so nothing
 * depends on knowing what a nop costs, and if the model shape is right then
 * frz_dc falls 1:1 with the gap and flattens out at the clean-fill cost of
 * 14.3 once the writeback is fully hidden.
 *
 * The read rows are the control: reads never dirty a line, so they never evict
 * one, and their frz_dc should sit flat at the fill cost no matter the spacing.
 * If the read rows slope too, the effect is not the writeback buffer.
 */
#include <kos.h>
#include <dc/perfctr.h>
#include <stdio.h>

#define ITERS  16
#define SPAN   (256 * 1024)   /* sixteen times the cache: every access misses */
#define STRIDE 32             /* one line per access */

static uint8_t buf[SPAN] __attribute__((aligned(32)));

static void walk(int write, unsigned n, unsigned nops) {
    volatile uint8_t *p = buf;
    unsigned off = 0;
    for (unsigned i = 0; i < n; i++) {
        if (write) p[off] = (uint8_t)i; else (void)p[off];
        off += STRIDE;
        if (off >= SPAN) off = 0;
        for (unsigned k = 0; k < nops; k++) __asm__ __volatile__("nop");
    }
}

static void measure(int write, unsigned n, unsigned nops, int event,
                    double *cyc, double *ev) {
    uint64 best_c = ~0ull, best_e = 0;
    for (int it = 0; it < ITERS; it++) {
        perf_cntr_clear(PRFC0); perf_cntr_clear(PRFC1);
        perf_cntr_start(PRFC0, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_CPU_CYCLES);
        perf_cntr_start(PRFC1, event, PMCR_COUNT_CPU_CYCLES);
        walk(write, n, nops);
        perf_cntr_stop(PRFC0); perf_cntr_stop(PRFC1);
        uint64 c = perf_cntr_count(PRFC0);
        if (c < best_c) { best_c = c; best_e = perf_cntr_count(PRFC1); }
    }
    *cyc = (double)best_c; *ev = (double)best_e;
}

static void row(const char *what, int write, unsigned nops) {
    const unsigned N = 4096;
    double c1, f1, c2, f2, m1, m2, x1, x2;

    walk(write, N, nops);   /* warm, and for writes leave every line dirty */

    measure(write, N,     nops, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, &c1, &f1);
    measure(write, 2 * N, nops, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, &c2, &f2);
    measure(write, N,     nops, PMCR_OPERAND_CACHE_MISS_MODE, &x1, &m1);
    measure(write, 2 * N, nops, PMCR_OPERAND_CACHE_MISS_MODE, &x2, &m2);

    const double cyc = (c2 - c1) / N;
    const double frz = (f2 - f1) / N;
    printf("WBS\t%-5s\t%4u\t%8.3f\t%8.3f\t%8.3f\t%6.3f\n",
           what, nops, cyc, frz, cyc - frz, (m2 - m1) / N);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("# dirty writeback spacing sweep\n");
    printf("# per access; (x(2N)-x(N))/N, N=4096, min of %d, stride %d\n",
           ITERS, STRIDE);
    printf("# every access misses. write rows also evict a dirty line.\n");
    printf("# read rows are the control: no dirty victims, so no writeback.\n");
    printf("# CCR = %08lx\n", (unsigned long)*((volatile uint32_t *)0xff00001c));
    printf("WBS\tcase\tnops\tcycles\tfrz_dc\tgap\tmiss\n");

    for (unsigned nops = 0; nops <= 96; nops = nops ? nops * 2 : 2)
        row("read", 0, nops);
    for (unsigned nops = 0; nops <= 96; nops = nops ? nops * 2 : 2)
        row("write", 1, nops);

    printf("# done\n");
    return 0;
}
