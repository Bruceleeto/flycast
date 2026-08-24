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
// Replays a recorded block execution trace through the cache model under a
// modified code layout, and reports what that layout would have cost.
//
// This is the point of the whole exercise. Measuring tells you the miss count
// once; replaying tells you whether moving the code helps, and answers it in
// seconds on a workstation instead of one hardware run per idea.
//
// It links the same model header flycast uses, deliberately: a sweep that
// optimises against a drifted copy of the model would be worse than no sweep.
//
#include "cachesim_model.h"
#include "cachesim_trace.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace cachesim;

namespace
{

struct Block
{
	u32 vaddr;
	u32 paddr;
	u32 size;
	u64 hash;
};

// A range of guest code to move, and by how much. Layout search is address
// arithmetic: this is the whole knob.
struct Shift
{
	u32 start = 0;
	u32 end = 0;
	int64_t delta = 0;

	// Addresses are compared with the region bits only. A trace records a
	// block's address as the guest used it, so the same RAM appears as
	// 0x8c... through P1 and 0x0c... physically; reports print the normalised
	// form, so a region typed in from a report has to match either.
	static u32 normalise(u32 addr) { return addr & 0x1fffffff; }

	bool covers(u32 addr) const
	{
		const u32 a = normalise(addr);
		return a >= normalise(start) && a < normalise(end);
	}
};

struct Options
{
	std::string tracePath;
	u32 lookahead = 0;
	bool iix = false;
	Shift shift;
	// Sweep of shift deltas: from, to, step
	bool sweep = false;
	int64_t from = 0, to = 0, step = 0;
	size_t topN = 10;

	// Discovery: find the regions worth moving instead of being told them
	bool automatic = false;
	size_t regions = 4;		// candidate regions to try
	u32 clusterGap = 64 << 10;	// lines further apart than this start a new region
	int64_t autoStep = 1024;	// shift granularity when sweeping a candidate
};

class Trace
{
public:
	bool load(const std::string& path)
	{
		FILE *f = std::fopen(path.c_str(), "rb");
		if (f == nullptr)
		{
			std::fprintf(stderr, "cannot open %s\n", path.c_str());
			return false;
		}
		std::fseek(f, 0, SEEK_END);
		const long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		bytes.resize(size);
		const size_t read = std::fread(bytes.data(), 1, size, f);
		std::fclose(f);
		bytes.resize(read);

		if (read < 4 || std::memcmp(bytes.data(), TRACE_MAGIC, 4) != 0)
		{
			std::fprintf(stderr, "%s is not a cachesim trace\n", path.c_str());
			return false;
		}
		pos = 4;
		const u64 version = readVarint();
		icSets = (u32)readVarint();
		lineBytes = (u32)readVarint();
		headerEnd = pos;
		if (version != TRACE_VERSION)
		{
			std::fprintf(stderr, "trace version %" PRIu64 ", expected %u\n", version, TRACE_VERSION);
			return false;
		}
		if (icSets != IC_SETS || lineBytes != LINE_BYTES)
		{
			std::fprintf(stderr, "trace geometry %u sets / %u byte lines does not match the model\n",
					icSets, lineBytes);
			return false;
		}
		return true;
	}

	// Replays the whole trace into `model`, moving any block the shift covers.
	// Returns the number of frames seen.
	u64 replay(Model& model, const Options& opt)
	{
		pos = headerEnd;
		blocks.clear();
		u64 frames = 0;
		u64 cycle = 0;

		for (;;)
		{
			if (pos >= bytes.size())
				break;
			const u64 v = readVarint();
			if (v != 0)
			{
				const u32 id = (u32)(v - 1);
				if (id < blocks.size())
					execute(model, blocks[id], opt);
				continue;
			}
			const u8 event = bytes[pos++];
			if (event == TraceEvent::DefineBlock)
			{
				const u32 id = (u32)readVarint();
				Block b;
				b.vaddr = (u32)readVarint();
				b.paddr = (u32)readVarint();
				b.size = (u32)readVarint();
				b.hash = readVarint();
				if (id >= blocks.size())
					blocks.resize(id + 1);
				blocks[id] = b;
			}
			else if (event == TraceEvent::InvalidateInst)
				model.invalidateInst();
			else if (event == TraceEvent::AddressArrayWrite)
			{
				const u32 addr = (u32)readVarint();
				const u32 data = (u32)readVarint();
				// The tag for an associative write needed an address
				// translation that only existed at record time, so those are
				// left alone here rather than guessed at
				model.writeInstAddressArray(addr, data, INVALID_LINE);
			}
			else if (event == TraceEvent::FrameBoundary)
				frames++;
			else if (event == TraceEvent::Cycle)
			{
				cycle += readVarint();
				model.setCycle(cycle);
			}
			else if (event == TraceEvent::End)
				break;
			else
			{
				std::fprintf(stderr, "unknown trace event %u at %zu\n", event, pos);
				break;
			}
		}
		lastCycle = cycle;
		return frames;
	}

