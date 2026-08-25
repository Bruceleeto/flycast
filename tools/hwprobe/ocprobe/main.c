/* Operand cache miss probe.
 *
 * The cache model's data-side miss count has been in doubt: an old comparison
 * put it at half what hardware reports. That comparison was invalid - it
 * measured hardware on one binary and the model on another - and the workload
 * it used misses so rarely (0.06% of a frame) that it could never have settled
 * the question either way.
 *
 * This settles it. It walks a buffer of known size at a known stride, so the
 * miss count is not merely measurable but DERIVABLE:
 *
 *   - stride >= 32 (the line size): every access touches a new line, so
 *     misses == accesses, as long as the buffer does not fit in the cache.
 *   - stride < 32: 32/stride accesses share a line, so misses == accesses
 *     divided by that.
 *   - buffer <= 16KB (the cache size): after the first pass it is all
 *     resident, so misses == 0.
 *
 * Three numbers per row, then: what hardware counts, what the model counts,
 * and what arithmetic says it must be. Two agreeing against one identifies
 * which side is wrong, which a straight hardware-versus-model comparison
 * cannot do.
 *
 * Same method as the rest of the harness: N and 2N passes, take the
 * difference, divide. That cancels loop and counter overhead. Minimum over
 * several runs, because an interrupt can only ever add.
 */
#include <kos.h>
#include <dc/perfctr.h>
#include <stdio.h>

#define ITERS 16
#define BUF_BYTES (256 * 1024)

/* Uncached-address alias is never used: we want these accesses cached, which
   is the whole point. 32-byte aligned so a stride of 32 lands one per line. */
static uint8_t buf[BUF_BYTES] __attribute__((aligned(32)));

/* Read `n` locations `stride` apart, wrapping inside `span` bytes.
   volatile so the compiler cannot hoist or vectorise the loads away. */
static void walk(unsigned n, unsigned stride, unsigned span) {
    volatile uint8_t *p = buf;
    unsigned off = 0;
    for (unsigned i = 0; i < n; i++) {
        (void)p[off];
        off += stride;
        if (off >= span)
            off = 0;
    }
}

static void measure(unsigned n, unsigned stride, unsigned span, int event,
                    double *cyc_out, double *ev_out) {
    uint64 best_c = ~0ull, best_e = 0;

    for (int it = 0; it < ITERS; it++) {
        perf_cntr_clear(PRFC0); perf_cntr_clear(PRFC1);
        perf_cntr_start(PRFC0, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_CPU_CYCLES);
        perf_cntr_start(PRFC1, event, PMCR_COUNT_CPU_CYCLES);
        walk(n, stride, span);
        perf_cntr_stop(PRFC0); perf_cntr_stop(PRFC1);

        uint64 c = perf_cntr_count(PRFC0);
        if (c < best_c) { best_c = c; best_e = perf_cntr_count(PRFC1); }
    }
    *cyc_out = (double)best_c;
    *ev_out  = (double)best_e;
}

/* What the miss count HAS to be, per access, from the geometry alone. */
static double expected(unsigned stride, unsigned span) {
    if (span <= 16 * 1024)
        return 0.0;                       /* resident after the first pass */
    if (stride >= 32)
        return 1.0;                       /* a new line every access */
    return (double)stride / 32.0;         /* 32/stride accesses share a line */
}

static void probe(unsigned stride, unsigned span) {
    double c1, e1, c2, e2, w1, w2;
    const unsigned N = 4096;

    walk(N, stride, span);                /* warm */

    measure(N,     stride, span, PMCR_OPERAND_CACHE_MISS_MODE, &c1, &e1);
    measure(2 * N, stride, span, PMCR_OPERAND_CACHE_MISS_MODE, &c2, &e2);
    measure(N,     stride, span, PMCR_OPERAND_READ_ACCESS_MODE, &w1, &c1);
    measure(2 * N, stride, span, PMCR_OPERAND_READ_ACCESS_MODE, &w2, &c2);

    printf("OC\t%u\t%u\t%7.3f\t%7.3f\t%7.3f\n",
           stride, span,
           (c2 - c1) / N,                 /* read accesses per access: ~1 */
           (e2 - e1) / N,                 /* MISSES per access - the number */
           expected(stride, span));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("# operand cache miss probe\n");
    printf("# per access; (count(2N)-count(N))/N, N=4096, min of %d\n", ITERS);
    printf("# cache is 16KB, 32-byte lines. span<=16K should miss ~0.\n");
    printf("OC\tstride\tspan\tread_acc\toc_miss\texpected\n");

    /* Fits in the cache: everything here should be ~0 misses. If it is not,
       the miss is not coming from capacity and the model has to explain it. */
    probe(32,   4 * 1024);
    probe(32,  16 * 1024);

    /* Bigger than the cache, so capacity misses are unavoidable. Stride at or
       above the line size means one miss per access, exactly. */
    probe(32,  256 * 1024);
    probe(64,  256 * 1024);
    probe(128, 256 * 1024);
    probe(512, 256 * 1024);

    /* Below the line size: several accesses share a line, so the miss rate
       should fall in proportion. This is the row that catches a model
       counting per access rather than per line. */
    probe(16,  256 * 1024);
    probe(8,   256 * 1024);
    probe(4,   256 * 1024);

    /* Exactly the cache size apart: every access lands in the same set. On a
       direct-mapped cache that is a miss every time regardless of span. */
    probe(16 * 1024, 256 * 1024);

    printf("# done\n");
    return 0;
}
