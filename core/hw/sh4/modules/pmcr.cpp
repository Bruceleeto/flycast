/*
	Copyright 2026 Flycast contributors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "pmcr.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/cachesim/cachesim.h"

#include <cstring>

namespace pmcr
{

enum : u32
{
	PMCR0_addr   = 0xFF000084,
	PMCR1_addr   = 0xFF000088,
	PMCTR0H_addr = 0xFF100004,
	PMCTR0L_addr = 0xFF100008,
	PMCTR1H_addr = 0xFF10000C,
	PMCTR1L_addr = 0xFF100010,
};

enum : u16
{
	PMENABLE = 0x8000,
	PMST     = 0x4000,
	RUN      = PMENABLE | PMST,
	CLR      = 0x2000,
	PMCLK    = 0x0100,
	PMMODE   = 0x003f,
};

// The events this build can answer, and where each one comes from. Anything not
// listed reads zero.
enum Event : u16
{
	EV_OPERAND_ACCESS         = 0x0e,
	EV_OPERAND_CACHE_MISS     = 0x0f,
	EV_ALL_INSTRUCTION_FETCH  = 0x0a,
	EV_INSTRUCTION_CACHE_MISS = 0x08,
	EV_INSTRUCTION_ISSUED     = 0x13,
	EV_ELAPSED_TIME           = 0x23,
	EV_FREEZE_ICACHE_MISS     = 0x24,
	EV_FREEZE_DCACHE_MISS     = 0x25,
	EV_FREEZE_CPU_REGISTER    = 0x28,
	EV_FREEZE_FPU             = 0x29,
	EV_PARALLEL_ISSUED        = 0x14,
	EV_OCACHE_READ_MISS       = 0x04,
	EV_OCACHE_WRITE_MISS      = 0x05,
	EV_ICACHE_FILL            = 0x21,
	EV_OCACHE_FILL            = 0x22,
};

struct Counter
{
	u16 control;
	u64 base;		// source quantity when the counter was last cleared
	u64 frozen;		// count accumulated before the last stop
};
static Counter counters[2];
// Running total of cycles spent halted, subtracted from the elapsed-time event.
static u64 sleepCycles;
// One warning per event mode per run: a guest polling an unmodelled counter
// would otherwise fill the log with the same line.
static bool warned[64];

// The live value of whatever the counter is watching. Everything is a running
// total since boot, so a counter is just this minus a baseline - which is also
// why nothing here has to be hooked into the hot path.
static u64 sourceValue(u16 mode)
{
	const cachesim::Counters& c = cachesim::counters();
	switch (mode)
	{
	case EV_ELAPSED_TIME:
	{
		// AWAKE cycles, not wall cycles: the counter stops while the CPU is
		// halted. See noteSleep() in the header for the measurement.
		const u64 now = sh4_sched_now64();
		return now > sleepCycles ? now - sleepCycles : 0;
	}
	case EV_INSTRUCTION_ISSUED:
	{
		// NOT the instruction count. Hardware counts issue SLOTS: a cycle that
		// issued two instructions increments this once and the parallel
		// counter once. So this plus EV_PARALLEL_ISSUED is the instruction
		// count, and that sum is what a comparison against hardware should be
		// made on - texture2d has it agreeing to 0.09%, where the raw counter
		// looked 35% out.
		return cachesim::pipeTotals().issueSlots;
	}
	case EV_ALL_INSTRUCTION_FETCH:
		// 32-bit fetch accesses, not instructions: the fetch unit reads two
		// instructions at a time, which is exactly what hardware counts here.
		return c.fetchOps;
	case EV_INSTRUCTION_CACHE_MISS:
		return c.misses[(int)cachesim::Stream::Inst];
	case EV_OPERAND_CACHE_MISS:
		return c.misses[(int)cachesim::Stream::Data];
	case EV_OCACHE_WRITE_MISS:
		return c.writeMisses;
	case EV_OCACHE_READ_MISS:
	{
		// cachesim counts write misses as a subset of the total, so the read
		// half is the remainder. Split out because the two do not cost the
		// same - a read waits for the data, a write does not - and one
		// cycles-per-miss figure averaged over an unknown mix is why the two
		// examples disagreed about the data-side cost where they agreed to 4%
		// about the instruction side.
		const u64 all = c.misses[(int)cachesim::Stream::Data];
		return all > c.writeMisses ? all - c.writeMisses : 0;
	}
	case EV_OPERAND_ACCESS:
		return c.dataAccesses;
	case EV_FREEZE_ICACHE_MISS:
		// Derived from miss counts and the penalty model, not measured. The
		// miss count above is the real output; this is the model's opinion of
		// what it cost, and comparing the two against hardware separately is
		// the whole reason both are exposed.
		return (u64)c.missCycles[(int)cachesim::Stream::Inst];
	case EV_FREEZE_DCACHE_MISS:
	{
		// Store queue drain belongs here. Hardware puts it in this counter and
		// not in a separate one: `validation/bruces_balls` freezes 47.46M
		// cycles on 0x25 while missing the operand cache 24786 times, which is
		// 1915 cycles per miss and obviously not a miss. Its operand cache FILL
		// counter (0x22) reads 506770, so 99% of that freeze has no fill behind
		// it. cachesim tracks the two separately and reporting only the miss
		// half made this counter read 0.005 on the one workload that uses the
		// store queues heavily.
		const cachesim::PenaltyConfig& p = cachesim::penaltyConfig();
		const double sq =
				c.sqFlushes[(int)cachesim::SqDest::Ta] * p.sqFlushTa
				+ (c.sqFlushes[(int)cachesim::SqDest::Ram]
					+ c.sqFlushes[(int)cachesim::SqDest::Other]) * p.sqFlushRam;
		return (u64)(c.missCycles[(int)cachesim::Stream::Data] + sq);
	}
	// Fill cycles. Hardware distinguishes these from the pipeline freeze
	// above: a fill occupies the bus, a freeze is what the CPU lost waiting
	// for it, and the difference is whatever the write-back buffer and the
	// fill-around hid. cachesim has one quantity and so answers both with it.
	// That is not a claim the two are equal - it is the model having nothing
	// to say about the difference, and a hardware capture where they diverge
	// says how much is missing.
	case EV_ICACHE_FILL:
		return (u64)c.missCycles[(int)cachesim::Stream::Inst];
	case EV_OCACHE_FILL:
		return (u64)c.missCycles[(int)cachesim::Stream::Data];
	// EV_ON_CHIP_IO_ACCESS is deliberately absent and reads zero. It counts
	// accesses to the SH4's OWN on-chip registers - the timers, the serial
	// port - and a Dreamcast does very few: 74 per frame in the texture2d
	// baseline. cachesim's nearest quantity is uncachedAccesses, which is
	// every access that bypassed the cache and so is dominated by the PVR,
	// 15294 per frame in the same run. Mapping one to the other made the two
	// columns differ by 206x and said nothing about either.
	// EV_PIPELINE_FREEZE_BY_BRANCH (0x27) and EV_OPERAND_READ_ACCESS (0x01)
	// are deliberately absent and read zero. pipesim analyses a block as if it
	// looped forever with no transition cost, so it has no branch penalty to
	// report; cachesim counts operand accesses without splitting them by
	// direction. Both are captured on hardware anyway - the first to size the
	// branch model pipesim needs, the second as the denominator the read and
	// write miss counts above are fitted against.
	// EV_BRANCH_ISSUED and EV_BRANCH_TAKEN are deliberately absent and read
	// zero. Block entries were reported here for a while, on the grounds that
	// a translated block ends in a control transfer - but blocks also end on
	// the op-count limit and on page boundaries, and a block ending on a
	// NOT-taken conditional was being counted as a taken branch. That put two
	// rows in the comparison reading 2.02x and 1.23x which measured the proxy
	// rather than flycast, and looked like findings. Counting real branches
	// needs the decoder to classify block terminators; until it does, zero.
	case EV_PARALLEL_ISSUED:
	{
		// The SH4 issues up to two instructions per cycle. pipesim decides
		// which pairs are legal, so this is that model's dual-issue rate and
		// not an independent one.
		//
		// Instructions that shared a cycle with the one ahead of them. With
		// EV_INSTRUCTION_ISSUED above this partitions the instruction count,
		// exactly as the two hardware counters do.
		//
		// Counted in the pipeline model rather than derived as
		// (instructions - issue cycles), which was the first attempt and was
		// wrong: pipesim charges a cycle as a stall if anything in the machine
		// was blocked in it, including cycles that issued behind the blockage.
		// Subtracting those gave 2.15 instructions per slot, which no
		// two-issue machine can do, and inflated the dual-issue rate to 53%
		// against hardware's 26%.
		return cachesim::pipeTotals().parallelIssues;
	}
	case EV_FREEZE_CPU_REGISTER:
		// Flow-dependency stalls on a CPU register. pipesim classifies these by
		// which register file the waited-on value lives in, which is the same
		// line hardware draws between this counter and the FPU one below.
		return cachesim::pipeTotals().byReason[(int)pipesim::StallReason::FlowDep];
	case EV_FREEZE_FPU:
		return cachesim::pipeTotals().byReason[(int)pipesim::StallReason::FpuDep];
	default:
		if (!warned[mode & 0x3f])
		{
			warned[mode & 0x3f] = true;
			INFO_LOG(SH4, "pmcr: event mode 0x%02x has no model, reading zero", mode);
		}
		return 0;
	}
}

static u64 count(const Counter& ctr)
{
	if ((ctr.control & RUN) != RUN)
		return ctr.frozen;
	const u64 now = sourceValue(ctr.control & PMMODE);
	// A reset takes the counters back to zero under a running PMCR, so treat a
	// decrease as a new epoch rather than letting the subtraction wrap.
	return ctr.frozen + (now >= ctr.base ? now - ctr.base : 0);
}

bool isControlAddr(u32 addr)
{
	return addr == PMCR0_addr || addr == PMCR1_addr;
}

u16 readControl(u32 addr)
{
	return counters[addr == PMCR1_addr].control;
}

void writeControl(u32 addr, u16 value)
{
	Counter& ctr = counters[addr == PMCR1_addr];
	const bool wasRunning = (ctr.control & RUN) == RUN;
	const u16 oldMode = ctr.control & PMMODE;

	if (wasRunning)
	{
		// Bank the elapsed count before anything about the configuration can
		// change underneath it.
		const u64 now = sourceValue(oldMode);
		ctr.frozen += now >= ctr.base ? now - ctr.base : 0;
	}

	ctr.control = value & ~CLR;
	if (value & CLR)
		ctr.frozen = 0;

	if ((ctr.control & RUN) == RUN)
		ctr.base = sourceValue(ctr.control & PMMODE);
}

bool isCounterAddr(u32 addr)
{
	return addr == PMCTR0H_addr || addr == PMCTR0L_addr
		|| addr == PMCTR1H_addr || addr == PMCTR1L_addr;
}

u32 readCounter(u32 addr)
{
	const bool second = addr == PMCTR1H_addr || addr == PMCTR1L_addr;
	const bool high = addr == PMCTR0H_addr || addr == PMCTR1H_addr;
	const u64 v = count(counters[second]);
	// 48 bits wide on the SH4: the high register holds the top 16.
	return high ? (u32)((v >> 32) & 0xffff) : (u32)v;
}

void noteSleep(u64 cycles)
{
	sleepCycles += cycles;
}

void reset()
{
	memset(counters, 0, sizeof(counters));
	memset(warned, 0, sizeof(warned));
	sleepCycles = 0;
}

} // namespace pmcr
