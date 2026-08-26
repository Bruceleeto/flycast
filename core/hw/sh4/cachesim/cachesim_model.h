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
//
// The SH4 cache model itself, with no dependency on flycast.
//
// This header is compiled both into flycast, where it is fed live by the
// dynarec, and into tools/cachesweep, which replays a recorded trace through
// it under a modified code layout. Those two must never be separate
// implementations: a sweep that optimises against a model which has drifted
// from the one that produced the measurement is worse than no sweep at all.
//
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cachesim
{

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr u32 LINE_BYTES = 32;
constexpr u32 LINE_SHIFT = 5;
constexpr u32 IC_SETS = 256;	// 8 KB, direct mapped
constexpr u32 OC_SETS = 512;	// 16 KB, direct mapped
constexpr u32 INVALID_LINE = 0xFFFFFFFF;

enum class MissKind : u8
{
	Compulsory,	// direct-mapped miss, fully-associative miss, line never seen
	Capacity,	// line seen before and an equally large associative cache would
			// have lost it too
	Conflict,	// direct-mapped miss but the fully-associative shadow hit: the
			// line was still resident in an equally large associative cache, so
			// a different code layout would have kept it. The only kind a layout
			// change can remove
	Invalidated,	// the line was resident until the guest flushed the cache.
			// Kept separate because a guest that invalidates often would
			// otherwise pile these into Capacity and point a layout search at
			// something layout cannot fix
	Count
};

enum class Stream : u8
{
	Inst,
	Data,
	Count
};

struct MissRecord
{
	u32 pc;
	u32 line;
	u32 evictedLine;	// what this fetch threw out, or INVALID_LINE
	u32 set;
	u64 cycle;
	Stream stream;
	bool write;
	MissKind kind;
};

enum class PenaltyModel : u8
{
	Fixed,
	RowAware,
	Count
};

struct PenaltyConfig
{
	PenaltyModel model = PenaltyModel::Fixed;
	// Instruction cache line fill. 25.7 cycles, not the 16.8 that
	// `tools/hwprobe` measures, because hwprobe measures an idle bus. A miss
	// costs what the memory system charges at the moment it happens, and
	// during a frame the same SDRAM is serving PVR texture reads, TA list
	// writes and refresh. Two examples with nothing in common - texture2d
	// (sprite blitter, 200 frames) and pvr_dma (transformed 3D, 200 frames) -
	// put the loaded cost at 25.2 and 26.2 cycles per miss, against a
	// hwprobe-derived 16.8 in both. The spread between the two is 4%; the gap
	// to the isolated figure is 53%, so this is contention, not workload.
	//
	// 16.8 is still the right number for an isolated miss and is what an
	// unloaded probe will keep reproducing. A single constant cannot be both,
	// and the profiler exists to explain frames, so it is fitted to frames.
	// The proper fix is a penalty that knows about bus occupancy, which is
	// what PenaltyModel::RowAware was a first sketch at. See validation/.
	double fixedCycles = 27.2;
	// Operand cache miss, measured separately from the instruction side because
	// there was never a reason to believe they were the same number and they
	// are not. Charged to every miss, read or write.
	//
	// 20.6 cycles, from PMCR 0x25 across texture2d and pvr_dma. The fill alone
	// (0x22) is 20.05 and 20.75 in those two, so the bus cost is flat and this
	// is very close to being just the fill - the rest of the data-side freeze
	// is on the write half, below.
	//
	// Not the 14.3 that `build/occost` reports for an isolated read miss, for
	// the same reason fixedCycles is not 16.8: an isolated probe has the bus
	// to itself.
	double dataFillCycles = 21.0;
	// What a WRITE miss costs on top of the fill: the dirty line it displaces
	// has to go out. 15.2 cycles.
	//
	// Flat, which was the surprise. `tools/hwprobe/wbsweep` fits an isolated
	// burst to max(7.0, 42.2 - spacing) - the write-back buffer holds one line
	// (SHC_PM 4.3.4) and drains on the B-clock, so an isolated eviction is
	// hidden and a stream of them is not. Over a whole frame that resolves to a
	// constant: two workloads whose write share of operand misses differs by
	// 2.7x (0.434 and 0.163) give 16.49 and 14.30 cycles of extra freeze per
	// write miss. The spacing model is not wrong, it is just not what a frame
	// averages out to, and the profiler reports frames.
	//
	// Note the sign. The old model had write misses costing LESS than reads
	// (floor 7.0 against a 14.3 fill) on the grounds that a write does not wait
	// for its data. Hardware says they cost about 75% more, because waiting for
	// the data was never the expensive part.
	double writeMissExtra = 13.0;
	double rowHitCycles = 14.0;
	double rowMissCycles = 24.0;
	u32 rowShift = 12;
	// Superseded by writeMissExtra; kept at zero so nothing charges twice.
	double writebackCycles = 0.0;
	// Cycles the CPU actually stalls per 32-byte store queue flush.
	//
	// This is NOT the transfer time. A flush is asynchronous: the CPU only
	// freezes when the next flush finds the queue still draining, and real
	// code alternates SQ0 and SQ1 so most of the drain overlaps with work.
	// So this is the residual stall, and it depends on how much work the
	// guest puts between flushes.
	//
	// 3.6 is measured: a Dreamcast running bruces_balls freezes 278,207
	// cycles per frame on the data-cache counter, against 77,100 flushes per
	// frame counted here, and its 188 operand cache misses account for about
	// 1% of that. Serialised with no work in between it is 11.1 (the RAM
	// figure from the sqprobe harness), which is the upper bound.
	// Store queue flush, per destination. FLAT: `tools/hwprobe/sqsweep` swept
	// the gap between flushes from 16 to 205 cycles and the data-side freeze
	// did not move - 2.0 cycles per flush at every spacing, to both RAM and the
	// TA. The queue drains fast enough that the CPU never catches up with it,
	// so there is nothing for a spacing model to represent.
	//
	// This replaced a spacing model built on the assumption that it would, and
	// the assumption survived exactly as long as it took to measure.
	//
	// Both destinations measure the same, and it does not move when the burst
	// is lengthened from 64 flushes to 2048 either - so this is not the TA's
	// input FIFO absorbing a short burst.
	//
	// It does NOT reconcile with `bruces_balls`, where total data-side freeze
	// over total flushes is 3.49 - 47,464,532 cycles of 0x25 over 13,440,600
	// flushes, 200 frames. At 2.0 per flush that workload has about 100,000
	// cycles a frame of freeze coming from somewhere else.
	//
	// Two candidates are now ruled out, which is as far as this has got:
	//
	//   - Not cache misses. 24,786 operand misses in the whole run, which at
	//     the measured cost is 0.6M of the 47.5M.
	//   - Not the fill. PMCR 0x22 reads 506,770 cycles against 47.5M of freeze,
	//     so 99% of that freeze has no bus fill behind it at all. An earlier
	//     note here guessed write misses; there are 2,111 of them, worth 32K.
	//
	// And it is NOT a per-flush cost, which is the thing that settles what to
	// do about it. Subtract the operand-miss model from each example's
	// data-side freeze and divide the remainder by its flush count:
	//
	//     texture2d      641,529 flushes    residual     31,786    0.05/flush
	//     bruces_balls 13,440,600 flushes    residual 46,921,853    3.49/flush
	//
	// Two loaded workloads, 70x apart. Raising this constant to 3.49 would fix
	// bruces_balls and put 2.2M cycles into texture2d where hardware measures
	// 32K. Whatever the missing term is, flush count is not what it scales
	// with, so no value of this constant can express it.
	//
	// The swept 2.0 stays. It is the only figure here that was measured against
	// a controlled sweep rather than inferred from one workload's total, and
	// the shortfall stays visible as `dcache_stall_cycles` reading 0.57 on
	// bruces_balls rather than being absorbed into a constant that would then
	// be wrong everywhere else. Still open - see plan 9j.
	double sqFlushTa = 3.45;
	double sqFlushRam = 2.0;
};

// Where a store queue flush landed. The SH4 drains a 32-byte queue to an
// external bus, and the cost depends entirely on the destination: RAM behaves
// like a burst write, the TA can back up behind the tile accelerator. Not a
// cache stream - store queue writes bypass the operand cache entirely, which
// hardware confirms (a flush to RAM produces exactly zero operand cache
// misses), so they are counted separately rather than folded into Data.
enum class SqDest : u8
{
	Ram,		// area 3, system RAM
	Ta,			// area 4, tile accelerator / PVR
	Other,
	Count
};

struct Counters
{
	// Store queue flushes, one per 32 bytes, split by destination. Counts only:
	// no cycles are charged yet, because the per-flush cost is still being
	// measured on hardware. See plan.md 9e.
	u64 sqFlushes[(int)SqDest::Count];
	u64 instFetched;
	// 32-bit fetch accesses, which is what an SH4 fetch counter counts: the
	// fetch unit reads 32 bits at a time, so one access covers up to two
	// instructions. Kept separate from instFetched because comparing a modelled
	// instruction count against a hardware fetch counter compares two different
	// quantities, and the ratio between them is workload dependent.
	u64 fetchOps;
	// Lines touched. NOT a hardware fetch count: the dynarec feed touches a line
	// once per block, where hardware fetches it once per instruction. Use
	// instFetched as the denominator of a miss rate, never this.
	u64 lineTouches[(int)Stream::Count];
	u64 misses[(int)Stream::Count];
	u64 missKinds[(int)Stream::Count][(int)MissKind::Count];
	u64 uncachedAccesses[(int)Stream::Count];
	u64 invalidations;
	double missCycles[(int)Stream::Count];
	// Guest data accesses seen, the only honest denominator for a data miss
	// rate. One access can span two lines, so this is not lineTouches[Data].
	u64 dataAccesses;
	// Stores that missed a write-through line and so did not allocate. Not
	// cache misses - the line was never wanted in the cache - but they are the
	// difference between the access count and what the cache actually saw.
	u64 writeThroughMisses;
	// Dirty operand cache lines written out on eviction.
	u64 writebacks;
	// Misses caused by a store. Separated from the total because a write miss
	// and a read miss cost different things for different reasons - see
	// PenaltyConfig::writeMissDrain - and one constant for both was wrong in
	// both directions.
	u64 writeMisses;
	// movca.l line allocations: misses that legitimately cost no line fill.
	// Reported rather than silently folded in, because the difference between
	// this and an ordinary write miss is the whole point of the instruction.
	u64 allocatingStores;
	double writebackCycles;
};

struct SetStat
{
	u64 accesses;
	u64 misses;
};

struct EvictPair
{
	u32 line;
	u32 evictedLine;
	u64 count;
};

struct SiteStat
{
	u32 line;
	u64 misses;
	u64 kinds[(int)MissKind::Count];
};

// FNV-1a. The identity of a block of guest code: code living in a JIT buffer
// lands at a different address every run, so its address can be used for layout
// arithmetic but never for naming it or matching it against a binary. Shared so
// that the live feed and anything checking a symbol file against a trace agree
// on what a block's identity is.
inline u64 hashCode(const u8 *bytes, size_t size)
{
	u64 hash = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < size; i++)
		hash = (hash ^ bytes[i]) * 0x100000001b3ull;
	return hash;
}