	u64 cycles() const { return lastCycle; }
	const std::vector<Block>& blockTable() const { return blocks; }

private:
	void execute(Model& model, const Block& b, const Options& opt)
	{
		u32 vaddr = b.vaddr;
		u32 paddr = b.paddr;
		if (opt.shift.end != 0 && opt.shift.covers(paddr))
		{
			vaddr = (u32)((int64_t)vaddr + opt.shift.delta);
			paddr = (u32)((int64_t)paddr + opt.shift.delta);
		}

		model.countBlockFetch(paddr, b.size, opt.lookahead);

		// P2 and P4 are uncached; everything else is cached, the cache having
		// been on for anything that reached the trace
		const u32 area = vaddr >> 29;
		if (area == 5 || area == 7)
		{
			model.countUncached(Stream::Inst, (b.size + LINE_BYTES - 1) / LINE_BYTES);
			return;
		}

		const u32 phys = paddr & 0x1fffffff;
		const u32 bytesFetched = b.size + opt.lookahead;
		u32 vline = vaddr & ~(LINE_BYTES - 1);
		u32 pline = phys & ~(LINE_BYTES - 1);
		const u32 lastVline = (vaddr + bytesFetched - 1) & ~(LINE_BYTES - 1);
		for (;;)
		{
			model.touchLine(Stream::Inst, Model::instIndex(vline, opt.iix), pline, vaddr, false);
			if (vline == lastVline)
				break;
			vline += LINE_BYTES;
			pline += LINE_BYTES;
		}
	}

	u64 readVarint()
	{
		u64 v = 0;
		int shift = 0;
		while (pos < bytes.size())
		{
			const u8 b = bytes[pos++];
			v |= (u64)(b & 0x7f) << shift;
			if ((b & 0x80) == 0)
				break;
			shift += 7;
		}
		return v;
	}

