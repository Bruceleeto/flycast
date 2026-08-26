/*
	Store queue submission rate, and what it costs.

	Derived from KOS's pvr/pvrmark_strips_direct, which is the direct-rendering
	fast path: vertices are written into a store queue and flushed to the tile
	accelerator, two queues used in alternation. The adaptive polygon search and
	the wall-clock frame-rate reporting are removed - both make the workload
	depend on how fast the machine happens to be, which is exactly what a
	simulator comparison cannot tolerate.

	WHY THIS EXAMPLE EXISTS

	The store queue cost model is the one term in cachesim with no defensible
	value. `tools/hwprobe/sqsweep` swept the gap between flushes from 16 to 205
	cycles and measured a flat 2.0 cycles per flush to both destinations. The
	examples disagree with that and with each other. Subtract the operand-miss
	model from each one's data-side freeze and divide the remainder by its
	flush count:

	    texture2d        641,529 flushes    residual     31,786    0.05/flush
	    bruces_balls  13,440,600 flushes    residual 46,921,853    3.49/flush

	Seventy times apart. So the missing term does not scale with flush count,
	and no value of a per-flush constant can express it.

	What the two differ in is RATE. bruces_balls submits 67,203 vertices a
	frame from a tight hand-scheduled loop; texture2d dribbles them through
	GLdc between other work. KOS's own pvrmark settles around 10-20K polygons a
	frame before it drops under 55fps, so bruces_balls is submitting several
	times faster than the tile accelerator can retire - and a store queue whose
	destination FIFO is full stalls the CPU on the next write.

	That is the hypothesis this example is built to test: the missing cost is
	TA backpressure, and it appears only above the rate the TA can absorb.

	HOW IT TESTS IT

	Three phases per frame, each submitting the SAME number of vertices through
	the SAME code path, differing only in how much integer filler work sits
	between one commit and the next:

	    burst   no filler          - fastest submission this loop can manage
	    med     ~8 filler rounds
	    slow    ~32 filler rounds  - slowest

	Flush count per phase is identical and known exactly, so DCPHASE's split of
	the data-side freeze counter gives three points on a rate curve from ONE
	hardware run. Backpressure predicts the per-flush cost falls to near zero
	in `slow` and rises in `burst`. A genuine per-flush constant predicts all
	three are equal. Spacing alone was already ruled out by sqsweep across
	16-205 cycles, so a flat result here would mean the term is neither rate nor
	spacing and the search moves elsewhere.

	The filler is pure integer arithmetic on a register-resident LCG: no loads,
	no stores, no branches into other blocks. It must not add cache misses,
	because operand cache cost is what the freeze counter is being separated
	from.

	SECOND PURPOSE

	This is also the suite's most integer-dominated workload. Not FPU-free -
	the vertex coordinates are int-to-float conversions and a flycast run puts
	about 4% of the frame in the FPU stall counter - but the LCG, the masking
	and the loop control are all integer, and `reg_stall_cycles` comes out
	around eight times the FPU figure where bruces_balls has it the other way
	round by a factor of fourteen. Register stalls read 2.4-3.5x across the
	other three examples with the FPU counter entangled in every one of them;
	this constrains the register half on its own.

	The `slow` phase is the useful one for that: it is 32 rounds of dependent
	integer multiply-add between commits, which is a long serial dependency
	chain of exactly the kind the flow-dependency model is worst at.
*/
#include <kos.h>
#include <stdlib.h>
#include <limits.h>

#ifdef DCBENCH_BUILD
#include "../bench/dcbench.h"
#endif

// Vertices submitted per phase, per frame. Three phases, so three times this
// per frame. Chosen so `burst` is comfortably above what the TA can retire and
// `slow` is comfortably below - the point is to straddle the knee, not to hit
// a particular frame rate.
#define VERTS_PER_PHASE 6000
#define STRIP_LEN       32

static pvr_init_params_t pvr_params = {
    { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0 },
    512 * 1024, 0, 0, 0, 0, 0
};

static pvr_poly_hdr_t hdr;
static int seed_base = 0xdeadbeef;

// KOS's LCG from pvrmark, kept identical so the vertex stream is the same
// shape. Register-resident: the seed never leaves a register inside a phase.
inline static int getnum(int *seed, int mn) {
    int num = (*seed & ((mn) - 1));
    *seed = *seed * 1164525 + 1013904223;
    return num;
}

inline static void get_vert(int *seed, int *x, int *y, int *col) {
    *x = (*x + ((getnum(seed, 64)) - 32)) & 1023;
    *y = (*y + ((getnum(seed, 64)) - 32)) & 511;
    *col = getnum(seed, INT32_MAX);
}

// Integer filler. Returns a value the caller consumes so the optimiser cannot
// delete it, and touches no memory so it adds no cache traffic.
inline static int filler(int v, int rounds) {
    for(int i = 0; i < rounds; i++)
        v = v * 1103515245 + 12345;
    return v;
}

// One phase: VERTS_PER_PHASE vertices in strips, `rounds` of filler between
// each commit. The flush count is identical for every value of `rounds`, which
// is what makes the three phases comparable.
static void submit(int *seed, int rounds) {
    pvr_vertex_t *vert;
    int x = 0, y = 0, z = 0, col = 0, junk = *seed;

    for(int i = 0; i < VERTS_PER_PHASE; i++) {
        get_vert(seed, &x, &y, &col);

        if((i % STRIP_LEN) == 0)
            z = getnum(seed, 128) + 1;

        vert = pvr_dr_target();
        vert->flags = ((i % STRIP_LEN) == (STRIP_LEN - 1))
                ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vert->x = (float)x;
        vert->y = (float)y;
        vert->z = (float)z;
        vert->argb = 0xff000000 | col;
        pvr_dr_commit(vert);

        if(rounds)
            junk = filler(junk, rounds);
    }
    // Consume the filler result so it cannot be optimised away.
    *seed ^= (junk & 1);
}

static void setup(void) {
    pvr_poly_cxt_t cxt;
    pvr_init(&pvr_params);
    pvr_set_bg_color(0, 0, 0);
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.shading = PVR_SHADE_FLAT;
    pvr_poly_compile(&hdr, &cxt);
}

#ifdef DCBENCH_BUILD
// No RNG state survives a pass: every pass starts from the same seed and
// therefore submits exactly the same vertex stream.
static void bench_seed(void) { seed_base = 0xdeadbeef; }
#endif

#ifndef DCBENCH_BUILD
static int check_start(void) {
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if(!cont) return 0;
    cont_state_t *state = (cont_state_t *)maple_dev_status(cont);
    return state && (state->buttons & CONT_START);
}
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setup();

#ifdef DCBENCH_BUILD
    dcbench_init("sqmark", bench_seed);
    while(dcbench_next_frame()) {
#else
    while(!check_start()) {
#endif
        pvr_dr_state_t dr_state;
        int seed = seed_base;

        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_dr_init(&dr_state);
        pvr_prim(&hdr, sizeof(hdr));

#ifdef DCBENCH_BUILD
        dcbench_phase(0, "burst");
#endif
        submit(&seed, 0);
#ifdef DCBENCH_BUILD
        dcbench_phase(1, "med");
#endif
        submit(&seed, 8);
#ifdef DCBENCH_BUILD
        dcbench_phase(2, "slow");
#endif
        submit(&seed, 32);

#ifdef DCBENCH_BUILD
        dcbench_phase(3, "rest");
#endif
        pvr_list_finish();
        pvr_scene_finish();
        seed_base = seed;
    }

#ifdef DCBENCH_BUILD
    dcbench_finish();
#endif
    return 0;
}