//
// Fully-associative LRU shadow of the same capacity as the modelled cache.
// A direct-mapped miss that hits here is a conflict miss: the line was still
// resident in an equally large cache, so a different code layout would have
// kept it. This is the classification hardware cannot report and the reason the
// shadow exists.
//
class ShadowCache
{
public:
	explicit ShadowCache(u32 capacity) : nodes(capacity), slots(nextPow2(capacity * 4), EMPTY)
	{
		mask = (u32)slots.size() - 1;
		clear();
	}

	void clear()
	{
		std::fill(slots.begin(), slots.end(), EMPTY);
		for (u32 i = 0; i < nodes.size(); i++)
		{
			nodes[i].line = INVALID_LINE;
			nodes[i].prev = i == 0 ? NONE : i - 1;
			nodes[i].next = i + 1 == nodes.size() ? NONE : i + 1;
		}
		head = 0;
		tail = (u32)nodes.size() - 1;
	}

	// Returns true if the line was resident. Either way it ends up as the most
	// recently used one.
	bool access(u32 line)
	{
		const u32 slot = find(line);
		if (slots[slot] != EMPTY)
		{
			touch(slots[slot]);
			return true;
		}
		const u32 node = tail;
		if (nodes[node].line != INVALID_LINE)
			erase(find(nodes[node].line));
		nodes[node].line = line;
		touch(node);
		// erase() may have moved entries along their probe chains, so the slot
		// found above is not necessarily still the right one
		slots[find(line)] = node;
		return false;
	}