	std::vector<u8> bytes;
	std::vector<Block> blocks;
	size_t pos = 0;
	size_t headerEnd = 0;
	u32 icSets = 0;
	u32 lineBytes = 0;
	u64 lastCycle = 0;
};

struct Result
{
	u64 misses;
	u64 conflict;
	u64 capacity;
	u64 compulsory;
	u64 invalidated;
	u64 fetches;
};

Result run(Trace& trace, const Options& opt)
{
	Model model;
	model.setMissRingSize(0);
	trace.replay(model, opt);
	const Counters& c = model.counters();
	const int inst = (int)Stream::Inst;
	return { c.misses[inst],
			c.missKinds[inst][(int)MissKind::Conflict],
			c.missKinds[inst][(int)MissKind::Capacity],
			c.missKinds[inst][(int)MissKind::Compulsory],
			c.missKinds[inst][(int)MissKind::Invalidated],
			c.instFetched };
}

void reportDetail(Trace& trace, const Options& opt)
{
	Model model;
	model.setMissRingSize(0);
	const u64 frames = trace.replay(model, opt);
	const Counters& c = model.counters();
	const int inst = (int)Stream::Inst;

	std::printf("blocks %zu   frames %" PRIu64 "   guest cycles %" PRIu64 "\n",
			trace.blockTable().size(), frames, trace.cycles());
	std::printf("fetches %" PRIu64 "   misses %" PRIu64 "   rate %.3f%%\n",
			c.instFetched, c.misses[inst],
			c.instFetched == 0 ? 0.0 : 100.0 * c.misses[inst] / c.instFetched);
	std::printf("  conflict %" PRIu64 "  capacity %" PRIu64 "  compulsory %" PRIu64
			"  invalidated %" PRIu64 "\n\n",
			c.missKinds[inst][(int)MissKind::Conflict],
			c.missKinds[inst][(int)MissKind::Capacity],
			c.missKinds[inst][(int)MissKind::Compulsory],
			c.missKinds[inst][(int)MissKind::Invalidated]);

	std::printf("worst sets\n");
	const Cache& cache = model.cacheFor(Stream::Inst);
	std::vector<u32> order;
	for (u32 set = 0; set < cache.sets; set++)
		if (cache.stats[set].misses != 0)
			order.push_back(set);
	std::sort(order.begin(), order.end(), [&cache](u32 a, u32 b) {
		return cache.stats[a].misses > cache.stats[b].misses;
	});
	if (order.size() > opt.topN)
		order.resize(opt.topN);
	for (u32 set : order)
	{
		std::printf("  set %3u  %10" PRIu64 " misses\n", set, cache.stats[set].misses);
		for (const EvictPair& e : model.setEvictors(Stream::Inst, set))
			std::printf("      %08x evicted %08x  x%" PRIu64 "\n",
					e.line, e.evictedLine, e.count);
	}
}

// A contiguous run of guest code that conflict misses cluster into. Derived
// from the trace rather than supplied, because the point of the tool is to work
// on a guest whose layout nobody has memorised.
struct Region
{
	u32 start;
	u32 end;
	u64 conflict;
};

std::vector<Region> discoverRegions(Model& model, const Options& opt)
{
	std::vector<SiteStat> sites = model.topSites(Stream::Inst, 4096);
	std::vector<SiteStat> conflicted;
	for (const SiteStat& s : sites)
		if (s.kinds[(int)MissKind::Conflict] > 0)
			conflicted.push_back(s);
	std::sort(conflicted.begin(), conflicted.end(),
			[](const SiteStat& a, const SiteStat& b) { return a.line < b.line; });

	std::vector<Region> regions;
	for (const SiteStat& s : conflicted)
	{
		if (!regions.empty() && s.line - regions.back().end < opt.clusterGap)
		{
			regions.back().end = s.line + LINE_BYTES;
			regions.back().conflict += s.kinds[(int)MissKind::Conflict];
			continue;
		}
		regions.push_back({ s.line, s.line + LINE_BYTES, s.kinds[(int)MissKind::Conflict] });
	}
	std::sort(regions.begin(), regions.end(),
			[](const Region& a, const Region& b) { return a.conflict > b.conflict; });
	return regions;
}

void usage(const char *exe)
{
	std::fprintf(stderr,
		"Usage: %s trace [options]\n"
		"\n"
		"Replays a cachesim trace under a modified code layout.\n"
		"\n"
		"  --auto                    find the regions worth moving and sweep them\n"
		"  --regions n               candidates to try in --auto (default 4)\n"
		"  --auto-step n             shift granularity in --auto (default 1024)\n"
		"  --shift start:end:delta   move guest code in [start,end) by delta bytes\n"
		"  --sweep start:end:from:to:step\n"
		"                            replay once per delta and print the curve\n"
		"  --lookahead n             bytes fetched past a block end (match the run)\n"
		"  --iix                     model CCR.IIX index enhancement\n"
		"  --top n                   worst sets to detail (default 10)\n"
		"\n"
		"Addresses are physical and may be given in hex with 0x.\n"
		"Miss counts are the output. Multiply by the measured cycles per miss to\n"
		"get cycles; this tool does not model time.\n", exe);
}

bool parseShift(const char *arg, Options& opt)
{
	return std::sscanf(arg, "%i:%i:%" SCNi64, &opt.shift.start, &opt.shift.end, &opt.shift.delta) == 3;
}

bool parseSweep(const char *arg, Options& opt)
{
	opt.sweep = true;
	return std::sscanf(arg, "%i:%i:%" SCNi64 ":%" SCNi64 ":%" SCNi64,
			&opt.shift.start, &opt.shift.end, &opt.from, &opt.to, &opt.step) == 5;
}

} // namespace

