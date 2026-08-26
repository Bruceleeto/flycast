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
#include "pipesim.h"

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
	// Pipeline model, computed once here rather than measured at runtime: a
	// block is a straight-line instruction sequence, so its issue schedule is
	// a property of the block. See core/hw/sh4/cachesim/pipesim.h.
	//
	// These are cycles the SH4 spends issuing and interlocking. They do NOT
	// include cache misses, which the model above counts separately and which
	// are reported in their own column - two uncertain models added together
	// give a number nobody can check.
	u32 pipeCycles;		// total, one execution, steady state
	// Stall EVENTS, not cycles: several pipeline sequences can stall in the
	// same cycle, so this can exceed pipeCycles and must never be subtracted
	// from it. It is a breakdown of why, not an amount of time.
	u32 pipeStalls;
	// Issue slots and dual issues, one execution. These are what the SH7091
	// counters 0x13 and 0x14 count, and they are separate fields rather than
	// (pipeCycles - pipeStalls) because that difference is not a slot count:
	// a cycle in which one sequence is blocked and another issues behind it is
	// charged as a stall and still issues. See pipesim::Result::issueSlots.
	u32 pipeIssueSlots;
	u32 pipeParallel;
	u16 pipeWrapStalls;	// diagnostic, see pipesim::Result::wrapStalls
	u16 pipeByReason[(int)pipesim::StallReason::Count];
	bool pipeModelled;	// false if any opcode had no pipeline data
	// `pref` instructions in this block. A store queue flush is a `pref` to the
	// SQ area, so this is an upper bound on the flushes one execution can do -
	// the address is not known until it runs, which is why the flush itself is
	// still counted at runtime. Dividing the block's cycles by this gives the
	// spacing between flushes, which is what decides whether a flush stalls at
	// all: the queue only freezes the CPU if the next flush finds it still
	// draining. See PenaltyConfig::sqDrain*.
	u16 prefCount;
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
// Whether the PIPELINE model drives emulated timing, replacing flycast's own
// per-block issue estimate. Separate opt-in from g_timing because it changes
// emulated time for every guest, not just the miss cost: see the note at the
// charge site in cachesim.cpp. Implies g_timing.
extern bool g_pipeTiming;
inline bool pipeTiming() { return g_pipeTiming; }
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
// `packed` is the access size in the low byte, plus flags:
//   bit 8  - this is a write
//   bit 9  - ALLOCATING write (movca.l). The line is allocated without reading
//            it in, so the miss costs no line fill. It still displaces whatever
//            was in the set, so a dirty victim is still written back, and it is
//            still a miss. See ACCESS_WRITE / ACCESS_ALLOCATE below.
void DYNACALL dataAccess(u32 vaddr, u32 packed);

enum : u32 { ACCESS_WRITE = 0x100, ACCESS_ALLOCATE = 0x200 };

// True if this guest opcode is movca.l R0,@Rn, which allocates a cache line
// without a block read (SH4 manual 4.3.8, 10.60). Used at compile time by the
// recompiler to set ACCESS_ALLOCATE, so nothing is decoded at runtime.
inline bool isAllocatingStore(u16 opcode) { return (opcode & 0xF0FF) == 0x00C3; }
// Per-instruction feed, used by the interpreter.
void traceFetch(u32 vaddr, u32 paddr, u32 bytes);

// A store queue flush: 32 bytes leaving the CPU for `area`. Called from
// storeq.cpp, once per flush, only while armed. Counted rather than charged -
// the per-flush cost is not modelled yet - but the count is the denominator
// needed to turn a hardware frz_dc measurement into a per-flush figure.
void sqFlush(u32 area);

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
// Per-frame execution count and store queue flushes for a block, by id. For
// block-level reporting; 0 for a block that has not run recently.
double blockExecsPerFrame(u32 id);
double blockSqFlushesPerFrame(u32 id);
// Store queue CYCLES per frame for a block. Not flushes times a constant: the
// cost of a flush depends on how far apart the flushes are, which is a property
// of the block. See sqFlushCost().
double blockSqCyclesPerFrame(u32 id);