	// Drop one line, for a guest invalidation that targets a single line. The
	// same instruction would drop it from an equally large associative cache,
	// so leaving it here would report the next refill as a conflict miss and
	// send a layout search after something layout cannot fix.
	void remove(u32 line)
	{
		const u32 slot = find(line);
		if (slots[slot] == EMPTY)
			return;
		const u32 node = slots[slot];
		nodes[node].line = INVALID_LINE;
		erase(slot);
		moveToTail(node);
	}

	template<class F>
	void forEachResident(F&& fn) const
	{
		for (const Node& node : nodes)
			if (node.line != INVALID_LINE)
				fn(node.line);
	}

private:
	static constexpr u32 EMPTY = 0xFFFFFFFF;
	static constexpr u32 NONE = 0xFFFFFFFF;

	struct Node
	{
		u32 line;
		u32 prev;
		u32 next;
	};

	static u32 nextPow2(u32 v)
	{
		u32 r = 1;
		while (r < v)
			r <<= 1;
		return r;
	}

	u32 slotFor(u32 line) const
	{
		return (line * 2654435761u) & mask;
	}

	u32 find(u32 line) const
	{
		u32 i = slotFor(line);
		while (slots[i] != EMPTY && nodes[slots[i]].line != line)
			i = (i + 1) & mask;
		return i;
	}