int main(int argc, char *argv[])
{
	Options opt;
	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		if (arg == "--shift" && i + 1 < argc) {
			if (!parseShift(argv[++i], opt)) { usage(argv[0]); return 1; }
		}
		else if (arg == "--sweep" && i + 1 < argc) {
			if (!parseSweep(argv[++i], opt)) { usage(argv[0]); return 1; }
		}
		else if (arg == "--lookahead" && i + 1 < argc)
			opt.lookahead = (u32)strtoul(argv[++i], nullptr, 0);
		else if (arg == "--top" && i + 1 < argc)
			opt.topN = strtoul(argv[++i], nullptr, 0);
		else if (arg == "--auto")
			opt.automatic = true;
		else if (arg == "--regions" && i + 1 < argc)
			opt.regions = strtoul(argv[++i], nullptr, 0);
		else if (arg == "--auto-step" && i + 1 < argc)
			opt.autoStep = strtoll(argv[++i], nullptr, 0);
		else if (arg == "--iix")
			opt.iix = true;
		else if (arg == "-h" || arg == "--help") {
			usage(argv[0]);
			return 0;
		}
		else if (arg[0] == '-') {
			std::fprintf(stderr, "unknown option %s\n", arg.c_str());
			return 1;
		}
		else
			opt.tracePath = arg;
	}
	if (opt.tracePath.empty()) {
		usage(argv[0]);
		return 1;
	}

	Trace trace;
	if (!trace.load(opt.tracePath))
		return 1;

	if (opt.automatic)
	{
		Options base = opt;
		base.shift = Shift{};
		Model probe;
		probe.setMissRingSize(0);
		trace.replay(probe, base);
		const Counters& bc = probe.counters();
		const u64 baseMisses = bc.misses[(int)Stream::Inst];
		const u64 baseConflict = bc.missKinds[(int)Stream::Inst][(int)MissKind::Conflict];
		std::printf("baseline: %" PRIu64 " misses, %" PRIu64 " conflict (%.1f%%)\n\n",
				baseMisses, baseConflict,
				baseMisses == 0 ? 0.0 : 100.0 * baseConflict / baseMisses);

		std::vector<Region> regions = discoverRegions(probe, opt);
		if (regions.size() > opt.regions)
			regions.resize(opt.regions);
		std::printf("candidate regions, by conflict misses:\n");
		for (const Region& r : regions)
			std::printf("  %08x-%08x  %10" PRIu64 " conflict (%.1f%% of all)\n",
					r.start, r.end, r.conflict,
					baseConflict == 0 ? 0.0 : 100.0 * r.conflict / baseConflict);
		std::printf("\n");

		// Shifting a region models inserting padding before it, so everything
		// above moves with it. Sweeping a whole cache size covers every
		// distinct set mapping: a shift of exactly one cache size is a no-op.
		const int64_t span = (int64_t)IC_SETS * LINE_BYTES;
		u64 bestMisses = baseMisses;
		Shift best;
		// Padding before the lowest region moves every piece of code by the same
		// amount, which preserves every relative distance and so cannot change a
		// single set mapping. Worth saying rather than rediscovering by replay.
		u32 lowest = 0xffffffff;
		for (const Region& r : regions)
			lowest = std::min(lowest, r.start);

		for (const Region& r : regions)
		{
			if (r.start == lowest)
			{
				std::printf("region %08x  nothing below it: padding here shifts all code"
						" equally and cannot change the layout\n", r.start);
				continue;
			}
			std::printf("region %08x  padding:\n", r.start);
			for (int64_t delta = opt.autoStep; delta < span; delta += opt.autoStep)
			{
				Options candidate = opt;
				candidate.shift = { r.start, 0x1fffffff, delta };
				const Result res = run(trace, candidate);
				std::printf("  %+7" PRId64 "  %12" PRIu64 " misses  %12" PRIu64 " conflict  %+7.2f%%\n",
						delta, res.misses, res.conflict,
						baseMisses == 0 ? 0.0 : 100.0 * ((double)res.misses - baseMisses) / baseMisses);
				if (res.misses < bestMisses)
				{
					bestMisses = res.misses;
					best = candidate.shift;
				}
			}
		}
		if (best.end != 0)
			std::printf("\nbest: pad %" PRId64 " bytes before %08x -> %" PRIu64 " misses, %.2f%% fewer\n",
					best.delta, best.start, bestMisses,
					100.0 * (baseMisses - bestMisses) / baseMisses);
		else
			std::printf("\nno candidate beat the measured layout\n");
		return 0;
	}

	if (!opt.sweep)
	{
		reportDetail(trace, opt);
		return 0;
	}

	// Baseline first, so every candidate is reported as a delta against the
	// layout that was actually measured
	Options base = opt;
	base.shift = Shift{};
	const Result baseline = run(trace, base);
	std::printf("baseline: %" PRIu64 " misses (%" PRIu64 " conflict) over %" PRIu64 " fetches\n\n",
			baseline.misses, baseline.conflict, baseline.fetches);
	std::printf("%12s %12s %12s %9s %9s\n", "delta", "misses", "conflict", "vs base", "conflict");

	int64_t bestDelta = 0;
	u64 bestMisses = baseline.misses;
	for (int64_t delta = opt.from; delta <= opt.to; delta += opt.step)
	{
		Options candidate = opt;
		candidate.shift.delta = delta;
		const Result r = run(trace, candidate);
		std::printf("%12" PRId64 " %12" PRIu64 " %12" PRIu64 " %8.2f%% %8.2f%%\n",
				delta, r.misses, r.conflict,
				baseline.misses == 0 ? 0.0 : 100.0 * ((double)r.misses - baseline.misses) / baseline.misses,
				baseline.conflict == 0 ? 0.0 : 100.0 * ((double)r.conflict - baseline.conflict) / baseline.conflict);
		if (r.misses < bestMisses)
		{
			bestMisses = r.misses;
			bestDelta = delta;
		}
		if (opt.step == 0)
			break;
	}

	if (bestDelta != 0)
		std::printf("\nbest: shift %" PRId64 " bytes, %" PRIu64 " misses, %.2f%% fewer than baseline\n",
				bestDelta, bestMisses, 100.0 * (baseline.misses - bestMisses) / baseline.misses);
	else
		std::printf("\nno candidate beat the measured layout\n");
	return 0;
}
