/* Operand cache miss COST probe.
 *
 * The miss count is now validated exact (see ocprobe / plan 9g). What has
 * never been measured is what a miss costs. The model charges 16.8 cycles,
 * borrowed from the instruction side, for every operand cache miss - the same
 * for a read that fills a clean line and for a write that evicts a dirty one
 * and has to write it back first.
 *
 * Method: the penalty is taken as a DIFFERENCE, never an absolute. Walking a
 * 4KB span fits in the 16KB cache and hits every time; walking a 256KB span
 * misses every time. Same loop, same instruction count, same stride - so
 * subtracting the two cancels the loop overhead and leaves the miss penalty.
 * The N / 2N slope on top of that cancels the fixed setup.
 *
 * Four cases, because the interesting question is whether they differ:
 *
 *   read  hit    baseline for reads
 *   read  miss   a clean line fill
 *   write hit    baseline for writes
 *   write miss   a fill, plus writing back whatever dirty line it evicted
 *
 * If read-miss and write-miss come out equal, one constant is defensible. If
 * write-miss is dearer, the model needs a second one, and the writeback count
 * it already tracks stops being decorative.
 */
#include <kos.h>
#include <dc/perfctr.h>
#include <stdio.h>

#define ITERS  16
#define SPAN_HIT  (4 * 1024)      /* fits in the 16KB cache */
#define SPAN_MISS (256 * 1024)    /* sixteen times the cache */
#define STRIDE 32                 /* one line per access */

static uint8_t buf[SPAN_MISS] __attribute__((aligned(32)));

static void walk_read(unsigned n, unsigned span) {
    volatile uint8_t *p = buf;
    unsigned off = 0;
    for (unsigned i = 0; i < n; i++) {
        (void)p[off];
        off += STRIDE;
        if (off >= span) off = 0;
    }
}

static void walk_write(unsigned n, unsigned span) {
    volatile uint8_t *p = buf;
    unsigned off = 0;
    for (unsigned i = 0; i < n; i++) {
        p[off] = (uint8_t)i;
        off += STRIDE;
        if (off >= span) off = 0;
    }
}

static void measure(int write, unsigned n, unsigned span, int event,
                    double *cyc, double *ev) {
    uint64 best_c = ~0ull, best_e = 0;
    for (int it = 0; it < ITERS; it++) {
        perf_cntr_clear(PRFC0); perf_cntr_clear(PRFC1);
        perf_cntr_start(PRFC0, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_CPU_CYCLES);
        perf_cntr_start(PRFC1, event, PMCR_COUNT_CPU_CYCLES);
        if (write) walk_write(n, span); else walk_read(n, span);
        perf_cntr_stop(PRFC0); perf_cntr_stop(PRFC1);
        uint64 c = perf_cntr_count(PRFC0);
        if (c < best_c) { best_c = c; best_e = perf_cntr_count(PRFC1); }
    }
    *cyc = (double)best_c; *ev = (double)best_e;
}

static void row(const char *what, int write, unsigned span) {
    const unsigned N = 4096;
    double c1, e1, c2, e2, f1, f2, d1, d2;

    if (write) walk_write(N, span); else walk_read(N, span);   /* warm */

    measure(write, N,     span, PMCR_OPERAND_CACHE_MISS_MODE, &c1, &e1);
    measure(write, 2 * N, span, PMCR_OPERAND_CACHE_MISS_MODE, &c2, &e2);
    measure(write, N,     span, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, &d1, &f1);
    measure(write, 2 * N, span, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, &d2, &f2);

    printf("OCC\t%-11s\t%7.3f\t%7.3f\t%7.3f\n",
           what,
           (c2 - c1) / N,        /* cycles per access */
           (e2 - e1) / N,        /* misses per access */
           (f2 - f1) / N);       /* dcache freeze cycles per access */
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("# operand cache miss cost probe\n");
    printf("# per access; (x(2N)-x(N))/N, N=4096, min of %d, stride %d\n",
           ITERS, STRIDE);
    printf("# penalty = (cycles at miss row) - (cycles at matching hit row)\n");
    printf("# CCR = %08lx\n", (unsigned long)*((volatile uint32_t *)0xff00001c));
    printf("OCC\tcase\tcycles\toc_miss\tfrz_dc\n");

    row("read_hit",   0, SPAN_HIT);
    row("read_miss",  0, SPAN_MISS);
    row("write_hit",  1, SPAN_HIT);
    row("write_miss", 1, SPAN_MISS);

    printf("# done\n");
    return 0;
}
