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
// Validation gate for the guest cache simulator.
//
// The simulator's whole value is that its miss counts are trustworthy, so the
// primary check is differential: an independent, deliberately naive reference
// model written here from the SH7750 description, fed the same address stream,
// must agree exactly on every miss and on every classification. A model that
// merely looks right produces plausible fiction, which is worse than no tool.
//
#include <gtest/gtest.h>

#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/cachesim/cachesim.h"
#include "hw/sh4/sh4_mmr.h"

#include <algorithm>
#include <list>
#include <random>
#include <unordered_set>

using namespace cachesim;

namespace
{

// Physical RAM, cached through P0/P1. Kept inside one 16 MB window so that a
// line address and its (tag, index) pair are interchangeable.
constexpr u32 RAM = 0x0c000000;

//
// Independent reference. Deliberately the slow, obvious implementation: a flat
// array for the direct-mapped cache, a list for the fully-associative shadow,
// and a set for "seen before". If this and the simulator agree on a few million
// accesses, the simulator's indexing, LRU and classification are right.
//
template<u32 SETS>
class ReferenceCache
{
public:
	void access(u32 lineAddr)
	{
		const u32 index = (lineAddr >> 5) & (SETS - 1);

		const bool dmHit = valid[index] && lines[index] == lineAddr;

		auto it = std::find(lru.begin(), lru.end(), lineAddr);
		const bool shadowHit = it != lru.end();
		if (shadowHit)
			lru.erase(it);
		lru.push_front(lineAddr);
		if (lru.size() > SETS)
			lru.pop_back();

		if (dmHit)
			return;

		const bool firstEver = seen.insert(lineAddr).second;
		misses++;
		if (shadowHit)
			conflict++;
		else if (firstEver)
			compulsory++;
		else
			capacity++;

		lines[index] = lineAddr;
		valid[index] = true;
	}

	u64 misses = 0;
	u64 compulsory = 0;
	u64 capacity = 0;
	u64 conflict = 0;

private:
	u32 lines[SETS] = {};
	bool valid[SETS] = {};
	std::list<u32> lru;
	std::unordered_set<u32> seen;
};

class CacheSimTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// frameBoundary() reads the SH4 scheduler clock, so the CPU context has
		// to exist even though these tests drive the model directly
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();

		CCN_CCR.reg_data = 0;
		CCN_CCR.ICE = 1;
		setMissRingSize(1 << 16);
		reset();
	}

	static u64 misses() { return counters().misses[(int)Stream::Inst]; }
	static u64 kind(MissKind k) { return counters().missKinds[(int)Stream::Inst][(int)k]; }
	static u64 dataMisses() { return counters().misses[(int)Stream::Data]; }
	static u64 dataKind(MissKind k) { return counters().missKinds[(int)Stream::Data][(int)k]; }

	// One guest load or store, as the compiled code would issue it
	static void load(u32 addr, u32 size = 4) { dataAccess(addr, size); }
	static void store(u32 addr, u32 size = 4) { dataAccess(addr, size | 0x100); }

	// P0, so that the write policy comes from CCR.WT rather than CCR.CB
	static constexpr u32 P0 = 0x0c000000;
};

TEST_F(CacheSimTest, AgreesWithReferenceModel)
{
	ReferenceCache<IC_SETS> ref;
	std::mt19937 rng(12345);
	// A spread wider than the cache, so the stream contains all three kinds
	std::uniform_int_distribution<u32> offset(0, 64_KB - 1);
	std::uniform_int_distribution<u32> length(2, 64);

	for (int i = 0; i < 200000; i++)
	{
		const u32 addr = RAM + (offset(rng) & ~1u);
		const u32 bytes = length(rng) & ~1u;
		traceFetch(addr, addr, bytes);

		const u32 first = addr & ~(LINE_BYTES - 1);
		const u32 last = (addr + bytes - 1) & ~(LINE_BYTES - 1);
		for (u32 line = first; line <= last; line += LINE_BYTES)
			ref.access(line);
	}

	EXPECT_EQ(ref.misses, misses());
	EXPECT_EQ(ref.compulsory, kind(MissKind::Compulsory));
	EXPECT_EQ(ref.capacity, kind(MissKind::Capacity));
	EXPECT_EQ(ref.conflict, kind(MissKind::Conflict));
	// The stream is wider than the cache, so a run that produced none of these
	// would mean the test, not the model, is broken
	EXPECT_GT(kind(MissKind::Conflict), 0u);
	EXPECT_GT(kind(MissKind::Capacity), 0u);
}

