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
// Guest-side SH4 cache simulator: the flycast side of it.
//
// The model itself is in cachesim_model.h, which knows nothing about flycast so
// that tools/cachesweep can replay a recorded trace through the very same code.
// This header is the live feed, the reporting and the run control.
//
// Miss counts are the measurement. Every cycle figure is derived from a stated
// penalty model and is labelled as such wherever it is reported.
//
// See docs/cachesim/plan.md
//
#pragma once
#include "types.h"
#include "cachesim_model.h"

#include <deque>
#include <string>

namespace cachesim
{

// Per-block descriptor built once at compile time and passed to the dynarec
// hook. Lives in a pool that is stable for the whole run: compiled code holds
// raw pointers into it and blocks outlive their RuntimeBlockInfo.
struct BlockTrace
{
	u32 vaddr;	// guest virtual address of the first instruction
	u32 paddr;	// guest physical address of the first instruction
	u32 size;	// guest bytes covered by the block
	u32 id;		// dense index, used as the trace stream symbol
	u32 guestCycles;	// flycast's issue-cycle estimate for one execution
	u64 hash;	// hash of the guest instruction bytes: identity for JIT-resident
			// code, whose address is not stable across runs
};

//
// Control
//
// Hot path: the interpreter feed tests this per instruction, so it must not be
// a cross-translation-unit call. Use armed(), not the variable.
extern bool g_armed;
inline bool armed() { return g_armed; }
// Whether guest loads and stores are fed to the operand cache model. Separate
// from armed() because it costs a call per access, and because the instruction
// cache answers most questions on its own.
extern bool g_dataFeed;
inline bool dataFeed() { return g_dataFeed; }
// Whether modelled miss cycles are charged to flycast's own timing. This makes
// the simulator change the emulation instead of only observing it: a run with
// it on is a different run, and nothing measured there is comparable with a
// run without it.
extern bool g_timing;
inline bool timingFeedback() { return g_timing; }
// Applies the config options. Must run before any block is translated.
void init();
// Only takes effect for blocks compiled afterwards, so the caller must reset
// the code cache for the change to reach already-translated code.
void setArmed(bool on);
void reset();
void term();

void setPenaltyConfig(const PenaltyConfig& cfg);
const PenaltyConfig& penaltyConfig();

// The SH4 fetch unit runs ahead of execution, so a block's last line is not the
// last line fetched. This is how many bytes past the end of a block are treated
// as fetched. It is CALIBRATED against hardware counters, not derived from the
// pipeline, so it is reported alongside the numbers it produces.
void setBlockLookahead(u32 bytes);
u32 blockLookahead();

// Frames to run before the counters are cleared. The compile and load storm at
// startup is not steady state and swamps it.
void setSkipFrames(u32 frames);
// Frames to measure after that, then freeze and write the report. The window
// has to be defined in guest frames, not wall clock: two runs of the same guest
// at different speeds otherwise measure different parts of the workload, which
// makes their numbers incomparable.
void setMeasureFrames(u32 frames);
// True once the measurement window has closed. The headless loop ends the run.
bool finished();
// Guest cycles actually charged to the emulated SH4 by the feedback mode. Zero
// when it is off, which is how a report says "this run was only observed".
u64 chargedTimingCycles();

//
// Feeds
//
// Dynarec block prologue: replays the whole instruction range of a block. A
// translated block is straight-line, so its fetch stream is exactly its address
// range.
void DYNACALL traceBlock(const BlockTrace *bt);
// Guest data access feed, called from compiled code before each load or store.
// `packed` is the access size in bytes with bit 8 set for a write. Only emitted
// into blocks compiled while armed, so arming must reset the code cache.
void DYNACALL dataAccess(u32 vaddr, u32 packed);
// Per-instruction feed, used by the interpreter.
void traceFetch(u32 vaddr, u32 paddr, u32 bytes);

// Guest-driven invalidation. The instruction cache is not coherent with writes
// to memory, so these are the ONLY things that drop lines: flycast's own block
// invalidation must not be mirrored here.
void invalidateInst();
void writeInstAddressArray(u32 addr, u32 data);
void invalidateData();

// Frame boundary, for windowed output.
void frameBoundary();

//
// Block descriptor pool
//
// Repeated compilations of the same block return the same descriptor, so the
// pool is bounded by distinct guest code, not by recompilation churn.
const BlockTrace *traceForBlock(u32 vaddr, u32 paddr, u32 size, u32 guestCycles);
const std::deque<BlockTrace>& blocks();

//
// Results
//
u64 guestCycles();
u64 frameGuestCycles();
const Counters& counters();
Counters frameCounters();
u64 frameCount();
// Counters since the last markLogWindow(), for reporting a window rather than
// a running average.
Counters logWindow();
u64 logWindowCycles();
void markLogWindow();

double derivedMissCycles(Stream stream);
const SetStat *setStats(Stream stream);
std::vector<EvictPair> setEvictors(Stream stream, u32 set);
std::vector<SiteStat> topSites(Stream stream, size_t limit);
std::vector<MissRecord> recentMisses();
void setMissRingSize(size_t records);

//
// Profile: where the frame's guest cycles go
//
// Everything here is PER FRAME, smoothed over recent frames. Totals over a run
// are dominated by loading and compilation and describe no frame that ever
// happened.
//
struct ProfileRow
{
	std::string name;	// symbol, or an address range when there is none
	u32 start;
	u32 end;
	double cycles;		// per frame, estimated: see the caveat below
	double missCycles;	// of which, icache fill. Derived from miss counts
	double dataMissCycles;	// of which, operand cache fill. Zero unless the data
			// feed is on, which costs speed and so is opt-in
	double calls;		// block entries per frame
	bool named;
};

// Rows sorted by cost, biggest first. Cycles are FLYCAST'S estimate of issue
// cost, not hardware truth - flycast retires fewer instructions per cycle than
// the real chip - so the ranking is meaningful and the absolute figures are not.
std::vector<ProfileRow> profile(size_t limit);
// Guest cycles in a frame, and how many of them the rows above account for.
// The difference is the CPU waiting: idle, asleep, or spinning somewhere that
// never executed a block. Without it the percentages would silently be shares
// of work done rather than shares of the frame.
double profileFrameCycles();
double profileAccountedCycles();

//
// Reporting (cachesim_report.cpp)
//
void logSummary();
bool writeReport(const std::string& path);
void setReportPath(const std::string& path);

//
// Trace recording (cachesim_trace.cpp)
//
// Records the block execution stream so that a layout can be re-simulated
// offline instead of costing an emulator run per candidate.
bool traceOpen(const std::string& path);
void traceClose();
bool tracing();
void traceBlockExec(const BlockTrace *bt, u64 cycle);
void traceEvent(u8 event, u32 a, u32 b);

} // namespace cachesim
