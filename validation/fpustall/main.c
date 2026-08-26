/*
	What does PMCR 0x29, "pipeline freeze by FPU", actually count?

	Nothing in the Dreamcast or SH4 document set defines it. The counter tables
	give a mode number, a name and a unit, and stop. The name is the only clue
	and it is an odd one: 0x28 next to it is called "freeze by CPU register",
	naming a register file, while 0x29 names a functional unit. Whoever wrote
	that table had the words "FP register" available and did not use them.

	It matters because pipesim has to put every flow-dependency stall in one
	bucket or the other, and the two readings disagree about a large class of
	instructions. All the FMOV variants are LS group, not FE (SHC_PM table 8.3
	entries 173-179, 194-200, 222-230), yet they write FP registers. Under an
	operand-keyed reading a stall waiting on FMOV's result is an FPU freeze;
	under a unit-keyed one it is a CPU-register freeze, because no FE-group
	instruction is involved at all.

	So this measures it instead of guessing. Four cases, each a long unrolled
	loop, counters 0x28 and 0x29 read over the same stretch of work:

	  A  fmov.s @rm,frn -> fmov.s frn,@rk
	     A read-after-write on an FP register with no FE instruction anywhere.
	     Latency 2, distance 1, so exactly one stall cycle per pair.
	     Operand-keyed puts this in 0x29. Unit-keyed puts it in 0x28.

	  B  fdiv, then independent fadds on disjoint registers
	     No dependency at all. FDIV locks F3 for ten cycles from cycle 2
	     (table 8.3), so the following FE instructions stall on the resource.
	     Unit-keyed counts this. Operand-keyed cannot - there is no operand.

	  C  fadd -> dependent fadd
	     FE group AND an FP register dependency. Must land in 0x29 under every
	     reading. If it does not, the harness is wrong and nothing else here
	     means anything - check this line first.

	  D  mov.l @rm,r1 -> mov.l r1,@rk
	     The exact GPR mirror of A: same LS group, same latency 2, same
	     distance 1, same one stall per pair. The only difference is that the
	     awaited register is r1 instead of fr0. A against D IS the experiment.
	     (Dependent integer adds were the obvious control and are useless -
	     latency 1 at distance 1 stalls for zero cycles, correctly.)

	Reading the result:

	  A in 0x29, B not          operand-keyed - split on the register file
	  A in 0x28, B in 0x29      unit-keyed - split on the functional unit
	  A and B both in 0x29      superset: FP registers OR FPU resources
	  neither                   harness or event mapping is wrong

	No sleep anywhere: 0x23 stops while the CPU is halted, so a loop that waits
	on anything measures a clock that is not running.
*/
#include <kos.h>
#include <dc/perfctr.h>
#include <stdio.h>

#define REPS 6000

// 8-byte aligned scratch for the FMOV cases. Two separate lines so the loads
// and stores do not fight over one cache line and add operand-cache freeze to
// a measurement that is supposed to be about the pipeline.
static float srcbuf[64] __attribute__((aligned(32)));
static float dstbuf[64] __attribute__((aligned(32)));

// A: FP-register dependency, no FE instruction. Each pair is a load into frN
// followed immediately by a store of frN, so the store waits on the load.
__attribute__((noinline)) static void case_a(void)
{
	register float *s __asm__("r4") = srcbuf;
	register float *d __asm__("r5") = dstbuf;
	for (int i = 0; i < REPS; i++)
		__asm__ __volatile__(
			".rept 12\n\t"
			"fmov.s @r4,fr0\n\t"
			"fmov.s fr0,@r5\n\t"
			".endr\n\t"
			: : "r"(s), "r"(d) : "fr0", "memory");
}

// B: FPU resource lock, no operand dependency. fdiv locks F3 for ten cycles;
// the fadds that follow touch none of its registers.
__attribute__((noinline)) static void case_b(void)
{
	for (int i = 0; i < REPS; i++)
		__asm__ __volatile__(
			".rept 12\n\t"
			"fdiv fr1,fr2\n\t"
			"fadd fr3,fr4\n\t"
			"fadd fr5,fr6\n\t"
			"fadd fr7,fr8\n\t"
			".endr\n\t"
			: : : "fr2", "fr4", "fr6", "fr8");
}

