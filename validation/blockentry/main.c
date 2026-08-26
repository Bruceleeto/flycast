/*
	Does a block boundary cost dual-issue pairing, and by how much?

	pipesim analyses one basic block as a straight-line sequence repeated
	forever. Two things that assumption could get wrong at a boundary:

	  - pairing restarts. Which instructions co-issue depends on how the fetch
	    lands, and after a taken branch that restarts at the target. Pairing is
	    what makes a dependency distance 0 rather than 1, and SHC_PM 8.3
	    charges FULL latency at distance 0 - so being off by one in pairing
	    perturbs every dependency in the block, not one of them.
	  - the wrap. The tail of one iteration feeds the head of the next, where
	    real execution enters from somewhere else entirely. Measured in-sim at
	    5.87M cycles against a 28M excess, so this is already known to be small.

	Measuring it through reg_stall cannot separate those, and neither can a
	"short blocks" probe - branch misattribution, entry pairing and wrap ALL
	predict "over-counts on short blocks with branches". So this measures
	PAIRING directly, with the dependency question taken off the table.

	Every case runs the same four-instruction body:

	    mov r1,r2      MT
	    add r3,r4      EX
	    mov r5,r6      MT
	    add r7,r8      EX

	MT and EX are different groups, so table 8.2 says every adjacent pair here
	is legally co-issuable, and no instruction reads a register any other one
	writes. In a long run this should approach 2 instructions per issue slot.
	The cases differ ONLY in how often a taken branch cuts the run up:

	    LONG    no branches at all - the pairing ceiling
	    BLK16   a taken branch every 16 instructions
	    BLK8    ...every 8
	    BLK4    ...every 4

	Each branch is `bra` + `nop` in the delay slot, so a block of N pairable
	instructions costs N+2 issued instructions. Subtracting the LONG case gives
	pairs lost per block entry, on hardware and in flycast, and comparing those
	two numbers is the whole point.

	Reading it:

	  hardware and flycast lose the same pairs per entry
	      the entry model is right and reg_stall's remainder is elsewhere
	  flycast loses fewer
	      it pairs across boundaries silicon does not - the suspected bug,
	      and the slope says how much of reg_stall it can account for
	  flycast loses more
	      over-correction, look at the branch/delay-slot handling instead

	No dependencies anywhere, so nothing here goes through the flow-dependency
	rule and a result is about issue behaviour alone.
*/
#include <kos.h>
#include <dc/perfctr.h>
#include <stdio.h>

#define REPS 4000

// Four pairable instructions, no dependencies between them.
#define BODY4 \
	"mov r1,r2\n\t" \
	"add r3,r4\n\t" \
	"mov r5,r6\n\t" \
	"add r7,r8\n\t"

// A taken branch to the next instruction, delay slot filled with a nop. Two
// more issued instructions, and one block boundary.
#define CUT(n) \
	"bra 1f\n\t" \
	"nop\n\t" \
	"1:\n\t"

__attribute__((noinline)) static void case_long(void)
{
	for (int i = 0; i < REPS; i++)
		__asm__ __volatile__(".rept 4\n\t" BODY4 BODY4 BODY4 BODY4
			"nop\n\tnop\n\t" ".endr\n\t"
			: : : "r2", "r4", "r6", "r8");
}

__attribute__((noinline)) static void case_blk16(void)
{
	for (int i = 0; i < REPS; i++)
		__asm__ __volatile__(".rept 4\n\t" BODY4 BODY4 BODY4 BODY4 CUT(16)
			".endr\n\t"
			: : : "r2", "r4", "r6", "r8");
}

__attribute__((noinline)) static void case_blk8(void)
{
	for (int i = 0; i < REPS; i++)
		__asm__ __volatile__(".rept 8\n\t" BODY4 BODY4 CUT(8) ".endr\n\t"
			: : : "r2", "r4", "r6", "r8");
}

__attribute__((noinline)) static void case_blk4(void)
{
	for (int i = 0; i < REPS; i++)
		__asm__ __volatile__(".rept 16\n\t" BODY4 CUT(4) ".endr\n\t"
			: : : "r2", "r4", "r6", "r8");
}

typedef void (*casefn)(void);

static void measure(const char *name, casefn fn,
		perf_cntr_event_t e0, perf_cntr_event_t e1,
		const char *n0, const char *n1)
{
	perf_cntr_clear(PRFC0);
	perf_cntr_clear(PRFC1);
	perf_cntr_start(PRFC0, e0, PMCR_COUNT_CPU_CYCLES);
	perf_cntr_start(PRFC1, e1, PMCR_COUNT_CPU_CYCLES);
	fn();
	const uint64_t a = perf_cntr_count(PRFC0);
	const uint64_t b = perf_cntr_count(PRFC1);
	perf_cntr_stop(PRFC0);
	perf_cntr_stop(PRFC1);
	printf("BLOCKENTRY %s %s=%llu %s=%llu\n", name, n0,
			(unsigned long long)a, n1, (unsigned long long)b);
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	case_long(); case_blk16(); case_blk8(); case_blk4();

	printf("BLOCKENTRY begin reps=%d\n", REPS);
	static const struct { const char *name; casefn fn; } cases[] = {
		{ "LONG",  case_long  },
		{ "BLK16", case_blk16 },
		{ "BLK8",  case_blk8  },
		{ "BLK4",  case_blk4  },
	};
	for (int i = 0; i < 4; i++)
	{
		measure(cases[i].name, cases[i].fn,
				PMCR_INSTRUCTION_ISSUED_MODE,
				PMCR_PARALLEL_INSTRUCTION_ISSUED_MODE,
				"issue_slots", "parallel_issued");
		measure(cases[i].name, cases[i].fn,
				PMCR_ELAPSED_TIME_MODE,
				PMCR_BRANCH_TAKEN_MODE,
				"cycles", "branch_taken");
		measure(cases[i].name, cases[i].fn,
				PMCR_PIPELINE_FREEZE_BY_BRANCH_MODE,
				PMCR_PIPELINE_FREEZE_BY_CPU_REGISTER_MODE,
				"branch_stall", "reg_stall");
	}
	printf("BLOCKENTRY end\n");
	return 0;
}