// What one flush to `dest` costs inside `bt`. The queue drains asynchronously,
// so a flush stalls only by however much of the drain the previous flush has
// not finished - which is the drain time minus the gap since it started.
double sqFlushCost(const BlockTrace& bt, SqDest dest);
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

// Pipeline model totals for the run so far. Issue and interlock cycles only:
// no cache cost is included, and the two are reported side by side rather than
// summed, because adding two separately-uncertain models produces a number
// that cannot be checked against anything.
struct PipeTotals
{
	u64 cycles;
	u64 stalls;
	// Cycles that issued at least one instruction, and instructions that
	// shared such a cycle with the one ahead. The two sum to the instruction
	// count, which is what makes them checkable against hardware.
	u64 issueSlots;
	u64 parallelIssues;
	u64 wrapStalls;
	u64 byReason[(int)pipesim::StallReason::Count];
	u64 unmodelledBlockExecs;
};
PipeTotals pipeTotals();
// Translated block entries since the counters were cleared. Every block ends in
// a control transfer, so this is also the closest thing the dynarec has to a
// taken-branch count - blocks that fall through to their successor are the
// error term, and it is reported rather than corrected for.
u64 blockExecs();
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
	// Store queue flushes per frame, and what they cost. Unlike the cache
	// columns this is a direct count of a real event rather than a modelled
	// one - the hook is on the flush itself - so only the per-flush cycle
	// figure is an estimate. See PenaltyConfig::sqFlushCycles.
	double sqFlushes;
	double sqCycles;

	// Pipeline model, per frame. Unlike `cycles` above - which is flycast's
	// own issue estimate and is good for ranking only - these come from the
	// SH4 pipeline model and have been checked against hardware: 0.8% on
	// instruction count and 18% low on cycles for a real FP workload, the
	// shortfall being cache and store queue, which are not in here.
	//
	// The three stall columns are what make a row actionable. High issue means
	// the code does too much work and scheduling will not help. High flow-dep
	// means reorder it. High icache/dcache means move it.
	// All four are CYCLES PER FRAME and they add up:
	//   pipeCycles == pipeIssue + pipeFlowDep + pipeResource + pipeStage
	//                 + pipeOtherStall
	// which is the whole point of counting stalls per cycle rather than per
	// event. An event count cannot be added to anything.
	double pipeCycles;		// issue + interlock, no cache cost
	double pipeIssue;		// actually issuing instructions
	double pipeFlowDep;		// waiting on a previous result
	// The FPU-register share of pipeFlowDep, not a separate quantity - it is
	// included above as well. Split out because hardware splits it (PMCR 0x28
	// against 0x29) and because it is the larger half: 7.0% of a texture2d
	// frame against 4.7% for the CPU registers.
	double pipeFpuDep;
	double pipeResource;	// non-parallel-executable groups colliding
	double pipeStage;		// a stage busy or locked
	double pipeOtherStall;	// output dependency, and knock-on from the above
	bool pipeComplete;		// false if any block had an unmodelled opcode

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
//
// Note this is FLYCAST'S frame, and flycast does not stall on the store queue
// at all, so a row's percentage can exceed 100 on a workload that feeds the
// tile accelerator hard. That is not a bug in the row: it is the size of the
// gap between what flycast charges and what hardware does, and it is most of
// why bruces_balls runs at 60fps here and 30fps on a Dreamcast.
double profileFrameCycles();
double profileAccountedCycles();

//
// Reporting (cachesim_report.cpp)
//
void logSummary();
// Everything logSummary does, plus the profile, the block table and the JSON
// report if one was asked for - the same output the -cachesim-frames limit
// produces when it fires. Call this when a run ends for any OTHER reason.
//
// Without it, `-cachesim-frames 0` - the only sensible setting when you do not
// know a title's frame count in advance, which is every commercial game -
// produced the summary lines and NO profile, because the profile was only ever
// written on the frame-limit path.
void reportFinal();
// Per-function table: where the frame goes, ranked, with a stall breakdown.
void logProfile(size_t limit = 24);
// Per-BLOCK breakdown, hottest first. The per-function table groups by symbol,
// which says nothing about a function that has been inlined into one enormous
// loop - the common case for a renderer. This drops to the block, so a hot
// span inside a function can be found and mapped back to source with addr2line.
void logBlocks(size_t limit = 24);
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