// C: control. FE group and an FP-register dependency at once.
__attribute__((noinline)) static void case_c(void)
{
	for (int i = 0; i < REPS; i++)
		__asm__ __volatile__(
			".rept 12\n\t"
			"fadd fr0,fr1\n\t"
			"fadd fr1,fr2\n\t"
			"fadd fr2,fr3\n\t"
			"fadd fr3,fr4\n\t"
			".endr\n\t"
			: : : "fr1", "fr2", "fr3", "fr4");
}

// D: the exact GPR mirror of case A. Same instruction group (LS), same
// latency 2, same distance 1, same one stall cycle per pair - the ONLY
// difference is that the register being waited on is r1 rather than fr0.
// That makes A-against-D the whole experiment: if the register file is what
// 0x29 keys on, these two land in different counters, and if the functional
// unit is what it keys on they land in the same one.
__attribute__((noinline)) static void case_d(void)
{
	register float *s __asm__("r4") = srcbuf;
	register float *d __asm__("r5") = dstbuf;
	for (int i = 0; i < 6000; i++)
		__asm__ __volatile__(
			".rept 12\n\t"
			"mov.l @r4,r1\n\t"
			"mov.l r1,@r5\n\t"
			".endr\n\t"
			: : "r"(s), "r"(d) : "r1", "memory");
}

typedef void (*casefn)(void);

static void measure(const char *name, casefn fn,
		perf_cntr_event_t ev0, perf_cntr_event_t ev1,
		const char *n0, const char *n1)
{
	perf_cntr_clear(PRFC0);
	perf_cntr_clear(PRFC1);
	perf_cntr_start(PRFC0, ev0, PMCR_COUNT_CPU_CYCLES);
	perf_cntr_start(PRFC1, ev1, PMCR_COUNT_CPU_CYCLES);
	fn();
	const uint64_t a = perf_cntr_count(PRFC0);
	const uint64_t b = perf_cntr_count(PRFC1);
	perf_cntr_stop(PRFC0);
	perf_cntr_stop(PRFC1);
	printf("FPUSTALL %s %s=%llu %s=%llu\n", name, n0,
			(unsigned long long)a, n1, (unsigned long long)b);
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	for (int i = 0; i < 64; i++)
	{
		srcbuf[i] = 1.5f + (float)i;
		dstbuf[i] = 0.0f;
	}
	// Warm the caches and the branch history so the measured pass is steady
	// state, then take every reading over identical work.
	case_a(); case_b(); case_c(); case_d();

	printf("FPUSTALL begin reps=%d unroll=12\n", REPS);
	static const struct { const char *name; casefn fn; } cases[] = {
		{ "A_fmov_dep",  case_a },
		{ "B_fdiv_lock", case_b },
		{ "C_fadd_dep",  case_c },
		{ "D_int_dep",   case_d },
	};
	for (int i = 0; i < 4; i++)
	{
		measure(cases[i].name, cases[i].fn,
				PMCR_PIPELINE_FREEZE_BY_CPU_REGISTER_MODE,
				PMCR_PIPELINE_FREEZE_BY_FPU_MODE,
				"reg_stall", "fpu_stall");
		measure(cases[i].name, cases[i].fn,
				PMCR_ELAPSED_TIME_MODE,
				PMCR_INSTRUCTION_ISSUED_MODE,
				"cycles", "issue_slots");
		// The disjoint-and-exhaustive check: if these five categories overlap,
		// a stall counted twice would invert a ratio all on its own.
		measure(cases[i].name, cases[i].fn,
				PMCR_PIPELINE_FREEZE_BY_ICACHE_MISS_MODE,
				PMCR_PIPELINE_FREEZE_BY_BRANCH_MODE,
				"icache_stall", "branch_stall");
		measure(cases[i].name, cases[i].fn,
				PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE,
				PMCR_PARALLEL_INSTRUCTION_ISSUED_MODE,
				"dcache_stall", "parallel_issued");
	}
	printf("FPUSTALL end\n");
	return 0;
}
