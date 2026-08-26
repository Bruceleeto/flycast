/*
	Deterministic performance harness for the flycast validation examples.

	The same source is built once and run in two places: on a Dreamcast, where
	the SH4's own performance counters answer, and under flycast, where the
	cache and pipeline models answer. Both print the same lines in the same
	format, so validating the simulator is diffing two files rather than
	correlating two different tools.

	The SH4 has only two performance counters, so a run is split into PASSES:
	each pass measures one pair of events over an identical stretch of work.
	Identical is the whole point - the workload is re-seeded at the start of
	every pass, so pass 3's frames are the same frames as pass 1's, and the
	counts can be divided by each other.

	Usage:

		dcbench_init("texture2d", my_seed_fn);
		while (dcbench_next_frame()) {
			BeginDrawing();
			...
			EndDrawing();
		}
		dcbench_finish();

	Nothing here reads the clock, waits for vblank on purpose, or looks at the
	controller: anything that couples the measurement to wall time makes two
	runs incomparable, which is the one property this has to have.
*/
#ifndef DCBENCH_H
#define DCBENCH_H

#include <dc/perfctr.h>
#include <kos/dbgio.h>
#include <dc/pvr.h>
#include <dc/pvr/pvr_regs.h>
#include <arch/timer.h>
#include <stdint.h>
#include <stdio.h>

#ifndef DCBENCH_WARMUP
// Frames run before each pass is measured. Caches, texture uploads and the
// first-touch page faults are not steady state and would otherwise land in
// whichever pass happened to run first.
#define DCBENCH_WARMUP 30
#endif
#ifndef DCBENCH_FRAMES
// Frames measured per pass. Long enough that per-frame noise averages out,
// short enough that the 48-bit counters cannot wrap.
#define DCBENCH_FRAMES 200
#endif

typedef void (*dcbench_seed_fn)(void);

#ifndef DCBENCH_MAX_PHASES
// Regions of a frame an example can attribute its counters to. Small on
// purpose: this answers "which part of the frame differs", and a long list of
// narrow regions answers it worse than a short list of wide ones.
#define DCBENCH_MAX_PHASES 6
#endif

typedef struct {
	perf_cntr_event_t ev;
	const char *name;
} dcbench_event;

typedef struct {
	dcbench_event a, b;
} dcbench_pass;

// The events, chosen to line up one-for-one with what cachesim and pipesim
// report. Order matters only in that pass 0 is the one every other pass is
// normalised against.
static const dcbench_pass dcbench_passes[] = {
	// The denominators. Everything below is a share of these two.
	{ { PMCR_ELAPSED_TIME_MODE,                    "cycles" },
	  { PMCR_INSTRUCTION_ISSUED_MODE,              "instructions" } },
	// The measurement proper: miss COUNTS, which is what cachesim actually
	// counts. Everything it reports in cycles is derived from these.
	{ { PMCR_INSTRUCTION_CACHE_MISS_MODE,          "icache_miss" },
	  { PMCR_OPERAND_CACHE_MISS_MODE,              "ocache_miss" } },
	// What those misses cost. This is the line that says whether cachesim's
	// penalty model is right, as opposed to whether its miss counting is.
	{ { PMCR_PIPELINE_FREEZE_BY_ICACHE_MISS_MODE,  "icache_stall_cycles" },
	  { PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE,  "dcache_stall_cycles" } },
	// Denominators for the miss rates: without these a miss count is a number
	// with no scale, and two runs that did different amounts of work look the
	// same.
	{ { PMCR_ALL_INSTRUCTION_FETCH_MODE,           "ifetch" },
	  { PMCR_OPERAND_ACCESS_MODE,                  "operand_access" } },
	// Pipeline stalls that are NOT cache: this is the half of the frame the
	// cache model says nothing about, and what pipesim's flow-dep and
	// resource columns are claiming to predict.
	{ { PMCR_PIPELINE_FREEZE_BY_CPU_REGISTER_MODE, "reg_stall_cycles" },
	  { PMCR_PIPELINE_FREEZE_BY_FPU_MODE,          "fpu_stall_cycles" } },
	// Issue behaviour. parallel/instructions is the dual-issue rate, which is
	// the single number pipesim is most likely to get wrong.
	{ { PMCR_PARALLEL_INSTRUCTION_ISSUED_MODE,     "parallel_issued" },
	  { PMCR_BRANCH_TAKEN_MODE,                    "branch_taken" } },
	// Accesses that never reach the cache, which on this hardware is mostly
	// the PVR. A guest that spins waiting on the graphics hardware does it
	// here: the poll is a load the operand cache never sees, so it is
	// invisible in every counter above. This is the one that says whether a
	// difference in total work between two runs is real work or waiting.
	{ { PMCR_ON_CHIP_IO_ACCESS_MODE,               "io_access" },
	  { PMCR_BRANCH_ISSUED_MODE,                   "branch_issued" } },
};

