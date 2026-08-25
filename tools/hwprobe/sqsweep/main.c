/* Store queue drain sweep.
 *
 * The model charges a flush by how much of the PREVIOUS flush's drain is still
 * outstanding:
 *
 *     stall = max(0, drain - gapSincePreviousFlush)
 *
 * so the cost falls as flushes are spread out and hits zero once the gap
 * covers the drain. The shape is from the manual; `drain` is not, and right now
 * each destination's value is fitted to a SINGLE measurement:
 *
 *     TA  35.6, from bruces_balls flushing every 32 cycles and stalling 3.6
 *     RAM 22.1, from the old sqprobe running back to back
 *
 * One point cannot distinguish a line from any other curve through it. This
 * sweeps the gap by putting a varying number of nops between flushes, so the
 * relationship can be seen rather than assumed. If the model is right, frz_dc
 * per flush falls linearly with slope -1 and reaches zero at `drain`.
 *
 * The gap is MEASURED, not assumed: for each row it is (cycles - frz_dc) per
 * flush, so nothing depends on knowing what a nop costs.
 *
 * Two things the earlier TA probe got wrong and this does not: it alternates
 * SQ0 and SQ1 the way real code does, instead of serialising everything
 * through one queue; and it submits real polygon headers and vertices inside a
 * real scene, instead of arbitrary bytes the TA then chokes on.
 */
#include <kos.h>
#include <dc/perfctr.h>
#include <dc/sq.h>
#include <stdio.h>

#define ITERS 16
/* The TA needs far more than a handful of vertices before it pushes back: its
   input FIFO swallows a short burst without the CPU ever noticing. The first
   version of this probe used 64 and measured 2.0 cycles per flush - the same as
   RAM - while bruces_balls, submitting 78,000 vertices a frame to a TA that is
   also rendering them, works out at 3.6. So the burst has to be long enough to
   reach steady state, and the geometry has to give the PVR real fill work
   rather than degenerate triangles it can discard. */
#define TA_N 2048

static uint8_t scratch[64 * 1024] __attribute__((aligned(32)));
static pvr_poly_hdr_t hdr;

/* Flush `n` times to RAM, alternating the two queues, with `nops` of filler
   between one flush and the next. */
static void burst_ram(unsigned n, unsigned nops) {
    uint32_t *base = sq_lock(scratch);
    for (unsigned i = 0; i < n; i++) {
        /* bit 5 selects SQ0 / SQ1 - the alternation is the whole point */
        uint32_t *sq = (uint32_t *)((uintptr_t)base ^ ((i & 1) << 5));
        sq[0] = i; sq[1] = i; sq[2] = i; sq[3] = i;
        sq[4] = i; sq[5] = i; sq[6] = i; sq[7] = i;
        sq_flush(sq);
        for (unsigned k = 0; k < nops; k++) __asm__ __volatile__("nop");
    }
    sq_unlock();
}

/* The same to the tile accelerator, as real vertices in a real scene. */
static void burst_ta(unsigned n, unsigned nops) {
    for (unsigned i = 0; i < n; i++) {
        pvr_vertex_t *v = (pvr_vertex_t *)pvr_dr_target();
        /* Strips of three, so the PVR actually rasterises. Spread over the
           screen and given area, otherwise it culls them and never works. */
        v->flags = (i % 3 == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x = (float)((i * 37) % 600) + (float)((i % 3) * 12);
        v->y = (float)((i * 53) % 440) + (float)((i % 3) * 9);
        v->z = 10.0f;
        v->u = v->v = 0.0f;
        v->argb = 0xffffffff;
        v->oargb = 0;
        pvr_dr_commit(v);
        for (unsigned k = 0; k < nops; k++) __asm__ __volatile__("nop");
    }
}

static void measure(int ta, unsigned n, unsigned nops, int event,
                    double *cyc, double *ev) {
    uint64 best_c = ~0ull, best_e = 0;
    for (int it = 0; it < ITERS; it++) {
        perf_cntr_clear(PRFC0); perf_cntr_clear(PRFC1);
        perf_cntr_start(PRFC0, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_CPU_CYCLES);
        perf_cntr_start(PRFC1, event, PMCR_COUNT_CPU_CYCLES);
        if (ta) burst_ta(n, nops); else burst_ram(n, nops);
        perf_cntr_stop(PRFC0); perf_cntr_stop(PRFC1);
        uint64 c = perf_cntr_count(PRFC0);
        if (c < best_c) { best_c = c; best_e = perf_cntr_count(PRFC1); }
    }
    *cyc = (double)best_c; *ev = (double)best_e;
}

static void row(const char *dest, int ta, unsigned nops) {
    const unsigned N = ta ? TA_N : 64;
    double c1, f1, c2, f2;

    if (ta) burst_ta(N, nops); else burst_ram(N, nops);   /* warm */

    measure(ta, N,     nops, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, &c1, &f1);
    measure(ta, 2 * N, nops, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, &c2, &f2);

    const double cyc = (c2 - c1) / N;      /* total cycles per flush */
    const double frz = (f2 - f1) / N;      /* of which, stalled */
    printf("SQS\t%-4s\t%4u\t%8.3f\t%8.3f\t%8.3f\n",
           dest, nops, cyc, frz, cyc - frz);   /* last column is the GAP */
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("# store queue drain sweep\n");
    printf("# per flush; (x(2N)-x(N))/N, N=64, min of %d\n", ITERS);
    printf("# gap = cycles - frz_dc, i.e. the un-stalled part. Model predicts\n");
    printf("#   frz_dc = max(0, drain - gap), so frz_dc should fall 1:1 with\n");
    printf("#   gap and reach 0 at gap == drain.\n");
    printf("SQS\tdest\tnops\tcycles\tfrz_dc\tgap\n");

    for (unsigned nops = 0; nops <= 96; nops = nops ? nops * 2 : 2)
        row("ram", 0, nops);

    /* A real scene, so the TA is actually consuming what it is given. */
    pvr_init_defaults();
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr, &cxt);

    for (unsigned nops = 0; nops <= 96; nops = nops ? nops * 2 : 2) {
        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&hdr, sizeof(hdr));
        row("ta", 1, nops);
        pvr_list_finish();
        pvr_scene_finish();
    }

    printf("# done\n");
    return 0;
}