TEST_F(CacheSimTest, BlockReplayMatchesPerInstructionFeed)
{
	std::mt19937 rng(999);
	std::uniform_int_distribution<u32> offset(0, 32_KB - 1);
	std::uniform_int_distribution<u32> length(1, 24);

	std::vector<std::pair<u32, u32>> ranges;
	for (int i = 0; i < 20000; i++)
	{
		const u32 addr = RAM + (offset(rng) & ~1u);
		ranges.emplace_back(addr, length(rng) * 2);
	}

	for (const auto& [addr, bytes] : ranges)
	{
		const BlockTrace bt { addr, addr, bytes, 0, 0 };
		traceBlock(&bt);
	}
	const Counters blockFed = counters();

	reset();
	for (const auto& [addr, bytes] : ranges)
		for (u32 pc = addr; pc < addr + bytes; pc += 2)
			traceFetch(pc, pc, 2);
	const Counters instFed = counters();

	// The misses are what must agree, and they agree exactly: replaying a
	// block's address range is equivalent to walking its instructions.
	EXPECT_EQ(blockFed.misses[(int)Stream::Inst], instFed.misses[(int)Stream::Inst]);
	for (int k = 0; k < (int)MissKind::Count; k++)
		EXPECT_EQ(blockFed.missKinds[(int)Stream::Inst][k], instFed.missKinds[(int)Stream::Inst][k]);
	// Line touches deliberately do NOT agree: the block feed touches a line once
	// per block where hardware fetches it once per instruction. This is why the
	// miss rate is reported per instruction fetched and never per line touched.
	EXPECT_LT(blockFed.lineTouches[(int)Stream::Inst], instFed.lineTouches[(int)Stream::Inst]);
	EXPECT_EQ(blockFed.instFetched, instFed.instFetched);
}

TEST_F(CacheSimTest, SequentialWalkMissesOncePerLine)
{
	constexpr u32 bytes = 4_KB;
	for (u32 pc = RAM; pc < RAM + bytes; pc += 2)
		traceFetch(pc, pc, 2);

	EXPECT_EQ(bytes / LINE_BYTES, misses());
	EXPECT_EQ(bytes / LINE_BYTES, kind(MissKind::Compulsory));
	EXPECT_EQ(0u, kind(MissKind::Conflict));

	// Second pass fits in the cache, so it must be free
	const u64 before = misses();
	for (u32 pc = RAM; pc < RAM + bytes; pc += 2)
		traceFetch(pc, pc, 2);
	EXPECT_EQ(before, misses());
}

TEST_F(CacheSimTest, AliasedLinesAreConflictMissesAndNameEachOther)
{
	// Two lines exactly one cache apart land in the same set, and each one
	// evicts the other: the case a layout change is supposed to fix
	const u32 a = RAM;
	const u32 b = RAM + 8_KB;

	for (int i = 0; i < 100; i++)
	{
		traceFetch(a, a, 2);
		traceFetch(b, b, 2);
	}

	EXPECT_EQ(200u, misses());
	EXPECT_EQ(2u, kind(MissKind::Compulsory));
	// Both lines stay resident in an equally large associative cache, so every
	// miss after the first two is a conflict, not capacity
	EXPECT_EQ(198u, kind(MissKind::Conflict));
	EXPECT_EQ(0u, kind(MissKind::Capacity));

	const std::vector<EvictPair> evictors = setEvictors(Stream::Inst, 0);
	ASSERT_EQ(2u, evictors.size());
	for (const EvictPair& pair : evictors)
	{
		EXPECT_NE(pair.line, pair.evictedLine);
		EXPECT_TRUE(pair.line == a || pair.line == b);
		EXPECT_TRUE(pair.evictedLine == a || pair.evictedLine == b);
	}
}