#define DCBENCH_NPASSES ((int)(sizeof(dcbench_passes) / sizeof(dcbench_passes[0])))

static struct {
	const char *name;
	dcbench_seed_fn seed;
	int pass;
	int frame;		// within the current pass, warm-up included
	int started;	// counters running for this pass
	// Per-region totals of whatever PRFC0 is counting this pass. One counter
	// read per boundary, so the instrumentation costs a handful of cycles and
	// lands inside the region it is charged to on both sides equally.
	uint64_t phaseAcc[DCBENCH_MAX_PHASES];
	const char *phaseName[DCBENCH_MAX_PHASES];
	uint64_t phaseLast;
	int phaseCur;
	int phaseUsed;
	// Renders started and vblanks elapsed across the window, against loop
	// iterations. These three say whether the guest is pacing itself against
	// the display or running free, which no cycle counter can distinguish:
	// a frame that waits for vblank and a frame that is simply slow cost the
	// same number of cycles.
	size_t frame0, vbl0;
	// An independent clock. PMCR's elapsed-time counter is assumed to tick at
	// the 200MHz CPU clock, and every cycle figure here rests on that; the TMU
	// is a separate timebase, so the two disagreeing is the only way to find
	// out that the assumption is wrong.
	uint64_t wall0;
} dcbench;

// Close the region opened by the last call and open `slot`. Cheap enough to
// call several times a frame; does nothing outside the measurement window, so
// the warm-up frames are not charged to anything.
static inline void dcbench_phase(int slot, const char *name)
{
	if (!dcbench.started || slot >= DCBENCH_MAX_PHASES)
		return;
	const uint64_t now = perf_cntr_count(PRFC0);
	dcbench.phaseAcc[dcbench.phaseCur] += now - dcbench.phaseLast;
	dcbench.phaseLast = now;
	dcbench.phaseCur = slot;
	dcbench.phaseName[slot] = name;
	if (slot >= dcbench.phaseUsed)
		dcbench.phaseUsed = slot + 1;
}

static inline void dcbench_init(const char *name, dcbench_seed_fn seed)
{
	dcbench.name = name;
	dcbench.seed = seed;
	dcbench.pass = 0;
	dcbench.frame = 0;
	dcbench.started = 0;
	if (dcbench.seed != NULL)
		dcbench.seed();
	printf("DCBENCH begin %s passes=%d warmup=%d frames=%d\n",
			name, DCBENCH_NPASSES, DCBENCH_WARMUP, DCBENCH_FRAMES);
	// The sync pulse generator registers, exactly as the guest left them.
	// These decide the vblank period, and therefore the frame rate of anything
	// that paces itself against the display - so if two machines disagree on
	// frame rate while agreeing on every instruction, this is where to look
	// first: either they hold different values, or the same values are being
	// turned into different periods.
	printf("DCSPG fb_r_ctrl=%08lx spg_hblank_int=%08lx spg_vblank_int=%08lx "
			"spg_control=%08lx spg_load=%08lx spg_vblank=%08lx\n",
			(unsigned long)PVR_GET(0x0044), (unsigned long)PVR_GET(0x00c8),
			(unsigned long)PVR_GET(0x00cc), (unsigned long)PVR_GET(0x00d0),
			(unsigned long)PVR_GET(0x00d8), (unsigned long)PVR_GET(0x00dc));
}