	// Linear probing needs backward-shift deletion: blanking a slot outright
	// would break the probe chain of any entry displaced past it, and a lookup
	// that then reported a resident line as absent would corrupt the very
	// number this class exists to produce.
	void erase(u32 i)
	{
		slots[i] = EMPTY;
		for (u32 j = (i + 1) & mask; slots[j] != EMPTY; j = (j + 1) & mask)
		{
			const u32 home = slotFor(nodes[slots[j]].line);
			const bool movable = j > i ? (home <= i || home > j) : (home <= i && home > j);
			if (movable)
			{
				slots[i] = slots[j];
				slots[j] = EMPTY;
				i = j;
			}
		}
	}

	void unlink(u32 node)
	{
		if (nodes[node].prev != NONE)
			nodes[nodes[node].prev].next = nodes[node].next;
		if (nodes[node].next != NONE)
			nodes[nodes[node].next].prev = nodes[node].prev;
		if (head == node)
			head = nodes[node].next;
		if (tail == node)
			tail = nodes[node].prev;
	}

	void moveToTail(u32 node)
	{
		if (tail == node)
			return;
		unlink(node);
		nodes[node].next = NONE;
		nodes[node].prev = tail;
		nodes[tail].next = node;
		tail = node;
	}

	void touch(u32 node)
	{
		if (head == node)
			return;
		unlink(node);
		nodes[node].prev = NONE;
		nodes[node].next = head;
		nodes[head].prev = node;
		head = node;
	}

