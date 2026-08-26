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
#include "cachesim.h"
#include "cachesim_symbols.h"

#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_cache.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"

namespace cachesim
{

bool g_armed = false;
// Read at block compile time, so it must not change without the code cache
// being thrown away with it.
bool g_dataFeed = false;
// Charging modelled miss cycles to flycast's own timing changes what the guest
// does, so it is a separate switch from measuring, and off unless asked for.
bool g_timing = false;
static std::string reportPath;

struct State
{
	Model model;

	u64 frames = 0;
	// Guest cycles are accumulated here rather than read from the scheduler when
	// the report is written: by then the game has been unloaded and the
	// scheduler clock is back to zero.
	u64 totalCycles = 0;
	// Pipeline model totals for the measurement window. Kept apart from the
	// cache counters on purpose: these are issue and interlock cycles and
	// contain no cache cost, and the two are not added together anywhere.
	u64 pipeCyclesTotal = 0;
	u64 pipeStallsTotal = 0;
	u64 pipeByReason[(int)pipesim::StallReason::Count] = {};
	u64 pipeUnmodelledBlocks = 0;
	// Block entries. Every translated block ends in a control transfer, so this
	// is the dynarec's nearest equivalent of a taken-branch count.
	u64 blockExecTotal = 0;
	u64 lastSchedCycles = 0;
	u64 frameCycles = 0;

	Counters frameStart{};
	// Snapshot at the last logSummary(), so each line can report the window
	// since the previous one. A whole-run average hides that the miss rate
	// swings by more than an order of magnitude between workload phases.
	Counters lastLog{};
	u64 lastLogCycles = 0;

	std::deque<BlockTrace> blockPool;
	std::unordered_map<u64, u32> blockLookup;

	// Per-block execution and miss counts, indexed by block id. Kept as a
	// cumulative count plus a snapshot at the last frame, because the only
	// useful view is per frame: a total over a run is mostly loading and
	// compilation and describes no frame that ever happened.
	std::vector<u64> execCount;
	std::vector<u64> execAtFrame;
	std::vector<u64> missCount;
	std::vector<u64> missAtFrame;
	// Exponentially smoothed per-frame rates, so the panel is readable rather
	// than flickering with whatever one frame happened to do
	std::vector<double> execPerFrame;
	std::vector<double> missPerFrame;
	// Operand cache misses, attributed the same way as instruction misses: to
	// the block that was running when they happened.
	std::vector<u64> dataMissCount;
	std::vector<u64> dataMissAtFrame;
	std::vector<double> dataMissPerFrame;
	// Dirty line evictions, attributed the same way. Separate from the miss
	// count because they cost half again as much as the fill they ride along
	// with, and a workload can have plenty of one and none of the other.
	std::vector<u64> dataWbCount;
	std::vector<u64> dataWbAtFrame;
	std::vector<double> dataWbPerFrame;
	// Misses caused by a store, which cost differently and spacing-dependently.
	std::vector<u64> dataWrMissCount;
	std::vector<u64> dataWrMissAtFrame;
	std::vector<double> dataWrMissPerFrame;
	// Store queue flushes, attributed the same way: to the block that was
	// running when the queue drained.
	std::vector<u64> sqFlushCount;
	std::vector<u64> sqFlushAtFrame;
	std::vector<double> sqFlushPerFrame;
	// Cycles, accumulated at flush time rather than derived afterwards: the
	// cost depends on the block the flush happened in, and that is known then
	// and cheap to look up.
	std::vector<double> sqCycleCount;
	std::vector<double> sqCycleAtFrame;
	std::vector<double> sqCyclePerFrame;
	// The block currently executing, so a data access can be charged to it. The
	// dynarec calls traceBlock on entry, and a block runs to completion.
	u32 currentBlock = 0xffffffff;
	u32 currentPc = 0;
	// Accesses whose address could not be translated. Reported rather than
	// silently dropped: a feed that quietly loses accesses reads as a guest
	// that makes fewer of them.
	u64 dataTranslationFailures = 0;
	// Miss cycles are fractional - the penalty is 16.8, not 17 - and the SH4
	// cycle counter is an integer. The remainder is carried rather than
	// truncated, which over a frame is the difference between 16.8 and 16.
	double chargeRemainder = 0;
	u64 chargedCycles = 0;
	double smoothedFrameCycles = 0;

