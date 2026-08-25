/* Store queue cost probe.
 *
 * The pipeline model accounts for 91.7% of measured cycles on bruces_balls.
 * Of the 8.3% left, the hardware counters attribute 7.9% to data-side pipeline
 * freeze - but there are only 188 operand cache misses per frame, so that is
 * 1,483 cycles per miss. It is not cache misses. It is the store queue.
 *
 * This measures what one 32-byte store queue transfer actually costs, to RAM
 * and to the PVR's tile accelerator, so the cost can be modelled from a
 * measurement rather than a guess.
 *
 * Method is the same as the rest of the harness: run N flushes and 2N flushes,
 * take the difference, and divide. That cancels loop and counter overhead.
 * Each figure is the minimum over several runs.
 */
#include <kos.h>
#include <dc/perfctr.h>
#include <dc/sq.h>
#include <stdio.h>

#define ITERS 32

static uint8_t scratch[64 * 1024] __attribute__((aligned(32)));

/* Fill and flush one store queue `n` times at `dst`.
   sq_lock() sets QACR and hands back the queue pointer; sq_flush() is the
   pref that starts the 32-byte transfer. */
static void sq_burst(void *dst, unsigned n) {
    uint32_t *sq = sq_lock(dst);

    for (unsigned i = 0; i < n; i++) {
        sq[0] = i; sq[1] = i; sq[2] = i; sq[3] = i;
        sq[4] = i; sq[5] = i; sq[6] = i; sq[7] = i;
        sq_flush(sq);
    }
    sq_unlock();
}

/* Returns cycles and the chosen event, per flush. */
static void measure(void *dst, unsigned n, int event,
                    double *cyc_out, double *ev_out) {
    uint64 best_c = ~0ull, best_e = 0;

    for (int it = 0; it < ITERS; it++) {
        perf_cntr_clear(PRFC0); perf_cntr_clear(PRFC1);
        perf_cntr_start(PRFC0, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_CPU_CYCLES);
        perf_cntr_start(PRFC1, event, PMCR_COUNT_CPU_CYCLES);
        sq_burst(dst, n);
        perf_cntr_stop(PRFC0); perf_cntr_stop(PRFC1);

        uint64 c = perf_cntr_count(PRFC0);
        if (c < best_c) { best_c = c; best_e = perf_cntr_count(PRFC1); }
    }
    *cyc_out = (double)best_c;
    *ev_out  = (double)best_e;
}

static void probe(const char *what, void *dst, int event, const char *evname) {
    double c1, e1, c2, e2;
    /* Warm the caches and the queue. */
    sq_burst(dst, 64);

    measure(dst, 64,  event, &c1, &e1);
    measure(dst, 128, event, &c2, &e2);

    printf("SQ\t%-10s\t%-8s\t%7.3f\t%7.3f\n",
           what, evname, (c2 - c1) / 64.0, (e2 - e1) / 64.0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("# store queue cost probe\n");
    printf("# per 32-byte flush; (cycles(2N)-cycles(N))/N, N=64, min of %d\n", ITERS);
    printf("SQ\tdest\tevent\tcycles\tevent_val\n");

    /* Area 3: main RAM. Always safe. */
    probe("ram", scratch, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, "frz_dc");
    probe("ram", scratch, PMCR_OPERAND_CACHE_MISS_MODE,             "oc_miss");
    probe("ram", scratch, PMCR_OPERAND_WRITE_ACCESS_MODE,           "wr_acc");

    /* Area 4: the tile accelerator, which is what a renderer actually hits.
       Submitted inside a scene so the TA is listening. */
    pvr_init_defaults();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    void *ta = (void *)0x10000000;
    probe("ta", ta, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, "frz_dc");
    probe("ta", ta, PMCR_OPERAND_CACHE_MISS_MODE,             "oc_miss");
    probe("ta", ta, PMCR_OPERAND_WRITE_ACCESS_MODE,           "wr_acc");

    printf("# done\n");
    return 0;
}