	std::vector<Node> nodes;
	std::vector<u32> slots;
	u32 mask;
	u32 head = 0;
	u32 tail = 0;
};

//
// One modelled cache: the direct-mapped array the hardware has, the shadow
// above, and the bookkeeping that makes a miss reportable.
//
struct Cache
{
	explicit Cache(u32 setCount) : sets(setCount), shadow(setCount)
	{
		lines.resize(setCount);
		stats.resize(setCount);
		evictors.resize(setCount);
		clear();
	}

	void clear()
	{
		for (auto& line : lines)
		{
			line.valid = false;
			line.dirty = false;
			line.tag = 0;
			line.lineAddr = INVALID_LINE;
		}
		shadow.clear();
	}

	void clearStats()
	{
		std::fill(stats.begin(), stats.end(), SetStat{});
		for (auto& e : evictors)
			e.clear();
		sites.clear();
		everSeen.clear();
		invalidatedLines.clear();
		lastRow = INVALID_LINE;
	}

	// A guest flush empties the associative shadow as well: an equally large
	// associative cache would have been flushed by the same instruction.
	// Without remembering what was resident, every one of those refills would
	// be counted as a capacity miss and a layout search would chase it.
	void flush()
	{
		shadow.forEachResident([this](u32 line) {
			invalidatedLines.insert(line);
		});
		clear();
	}

	struct Line
	{
		u32 tag;
		u32 lineAddr;	// full physical line address, kept so reports are exact
		bool valid;
		bool dirty;		// operand cache only: written while in copy-back mode
	};

	// Bounded top-K of (line, evicted line) pairs. Per-set state for hundreds of
	// sets, so it cannot be allowed to grow.
	static constexpr size_t EVICTORS_PER_SET = 8;

	struct EvictorTable
	{
		std::array<EvictPair, EVICTORS_PER_SET> entries;
		size_t used = 0;

		void clear() { used = 0; }

		void add(u32 line, u32 evictedLine)
		{
			for (size_t i = 0; i < used; i++)
				if (entries[i].line == line && entries[i].evictedLine == evictedLine) {
					entries[i].count++;
					return;
				}
			if (used < EVICTORS_PER_SET) {
				entries[used++] = { line, evictedLine, 1 };
				return;
			}
			// Full: replace the rarest entry, and only once it has been beaten
			// often enough that we are not just thrashing the table
			size_t rarest = 0;
			for (size_t i = 1; i < used; i++)
				if (entries[i].count < entries[rarest].count)
					rarest = i;
			if (--entries[rarest].count == 0)
				entries[rarest] = { line, evictedLine, 1 };
		}
	};

	u32 sets;
	std::vector<Line> lines;
	ShadowCache shadow;
	std::vector<SetStat> stats;
	std::vector<EvictorTable> evictors;
	std::unordered_map<u32, SiteStat> sites;
	std::unordered_set<u32> everSeen;
	std::unordered_set<u32> invalidatedLines;
	u32 lastRow = INVALID_LINE;
};

//
// The model. Knows nothing about flycast, the MMU, or where the addresses came
// from: callers hand it lines that have already been translated.
//
class Model
{
public:
	Model() : ic(IC_SETS), oc(OC_SETS) {}

	void reset()
	{
		ic.clear();
		ic.clearStats();
		oc.clear();
		oc.clearStats();
		total = Counters{};
		missRingHead = 0;
		missRingWrapped = false;
		cycle = 0;
	}

	void setPenalty(const PenaltyConfig& cfg) { penaltyCfg = cfg; }
	const PenaltyConfig& penalty() const { return penaltyCfg; }

	void setMissRingSize(size_t records)
	{
		missRing.assign(records, MissRecord{});
		missRingHead = 0;
		missRingWrapped = false;
	}

	// Current guest cycle, stamped onto every miss so that bursts can be found
	// in time rather than only counted.
	void setCycle(u64 c) { cycle = c; }
	u64 currentCycle() const { return cycle; }

