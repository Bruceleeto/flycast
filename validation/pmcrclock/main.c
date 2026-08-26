/*
	pmcrclock - what does PMCR's elapsed-time counter actually count?

	Every cycle figure in this validation suite is read from PMCR event 0x23
	and divided by an assumed 200MHz. On a Dreamcast that assumption produces
	138.2 MHz instead, which matches no documented clock on the machine - the
	closest is a 123MHz effective root-bus throughput figure, which is not a
	clock domain. Under flycast the same read gives 199.6 MHz.

	The suspicion is that 0x23 stops while the CPU is halted. SHC_PM section
	9.1.1 table 9.1 says that in sleep the clock generator keeps running and
	only the CPU halts, and it does not say which side of that line the
	performance counter falls on. No local document does. So measure it.

	Three intervals of the same wall-clock length, timed by the TMU, which is
	a different timebase and keeps running regardless:

	  busy    a spin loop. Establishes counts per second with the CPU awake,
	          and therefore the denominator - which settles the 138MHz
	          question on its own, whichever way the sleep answer falls.
	  sleep   thd_sleep, which parks on the idle thread and executes SLEEP.
	  vblank  waiting on the PVR, which is what a frame loop actually does and
	          the case this whole question came from.

	If the counter free-runs, all three report the same rate. If it counts only
	while the CPU is executing, busy reports ~200MHz and the other two report
	far less - and every hardware cycle figure in the suite is awake-cycles,
	not wall-cycles, and must not be compared against an emulator that counts
	wall time.
*/
#include <kos.h>
#include <dc/perfctr.h>
#include <dc/pvr.h>
#include <arch/timer.h>
#include <stdio.h>

#define INTERVAL_NS 200000000ULL	// 200ms per case

static void report(const char *what, uint64_t cycles, uint64_t wall)
{
	// Rate in MHz, and what that would be as a fraction of the 200MHz CPU
	// clock the suite assumes.
	const double mhz = wall == 0 ? 0.0 : (double)cycles * 1000.0 / (double)wall;
	printf("DCPMCR %-6s cycles=%llu wall_ns=%llu mhz=%.2f frac_of_200=%.4f\n",
			what, (unsigned long long)cycles, (unsigned long long)wall,
			mhz, mhz / 200.0);
}

int main(int argc, char **argv)
{
	pvr_init_defaults();

	perf_cntr_start(PRFC0, PMCR_ELAPSED_TIME_MODE, PMCR_COUNT_CPU_CYCLES);

	printf("DCPMCR begin interval_ns=%llu\n", (unsigned long long)INTERVAL_NS);

	// 1. Awake. Nothing here blocks, so the CPU never halts.
	{
		const uint64_t w0 = timer_ns_gettime64();
		const uint64_t c0 = perf_cntr_count(PRFC0);
		while (timer_ns_gettime64() - w0 < INTERVAL_NS)
			;
		const uint64_t c1 = perf_cntr_count(PRFC0);
		const uint64_t w1 = timer_ns_gettime64();
		report("busy", c1 - c0, w1 - w0);
	}

	// 2. Asleep. thd_sleep parks this thread and leaves the idle thread to
	// execute SLEEP until the timer wakes it.
	{
		const uint64_t w0 = timer_ns_gettime64();
		const uint64_t c0 = perf_cntr_count(PRFC0);
		thd_sleep((int)(INTERVAL_NS / 1000000ULL));
		const uint64_t c1 = perf_cntr_count(PRFC0);
		const uint64_t w1 = timer_ns_gettime64();
		report("sleep", c1 - c0, w1 - w0);
	}

	// 3. Waiting on the PVR, which is what a frame loop does. Submits nothing:
	// the point is the waiting, not the rendering.
	{
		const uint64_t w0 = timer_ns_gettime64();
		const uint64_t c0 = perf_cntr_count(PRFC0);
		while (timer_ns_gettime64() - w0 < INTERVAL_NS)
		{
			pvr_wait_ready();
			pvr_scene_begin();
			pvr_list_begin(PVR_LIST_OP_POLY);
			pvr_list_finish();
			pvr_scene_finish();
		}
		const uint64_t c1 = perf_cntr_count(PRFC0);
		const uint64_t w1 = timer_ns_gettime64();
		report("vblank", c1 - c0, w1 - w0);
	}

	printf("DCPMCR end\n");
	return 0;
}