TEST_F(CacheSimTest, MissRecordNamesTheEvictedLine)
{
	const u32 a = RAM;
	const u32 b = RAM + 8_KB;
	traceFetch(a, a, 2);
	traceFetch(b, b, 2);

	const std::vector<MissRecord> records = recentMisses();
	ASSERT_EQ(2u, records.size());
	EXPECT_EQ(INVALID_LINE, records[0].evictedLine);	// set was empty
	EXPECT_EQ(a, records[1].evictedLine);
	EXPECT_EQ(b, records[1].line);
	EXPECT_EQ(0u, records[1].set);
	// First ever touch of b, so compulsory even though it threw a out. The
	// eviction is reported regardless: that field is what a layout change acts
	// on, and it is independent of why the line was being fetched.
	EXPECT_EQ(MissKind::Compulsory, records[1].kind);
}

TEST_F(CacheSimTest, CapacityMissesWhenTheWorkingSetDoesNotFit)
{
	// A 16 KB loop in an 8 KB cache: nothing survives a lap, and because the
	// associative shadow cannot hold it either these are capacity, not conflict
	constexpr u32 span = 16_KB;
	for (int lap = 0; lap < 3; lap++)
		for (u32 pc = RAM; pc < RAM + span; pc += 2)
			traceFetch(pc, pc, 2);

	EXPECT_EQ(3 * span / LINE_BYTES, misses());
	EXPECT_EQ(span / LINE_BYTES, kind(MissKind::Compulsory));
	EXPECT_EQ(2 * span / LINE_BYTES, kind(MissKind::Capacity));
	EXPECT_EQ(0u, kind(MissKind::Conflict));
}

TEST_F(CacheSimTest, UncachedAreasAreNotModelled)
{
	// P2: no caching at all
	const u32 p2 = 0xa0000000 | (RAM & 0x1fffffff);
	for (int i = 0; i < 10; i++)
		traceFetch(p2 + i * LINE_BYTES, RAM + i * LINE_BYTES, 2);

	EXPECT_EQ(0u, misses());
	EXPECT_EQ(0u, counters().lineTouches[(int)Stream::Inst]);
	EXPECT_EQ(10u, counters().uncachedAccesses[(int)Stream::Inst]);
	// Instructions fetched still counts: it is the denominator of the miss rate
	EXPECT_EQ(10u, counters().instFetched);
}

TEST_F(CacheSimTest, DisabledCacheIsNotModelled)
{
	CCN_CCR.ICE = 0;
	for (int i = 0; i < 10; i++)
		traceFetch(RAM + i * LINE_BYTES, RAM + i * LINE_BYTES, 2);

	EXPECT_EQ(0u, misses());
	EXPECT_EQ(10u, counters().uncachedAccesses[(int)Stream::Inst]);
}

TEST_F(CacheSimTest, IndexEnhancementChangesTheSet)
{
	// With CCR.IIX, bit 25 of the address replaces the top index bit
	CCN_CCR.IIX = 1;
	traceFetch(RAM, RAM, 2);
	traceFetch(RAM | (1 << 25), RAM | (1 << 25), 2);

	const SetStat *stats = setStats(Stream::Inst);
	EXPECT_EQ(1u, stats[0].accesses);
	EXPECT_EQ(1u, stats[0x80].accesses);
}