	// The index the hardware picks comes from the virtual address; the tag and
	// the line's identity come from the physical one.
	void touchLine(Stream stream, u32 index, u32 lineAddr, u32 pc, bool write)
	{
		Cache& cache = cacheFor(stream);
		const u32 tag = (lineAddr >> 10) & 0x7ffff;

		total.lineTouches[(int)stream]++;
		cache.stats[index].accesses++;

		Cache::Line& line = cache.lines[index];
		const bool dmHit = line.valid && line.tag == tag;
		const bool shadowHit = cache.shadow.access(lineAddr);

		if (dmHit)
			return;

		// A line's first ever access always misses in the direct-mapped cache,
		// so inserting here is enough to answer "seen before".
		const bool firstEver = cache.everSeen.insert(lineAddr).second;
		const MissKind kind = shadowHit ? MissKind::Conflict
				: firstEver ? MissKind::Compulsory
				: cache.invalidatedLines.erase(lineAddr) != 0 ? MissKind::Invalidated
				: MissKind::Capacity;

		recordMiss(stream, pc, lineAddr, line.valid ? line.lineAddr : INVALID_LINE,
				index, write, kind);
		total.missCycles[(int)stream] += fillCycles(cache, lineAddr, stream);

		line.valid = true;
		line.tag = tag;
		line.lineAddr = lineAddr;
	}

	// One guest load or store against the operand cache.
	//
	// Write policy is the part that cannot be guessed at: in copy-back mode a
	// store that misses fills the line and dirties it, while in write-through
	// mode it does not allocate at all - the store goes straight out and the
	// cache is left alone. Modelling every store as an allocation would invent
	// misses in exactly the guests that avoid them.
	//
	// `copyBack` is the caller's decode of CCR.CB / CCR.WT and the page's WT
	// bit, because only the caller can see the MMU.
	// `allocate` is movca.l: the line is claimed without a block read, so the
	// miss is real and the eviction is real but the fill costs nothing.
	void touchData(u32 index, u32 lineAddr, u32 pc, bool write, bool copyBack,
			bool allocate = false)
	{
		Cache& cache = oc;
		const u32 tag = (lineAddr >> 10) & 0x7ffff;

		total.lineTouches[(int)Stream::Data]++;
		cache.stats[index].accesses++;

		Cache::Line& line = cache.lines[index];
		if (line.valid && line.tag == tag)
		{
			// A hit still has to refresh the shadow, or its LRU order drifts
			// away from the recency the classification depends on.
			cache.shadow.access(lineAddr);
			if (write && copyBack)
				line.dirty = true;
			return;
		}

		if (write && !copyBack)
		{
			// No allocation, and deliberately no shadow touch: an equally large
			// associative cache would not have held this line either, so
			// counting it as a miss would put work in front of a layout search
			// that no layout can remove.
			total.writeThroughMisses++;
			return;
		}

		const bool shadowHit = cache.shadow.access(lineAddr);
		const bool firstEver = cache.everSeen.insert(lineAddr).second;
		const MissKind kind = shadowHit ? MissKind::Conflict
				: firstEver ? MissKind::Compulsory
				: cache.invalidatedLines.erase(lineAddr) != 0 ? MissKind::Invalidated
				: MissKind::Capacity;

		recordMiss(Stream::Data, pc, lineAddr, line.valid ? line.lineAddr : INVALID_LINE,
				index, write, kind);
		// No fill for movca.l: SH4 manual 4.3.8 - "the cache block will be
		// allocated but an R0 data write will be performed to that cache block
		// without performing a block read". Charging one here is inventing
		// traffic the bus never carries. That case and only that case.
		//
		// Every other miss fills, writes included. This used to skip write
		// misses entirely - the manual's movca.l wording read as covering all
		// allocating stores - which made them free, and dragged the average
		// cost per operand miss down to 9.4 cycles against hardware's 23-27.
		// PMCR 0x22 settles it: operand cache fill cycles per miss are 20.05
		// on texture2d and 20.75 on pvr_dma, two workloads whose write share
		// of misses differs by 2.7x. Divide the same fill cycles by READ
		// misses alone and the two come out 35.4 and 24.8, disagreeing by 43%.
		// A term that is flat across a mix that varies is a term that applies
		// to the whole mix.
		if (allocate)
			total.allocatingStores++;
		else
		{
			total.missCycles[(int)Stream::Data] += fillCycles(cache, lineAddr, Stream::Data);
			// And a write miss costs more than the fill, because the dirty
			// line it displaces has to go out. Flat, not spacing-dependent:
			// see PenaltyConfig::writeMissExtra.
			if (write)
				total.missCycles[(int)Stream::Data] += penaltyCfg.writeMissExtra;
		}

		if (line.valid && line.dirty)
		{
			total.writebacks++;
			total.writebackCycles += penaltyCfg.writebackCycles;
		}

		line.valid = true;
		line.dirty = write && copyBack;
		line.tag = tag;
		line.lineAddr = lineAddr;
	}

