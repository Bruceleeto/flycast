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

#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"

namespace cachesim
{

bool g_armed = false;
static std::string reportPath;

struct State
{
	Model model;

	u64 frames = 0;
	// Guest cycles are accumulated here rather than read from the scheduler when
	// the report is written: by then the game has been unloaded and the
	// scheduler clock is back to zero.
	u64 totalCycles = 0;
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
	fetchRange(bt->vaddr, bt->paddr, bt->size, bt->vaddr, st.lookahead);
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

	if (st.skipFrames != 0 && !st.skipped && st.frames >= st.skipFrames)
	{
		// Startup is not steady state: drop everything measured so far
		reset();
		st.skipped = true;
		NOTICE_LOG(SH4, "cachesim: counters cleared after %u warm-up frames", st.skipFrames);
		if (!std::string(config::CacheSimTrace).empty())
			traceOpen(config::CacheSimTrace);
	}

	if (st.measureFrames != 0 && !st.done && st.frames >= st.measureFrames)
	{
		st.done = true;
		logSummary();
		if (!reportPath.empty())
			writeReport(reportPath);
		traceClose();
		return;
	}

	// Picks up the setting being toggled from the GUI while a game runs. The
	// hook only exists in blocks compiled while armed, so the code cache has to
	// go with it.
	if (config::CacheSim != g_armed)
	{
		setArmed(config::CacheSim);
		emu.getSh4Executor()->ResetCache();
	}
}

//
// Control
//
void init()
{
	reportPath = config::CacheSimReport;
	setBlockLookahead(config::CacheSimLookahead);
	setSkipFrames(config::CacheSimSkipFrames);
	setMeasureFrames(config::CacheSimFrames);
	setArmed(config::CacheSim);
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
	st.lastSchedCycles = 0;
	st.frameCycles = 0;
	st.frameStart = Counters{};
	st.lastLog = Counters{};
	st.lastLogCycles = 0;
	st.skipped = false;
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
void setMissRingSize(size_t records) { state().model.setMissRingSize(records); }
void setReportPath(const std::string& path) { reportPath = path; }

//
// Block pool
//
// FNV-1a over the guest instruction bytes. This is the identity of a block:
// code living in a guest JIT buffer lands at a different address every run, so
// its address can be used for layout arithmetic but never for naming it or
// diffing it against another run.
static u64 hashGuestCode(u32 paddr, u32 size)
{
	u64 hash = 0xcbf29ce484222325ull;
	const u8 *mem = GetMemPtr(paddr, size);
	for (u32 i = 0; i < size; i++)
	{
		const u8 byte = mem != nullptr ? mem[i] : (u8)addrspace::read8(paddr + i);
		hash = (hash ^ byte) * 0x100000001b3ull;
	}
	return hash;
}

const BlockTrace *traceForBlock(u32 vaddr, u32 paddr, u32 size)
{
	State& st = state();
	const u64 hash = hashGuestCode(paddr, size);
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
	st.blockPool.push_back({ vaddr, paddr, size, (u32)st.blockPool.size(), hash });
	return &st.blockPool.back();
}

const std::deque<BlockTrace>& blocks() { return state().blockPool; }

//
// Results
//
const Counters& counters() { return state().model.counters(); }
u64 frameCount() { return state().frames; }
u64 guestCycles() { return state().totalCycles; }
u64 frameGuestCycles() { return state().frameCycles; }

static Counters since(const Counters& base)
{
	Counters delta = state().model.counters();
	delta.instFetched -= base.instFetched;
	delta.fetchOps -= base.fetchOps;
	delta.invalidations -= base.invalidations;
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

} // namespace cachesim