TEST_F(CacheSimTest, GuestInvalidationDropsLines)
{
	traceFetch(RAM, RAM, 2);
	EXPECT_EQ(1u, misses());

	invalidateInst();

	// The line is gone, so this misses again. It is the flush's doing, not the
	// layout's: an equally large associative cache would have been flushed too,
	// so this must not be reported as capacity or conflict
	traceFetch(RAM, RAM, 2);
	EXPECT_EQ(2u, misses());
	EXPECT_EQ(1u, kind(MissKind::Compulsory));
	EXPECT_EQ(1u, kind(MissKind::Invalidated));
	EXPECT_EQ(0u, kind(MissKind::Capacity));
	EXPECT_EQ(0u, kind(MissKind::Conflict));
	EXPECT_EQ(1u, counters().invalidations);

	// Only the first refill after the flush is attributed to it
	invalidateInst();
	traceFetch(RAM, RAM, 2);
	traceFetch(RAM, RAM, 2);
	EXPECT_EQ(3u, misses());
	EXPECT_EQ(2u, kind(MissKind::Invalidated));
}

TEST_F(CacheSimTest, AddressArrayWriteInvalidatesOneSet)
{
	traceFetch(RAM, RAM, 2);
	traceFetch(RAM + LINE_BYTES, RAM + LINE_BYTES, 2);
	EXPECT_EQ(2u, misses());

	// Non-associative write to set 0 clearing the valid bit
	writeInstAddressArray(0xf0000000, 0);
	traceFetch(RAM, RAM, 2);
	EXPECT_EQ(3u, misses());
	EXPECT_EQ(1u, kind(MissKind::Invalidated));

	// Set 1 was untouched
	traceFetch(RAM + LINE_BYTES, RAM + LINE_BYTES, 2);
	EXPECT_EQ(3u, misses());
}

TEST_F(CacheSimTest, DerivedCyclesFollowThePenaltyModel)
{
	PenaltyConfig cfg;
	cfg.model = PenaltyModel::Fixed;
	cfg.fixedCycles = 16.8;
	setPenaltyConfig(cfg);

	for (int i = 0; i < 10; i++)
		traceFetch(RAM + i * LINE_BYTES, RAM + i * LINE_BYTES, 2);
	EXPECT_EQ(10u, misses());
	EXPECT_NEAR(168.0, derivedMissCycles(Stream::Inst), 0.001);

	// Row-aware: the first fill of a row costs a row miss, the rest hit it
	reset();
	cfg.model = PenaltyModel::RowAware;
	cfg.rowHitCycles = 14.0;
	cfg.rowMissCycles = 24.0;
	cfg.rowShift = 12;
	setPenaltyConfig(cfg);

	for (int i = 0; i < 10; i++)	// 10 lines inside one 4 KB row
		traceFetch(RAM + i * LINE_BYTES, RAM + i * LINE_BYTES, 2);
	EXPECT_NEAR(24.0 + 9 * 14.0, derivedMissCycles(Stream::Inst), 0.001);

	setPenaltyConfig(PenaltyConfig{});
}

TEST_F(CacheSimTest, FrameCountersMeasureOneFrame)
{
	traceFetch(RAM, RAM, 2);
	frameBoundary();
	traceFetch(RAM + 8_KB, RAM + 8_KB, 2);
	traceFetch(RAM + 16_KB, RAM + 16_KB, 2);

	EXPECT_EQ(3u, misses());
	EXPECT_EQ(2u, frameCounters().misses[(int)Stream::Inst]);
	EXPECT_EQ(1u, frameCount());
}

} // namespace

//
// Operand cache
//
// The write policy is the part that cannot be inferred from a miss count: in
// write-through mode a store that misses does not bring the line in at all, so
// a model that allocates on every store invents misses in exactly the guests
// that were careful to avoid them.
//
TEST_F(CacheSimTest, WriteThroughStoreDoesNotAllocate)
{
	CCN_CCR.OCE = 1;
	CCN_CCR.WT = 1;

	store(P0 + 0x1000);
	EXPECT_EQ(0u, dataMisses());
	EXPECT_EQ(1u, counters().writeThroughMisses);

	// The line was never brought in, so reading it now is the first miss
	load(P0 + 0x1000);
	EXPECT_EQ(1u, dataMisses());
	EXPECT_EQ(1u, dataKind(MissKind::Compulsory));
}