	void countDataAccess() { total.dataAccesses++; }

	void countSqFlush(SqDest dest) { total.sqFlushes[(int)dest]++; }

	// Operand cache index. The same decode flycast's own cache implements, kept
	// identical on purpose: RAM mode steals the half of the cache selected by
	// index bit 7, and area 3 is forced into it.
	static u32 dataIndex(u32 vaddr, bool oix, bool ora)
	{
		u32 index = oix
				? ((vaddr >> (25 - 8)) & 0x100) | ((vaddr >> LINE_SHIFT) & (ora ? 0x7f : 0xff))
				: (vaddr >> LINE_SHIFT) & (ora ? 0x17f : 0x1ff);
		if (ora && (vaddr >> 29) == 3)
			index |= 0x80;
		return index;
	}

	// True when RAM mode has taken this index out of the cache entirely.
	static bool dataIndexIsRam(u32 index, bool ora) { return ora && (index & 0x80) != 0; }

	static u32 instIndex(u32 vaddr, bool iix)
	{
		return iix ? ((vaddr >> LINE_SHIFT) & 0x7f) | ((vaddr >> (25 - 7)) & 0x80)
				: (vaddr >> LINE_SHIFT) & 0xff;
	}

	// Counts one block's fetch, in both currencies at once. Both callers - the
	// live feed and the offline replay - must agree on this or their numbers
	// stop being comparable, and they did drift when each counted it itself, so
	// it lives here and nowhere else. Lookahead counts as fetched but not as
	// executed: the fetch unit really does issue those accesses, while the
	// instructions in them never run.
	void countBlockFetch(u32 paddr, u32 size, u32 lookahead)
	{
		total.instFetched += size / 2;
		const u32 bytes = size + lookahead;
		total.fetchOps += (((paddr + bytes + 3) & ~3u) - (paddr & ~3u)) / 4;
	}

	void countInstructions(u64 n) { total.instFetched += n; }
	void countUncached(Stream stream, u64 lines) { total.uncachedAccesses[(int)stream] += lines; }

	void invalidateInst()
	{
		total.invalidations++;
		ic.flush();
	}

	void invalidateData()
	{
		total.invalidations++;
		oc.flush();
	}

	// P4 instruction cache address array write, decoded as the hardware does.
	// `tag` is only used for an associative write; pass INVALID_LINE when it
	// could not be determined, and the write is ignored rather than guessed at.
	void writeInstAddressArray(u32 addr, u32 data, u32 assocTag)
	{
		total.invalidations++;

		const u32 index = (addr >> LINE_SHIFT) & (IC_SETS - 1);
		Cache::Line& line = ic.lines[index];

		if (line.valid)
		{
			ic.invalidatedLines.insert(line.lineAddr);
			ic.shadow.remove(line.lineAddr);
		}

		if ((addr & 8) == 0)
		{
			// Direct write: the guest sets the tag and valid bit itself
			line.tag = (data >> 10) & 0x7ffff;
			line.valid = data & 1;
			// Bits 12:10 of the line address come from the tag, 9:5 from the index
			line.lineAddr = (line.tag << 10) | ((index << LINE_SHIFT) & 0x3e0);
			if (line.valid)
				ic.shadow.access(line.lineAddr);
			return;
		}
		// Associative write: only takes effect when the tag matches
		if (assocTag == INVALID_LINE || !line.valid || line.tag != assocTag)
			return;
		line.valid = data & 1;
	}