	u32 lookahead = 0;
	u32 skipFrames = 0;
	u32 measureFrames = 0;
	bool skipped = false;
	bool done = false;
};

static State& state()
{
	static State st;
	return st;
}

// Charges modelled stall cycles to the emulated SH4. Taken from the model's own
// cycle accumulator rather than from a miss count times a constant, so whatever
// penalty model is configured is the one that gets charged.
static void chargeCycles(State& st, double cycles)
{
	if (!g_timing || cycles <= 0)
		return;
	st.chargeRemainder += cycles;
	const int whole = (int)st.chargeRemainder;
	if (whole <= 0)
		return;
	st.chargeRemainder -= whole;
	st.chargedCycles += whole;
	Sh4cntx.cycle_counter -= whole;
}

//
// Feeds
//
// P2 and P4 are uncached, everything else is cached when the cache is enabled.
static bool instCacheable(u32 vaddr)
{
	if (!CCN_CCR.ICE)
		return false;
	const u32 area = vaddr >> 29;
	return area != 5 && area != 7;
}

static void fetchRange(u32 vaddr, u32 paddr, u32 bytes, u32 pc, u32 lookahead = 0)
{
	if (bytes == 0)
		return;
	State& st = state();
	// Lookahead bytes are fetched but not executed, so they must not inflate the
	// denominator of the miss rate
	st.model.countBlockFetch(paddr, bytes, lookahead);
	bytes += lookahead;

	if (!instCacheable(vaddr))
	{
		st.model.countUncached(Stream::Inst, (bytes + LINE_BYTES - 1) / LINE_BYTES);
		return;
	}

	// Physical addresses are normalised to 29 bits so that the same RAM reached
	// through P0, P1 and P3 is one line, which is how the hardware tag compare
	// behaves and how the associative shadow has to key its entries
	const u32 phys = paddr & 0x1fffffff;
	// With the MMU on, consecutive virtual lines are not necessarily consecutive
	// physical ones, so each line past the first is translated rather than
	// extrapolated. Guests that map their own address space - anything running
	// its own emulator or loader - cross pages often enough for this to matter.
	const bool translate = mmu_enabled();
	const bool iix = CCN_CCR.IIX;

	u32 vline = vaddr & ~(LINE_BYTES - 1);
	u32 pline = phys & ~(LINE_BYTES - 1);
	const u32 lastVline = (vaddr + bytes - 1) & ~(LINE_BYTES - 1);
	for (;;)
	{
		st.model.touchLine(Stream::Inst, Model::instIndex(vline, iix), pline, pc, false);
		if (vline == lastVline)
			break;
		vline += LINE_BYTES;
		if (!translate || (vline & 0xfff) != 0)
			pline += LINE_BYTES;
		else
		{
			// New page: look up where it actually lives
			u32 translated;
			if (mmu_instruction_translation(vline, translated) != MmuError::NONE)
				break;
			pline = (translated & 0x1fffffff) & ~(LINE_BYTES - 1);
		}
	}
}

void DYNACALL traceBlock(const BlockTrace *bt)
{
	State& st = state();
	// Stamped on every miss so bursts can be found in time, not only counted
	st.model.setCycle(st.totalCycles + (sh4_sched_now64() - st.lastSchedCycles));
	if (tracing())
		traceBlockExec(bt, st.model.currentCycle());

	if (bt->id >= st.execCount.size())
	{
		const size_t size = bt->id + 1;
		st.execCount.resize(size, 0);
		st.execAtFrame.resize(size, 0);
		st.missCount.resize(size, 0);
		st.missAtFrame.resize(size, 0);
		st.execPerFrame.resize(size, 0.0);
		st.missPerFrame.resize(size, 0.0);
		st.dataMissCount.resize(size, 0);
		st.dataMissAtFrame.resize(size, 0);
		st.dataMissPerFrame.resize(size, 0.0);
		st.dataWbCount.resize(size, 0);
		st.dataWbAtFrame.resize(size, 0);
		st.dataWbPerFrame.resize(size, 0.0);
		st.dataWrMissCount.resize(size, 0);
		st.dataWrMissAtFrame.resize(size, 0);
		st.dataWrMissPerFrame.resize(size, 0.0);
		st.sqFlushCount.resize(size, 0);
		st.sqFlushAtFrame.resize(size, 0);
		st.sqFlushPerFrame.resize(size, 0.0);
		st.sqCycleCount.resize(size, 0.0);
		st.sqCycleAtFrame.resize(size, 0.0);
		st.sqCyclePerFrame.resize(size, 0.0);
	}
	st.execCount[bt->id]++;
	st.blockExecTotal++;
	// Pipeline cycles are a property of the block, so this is a multiply, not
	// a measurement: no per-instruction work on the hot path.
	st.pipeCyclesTotal += bt->pipeCycles;
	st.pipeStallsTotal += bt->pipeStalls;
	for (int i = 0; i < (int)pipesim::StallReason::Count; i++)
		st.pipeByReason[i] += bt->pipeByReason[i];
	if (!bt->pipeModelled)
		st.pipeUnmodelledBlocks++;
	st.currentBlock = bt->id;
	st.currentPc = bt->vaddr;

	// Misses are attributed to the block that was fetching when they happened,
	// which is what makes them addable to a per-function cost
	const u64 before = st.model.counters().misses[(int)Stream::Inst];
	const double cyclesBefore = st.model.counters().missCycles[(int)Stream::Inst];
	fetchRange(bt->vaddr, bt->paddr, bt->size, bt->vaddr, st.lookahead);
	st.missCount[bt->id] += st.model.counters().misses[(int)Stream::Inst] - before;
	// Charged at block entry rather than spread across the block: the feed
	// replays the whole fetch stream in one call, so there is no finer moment
	// to attribute it to
	chargeCycles(st, st.model.counters().missCycles[(int)Stream::Inst] - cyclesBefore);
}

//
// Operand cache feed.
//
// One call per guest load or store, from the block the dynarec compiled while
// armed. Everything the hardware decides before the cache is consulted -
// whether the access is cacheable, whether the line is written back or written
// through - depends on the MMU page as well as CCR, so it is decoded here and
// the model is handed the answer. The decode is the one flycast's own operand
// cache implements in sh4_cache.h, deliberately: two decodes of the same
// hardware that disagree would be a bug nobody would see.
//
double sqFlushCost(const BlockTrace& bt, SqDest dest)
{
	const PenaltyConfig& p = state().model.penalty();
	(void)bt;	// spacing does not affect this - measured, see plan 9j
	return dest == SqDest::Ta ? p.sqFlushTa : p.sqFlushRam;
}

void sqFlush(u32 area)
{
	State& st = state();
	const SqDest dest = area == 4 ? SqDest::Ta
			: area == 3 ? SqDest::Ram : SqDest::Other;
	st.model.countSqFlush(dest);
	if (st.currentBlock < st.sqFlushCount.size())
	{
		st.sqFlushCount[st.currentBlock]++;
		st.sqCycleCount[st.currentBlock] +=
				sqFlushCost(st.blockPool[st.currentBlock], dest);
	}
}

void DYNACALL dataAccess(u32 vaddr, u32 packed)
{
	State& st = state();
	Model& model = st.model;
	const u32 size = packed & 0xff;
	const bool write = (packed & ACCESS_WRITE) != 0;
	// movca.l. Only meaningful on the first line an access touches: a straddling
	// access cannot be a movca.l, which is always one aligned longword.
	const bool allocate = (packed & ACCESS_ALLOCATE) != 0;

	model.countDataAccess();

	const u32 area = vaddr >> 29;
	bool cached = CCN_CCR.OCE && cachedArea(area);
	// P1 uses CCR.CB, everything else the inverse of CCR.WT
	bool copyBack = area == 4 ? (bool)CCN_CCR.CB : !CCN_CCR.WT;

	u32 paddr = vaddr;
	const bool translate = mmu_enabled() && translatedArea(area)
			&& (vaddr & 0xFC000000) != 0x7C000000;
	if (translate)
	{
		const TLB_Entry *entry;
		if (mmu_full_lookup(vaddr, &entry, paddr) != MmuError::NONE)
		{
			st.dataTranslationFailures++;
			return;
		}
		cached = cached && entry->Data.C;
		copyBack = copyBack && entry->Data.WT == 0;
	}
	if (!cached)
	{
		model.countUncached(Stream::Data, 1);
		return;
	}

	const bool oix = CCN_CCR.OIX;
	const bool ora = CCN_CCR.ORA;
	const u64 before = model.counters().misses[(int)Stream::Data];
	const u64 wbBefore = model.counters().writebacks;
	const u64 wmBefore = model.counters().writeMisses;
	const double cyclesBefore = model.counters().missCycles[(int)Stream::Data];

	// An access can straddle a line, and under an MMU the second line can live
	// on a different page, so it is translated rather than extrapolated.
	const u32 lastVline = (vaddr + (size == 0 ? 1 : size) - 1) & ~(LINE_BYTES - 1);
	u32 vline = vaddr & ~(LINE_BYTES - 1);
	u32 pline = (paddr & 0x1fffffff) & ~(LINE_BYTES - 1);
	for (;;)
	{
		const u32 index = Model::dataIndex(vline, oix, ora);
		// RAM mode takes half the cache out of service; those accesses are not
		// cache accesses at all
		if (!Model::dataIndexIsRam(index, ora))
			model.touchData(index, pline, st.currentPc, write, copyBack, allocate);
		if (vline == lastVline)
			break;
		vline += LINE_BYTES;
		if (!translate || (vline & 0xfff) != 0)
			pline += LINE_BYTES;
		else
		{
			const TLB_Entry *entry;
			u32 translated;
			if (mmu_full_lookup(vline, &entry, translated) != MmuError::NONE)
			{
				st.dataTranslationFailures++;
				break;
			}
			pline = (translated & 0x1fffffff) & ~(LINE_BYTES - 1);
		}
	}

	if (st.currentBlock < st.dataMissCount.size())
	{
		st.dataMissCount[st.currentBlock] +=
				model.counters().misses[(int)Stream::Data] - before;
		st.dataWbCount[st.currentBlock] += model.counters().writebacks - wbBefore;
		st.dataWrMissCount[st.currentBlock] +=
				model.counters().writeMisses - wmBefore;
	}
	chargeCycles(st, model.counters().missCycles[(int)Stream::Data] - cyclesBefore);
}

void traceFetch(u32 vaddr, u32 paddr, u32 bytes)
{
	fetchRange(vaddr, paddr, bytes, vaddr);
}

void invalidateInst()
{
	if (tracing())
		traceEvent(1, 0, 0);
	state().model.invalidateInst();
}

void writeInstAddressArray(u32 addr, u32 data)
{
	// Deriving the tag for an associative write needs the address translated,
	// which we only do for the untranslated case; an associative write under an
	// enabled MMU is left alone rather than guessed at.
	u32 assocTag = INVALID_LINE;
	if ((addr & 8) != 0 && !mmu_enabled())
		assocTag = ((data & ~0x3ffu) >> 10) & 0x7ffff;
	if (tracing())
		traceEvent(2, addr, data);
	state().model.writeInstAddressArray(addr, data, assocTag);
}

void invalidateData()
{
	state().model.invalidateData();
}

void frameBoundary()
{
	State& st = state();
	st.frames++;
	st.frameStart = st.model.counters();

	const u64 now = sh4_sched_now64();
	// A reset takes the scheduler clock back to zero, so a decrease is a new
	// epoch rather than time running backwards
	st.frameCycles = now >= st.lastSchedCycles ? now - st.lastSchedCycles : now;
	st.lastSchedCycles = now;
	st.totalCycles += st.frameCycles;

	if (tracing())
		traceEvent(3, 0, 0);

	// Per-frame rates, smoothed. A frame is the only window that means
	// anything here, and one frame on its own is too noisy to read.
	constexpr double alpha = 0.1;
	for (size_t i = 0; i < st.execCount.size(); i++)
	{
		const double execs = (double)(st.execCount[i] - st.execAtFrame[i]);
		const double misses = (double)(st.missCount[i] - st.missAtFrame[i]);
		const double dmisses = (double)(st.dataMissCount[i] - st.dataMissAtFrame[i]);
		const double sqf = (double)(st.sqFlushCount[i] - st.sqFlushAtFrame[i]);
		const double sqc = st.sqCycleCount[i] - st.sqCycleAtFrame[i];
		const double wbs = (double)(st.dataWbCount[i] - st.dataWbAtFrame[i]);
		const double wms = (double)(st.dataWrMissCount[i] - st.dataWrMissAtFrame[i]);
		st.execAtFrame[i] = st.execCount[i];
		st.missAtFrame[i] = st.missCount[i];
		st.dataMissAtFrame[i] = st.dataMissCount[i];
		st.sqFlushAtFrame[i] = st.sqFlushCount[i];
		st.sqCycleAtFrame[i] = st.sqCycleCount[i];
		st.dataWbAtFrame[i] = st.dataWbCount[i];
		st.dataWrMissAtFrame[i] = st.dataWrMissCount[i];
		st.execPerFrame[i] += alpha * (execs - st.execPerFrame[i]);
		st.missPerFrame[i] += alpha * (misses - st.missPerFrame[i]);
		st.dataMissPerFrame[i] += alpha * (dmisses - st.dataMissPerFrame[i]);
		st.sqFlushPerFrame[i] += alpha * (sqf - st.sqFlushPerFrame[i]);
		st.sqCyclePerFrame[i] += alpha * (sqc - st.sqCyclePerFrame[i]);
		st.dataWbPerFrame[i] += alpha * (wbs - st.dataWbPerFrame[i]);
		st.dataWrMissPerFrame[i] += alpha * (wms - st.dataWrMissPerFrame[i]);
	}
	st.smoothedFrameCycles += alpha * ((double)st.frameCycles - st.smoothedFrameCycles);

	if (st.skipFrames != 0 && !st.skipped && st.frames >= st.skipFrames)
	{
		// Startup is not steady state: drop everything measured so far
		reset();
		st.skipped = true;
		NOTICE_LOG(SH4, "cachesim: counters cleared after %u warm-up frames", st.skipFrames);
		if (!std::string(config::CacheSimTrace).empty())
			traceOpen(config::CacheSimTrace);
	}

	// The measurement window only starts once the warm-up has been skipped.
	// Without this, a smaller -cachesim-frames than -cachesim-skip silently
	// measures the boot instead of the game and reports it as a result: the
	// miss rate during loading is an order of magnitude away from steady state,
	// so the number looks plausible and is answering a different question.
	if (st.measureFrames != 0 && !st.done && (st.skipFrames == 0 || st.skipped)
			&& st.frames >= st.measureFrames)
	{
		st.done = true;
		logSummary();
		logProfile();
		logBlocks();
		if (!reportPath.empty())
			writeReport(reportPath);
		traceClose();
		return;
	}

	// Picks up the setting being toggled from the GUI while a game runs. The
	// hook only exists in blocks compiled while armed, so the code cache has to
	// go with it.
	// Timing feedback is a plain runtime check inside a hook that already
	// exists, so it can be toggled without recompiling anything
	g_timing = g_armed && config::CacheSimTiming;

	const bool dataFeedWanted = config::CacheSim && config::CacheSimData;
	if (config::CacheSim != g_armed || dataFeedWanted != g_dataFeed)
	{
		g_dataFeed = dataFeedWanted;
		setArmed(config::CacheSim);
		// setArmed does nothing when only the data feed changed, so the reset
		// has to be unconditional here
		reset();
		emu.getSh4Executor()->ResetCache();
	}
}

//
// Control
//
void init()
{
	reportPath = config::CacheSimReport;
	// The data hook is emitted into generated code and costs a call per guest
	// access, so it has to follow the master switch: leaving the operand cache
	// option ticked with the simulator off would slow the guest down for nothing
	g_dataFeed = config::CacheSim && config::CacheSimData;
	g_timing = config::CacheSim && config::CacheSimTiming;
	if (g_timing)
		// Loud, because from here on flycast is not emulating the same machine
		// it emulates with the option off, and any timing-sensitive result
		// taken from this run has to be read with that in mind
		NOTICE_LOG(SH4, "cachesim: charging modelled miss cycles to guest timing."
				" The guest will run differently than with this off.");
	setBlockLookahead(config::CacheSimLookahead);
	setSkipFrames(config::CacheSimSkipFrames);
	setMeasureFrames(config::CacheSimFrames);
	setArmed(config::CacheSim);
	if (g_armed)
	{
		// A disc image has no symbols of its own, so an explicit ELF wins and
		// otherwise one sitting beside the content is used. Either way it is
		// checked against the code that actually ran before any name is shown.
		const std::string explicitPath = config::CacheSimSymbols;
		if (!explicitPath.empty())
			loadSymbols(explicitPath);
		else
			discoverSymbols(settings.content.path);
	}
	// The trace is opened when the warm-up ends, not here: recording the boot
	// and compile storm would multiply the file size for a phase nobody wants
	// to sweep over
	if (g_armed && state().skipFrames == 0 && !std::string(config::CacheSimTrace).empty())
		traceOpen(config::CacheSimTrace);
}

void setArmed(bool on)
{
	State& st = state();
	if (g_armed == on)
		return;
	g_armed = on;
	if (on)
		st.model.setMissRingSize(65536);
	reset();
}

void reset()
{
	State& st = state();
	st.model.reset();
	st.frames = 0;
	st.totalCycles = 0;
	st.pipeCyclesTotal = 0;
	st.pipeStallsTotal = 0;
	memset(st.pipeByReason, 0, sizeof(st.pipeByReason));
	st.pipeUnmodelledBlocks = 0;
	st.blockExecTotal = 0;
	// The scheduler clock does not restart with us. Zeroing this instead of
	// sampling it makes the next frame boundary charge everything that happened
	// before the reset to a single frame, which silently inflates the guest
	// cycle denominator of every rate in the report - by a factor of six on a
	// run that skips 1500 frames and measures 300.
	st.lastSchedCycles = sh4_sched_now64();
	st.frameCycles = 0;
	st.frameStart = Counters{};
	st.lastLog = Counters{};
	st.lastLogCycles = 0;
	st.skipped = false;
	std::fill(st.execCount.begin(), st.execCount.end(), 0);
	std::fill(st.execAtFrame.begin(), st.execAtFrame.end(), 0);
	std::fill(st.missCount.begin(), st.missCount.end(), 0);
	std::fill(st.missAtFrame.begin(), st.missAtFrame.end(), 0);
	std::fill(st.execPerFrame.begin(), st.execPerFrame.end(), 0.0);
	std::fill(st.missPerFrame.begin(), st.missPerFrame.end(), 0.0);
	std::fill(st.dataMissCount.begin(), st.dataMissCount.end(), 0);
	std::fill(st.dataMissAtFrame.begin(), st.dataMissAtFrame.end(), 0);
	std::fill(st.dataMissPerFrame.begin(), st.dataMissPerFrame.end(), 0.0);
	std::fill(st.sqFlushCount.begin(), st.sqFlushCount.end(), 0);
	std::fill(st.sqFlushAtFrame.begin(), st.sqFlushAtFrame.end(), 0);
	std::fill(st.sqFlushPerFrame.begin(), st.sqFlushPerFrame.end(), 0.0);
	std::fill(st.sqCycleCount.begin(), st.sqCycleCount.end(), 0.0);
	std::fill(st.sqCycleAtFrame.begin(), st.sqCycleAtFrame.end(), 0.0);
	std::fill(st.sqCyclePerFrame.begin(), st.sqCyclePerFrame.end(), 0.0);
	std::fill(st.dataWbCount.begin(), st.dataWbCount.end(), 0);
	std::fill(st.dataWbAtFrame.begin(), st.dataWbAtFrame.end(), 0);
	std::fill(st.dataWbPerFrame.begin(), st.dataWbPerFrame.end(), 0.0);
	std::fill(st.dataWrMissCount.begin(), st.dataWrMissCount.end(), 0);
	std::fill(st.dataWrMissAtFrame.begin(), st.dataWrMissAtFrame.end(), 0);
	std::fill(st.dataWrMissPerFrame.begin(), st.dataWrMissPerFrame.end(), 0.0);
	st.dataTranslationFailures = 0;
	st.chargeRemainder = 0;
	st.chargedCycles = 0;
	st.smoothedFrameCycles = 0;
}

void term()
{
	if (g_armed && !reportPath.empty() && !state().done)
		writeReport(reportPath);
	traceClose();
}

void setPenaltyConfig(const PenaltyConfig& cfg) { state().model.setPenalty(cfg); }
const PenaltyConfig& penaltyConfig() { return state().model.penalty(); }
void setBlockLookahead(u32 bytes) { state().lookahead = bytes; }
u32 blockLookahead() { return state().lookahead; }
void setSkipFrames(u32 frames) { state().skipFrames = frames; }
void setMeasureFrames(u32 frames) { state().measureFrames = frames; }
bool finished() { return state().done; }
u64 chargedTimingCycles() { return state().chargedCycles; }
void setMissRingSize(size_t records) { state().model.setMissRingSize(records); }
void setReportPath(const std::string& path) { reportPath = path; }

//
// Block pool
//
// Read a block's guest instructions out into a buffer. Both the hash and the
// pipeline analysis need the same bytes, and reading them twice through
// addrspace on an unmapped block is not free.
static void readGuestCode(u32 paddr, u32 size, std::vector<u8>& out)
{
	out.resize(size);
	const u8 *mem = GetMemPtr(paddr, size);
	if (mem != nullptr)
		memcpy(out.data(), mem, size);
	else
		for (u32 i = 0; i < size; i++)
			out[i] = (u8)addrspace::read8(paddr + i);
}

// Run the pipeline model over a block's instructions, once, at compile time.
static void analyzeBlockPipeline(const std::vector<u8>& code, BlockTrace& bt)
{
	const u32 count = (u32)(code.size() / 2);
	bt.pipeCycles = 0;
	bt.pipeStalls = 0;
	memset(bt.pipeByReason, 0, sizeof(bt.pipeByReason));
	bt.pipeModelled = false;
	bt.prefCount = 0;
	if (count == 0)
		return;

	std::vector<u16> ops(count);
	for (u32 i = 0; i < count; i++)
		ops[i] = (u16)(code[i * 2] | (code[i * 2 + 1] << 8));

	// The block is measured back to back with itself rather than in isolation,
	// so what comes out is the steady-state cost of running it again - the same
	// quantity the hardware test measures, and the one that multiplies by an
	// execution count. Analysing it once instead would include the cycles it
	// takes to fill an empty pipeline, which a loop pays once and not per
	// iteration.
	std::vector<u16> once = ops;
	std::vector<u16> twice = ops;
	twice.insert(twice.end(), ops.begin(), ops.end());

	pipesim::Result a = pipesim::analyze(once.data(), (u32)once.size());
	pipesim::Result b = pipesim::analyze(twice.data(), (u32)twice.size());
	if (a.stuck || b.stuck || b.cycles < a.cycles)
		return;

	bt.pipeCycles = b.cycles - a.cycles;
	bt.pipeStalls = b.stallCycles > a.stallCycles ? b.stallCycles - a.stallCycles : 0;
	for (int i = 0; i < (int)pipesim::StallReason::Count; i++)
		bt.pipeByReason[i] = (u16)std::min<u32>(0xffff,
				b.byReason[i] > a.byReason[i] ? b.byReason[i] - a.byReason[i] : 0);
	bt.pipeModelled = pipesim::fullyModelled(ops.data(), count);

	// pref @Rn is 0000nnnn10000011. Counted here because the bytes are already
	// in hand; nothing decodes at runtime.
	for (u32 i = 0; i < count; i++)
		if ((ops[i] & 0xF0FF) == 0x0083)
			bt.prefCount++;
}


const BlockTrace *traceForBlock(u32 vaddr, u32 paddr, u32 size, u32 guestCycles)
{
	State& st = state();
	std::vector<u8> code;
	readGuestCode(paddr, size, code);
	const u64 hash = hashCode(code.data(), size);
	const u64 key = hash ^ ((u64)paddr << 24) ^ size;

	auto [it, inserted] = st.blockLookup.insert({ key, (u32)st.blockPool.size() });
	if (!inserted)
	{
		const BlockTrace& existing = st.blockPool[it->second];
		if (existing.vaddr == vaddr && existing.paddr == paddr && existing.size == size
				&& existing.hash == hash)
			return &existing;
		// Key collision: fall through and add a second descriptor, which costs
		// a pool slot and nothing else
	}
	// std::deque so that the raw pointers baked into compiled code stay valid
	// as the pool grows
	st.blockPool.push_back({ vaddr, paddr, size, (u32)st.blockPool.size(), guestCycles,
			0, 0, {}, false, hash });
	analyzeBlockPipeline(code, st.blockPool.back());
	return &st.blockPool.back();
}

double blockExecsPerFrame(u32 id)
{
	const State& st = state();
	return id < st.execPerFrame.size() ? st.execPerFrame[id] : 0.0;
}

double blockSqFlushesPerFrame(u32 id)
{
	const State& st = state();
	return id < st.sqFlushPerFrame.size() ? st.sqFlushPerFrame[id] : 0.0;
}

double blockSqCyclesPerFrame(u32 id)
{
	const State& st = state();
	return id < st.sqCyclePerFrame.size() ? st.sqCyclePerFrame[id] : 0.0;
}

const std::deque<BlockTrace>& blocks() { return state().blockPool; }

PipeTotals pipeTotals()
{
	State& st = state();
	PipeTotals t{};
	t.cycles = st.pipeCyclesTotal;
	t.stalls = st.pipeStallsTotal;
	for (int i = 0; i < (int)pipesim::StallReason::Count; i++)
		t.byReason[i] = st.pipeByReason[i];
	t.unmodelledBlockExecs = st.pipeUnmodelledBlocks;
	return t;
}

//
// Results
//
const Counters& counters() { return state().model.counters(); }
u64 blockExecs() { return state().blockExecTotal; }
u64 frameCount() { return state().frames; }
u64 guestCycles() { return state().totalCycles; }
u64 frameGuestCycles() { return state().frameCycles; }

static Counters since(const Counters& base)
{
	Counters delta = state().model.counters();
	delta.instFetched -= base.instFetched;
	delta.fetchOps -= base.fetchOps;
	delta.invalidations -= base.invalidations;
	for (int d = 0; d < (int)SqDest::Count; d++)
		delta.sqFlushes[d] -= base.sqFlushes[d];
	for (int s = 0; s < (int)Stream::Count; s++)
	{
		delta.lineTouches[s] -= base.lineTouches[s];
		delta.misses[s] -= base.misses[s];
		delta.uncachedAccesses[s] -= base.uncachedAccesses[s];
		delta.missCycles[s] -= base.missCycles[s];
		for (int k = 0; k < (int)MissKind::Count; k++)
			delta.missKinds[s][k] -= base.missKinds[s][k];
	}
	return delta;
}

Counters frameCounters() { return since(state().frameStart); }
Counters logWindow() { return since(state().lastLog); }
u64 logWindowCycles() { return state().totalCycles - state().lastLogCycles; }

void markLogWindow()
{
	State& st = state();
	st.lastLog = st.model.counters();
	st.lastLogCycles = st.totalCycles;
}

double derivedMissCycles(Stream stream)
{
	return state().model.counters().missCycles[(int)stream];
}

const SetStat *setStats(Stream stream)
{
	return state().model.cacheFor(stream).stats.data();
}

std::vector<EvictPair> setEvictors(Stream stream, u32 set)
{
	return state().model.setEvictors(stream, set);
}

std::vector<SiteStat> topSites(Stream stream, size_t limit)
{
	return state().model.topSites(stream, limit);
}

std::vector<MissRecord> recentMisses()
{
	return state().model.recentMisses();
}

//
// Profile
//
double profileFrameCycles()
{
	return state().smoothedFrameCycles;
}

double profileAccountedCycles()
{
	const State& st = state();
	double total = 0;
	for (size_t i = 0; i < st.execPerFrame.size(); i++)
		total += st.execPerFrame[i] * st.blockPool[i].guestCycles;
	return total;
}

std::vector<ProfileRow> profile(size_t limit)
{
	const State& st = state();
	const double cyclesPerMiss = st.model.penalty().fixedCycles;
	// Measured separately on hardware: a data fill is cheaper than an
	// instruction fill, and a dirty eviction costs half again on top. See 9h.
	const double cyclesPerDataMiss = st.model.penalty().dataFillCycles;
	(void)st.model.penalty().writebackCycles;	// superseded, see 9j

	// Group by symbol where there is one. Everything else is generated code or
	// a binary with no symbols, and gets grouped by the region it lives in:
	// naming it after the nearest symbol would be a guess presented as a fact.
	std::unordered_map<std::string, ProfileRow> rows;
	for (size_t i = 0; i < st.execPerFrame.size(); i++)
	{
		if (st.execPerFrame[i] < 0.01)
			continue;
		const BlockTrace& block = st.blockPool[i];
		const char *symbol = symbolsLoaded() ? symbolFor(block.vaddr) : nullptr;

		char label[64];
		if (symbol == nullptr)
			// 64 KB granularity: enough to separate a JIT buffer from a loader
			// from static code, without inventing detail we do not have
			std::snprintf(label, sizeof(label), "%08x-%08x",
					block.vaddr & ~0xffffu, (block.vaddr & ~0xffffu) + 0x10000);

		ProfileRow& row = rows[symbol != nullptr ? symbol : label];
		if (row.name.empty())
		{
			row.name = symbol != nullptr ? symbol : label;
			row.named = symbol != nullptr;
			row.start = block.vaddr;
			row.end = block.vaddr + block.size;
			row.pipeComplete = true;
		}
		row.start = std::min(row.start, block.vaddr);
		row.end = std::max(row.end, block.vaddr + block.size);
		row.calls += st.execPerFrame[i];
		row.cycles += st.execPerFrame[i] * block.guestCycles;
		row.missCycles += st.missPerFrame[i] * cyclesPerMiss;
		// Read misses cost a flat fill. Write misses cost
		//     max(floor, drain - gap)
		// where the gap is how far apart this block's write misses are - its
		// cycle count divided by how many it does per execution. Both measured;
		// see plan 9j.
		const double blockExecs = st.execPerFrame[i];
		const double wrMiss = st.dataWrMissPerFrame[i];
		const double rdMiss = st.dataMissPerFrame[i] > wrMiss
				? st.dataMissPerFrame[i] - wrMiss : 0.0;
		row.dataMissCycles += rdMiss * cyclesPerDataMiss;
		if (wrMiss > 0.0)
		{
			const double perExec = blockExecs > 0.0 ? wrMiss / blockExecs : wrMiss;
			// No block cycle count, or fewer than one write miss per execution,
			// means they are far enough apart that the buffer keeps up.
			const double gap = perExec >= 1.0 && block.pipeCycles > 0
					? (double)block.pipeCycles / perExec
					: st.model.penalty().writeMissDrain;
			const double cost = std::max(st.model.penalty().writeMissFloor,
					st.model.penalty().writeMissDrain - gap);
			row.dataMissCycles += wrMiss * cost;
		}
		row.sqFlushes += st.sqFlushPerFrame[i];
		row.sqCycles += st.sqCyclePerFrame[i];

		// Pipeline cycles are per execution and already steady-state, so this
		// is the same multiply the run totals use.
		const double execs = st.execPerFrame[i];
		row.pipeCycles += execs * block.pipeCycles;
		row.pipeFlowDep += execs * block.pipeByReason[(int)pipesim::StallReason::FlowDep];
		row.pipeResource += execs * block.pipeByReason[(int)pipesim::StallReason::ResourceHazard];
		row.pipeStage += execs * (block.pipeByReason[(int)pipesim::StallReason::StageFull]
				+ block.pipeByReason[(int)pipesim::StallReason::StageLocked]);
		// The two remaining reasons are folded in rather than given columns of
		// their own, so the breakdown still adds up to the total exactly.
		// OutputDep is a dependency stall - write-after-write - and the advice
		// for it is the same as for a flow dependency, so it joins that.
		// PrevStalled is a structural knock-on from something ahead in the
		// pipeline, so it joins the structural column.
		row.pipeFlowDep += execs * block.pipeByReason[(int)pipesim::StallReason::OutputDep];
		row.pipeStage += execs * block.pipeByReason[(int)pipesim::StallReason::PrevStalled];
		row.pipeOtherStall = 0.0;
		// Whatever was not spent stalling was spent issuing. Derived rather
		// than counted so the parts cannot drift from the total.
		row.pipeIssue += execs * (block.pipeCycles - block.pipeStalls);
		if (!block.pipeModelled)
			row.pipeComplete = false;
	}

	std::vector<ProfileRow> out;
	out.reserve(rows.size());
	for (auto& entry : rows)
		out.push_back(std::move(entry.second));
	// Rank by the pipeline model where it produced anything, since that is the
	// figure with hardware behind it, and fall back to flycast's own estimate
	// otherwise. Instruction cache cycles are added: a row can be cheap to
	// issue and expensive to fetch, and the penalty constant survives contact
	// with hardware - 16.8 modelled against 26.3 measured on bruces_balls, the
	// same ballpark as the 16.4 to 17.5 measured on other workloads.
	//
	// Store queue cycles are added too. They are a counted event rather than a
	// modelled one - the hook sits on the flush - and on a workload that feeds
	// the tile accelerator they are the second largest term after issue: 77,100
	// flushes per frame at 3.6 cycles is 8% of the frame, and before this was
	// hooked none of it appeared anywhere.
	//
	// Operand cache cycles ARE added, as of the miss-count validation in 9g.
	// They were excluded on two grounds and both turned out to be wrong: the
	// 1,483 cycles per miss that made line fills look absurd was the store
	// queue, now charged above; and the "model finds half the misses hardware
	// does" was an invalid comparison - different binaries, on a workload where
	// misses are 0.06% of a frame. Measured against a known-geometry walk the
	// miss count is exact to 0.5%. See phase 9g in docs/cachesim/plan.md.
	std::sort(out.begin(), out.end(), [](const ProfileRow& a, const ProfileRow& b) {
		const double ta = (a.pipeCycles > 0.0 ? a.pipeCycles : a.cycles)
				+ a.missCycles + a.dataMissCycles + a.sqCycles;
		const double tb = (b.pipeCycles > 0.0 ? b.pipeCycles : b.cycles)
				+ b.missCycles + b.dataMissCycles + b.sqCycles;
		return ta > tb;
	});
	if (out.size() > limit)
		out.resize(limit);
	return out;
}

} // namespace cachesim
