/*
	SH4 pipeline model.

	Computes, for a straight-line run of SH4 instructions, how many cycles it
	takes to issue and how many of those cycles are stalls - split by the
	reason the manual gives for them.

	This is a static analysis. A translated block is a straight-line
	instruction sequence, so its issue schedule is a property of the block
	rather than of the run: it is computed once when the block is compiled and
	multiplied by the block's execution count at report time. There is no
	per-instruction runtime hook and no measurable cost to having it on.

	The model is section 8 of the SH4 program manual and nothing else:
	figure 8.2 execution patterns, table 8.1/8.2 parallel-executability, and
	the stall rules in 8.3. Per-opcode issue rate, latency, functional unit
	and execution-pattern number all come from flycast's existing
	sh4_opcodelistentry table, which already carries them.

	What it does NOT model, and what therefore has to stay in its own column
	rather than being folded into these numbers:

	  - cache misses. cachesim measures those separately and its accuracy is
	    its own question; merging an uncertain model into an uncertain model
	    produces a number nobody can check.
	  - store queue drain, PVR/G2 contention, SDRAM timing. An SH4 stall on an
	    external access freezes the whole pipeline including the FPU, and none
	    of that is here.
	  - exceptions and interrupts taken mid-block.

	So these cycles are a floor, exact for what they cover. Comparing two
	versions of the same routine is what they are for. Comparing two unrelated
	routines needs the columns this model does not fill.
*/
#pragma once
#include "types.h"
// Deliberately not including sh4_opcode_list.h here: it puts unscoped enums
// like Normal into the global namespace, and this header is reachable from the
// UI translation units where that collides with naomi_roms.h. Only the opcode
// table needs it.

namespace pipesim
{

// Why a cycle was spent. Ordered so that the first matching reason wins, the
// same order the manual's rules are applied in.
enum class StallReason : u8
{
	None = 0,
	StageFull,		// two instructions already occupy the next stage
	StageLocked,	// a multi-cycle instruction holds the stage
	ResourceHazard,	// non-parallel-executable groups want the same stage
	FlowDep,		// read-after-write on a CPU register: waiting for a result
	FpuDep,			// read-after-write on an FPU register. Split out because
					// hardware splits it: PMCR 0x28 counts freeze by CPU
					// register and 0x29 freeze by FPU, and they are different
					// sizes - 4.7% and 7.0% of a texture2d frame. One bucket
					// cannot be checked against either.
	OutputDep,		// write-after-write
	PrevStalled,	// in-order issue: the instruction ahead is stalled
	Count
};

const char *stallReasonName(StallReason r);

struct Result
{
	u32 cycles = 0;			// total cycles for the sequence
	u32 instructions = 0;
	// Cycles in which something was blocked, attributed one cycle at a time to
	// the most upstream cause. byReason sums to this exactly, and this plus
	// issueCycles() is the total - so a breakdown can be added up, which an
	// event count never could.
	u32 stallCycles = 0;
	u32 byReason[(int)StallReason::Count] = {};
	// Issue slots, in the sense the SH7091 performance counter means: cycles
	// in which at least one instruction moved out of I into D. `parallelIssues`
	// counts the ones that shared such a cycle with the instruction ahead, so
	// issueSlots + parallelIssues is the instruction count and parallelIssues
	// alone is the dual-issue count. Neither is derivable from cycles and
	// stallCycles: a cycle can be charged as a stall because one sequence was
	// blocked and still issue behind it, so `cycles - stallCycles` undercounts
	// slots - by enough that dividing instructions by it gave 2.15 per slot on
	// a machine that cannot exceed 2.
	u32 issueSlots = 0;
	u32 parallelIssues = 0;
	// DIAGNOSTIC. Stall cycles charged to an instruction at or after pairFrom
	// that were blamed on one BEFORE it - i.e. in the steady-state analysis of
	// a block repeated twice, stalls in the second copy caused by the first.
	// A block is analysed as though it looped straight into itself, and real
	// execution enters it from somewhere else, so this is an upper bound on
	// what that assumption invents.
	u32 wrapStalls = 0;
	// DIAGNOSTIC. Flow-dependency stall cycles split by how long the blamed
	// producer sat in the decode stage: `Fast` is one cycle (it issued
	// straight through), `Held` is longer. Since the latency clock starts at
	// D-exit, a producer held in D pushes its result back, so `Held` is the
	// share of the stall total that depends on the ISSUE model being right
	// rather than on the latency rule being right.
	u32 stallsProducerFast = 0;
	u32 stallsProducerHeld = 0;
	// Set when the model failed to make progress and gave up. The cycle count
	// is then a floor, not an answer, and callers must not report it as one.
	bool stuck = false;

	// Cycles in which nothing anywhere in the pipeline was blocked. This is
	// NOT the issue-slot count - see issueSlots above.
	u32 issueCycles() const { return cycles - stallCycles; }
};

// Per-instruction detail, only filled when analyze() is given somewhere to
// put it. This is what answers "why is this line slow" rather than
// "which function is slow".
struct InsnDetail
{
	u32 offset;			// byte offset within the analysed sequence
	u16 op;
	u16 stallCycles;
	StallReason reason;
	u8 blamedBy;		// index of the instruction we waited on, 0xff if none
};

// Analyse a straight-line sequence of `count` SH4 instructions.
// `detail`, if non-null, must have room for `count` entries.
//
// `pairFrom` restricts parallelIssues to instructions at or after that index.
// Steady-state cost is measured by analysing a block twice and looking at the
// second copy, and dual issue cannot be recovered from that by subtraction:
// the last instruction of copy one has nothing to pair with when copy one is
// analysed alone and pairs with the head of copy two when it is not, so the
// difference overcounts by roughly one per block. Counting the second copy
// directly is exact.
// `pcParity` is ((address of ops[0]) >> 1) & 1 - whether the first instruction
// sits at a 4-byte boundary or halfway through one. The SH4 fetches in aligned
// 32-bit pairs, so an instruction at 4n+2 cannot be the first of a co-issued
// pair; it goes alone and the pairing realigns behind it. A caller that does
// not know the address should pass 0, which is the aligned case.
Result analyze(const u16 *ops, u32 count, InsnDetail *detail = nullptr,
		u32 pairFrom = 0, u32 pcParity = 0);

// True if every opcode in the sequence has pipeline data. An opcode with no
// entry is charged its issue rate and no stalls, which understates it - so
// anything reporting these numbers needs to know whether that happened.
bool fullyModelled(const u16 *ops, u32 count, u32 *firstUnknownIndex = nullptr);

}