TEST_F(CacheSimTest, CopyBackStoreAllocatesAndWritesBackWhenEvicted)
{
	CCN_CCR.OCE = 1;
	CCN_CCR.WT = 0;

	store(P0 + 0x1000);
	EXPECT_EQ(1u, dataMisses());
	EXPECT_EQ(0u, counters().writeThroughMisses);

	// Allocated, so reading it back hits
	load(P0 + 0x1000);
	EXPECT_EQ(1u, dataMisses());

	// Evict it with a line that lands in the same set: the dirty line has to be
	// written out
	EXPECT_EQ(0u, counters().writebacks);
	load(P0 + 0x1000 + OC_SETS * LINE_BYTES);
	EXPECT_EQ(1u, counters().writebacks);
}

TEST_F(CacheSimTest, WriteThroughHitLeavesTheLineClean)
{
	CCN_CCR.OCE = 1;
	CCN_CCR.WT = 1;

	load(P0 + 0x2000);			// allocates, clean
	store(P0 + 0x2000);			// hit, written through: still clean
	load(P0 + 0x2000 + OC_SETS * LINE_BYTES);	// evicts it

	EXPECT_EQ(0u, counters().writebacks);
}

TEST_F(CacheSimTest, AccessSpanningTwoLinesTouchesBoth)
{
	CCN_CCR.OCE = 1;

	// Four bytes starting two bytes before a line boundary
	load(P0 + LINE_BYTES - 2, 4);
	EXPECT_EQ(2u, dataMisses());
	EXPECT_EQ(1u, counters().dataAccesses);
}

TEST_F(CacheSimTest, RamModeTakesHalfTheCacheOutOfService)
{
	CCN_CCR.OCE = 1;
	CCN_CCR.ORA = 1;

	// Area 3 is forced into the RAM half, which is not a cache access at all
	const u64 before = counters().lineTouches[(int)Stream::Data];
	load(0x60000000);
	EXPECT_EQ(before, counters().lineTouches[(int)Stream::Data]);
	EXPECT_EQ(0u, dataMisses());
	// Still counted as an access the guest made: the denominator must not
	// quietly shrink
	EXPECT_EQ(1u, counters().dataAccesses);
}

TEST_F(CacheSimTest, UncachedDataIsNotModelled)
{
	CCN_CCR.OCE = 0;
	load(P0);
	EXPECT_EQ(0u, dataMisses());
	EXPECT_EQ(1u, counters().uncachedAccesses[(int)Stream::Data]);
}

TEST_F(CacheSimTest, DataAgreesWithReferenceModel)
{
	CCN_CCR.OCE = 1;
	CCN_CCR.WT = 0;			// copy-back, so every access allocates and the
							// reference does not need a write policy of its own

	ReferenceCache<OC_SETS> ref;
	std::mt19937 rng(999);
	// Wider than the 16 KB cache, so the stream contains all three kinds
	std::uniform_int_distribution<u32> offset(0, 128_KB - 1);
	std::uniform_int_distribution<u32> write(0, 1);

	for (int i = 0; i < 200000; i++)
	{
		const u32 addr = P0 + (offset(rng) & ~3u);
		if (write(rng))
			store(addr);
		else
			load(addr);
		ref.access(addr & ~(LINE_BYTES - 1));
	}

	EXPECT_EQ(ref.misses, dataMisses());
	EXPECT_EQ(ref.compulsory, dataKind(MissKind::Compulsory));
	EXPECT_EQ(ref.capacity, dataKind(MissKind::Capacity));
	EXPECT_EQ(ref.conflict, dataKind(MissKind::Conflict));
	EXPECT_GT(dataKind(MissKind::Conflict), 0u);
	EXPECT_GT(dataKind(MissKind::Capacity), 0u);
}
