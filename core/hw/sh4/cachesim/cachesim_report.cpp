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
// Machine-readable output for the guest cache simulator. This is the primary
// interface: a headless run writes one JSON file and exits. Anything visual is
// a second reader of the same numbers, never the only way to get at them.
//
#include "cachesim.h"
#include "cachesim_symbols.h"

#include "nowide/cstdio.hpp"

#include <cinttypes>
#include <cstdio>

namespace cachesim
{

static const char *penaltyModelName(PenaltyModel model)
{
	return model == PenaltyModel::RowAware ? "row_aware" : "fixed";
}

static double perSecond(u64 count, u64 cycles)
{
	return cycles == 0 ? 0.0 : (double)count * SH4_MAIN_CLOCK / (double)cycles;
}

void logSummary()
{
	const Counters& c = counters();
	const u64 cycles = guestCycles();
	const int inst = (int)Stream::Inst;

	// The window since the previous line. Reported first because it is the
	// number that means something: the miss rate of this workload swings by
	// more than an order of magnitude between loading, compiling and running,
	// so a running average of all three describes no moment of the run.
	const Counters window = logWindow();
	const u64 windowCycles = logWindowCycles();
	const u64 wMisses = window.misses[inst];

	NOTICE_LOG(SH4, "cachesim frame %" PRIu64 " window: %" PRIu64 " misses"
			" (%.3f%% of %" PRIu64 " instr, %.2fM/s)"
			" %" PRIu64 "%% conflict | derived %.1f%% of guest cycles"
			" || run: %" PRIu64 " misses (%.3f%%, %.1f%%) (%s model)",
			frameCount(), wMisses,
			window.instFetched == 0 ? 0.0 : 100.0 * wMisses / window.instFetched,
			window.instFetched, perSecond(wMisses, windowCycles) / 1e6,
			wMisses == 0 ? 0 : window.missKinds[inst][(int)MissKind::Conflict] * 100 / wMisses,
			windowCycles == 0 ? 0.0 : 100.0 * window.missCycles[inst] / windowCycles,
			c.misses[inst],
			c.instFetched == 0 ? 0.0 : 100.0 * c.misses[inst] / c.instFetched,
			cycles == 0 ? 0.0 : 100.0 * c.missCycles[inst] / cycles,
			penaltyModelName(penaltyConfig().model));
	// Pipeline model. Deliberately its own line and its own percentage: these
	// are issue and interlock cycles with no cache cost in them, and the two
	// numbers are never added together. Validated against a Dreamcast at 0.58
	// cycles mean absolute error per block - see docs/cachesim/pipesim_notes.md
	// for what is still wrong with it.
	const PipeTotals pipe = pipeTotals();
	if (pipe.cycles != 0)
	{
		// Stall counts are events, not cycles - more than one pipeline
		// sequence can stall in the same cycle - so they are shown as shares
		// of each other and never subtracted from the cycle total.
		const double ev = pipe.stalls == 0 ? 1.0 : (double)pipe.stalls;
		NOTICE_LOG(SH4, "cachesim frame %" PRIu64 " pipeline: %" PRIu64 " cycles"
				" (%.1f%% of guest cycles) | stall events %" PRIu64
				": flow-dep %.0f%% resource %.0f%% stage %.0f%% output-dep %.0f%%"
				" | issue+interlock only, no cache cost%s",
				frameCount(), pipe.cycles,
				cycles == 0 ? 0.0 : 100.0 * pipe.cycles / cycles,
				pipe.stalls,
				100.0 * pipe.byReason[(int)pipesim::StallReason::FlowDep] / ev,
				100.0 * pipe.byReason[(int)pipesim::StallReason::ResourceHazard] / ev,
				100.0 * (pipe.byReason[(int)pipesim::StallReason::StageFull]
					+ pipe.byReason[(int)pipesim::StallReason::StageLocked]) / ev,
				100.0 * pipe.byReason[(int)pipesim::StallReason::OutputDep] / ev,
				pipe.unmodelledBlockExecs != 0 ? " (some blocks not fully modelled)" : "");
	}

	if (dataFeed())
	{
		const int data = (int)Stream::Data;
		NOTICE_LOG(SH4, "cachesim frame %" PRIu64 " ocache window: %" PRIu64 " misses"
				" (%.3f%% of %" PRIu64 " accesses) %" PRIu64 "%% conflict"
				" | %" PRIu64 " writebacks | derived %.1f%% of guest cycles",
				frameCount(), window.misses[data],
				window.dataAccesses == 0 ? 0.0 : 100.0 * window.misses[data] / window.dataAccesses,
				window.dataAccesses,
				window.misses[data] == 0 ? 0
						: window.missKinds[data][(int)MissKind::Conflict] * 100 / window.misses[data],
				window.writebacks,
				windowCycles == 0 ? 0.0 : 100.0 * window.missCycles[data] / windowCycles);
	}

	// Store queue flushes. Counted, not charged: the per-flush cost is a
	// hardware measurement still in progress, and this count is the
	// denominator that turns a measured frz_dc figure into cycles per flush.
	{
		const u64 ram = window.sqFlushes[(int)SqDest::Ram];
		const u64 ta = window.sqFlushes[(int)SqDest::Ta];
		const u64 other = window.sqFlushes[(int)SqDest::Other];
		if (ram + ta + other != 0)
			NOTICE_LOG(SH4, "cachesim frame %" PRIu64 " store queue window: %" PRIu64 " flushes"
					" (%" PRIu64 " ram, %" PRIu64 " ta, %" PRIu64 " other) = %" PRIu64 " bytes"
					" | not charged, see plan 9e",
					frameCount(), ram + ta + other, ram, ta, other,
					(ram + ta + other) * 32);
	}

	markLogWindow();
}

// The per-function table. This is the output the whole tool exists to produce:
// where the frame goes, ranked, and enough of a breakdown per row to say what
// to do about it.
void logProfile(size_t limit)
{
	const std::vector<ProfileRow> rows = profile(limit);
	if (rows.empty())
		return;

	const double frameCycles = profileFrameCycles();
	NOTICE_LOG(SH4, "cachesim per-function profile, per frame"
			" (pipeline cycles are issue+interlock, hardware-checked;"
			" cache columns are derived from miss counts)");
	NOTICE_LOG(SH4, "  %-34s %9s %7s %8s %8s %8s %8s %8s %9s",
			"function", "cycles", "%frame", "flow-dep", "resource", "stage",
			"icache", "storeq", "calls");

	for (const ProfileRow& r : rows)
	{
		const double pipe = r.pipeCycles > 0.0 ? r.pipeCycles : r.cycles;
		// Store queue cycles are in; operand cache cycles are excluded on
		// purpose. See the ranking comment in profile().
		const double total = pipe + r.missCycles + r.sqCycles;
		if (total < 1.0)
			continue;
		// Stall columns are events, so they are shown as a share of this row's
		// own stalls rather than as cycles, which they are not.
		const double ev = r.pipeFlowDep + r.pipeResource + r.pipeStage;
		NOTICE_LOG(SH4, "  %-34s %9.0f %6.1f%% %7.0f%% %7.0f%% %7.0f%% %8.0f %8.0f %9.1f%s",
				r.name.c_str(), total,
				frameCycles == 0.0 ? 0.0 : 100.0 * total / frameCycles,
				ev == 0.0 ? 0.0 : 100.0 * r.pipeFlowDep / ev,
				ev == 0.0 ? 0.0 : 100.0 * r.pipeResource / ev,
				ev == 0.0 ? 0.0 : 100.0 * r.pipeStage / ev,
				r.missCycles, r.sqCycles, r.calls,
				r.pipeComplete ? "" : " *");
	}
	NOTICE_LOG(SH4, "  (* = a block in this row had an opcode with no pipeline"
			" data and was charged its issue rate with no stalls)");
}

// Per-block breakdown. The per-function table cannot see inside a renderer's
// main loop, because the whole loop is one inlined symbol. This can.
void logBlocks(size_t limit)
{
	struct Row
	{
		u32 vaddr;
		u32 size;
		double execs;
		double cycles;
		double sqCycles;
		u32 pipeCycles;
		// u32 rather than u16: `stage` is the sum of two counters, and the
		// addition promotes to int before it is stored
		u32 flowDep, resource, stage;
		bool modelled;
	};
	std::vector<Row> rows;
	const double sqCost = penaltyConfig().sqFlushCycles;
	for (const BlockTrace& b : blocks())
	{
		const double execs = blockExecsPerFrame(b.id);
		if (execs < 0.5)
			continue;
		const double sq = blockSqFlushesPerFrame(b.id) * sqCost;
		rows.push_back({ b.vaddr, b.size, execs, execs * b.pipeCycles + sq, sq,
				b.pipeCycles,
				b.pipeByReason[(int)pipesim::StallReason::FlowDep],
				b.pipeByReason[(int)pipesim::StallReason::ResourceHazard],
				(u32)(b.pipeByReason[(int)pipesim::StallReason::StageFull]
						+ b.pipeByReason[(int)pipesim::StallReason::StageLocked]),
				b.pipeModelled });
	}
	if (rows.empty())
		return;
	std::sort(rows.begin(), rows.end(),
			[](const Row& a, const Row& b) { return a.cycles > b.cycles; });
	if (rows.size() > limit)
		rows.resize(limit);

	const double frameCycles = profileFrameCycles();
	NOTICE_LOG(SH4, "cachesim per-block profile, per frame"
			" (cyc/exec is one execution, steady state)");
	NOTICE_LOG(SH4, "  %-10s %5s %9s %7s %9s %8s %8s %8s %8s %8s",
			"address", "bytes", "cycles", "%frame", "execs", "cyc/exec",
			"flow-dep", "resource", "stage", "storeq");
	for (const Row& r : rows)
		NOTICE_LOG(SH4, "  %08x   %5u %9.0f %6.1f%% %9.0f %8u %8u %8u %8u %8.0f%s",
				r.vaddr, r.size, r.cycles,
				frameCycles == 0.0 ? 0.0 : 100.0 * r.cycles / frameCycles,
				r.execs, r.pipeCycles, r.flowDep, r.resource, r.stage,
				r.sqCycles, r.modelled ? "" : " *");
}

bool writeReport(const std::string& path)
{
	FILE *f = nowide::fopen(path.c_str(), "w");
	if (f == nullptr)
	{
		WARN_LOG(SH4, "cachesim: cannot write %s", path.c_str());
		return false;
	}

	const Counters& c = counters();
	const PenaltyConfig& penalty = penaltyConfig();
	const u64 cycles = guestCycles();
	const int inst = (int)Stream::Inst;

	fprintf(f, "{\n");
	fprintf(f, "  \"schema\": \"flycast-cachesim/1\",\n");
	// Every consumer of this file should see the limits next to the numbers
	fprintf(f, "  \"limits\": [\n");
	fprintf(f, "    \"Miss counts are measured; every cycle figure is derived from the penalty model below.\",\n");
	fprintf(f, "    \"Instruction fetch past a taken branch is not modelled: up to one line per block end is undercounted.\",\n");
	fprintf(f, "    \"Addresses inside guest-generated code are not stable across runs. Use block hashes to compare runs.\",\n");
	fprintf(f, "    \"Flycast's own timing does not charge these penalties, so its frame rate does not reflect them.\",\n");
	fprintf(f, "    \"line_touches counts a line once per block, not once per instruction fetch:"
			" use instructions_fetched as the miss rate denominator.\"\n");
	fprintf(f, "  ],\n");
	fprintf(f, "  \"penalty_model\": {\"name\": \"%s\", \"fixed_cycles\": %.3f,"
			" \"row_hit_cycles\": %.3f, \"row_miss_cycles\": %.3f, \"row_shift\": %u},\n",
			penaltyModelName(penalty.model), penalty.fixedCycles,
			penalty.rowHitCycles, penalty.rowMissCycles, penalty.rowShift);
	fprintf(f, "  \"block_lookahead_bytes\": %u,\n", blockLookahead());
	fprintf(f, "  \"frames\": %" PRIu64 ",\n", frameCount());
	fprintf(f, "  \"guest_cycles\": %" PRIu64 ",\n", cycles);
	fprintf(f, "  \"guest_seconds\": %.6f,\n", (double)cycles / SH4_MAIN_CLOCK);
	fprintf(f, "  \"blocks_traced\": %zu,\n", blocks().size());

	// Names are decoration, and only shown once the binary they came from has
	// been checked against the code that actually ran
	const SymbolVerification symbols = verifySymbols();
	if (symbolsLoaded())
		fprintf(f, "  \"symbols\": {\"source\": \"%s\", \"blocks_checked\": %zu,"
				" \"blocks_matched\": %zu, \"trusted\": %s},\n",
				symbolSource().c_str(), symbols.checked, symbols.matched,
				symbols.trusted ? "true" : "false");
	else
		fprintf(f, "  \"symbols\": null,\n");

	fprintf(f, "  \"icache\": {\n");
	fprintf(f, "    \"sets\": %u, \"line_bytes\": %u, \"bytes\": %u,\n",
			IC_SETS, LINE_BYTES, IC_SETS * LINE_BYTES);
	fprintf(f, "    \"instructions_fetched\": %" PRIu64 ",\n", c.instFetched);
	// What an SH4 fetch counter counts: 32-bit accesses, up to two instructions
	// each. Compare hardware fetch counters against this, never against
	// instructions_fetched.
	fprintf(f, "    \"fetch_ops\": %" PRIu64 ",\n", c.fetchOps);
	fprintf(f, "    \"instructions_per_fetch_op\": %.4f,\n",
			c.fetchOps == 0 ? 0.0 : (double)c.instFetched / c.fetchOps);
	fprintf(f, "    \"fetch_ops_per_second\": %.1f,\n", perSecond(c.fetchOps, cycles));
	fprintf(f, "    \"line_touches\": %" PRIu64 ",\n", c.lineTouches[inst]);
	fprintf(f, "    \"uncached_accesses\": %" PRIu64 ",\n", c.uncachedAccesses[inst]);
	fprintf(f, "    \"invalidations\": %" PRIu64 ",\n", c.invalidations);
	fprintf(f, "    \"misses\": %" PRIu64 ",\n", c.misses[inst]);
	fprintf(f, "    \"compulsory\": %" PRIu64 ",\n", c.missKinds[inst][(int)MissKind::Compulsory]);
	fprintf(f, "    \"capacity\": %" PRIu64 ",\n", c.missKinds[inst][(int)MissKind::Capacity]);
	fprintf(f, "    \"conflict\": %" PRIu64 ",\n", c.missKinds[inst][(int)MissKind::Conflict]);
	fprintf(f, "    \"invalidated\": %" PRIu64 ",\n", c.missKinds[inst][(int)MissKind::Invalidated]);
	fprintf(f, "    \"miss_rate_per_fetch\": %.6f,\n",
			c.instFetched == 0 ? 0.0 : (double)c.misses[inst] / c.instFetched);
	fprintf(f, "    \"misses_per_second\": %.1f,\n", perSecond(c.misses[inst], cycles));
	fprintf(f, "    \"derived_miss_cycles\": %.1f,\n", c.missCycles[inst]);
	fprintf(f, "    \"derived_cycle_fraction\": %.6f\n",
			cycles == 0 ? 0.0 : c.missCycles[inst] / cycles);
	fprintf(f, "  },\n");

	fprintf(f, "  \"timing_feedback\": {\n");
	fprintf(f, "    \"enabled\": %s,\n", timingFeedback() ? "true" : "false");
	fprintf(f, "    \"charged_cycles\": %" PRIu64 ",\n", chargedTimingCycles());
	// The share of the run that the model added to the guest's own time. With
	// the mode off this is zero and the run is an observation; with it on, the
	// guest did less work per frame than it otherwise would have, and no number
	// from this run is comparable with one from a run without it.
	fprintf(f, "    \"charged_fraction\": %.6f\n",
			cycles == 0 ? 0.0 : (double)chargedTimingCycles() / cycles);
	fprintf(f, "  },\n");

	// Operand cache. Present whether or not the feed was on: all zeroes says
	// "not measured", and a reader that cannot tell the difference between
	// "no misses" and "not looked at" would draw the wrong conclusion from a
	// missing section.
	const int data = (int)Stream::Data;
	fprintf(f, "  \"ocache\": {\n");
	fprintf(f, "    \"measured\": %s,\n", dataFeed() ? "true" : "false");
	fprintf(f, "    \"sets\": %u, \"line_bytes\": %u, \"bytes\": %u,\n",
			OC_SETS, LINE_BYTES, OC_SETS * LINE_BYTES);
	fprintf(f, "    \"accesses\": %" PRIu64 ",\n", c.dataAccesses);
	fprintf(f, "    \"line_touches\": %" PRIu64 ",\n", c.lineTouches[data]);
	fprintf(f, "    \"uncached_accesses\": %" PRIu64 ",\n", c.uncachedAccesses[data]);
	fprintf(f, "    \"misses\": %" PRIu64 ",\n", c.misses[data]);
	fprintf(f, "    \"compulsory\": %" PRIu64 ",\n", c.missKinds[data][(int)MissKind::Compulsory]);
	fprintf(f, "    \"capacity\": %" PRIu64 ",\n", c.missKinds[data][(int)MissKind::Capacity]);
	fprintf(f, "    \"conflict\": %" PRIu64 ",\n", c.missKinds[data][(int)MissKind::Conflict]);
	fprintf(f, "    \"invalidated\": %" PRIu64 ",\n", c.missKinds[data][(int)MissKind::Invalidated]);
	// Stores that missed a write-through line. Not misses: the line was never
	// going to be allocated, so no layout change removes them.
	fprintf(f, "    \"write_through_misses\": %" PRIu64 ",\n", c.writeThroughMisses);
	// Dirty lines written out. Counted, and charged only if a writeback cost
	// has been set: the write-back buffer hides most of it.
	fprintf(f, "    \"writebacks\": %" PRIu64 ",\n", c.writebacks);
	fprintf(f, "    \"sq_flushes_ram\": %" PRIu64 ",\n", c.sqFlushes[(int)SqDest::Ram]);
	fprintf(f, "    \"sq_flushes_ta\": %" PRIu64 ",\n", c.sqFlushes[(int)SqDest::Ta]);
	fprintf(f, "    \"sq_flushes_other\": %" PRIu64 ",\n", c.sqFlushes[(int)SqDest::Other]);
	fprintf(f, "    \"writeback_cycles\": %.1f,\n", c.writebackCycles);
	fprintf(f, "    \"miss_rate_per_access\": %.6f,\n",
			c.dataAccesses == 0 ? 0.0 : (double)c.misses[data] / c.dataAccesses);
	fprintf(f, "    \"misses_per_second\": %.1f,\n", perSecond(c.misses[data], cycles));
	fprintf(f, "    \"derived_miss_cycles\": %.1f,\n", c.missCycles[data]);
	fprintf(f, "    \"derived_cycle_fraction\": %.6f\n",
			cycles == 0 ? 0.0 : c.missCycles[data] / cycles);
	fprintf(f, "  },\n");

	// The lines that miss most. Address only: naming them is the symbolization
	// layer's job, and code in a guest JIT buffer has no name to give.
	fprintf(f, "  \"top_sites\": [\n");
	const std::vector<SiteStat> sites = topSites(Stream::Inst, 64);
	for (size_t i = 0; i < sites.size(); i++)
	{
		const SiteStat& s = sites[i];
		const char *name = symbols.trusted ? symbolFor(s.line) : nullptr;
		fprintf(f, "    {\"line\": \"0x%08x\", \"misses\": %" PRIu64 ", \"compulsory\": %" PRIu64
				", \"capacity\": %" PRIu64 ", \"conflict\": %" PRIu64 ", \"symbol\": ",
				s.line, s.misses, s.kinds[(int)MissKind::Compulsory],
				s.kinds[(int)MissKind::Capacity], s.kinds[(int)MissKind::Conflict]);
		if (name != nullptr)
			fprintf(f, "\"%s\"}%s\n", name, i + 1 == sites.size() ? "" : ",");
		else
			// Generated code has no name to give, and an untrusted binary must
			// not supply one
			fprintf(f, "null}%s\n", i + 1 == sites.size() ? "" : ",");
	}
	fprintf(f, "  ],\n");

	// Where the frame goes. Per frame and smoothed, never a run total: a total
	// is mostly loading and compilation and describes no frame that happened.
	fprintf(f, "  \"profile_per_frame\": {\n");
	fprintf(f, "    \"guest_cycles\": %.0f,\n", profileFrameCycles());
	fprintf(f, "    \"accounted_cycles\": %.0f,\n", profileAccountedCycles());
	fprintf(f, "    \"note\": \"estimated issue cycles from flycast's model, not hardware;"
			" ranking is meaningful, absolute values are not. Unaccounted cycles are the CPU"
			" idle, asleep or spinning outside any block.\",\n");
	fprintf(f, "    \"rows\": [\n");
	const std::vector<ProfileRow> rows = profile(64);
	for (size_t i = 0; i < rows.size(); i++)
	{
		const ProfileRow& r = rows[i];
		fprintf(f, "      {\"name\": \"%s\", \"named\": %s, \"start\": \"0x%08x\","
				" \"cycles\": %.0f, \"icache_cycles\": %.0f, \"ocache_cycles\": %.0f,"
				" \"calls\": %.1f, \"pipe_cycles\": %.0f, \"pipe_flow_dep\": %.0f,"
				" \"pipe_resource\": %.0f, \"pipe_stage\": %.0f,"
				" \"pipe_complete\": %s}%s\n",
				r.name.c_str(), r.named ? "true" : "false", r.start,
				r.cycles, r.missCycles, r.dataMissCycles, r.calls,
				r.pipeCycles, r.pipeFlowDep, r.pipeResource, r.pipeStage,
				r.pipeComplete ? "true" : "false",
				i + 1 == rows.size() ? "" : ",");
	}
	fprintf(f, "    ]\n  },\n");

	// Per-set pressure, and for each set the lines that threw each other out.
	// This is the part a layout change acts on.
	fprintf(f, "  \"sets\": [\n");
	const SetStat *stats = setStats(Stream::Inst);
	bool firstSet = true;
	for (u32 set = 0; set < IC_SETS; set++)
	{
		if (stats[set].misses == 0)
			continue;
		if (!firstSet)
			fprintf(f, ",\n");
		firstSet = false;
		fprintf(f, "    {\"set\": %u, \"accesses\": %" PRIu64 ", \"misses\": %" PRIu64 ", \"evictors\": [",
				set, stats[set].accesses, stats[set].misses);
		const std::vector<EvictPair> evictors = setEvictors(Stream::Inst, set);
		for (size_t i = 0; i < evictors.size(); i++)
			fprintf(f, "%s{\"line\": \"0x%08x\", \"evicted\": \"0x%08x\", \"count\": %" PRIu64 "}",
					i == 0 ? "" : ", ", evictors[i].line, evictors[i].evictedLine, evictors[i].count);
		fprintf(f, "]}");
	}
	fprintf(f, "\n  ]\n}\n");

	fclose(f);
	NOTICE_LOG(SH4, "cachesim: wrote %s", path.c_str());
	return true;
}

} // namespace cachesim
