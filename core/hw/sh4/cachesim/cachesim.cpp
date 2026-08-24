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
	// The block currently executing, so a data access can be charged to it. The
	// dynarec calls traceBlock on entry, and a block runs to completion.
	u32 currentBlock = 0xffffffff;
	u32 currentPc = 0;
	// Accesses whose address could not be translated. Reported rather than
	// silently dropped: a feed that quietly loses accesses reads as a guest
	// that makes fewer of them.
	u64 dataTranslationFailures = 0;
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
	}
	st.execCount[bt->id]++;
	st.currentBlock = bt->id;
	st.currentPc = bt->vaddr;

	// Misses are attributed to the block that was fetching when they happened,
	// which is what makes them addable to a per-function cost
	const u64 before = st.model.counters().misses[(int)Stream::Inst];
	fetchRange(bt->vaddr, bt->paddr, bt->size, bt->vaddr, st.lookahead);
	st.missCount[bt->id] += st.model.counters().misses[(int)Stream::Inst] - before;
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
void DYNACALL dataAccess(u32 vaddr, u32 packed)
{
	State& st = state();
	Model& model = st.model;
	const u32 size = packed & 0xff;
	const bool write = (packed & 0x100) != 0;

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
			model.touchData(index, pline, st.currentPc, write, copyBack);
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
		st.dataMissCount[st.currentBlock] +=
				model.counters().misses[(int)Stream::Data] - before;
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
		st.execAtFrame[i] = st.execCount[i];
		st.missAtFrame[i] = st.missCount[i];
		st.dataMissAtFrame[i] = st.dataMissCount[i];
		st.execPerFrame[i] += alpha * (execs - st.execPerFrame[i]);
		st.missPerFrame[i] += alpha * (misses - st.missPerFrame[i]);
		st.dataMissPerFrame[i] += alpha * (dmisses - st.dataMissPerFrame[i]);
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
		if (!reportPath.empty())
			writeReport(reportPath);
		traceClose();
		return;
	}

	// Picks up the setting being toggled from the GUI while a game runs. The
	// hook only exists in blocks compiled while armed, so the code cache has to
	// go with it.
	if (config::CacheSim != g_armed || (g_armed && config::CacheSimData != g_dataFeed))
	{
		g_dataFeed = config::CacheSimData;
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
	g_dataFeed = config::CacheSimData;
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
	st.lastSchedCycles = 0;
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
	st.dataTranslationFailures = 0;
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
void setMissRingSize(size_t records) { state().model.setMissRingSize(records); }
void setReportPath(const std::string& path) { reportPath = path; }

//
// Block pool
//
static u64 hashGuestCode(u32 paddr, u32 size)
{
	const u8 *mem = GetMemPtr(paddr, size);
	if (mem != nullptr)
		return hashCode(mem, size);
	// Not directly mapped: copy it out so the hash is computed over the same
	// bytes, by the same function, as anything checking a binary against it
	std::vector<u8> bytes(size);
	for (u32 i = 0; i < size; i++)
		bytes[i] = (u8)addrspace::read8(paddr + i);
	return hashCode(bytes.data(), size);
}

const BlockTrace *traceForBlock(u32 vaddr, u32 paddr, u32 size, u32 guestCycles)
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
	st.blockPool.push_back({ vaddr, paddr, size, (u32)st.blockPool.size(), guestCycles, hash });
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
		}
		row.start = std::min(row.start, block.vaddr);
		row.end = std::max(row.end, block.vaddr + block.size);
		row.calls += st.execPerFrame[i];
		row.cycles += st.execPerFrame[i] * block.guestCycles;
		row.missCycles += st.missPerFrame[i] * cyclesPerMiss;
		row.dataMissCycles += st.dataMissPerFrame[i] * cyclesPerMiss;
	}

	std::vector<ProfileRow> out;
	out.reserve(rows.size());
	for (auto& entry : rows)
		out.push_back(std::move(entry.second));
	std::sort(out.begin(), out.end(), [](const ProfileRow& a, const ProfileRow& b) {
		return a.cycles + a.missCycles + a.dataMissCycles
				> b.cycles + b.missCycles + b.dataMissCycles;
	});
	if (out.size() > limit)
		out.resize(limit);
	return out;
}

} // namespace cachesim