	const Counters& counters() const { return total; }
	Cache& cacheFor(Stream stream) { return stream == Stream::Inst ? ic : oc; }
	const Cache& cacheFor(Stream stream) const { return stream == Stream::Inst ? ic : oc; }

	std::vector<EvictPair> setEvictors(Stream stream, u32 set) const
	{
		const Cache& cache = cacheFor(stream);
		if (set >= cache.sets)
			return {};
		const Cache::EvictorTable& table = cache.evictors[set];
		std::vector<EvictPair> out(table.entries.begin(), table.entries.begin() + table.used);
		std::sort(out.begin(), out.end(), [](const EvictPair& a, const EvictPair& b) {
			return a.count > b.count;
		});
		return out;
	}

	std::vector<SiteStat> topSites(Stream stream, size_t limit) const
	{
		const Cache& cache = cacheFor(stream);
		std::vector<SiteStat> out;
		out.reserve(cache.sites.size());
		for (const auto& entry : cache.sites)
			out.push_back(entry.second);
		std::sort(out.begin(), out.end(), [](const SiteStat& a, const SiteStat& b) {
			return a.misses > b.misses;
		});
		if (out.size() > limit)
			out.resize(limit);
		return out;
	}

	std::vector<MissRecord> recentMisses() const
	{
		if (missRing.empty())
			return {};
		std::vector<MissRecord> out;
		if (missRingWrapped)
		{
			out.reserve(missRing.size());
			out.insert(out.end(), missRing.begin() + missRingHead, missRing.end());
		}
		out.insert(out.end(), missRing.begin(), missRing.begin() + missRingHead);
		return out;
	}

private:
	// Cycle derivation. Never measured: the model in use is reported next to
	// every figure that comes out of here.
	double fillCycles(Cache& cache, u32 lineAddr, Stream stream)
	{
		if (penaltyCfg.model == PenaltyModel::Fixed)
			return stream == Stream::Data ? penaltyCfg.dataFillCycles
					: penaltyCfg.fixedCycles;

		const u32 row = lineAddr >> penaltyCfg.rowShift;
		const bool rowHit = cache.lastRow == row;
		cache.lastRow = row;
		return rowHit ? penaltyCfg.rowHitCycles : penaltyCfg.rowMissCycles;
	}

	void recordMiss(Stream stream, u32 pc, u32 line, u32 evictedLine, u32 set,
			bool write, MissKind kind)
	{
		total.misses[(int)stream]++;
		total.missKinds[(int)stream][(int)kind]++;
		if (write && stream == Stream::Data)
			total.writeMisses++;

		Cache& cache = cacheFor(stream);
		cache.stats[set].misses++;
		if (evictedLine != INVALID_LINE)
			cache.evictors[set].add(line, evictedLine);

		SiteStat& site = cache.sites[line];
		site.line = line;
		site.misses++;
		site.kinds[(int)kind]++;

		if (!missRing.empty())
		{
			missRing[missRingHead] = { pc, line, evictedLine, set, cycle, stream, write, kind };
			missRingHead = (missRingHead + 1) % missRing.size();
			if (missRingHead == 0)
				missRingWrapped = true;
		}
	}

	Cache ic;
	Cache oc;
	Counters total{};
	PenaltyConfig penaltyCfg;
	std::vector<MissRecord> missRing;
	size_t missRingHead = 0;
	bool missRingWrapped = false;
	u64 cycle = 0;
};

} // namespace cachesim