static inline void dcbench_emit(void)
{
	const dcbench_pass *p = &dcbench_passes[dcbench.pass];
	const uint64_t a = perf_cntr_count(PRFC0);
	const uint64_t b = perf_cntr_count(PRFC1);
	perf_cntr_stop(PRFC0);
	perf_cntr_stop(PRFC1);
	// Counters are stopped, so the reporting below costs the measurement
	// nothing however slow the console is.
	dbgio_enable();
	// One line per pass, name=value, so a parser never has to know which
	// events a given example chose to measure.
	pvr_stats_t s1;
	pvr_get_stats(&s1);

	printf("DCBENCH %s frames=%d %s=%llu %s=%llu renders=%u vblanks=%u "
			"wall_ns=%llu\n",
			dcbench.name, DCBENCH_FRAMES,
			p->a.name, (unsigned long long)a,
			p->b.name, (unsigned long long)b,
			(unsigned)(s1.frame_count - dcbench.frame0),
			(unsigned)(s1.vbl_count - dcbench.vbl0),
			(unsigned long long)(timer_ns_gettime64() - dcbench.wall0));

	// Where PRFC0's event went, by region. Only PRFC0 is broken down: two
	// counter reads per boundary would start to show up in the totals.
	if (dcbench.phaseUsed > 0)
	{
		// Close the region that was open when the window ended.
		dcbench.phaseAcc[dcbench.phaseCur] += a - dcbench.phaseLast;
		printf("DCPHASE %s %s", dcbench.name, p->a.name);
		for (int i = 0; i < dcbench.phaseUsed; i++)
			printf(" %s=%llu",
					dcbench.phaseName[i] == NULL ? "?" : dcbench.phaseName[i],
					(unsigned long long)dcbench.phaseAcc[i]);
		printf("\n");
	}
}

// Returns 0 when every pass is done. Call it as the loop condition; it handles
// warm-up, starting and stopping the counters, and moving between passes.
static inline int dcbench_next_frame(void)
{
	if (dcbench.pass >= DCBENCH_NPASSES)
		return 0;

	if (dcbench.frame == DCBENCH_WARMUP)
	{
		// Steady state reached: start counting.
		const dcbench_pass *p = &dcbench_passes[dcbench.pass];
		// The console is not part of the workload, and it is not the same
		// device in every environment: on hardware with dc-tool attached KOS
		// sends output over dcload, and anywhere else it falls back to the
		// serial port and spins on the transmit FIFO. Measuring one against
		// the other measures the console. Turn it off for the window.
		dbgio_disable();
		perf_cntr_start(PRFC0, p->a.ev, PMCR_COUNT_CPU_CYCLES);
		perf_cntr_start(PRFC1, p->b.ev, PMCR_COUNT_CPU_CYCLES);
		dcbench.started = 1;
		for (int i = 0; i < DCBENCH_MAX_PHASES; i++)
			dcbench.phaseAcc[i] = 0;
		dcbench.phaseLast = 0;
		dcbench.phaseCur = 0;
		{
			pvr_stats_t s0;
			pvr_get_stats(&s0);
			dcbench.frame0 = s0.frame_count;
			dcbench.vbl0 = s0.vbl_count;
			dcbench.wall0 = timer_ns_gettime64();
		}
	}
	else if (dcbench.started && dcbench.frame == DCBENCH_WARMUP + DCBENCH_FRAMES)
	{
		dcbench_emit();
		dcbench.started = 0;
		dcbench.pass++;
		dcbench.frame = 0;
		if (dcbench.pass >= DCBENCH_NPASSES)
			return 0;
		// Re-seed so this pass draws exactly what the last one drew.
		if (dcbench.seed != NULL)
			dcbench.seed();
		return 1;
	}

	dcbench.frame++;
	return 1;
}

static inline void dcbench_finish(void)
{
	printf("DCBENCH end %s\n", dcbench.name);
}

#endif
