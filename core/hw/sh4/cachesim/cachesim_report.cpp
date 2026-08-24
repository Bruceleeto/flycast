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
	markLogWindow();
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

	// The lines that miss most. Address only: naming them is the symbolization
	// layer's job, and code in a guest JIT buffer has no name to give.
	fprintf(f, "  \"top_sites\": [\n");
	const std::vector<SiteStat> sites = topSites(Stream::Inst, 64);
	for (size_t i = 0; i < sites.size(); i++)
	{
		const SiteStat& s = sites[i];
		fprintf(f, "    {\"line\": \"0x%08x\", \"misses\": %" PRIu64 ", \"compulsory\": %" PRIu64
				", \"capacity\": %" PRIu64 ", \"conflict\": %" PRIu64 "}%s\n",
				s.line, s.misses, s.kinds[(int)MissKind::Compulsory],
				s.kinds[(int)MissKind::Capacity], s.kinds[(int)MissKind::Conflict],
				i + 1 == sites.size() ? "" : ",");
	}
	fprintf(f, "  ],\n");

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
