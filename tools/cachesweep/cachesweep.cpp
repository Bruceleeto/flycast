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

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
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
	std::string jsonPath;
	u32 lookahead = 0;
	bool iix = false;
	Shift shift;
	// Sweep of shift deltas: from, to, step
	bool sweep = false;
	int64_t from = 0, to = 0, step = 0;
	size_t topN = 5;

	// Moves already decided on, applied before `shift`. The reorder search
	// accumulates into this: each unit it places stays placed while the next
	// one is searched.
	std::vector<Shift> fixed;

	// Symbol names for the report. Decoration only - every search works on
	// addresses, because generated code has no symbols and never will.
	std::string symbolPath;
	// Names for generated code, dumped by the guest's own JIT
	std::string jitmapPath;

	// Discovery: find the regions worth moving instead of being told them
	bool automatic = false;
	bool reorder = false;
	// Storms
	bool storms = false;
	// Flat execution profile: no cache model, just where the instructions go
	bool profile = false;
	size_t callerRows = 0;	// callers to break down for each of the top rows
	u64 bucketCycles = 32768;	// ~0.16 ms, about 100 buckets per frame
	double stormFactor = 4.0;
	size_t reorderUnits = 4;	// functions to place
	int64_t reorderStep = 512;	// alignment granularity searched per function
	size_t regions = 4;		// candidate regions to try
	u32 clusterGap = 64 << 10;	// lines further apart than this start a new region
	int64_t autoStep = 1024;	// shift granularity when sweeping a candidate
};

// Watches a replay as it happens. The trace carries a timestamp every 4096
// cycles, which is what makes it possible to ask when the misses happened
// rather than only how many there were.
struct ReplayWatcher
{
	virtual ~ReplayWatcher() = default;
	// Called after each block, with what that block cost
	virtual void block(u64 cycle, u32 blockId, u64 misses, u64 conflict) = 0;
	virtual void frame(u64 cycle) = 0;
};

// Machine-readable output, in the same spirit as flycast's own report: the
// terminal text is for reading, this is for diffing one experiment against the
// next. Written alongside the text rather than instead of it.
struct JsonOut
{
	FILE *f = nullptr;

	bool open(const std::string& path)
	{
		if (path.empty())
			return true;
		f = std::fopen(path.c_str(), "w");
		if (f == nullptr)
			std::fprintf(stderr, "cannot write %s\n", path.c_str());
		return f != nullptr;
	}
	void close()
	{
		if (f != nullptr)
			std::fclose(f);
		f = nullptr;
	}
	explicit operator bool() const { return f != nullptr; }

	void raw(const char *text) { if (f != nullptr) std::fputs(text, f); }

	template<typename... A>
	void w(const char *fmt, A... a) { if (f != nullptr) std::fprintf(f, fmt, a...); }

	// Symbol names come from an ELF somebody else built, so they are escaped
	// rather than trusted to be JSON-safe
	void str(const char *v)
	{
		if (f == nullptr)
			return;
		std::fputc('"', f);
		for (const char *p = v; *p != 0; p++)
		{
			if (*p == '"' || *p == '\\')
				std::fputc('\\', f);
			if ((unsigned char)*p < 0x20)
				continue;
			std::fputc(*p, f);
		}
		std::fputc('"', f);
	}
};

JsonOut json;

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
	u64 replay(Model& model, const Options& opt, ReplayWatcher *watcher = nullptr)
	{
		pos = headerEnd;
		blocks.clear();
		u64 frames = 0;
		u64 cycle = 0;
		executions = 0;

		for (;;)
		{
			if (pos >= bytes.size())
				break;
			const u64 v = readVarint();
			if (v != 0)
			{
				const u32 id = (u32)(v - 1);
				if (id < blocks.size())
				{
					executions++;
					if (watcher == nullptr)
					{
						execute(model, blocks[id], opt);
						continue;
					}
					const Counters& c = model.counters();
					const u64 m0 = c.misses[(int)Stream::Inst];
					const u64 k0 = c.missKinds[(int)Stream::Inst][(int)MissKind::Conflict];
					execute(model, blocks[id], opt);
					watcher->block(cycle, id, c.misses[(int)Stream::Inst] - m0,
							c.missKinds[(int)Stream::Inst][(int)MissKind::Conflict] - k0);
				}
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
			{
				frames++;
				if (watcher != nullptr)
					watcher->frame(cycle);
			}
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

	// Counts where the instructions go, without modelling anything. The cache
	// report ranks by misses, which is a different question from where the time
	// is: a function can be most of the frame and never miss.
	// `edges` is keyed by (previous block << 32 | this block), which is the call
	// graph the trace already contains without recording anything extra.
	struct Profile
	{
		std::vector<u64> execs;
		std::unordered_map<u64, u64> edges;
		u64 frames = 0;
	};

	Profile profile()
	{
		Profile p;
		pos = headerEnd;
		blocks.clear();
		u32 prev = UINT32_MAX;
		for (;;)
		{
			if (pos >= bytes.size())
				break;
			const u64 v = readVarint();
			if (v != 0)
			{
				const u32 id = (u32)(v - 1);
				if (id < blocks.size())
				{
					if (id >= p.execs.size())
						p.execs.resize(blocks.size(), 0);
					p.execs[id]++;
					if (prev != UINT32_MAX && prev != id && p.edges.size() < (4u << 20))
						p.edges[((u64)prev << 32) | id]++;
					prev = id;
				}
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
				if (id >= p.execs.size())
					p.execs.resize(id + 1, 0);
			}
			else if (event == TraceEvent::AddressArrayWrite)
			{
				readVarint();
				readVarint();
			}
			else if (event == TraceEvent::FrameBoundary)
				p.frames++;
			else if (event == TraceEvent::Cycle)
				readVarint();
			else if (event == TraceEvent::End)
				break;
			else if (event != TraceEvent::InvalidateInst)
			{
				std::fprintf(stderr, "unknown trace event %u at %zu\n", event, pos);
				break;
			}
		}
		p.execs.resize(blocks.size(), 0);
		return p;
	}

	u64 cycles() const { return lastCycle; }
	u64 blockExecutions() const { return executions; }
	const std::vector<Block>& blockTable() const { return blocks; }

private:
	void execute(Model& model, const Block& b, const Options& opt)
	{
		u32 vaddr = b.vaddr;
		u32 paddr = b.paddr;
		int64_t delta = 0;
		// First match wins. Moves never overlap: a placed unit is given an
		// address range of its own, so the order here cannot change a result,
		// only the cost of finding one.
		for (const Shift& m : opt.fixed)
			if (m.end != 0 && m.covers(paddr))
			{
				delta = m.delta;
				break;
			}
		if (delta == 0 && opt.shift.end != 0 && opt.shift.covers(paddr))
			delta = opt.shift.delta;
		if (delta != 0)
		{
			vaddr = (u32)((int64_t)vaddr + delta);
			paddr = (u32)((int64_t)paddr + delta);
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
	u64 executions = 0;
};

// ---------------------------------------------------------------------------
// Symbols
//
// Read straight out of the ELF here rather than reusing flycast's symbol layer,
// which is tied to the emulator. Names are decoration: every search below works
// on addresses, because a retail disc and a JIT buffer have no symbols at all.
// ---------------------------------------------------------------------------

struct Sym
{
	u32 start;
	u32 size;
	std::string name;
};

std::vector<Sym> g_syms;	// sorted by start, non-overlapping enough to bisect

struct ElfReader
{
	const std::vector<u8>& b;
	bool le;

	uint16_t u16At(size_t o) const
	{
		if (o + 2 > b.size())
			return 0;
		return le ? (uint16_t)(b[o] | (b[o + 1] << 8)) : (uint16_t)((b[o] << 8) | b[o + 1]);
	}
	u32 u32At(size_t o) const
	{
		if (o + 4 > b.size())
			return 0;
		return le ? (u32)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((u32)b[o + 3] << 24))
				  : (u32)((u32)(b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]);
	}
};

bool loadElfSymbols(const std::string& path)
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
	std::vector<u8> b((size_t)std::max(size, 0L));
	b.resize(std::fread(b.data(), 1, b.size(), f));
	std::fclose(f);

	if (b.size() < 52 || b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F')
	{
		std::fprintf(stderr, "%s is not an ELF\n", path.c_str());
		return false;
	}
	if (b[4] != 1)
	{
		std::fprintf(stderr, "%s is not 32 bit; SH4 images are\n", path.c_str());
		return false;
	}
	const ElfReader r{ b, b[5] != 2 };

	const u32 shoff = r.u32At(0x20);
	const uint16_t shentsize = r.u16At(0x2e);
	const uint16_t shnum = r.u16At(0x30);
	if (shoff == 0 || shentsize < 40 || shnum == 0)
	{
		std::fprintf(stderr, "%s has no section table, so no symbols\n", path.c_str());
		return false;
	}

	for (uint16_t i = 0; i < shnum; i++)
	{
		const size_t sh = shoff + (size_t)i * shentsize;
		if (r.u32At(sh + 4) != 2)		// SHT_SYMTAB
			continue;
		const u32 off = r.u32At(sh + 16);
		const u32 len = r.u32At(sh + 20);
		const u32 link = r.u32At(sh + 24);
		const u32 entsize = r.u32At(sh + 36);
		if (entsize < 16 || link >= shnum)
			continue;
		const size_t strsh = shoff + (size_t)link * shentsize;
		const u32 stroff = r.u32At(strsh + 16);
		const u32 strlen_ = r.u32At(strsh + 20);

		for (u32 o = 0; o + entsize <= len; o += entsize)
		{
			const size_t e = off + o;
			const u32 nameOff = r.u32At(e);
			const u32 value = r.u32At(e + 4);
			const u32 symSize = r.u32At(e + 8);
			const u8 info = e + 12 < b.size() ? b[e + 12] : 0;
			if ((info & 0xf) != 2 || symSize == 0)	// STT_FUNC with a body
				continue;
			if (nameOff >= strlen_)
				continue;
			const char *name = (const char *)b.data() + stroff + nameOff;
			const size_t maxLen = b.size() - (stroff + nameOff);
			g_syms.push_back({ value, symSize, std::string(name, strnlen(name, maxLen)) });
		}
	}
	std::sort(g_syms.begin(), g_syms.end(),
			[](const Sym& a, const Sym& c) { return a.start < c.start; });
	std::printf("%zu function symbols from %s\n", g_syms.size(), path.c_str());
	return !g_syms.empty();
}

// Region bits only: a trace records addresses as the guest used them, so the
// same RAM appears through P1 while an ELF names it physically.
// Names generated code from a block map the guest dumped: one line per block,
// "LRBLK <guest_pc> <host_addr> <host_size>". Generated code has no symbols and
// never will, but the JIT that emitted it knows exactly which guest function
// each block came from, so this is the only way that third of the profile ever
// gets a name.
bool loadJitMap(const std::string& path)
{
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr)
	{
		std::fprintf(stderr, "cannot open %s\n", path.c_str());
		return false;
	}
	size_t added = 0, skipped = 0;
	char line[256];
	while (std::fgets(line, sizeof(line), f) != nullptr)
	{
		u32 guest = 0, host = 0, size = 0;
		if (std::sscanf(line, "LRBLK %x %x %u", &guest, &host, &size) != 3)
			continue;
		// Blocks registered but never emitted are printed with a zero host
		// address: they occupy no space and would otherwise alias address 0
		if (host == 0 || size == 0)
		{
			skipped++;
			continue;
		}
		char name[32];
		std::snprintf(name, sizeof(name), "ps1:%08x", guest);
		g_syms.push_back({ host, size, name });
		added++;
	}
	std::fclose(f);
	std::sort(g_syms.begin(), g_syms.end(),
			[](const Sym& a, const Sym& b) { return Shift::normalise(a.start) < Shift::normalise(b.start); });
	std::printf("%zu generated blocks from %s (%zu never emitted)\n", added, path.c_str(), skipped);
	return added != 0;
}

const Sym *symbolFor(u32 addr)
{
	const u32 a = Shift::normalise(addr);
	auto it = std::upper_bound(g_syms.begin(), g_syms.end(), a,
			[](u32 v, const Sym& s) { return v < Shift::normalise(s.start); });
	if (it == g_syms.begin())
		return nullptr;
	--it;
	const u32 start = Shift::normalise(it->start);
	return a < start + it->size ? &*it : nullptr;
}

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

	// Instructions per block execution. A guest running JIT-generated code has
	// far shorter blocks than a normal game, and short blocks mean the dynarec
	// spends proportionally more of its time dispatching than executing.
	std::printf("block executions %" PRIu64 "   instructions per execution %.2f\n",
			trace.blockExecutions(),
			trace.blockExecutions() == 0 ? 0.0
					: (double)c.instFetched / trace.blockExecutions());
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

	if (json)
	{
		json.w("{\n  \"schema\": \"cachesweep/1\",\n  \"mode\": \"detail\",\n");
		json.w("  \"blocks\": %zu,\n  \"frames\": %" PRIu64 ",\n  \"guest_cycles\": %"
				PRIu64 ",\n", trace.blockTable().size(), frames, trace.cycles());
		json.w("  \"fetches\": %" PRIu64 ",\n  \"misses\": %" PRIu64 ",\n",
				c.instFetched, c.misses[inst]);
		json.w("  \"conflict\": %" PRIu64 ", \"capacity\": %" PRIu64
				", \"compulsory\": %" PRIu64 ", \"invalidated\": %" PRIu64 ",\n",
				c.missKinds[inst][(int)MissKind::Conflict],
				c.missKinds[inst][(int)MissKind::Capacity],
				c.missKinds[inst][(int)MissKind::Compulsory],
				c.missKinds[inst][(int)MissKind::Invalidated]);
		json.raw("  \"worst_sets\": [");
	}
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
	bool firstSet = true;
	for (u32 set : order)
	{
		std::printf("  set %3u  %10" PRIu64 " misses\n", set, cache.stats[set].misses);
		if (json)
		{
			json.raw(firstSet ? "\n    " : ",\n    ");
			firstSet = false;
			json.w("{\"set\": %u, \"misses\": %" PRIu64 ", \"evictors\": [",
					set, cache.stats[set].misses);
			bool firstEvictor = true;
			for (const EvictPair& e : model.setEvictors(Stream::Inst, set))
			{
				json.w("%s{\"line\": \"0x%08x\", \"evicted\": \"0x%08x\","
						" \"count\": %" PRIu64 ", \"symbol\": ",
						firstEvictor ? "" : ", ", e.line, e.evictedLine, e.count);
				firstEvictor = false;
				const Sym *sym = symbolFor(e.line);
				if (sym != nullptr)
					json.str(sym->name.c_str());
				else
					json.raw("null");
				json.raw("}");
			}
			json.raw("]}");
		}
		for (const EvictPair& e : model.setEvictors(Stream::Inst, set))
		{
			// The pair is the useful part: what keeps throwing what out. Names
			// turn that into two functions somebody can move.
			const Sym *line = symbolFor(e.line);
			const Sym *evicted = symbolFor(e.evictedLine);
			std::printf("      %08x evicted %08x  x%" PRIu64 "%s%s%s%s\n",
					e.line, e.evictedLine, e.count,
					line != nullptr ? "   " : "",
					line != nullptr ? line->name.c_str() : "",
					evicted != nullptr ? " over " : "",
					evicted != nullptr ? evicted->name.c_str() : "");
		}
	}
	json.raw(firstSet ? "]\n}\n" : "\n  ]\n}\n");
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

// ---------------------------------------------------------------------------
// Reorder
//
// Padding shifts everything above it, which is a blunt knob: it changes every
// distance at once. Moving one function changes only that function's set
// mapping, which is what a linker script or a section attribute actually does.
//
// A unit is relocated into an arena above every address the trace ever touched,
// so no two units can collide and the search never has to reason about holes.
// Only the low bits of the destination matter - the index is the address modulo
// the cache size - so what the search reports is an alignment, not a place.
// ---------------------------------------------------------------------------

struct Unit
{
	u32 start;
	u32 end;
	u64 conflict;
	std::string name;
};

std::vector<Unit> discoverUnits(Model& model, const Options& opt)
{
	std::vector<SiteStat> sites = model.topSites(Stream::Inst, 8192);
	std::vector<Unit> units;

	if (!g_syms.empty())
	{
		// Group conflict misses by the function they landed in. Lines with no
		// symbol - generated code, mostly - fall through to the clustering
		// below so that a JIT buffer is still a candidate.
		std::vector<u64> perSym(g_syms.size(), 0);
		std::vector<SiteStat> unnamed;
		for (const SiteStat& s : sites)
		{
			const u64 c = s.kinds[(int)MissKind::Conflict];
			if (c == 0)
				continue;
			const Sym *sym = symbolFor(s.line);
			if (sym != nullptr)
				perSym[sym - &g_syms[0]] += c;
			else
				unnamed.push_back(s);
		}
		for (size_t i = 0; i < g_syms.size(); i++)
			if (perSym[i] != 0)
				units.push_back({ Shift::normalise(g_syms[i].start),
						Shift::normalise(g_syms[i].start) + g_syms[i].size,
						perSym[i], g_syms[i].name });

		std::sort(unnamed.begin(), unnamed.end(),
				[](const SiteStat& a, const SiteStat& b) { return a.line < b.line; });
		for (const SiteStat& s : unnamed)
		{
			const u32 line = Shift::normalise(s.line);
			if (!units.empty() && units.back().name.empty()
					&& line - units.back().end < opt.clusterGap)
			{
				units.back().end = line + LINE_BYTES;
				units.back().conflict += s.kinds[(int)MissKind::Conflict];
				continue;
			}
			units.push_back({ line, line + LINE_BYTES, s.kinds[(int)MissKind::Conflict], "" });
		}
	}
	else
	{
		for (const Region& r : discoverRegions(model, opt))
			units.push_back({ Shift::normalise(r.start), Shift::normalise(r.end), r.conflict, "" });
	}

	std::sort(units.begin(), units.end(),
			[](const Unit& a, const Unit& b) { return a.conflict > b.conflict; });
	return units;
}

std::string unitLabel(const Unit& u)
{
	if (!u.name.empty())
		return u.name;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%08x-%08x", u.start, u.end);
	return buf;
}

int runReorder(Trace& trace, Options opt)
{
	const int64_t span = (int64_t)IC_SETS * LINE_BYTES;
	if (opt.reorderStep <= 0 || (span % opt.reorderStep) != 0)
	{
		std::fprintf(stderr, "--reorder-step must divide the cache size (%" PRId64 ")\n", span);
		return 1;
	}

	Options base = opt;
	base.shift = Shift{};
	base.fixed.clear();
	Model probe;
	probe.setMissRingSize(0);
	trace.replay(probe, base);
	const u64 baseMisses = probe.counters().misses[(int)Stream::Inst];
	const u64 baseConflict = probe.counters().missKinds[(int)Stream::Inst][(int)MissKind::Conflict];
	std::printf("baseline: %" PRIu64 " misses, %" PRIu64 " conflict (%.1f%%)\n\n",
			baseMisses, baseConflict,
			baseMisses == 0 ? 0.0 : 100.0 * baseConflict / baseMisses);

	std::vector<Unit> units = discoverUnits(probe, opt);
	if (units.size() > opt.reorderUnits)
		units.resize(opt.reorderUnits);
	if (units.empty())
	{
		std::printf("no conflict misses to work with\n");
		return 0;
	}
	if (json)
	{
		json.w("{\n  \"schema\": \"cachesweep/1\",\n  \"mode\": \"reorder\",\n");
		json.w("  \"baseline\": {\"misses\": %" PRIu64 ", \"conflict\": %" PRIu64 "},\n",
				baseMisses, baseConflict);
		json.w("  \"step\": %" PRId64 ",\n  \"units\": [", opt.reorderStep);
	}
	std::printf("units to place, by conflict misses:\n");
	for (const Unit& u : units)
		std::printf("  %-32s %08x-%08x %10" PRIu64 " (%.1f%% of conflict)\n",
				unitLabel(u).c_str(), u.start, u.end, u.conflict,
				baseConflict == 0 ? 0.0 : 100.0 * u.conflict / baseConflict);
	std::printf("\n");

	// Above everything the trace touched, so a relocated unit can never land on
	// live code. Rounded to the cache size so a slot offset is an alignment.
	u32 highest = 0;
	for (const Block& b : trace.blockTable())
		highest = std::max(highest, Shift::normalise(b.paddr) + b.size);
	u64 arena = ((u64)highest + span - 1) / span * span + span;

	u64 bestTotal = baseMisses;
	for (size_t i = 0; i < units.size(); i++)
	{
		const Unit& u = units[i];
		const u32 size = u.end - u.start;
		const u64 slot = arena;
		arena += ((size + span - 1) / span) * span + span;

		std::printf("%s  (at %08x, alignment %04x; %% is against the running total"
				" %" PRIu64 ", not the baseline)\n",
				unitLabel(u).c_str(), u.start, (u32)(u.start & (span - 1)), bestTotal);
		if (json)
		{
			json.raw(i == 0 ? "\n    {\"name\": " : ",\n    {\"name\": ");
			json.str(unitLabel(u).c_str());
			json.w(", \"named\": %s, \"start\": \"0x%08x\", \"end\": \"0x%08x\","
					" \"conflict\": %" PRIu64 ", \"alignment\": \"0x%04x\",\n"
					"     \"candidates\": [",
					u.name.empty() ? "false" : "true", u.start, u.end, u.conflict,
					(u32)(u.start & (span - 1)));
		}

		int64_t bestDelta = 0;
		u64 bestMisses = bestTotal;
		for (int64_t off = 0; off < span; off += opt.reorderStep)
		{
			Options candidate = opt;
			candidate.shift = Shift{};
			candidate.fixed.insert(candidate.fixed.begin(),
					Shift{ u.start, u.end, (int64_t)(slot + off) - (int64_t)u.start });
			const Result res = run(trace, candidate);
			std::printf("    alignment %04x  %12" PRIu64 " misses  %12" PRIu64 " conflict  %+7.2f%%\n",
					(u32)off, res.misses, res.conflict,
					bestTotal == 0 ? 0.0 : 100.0 * ((double)res.misses - bestTotal) / bestTotal);
			json.w("%s{\"alignment\": \"0x%04x\", \"misses\": %" PRIu64
					", \"conflict\": %" PRIu64 "}",
					off == 0 ? "" : ", ", (u32)off, res.misses, res.conflict);
			if (res.misses < bestMisses)
			{
				bestMisses = res.misses;
				bestDelta = (int64_t)(slot + off) - (int64_t)u.start;
			}
		}
		if (bestDelta == 0)
		{
			std::printf("    no alignment beat leaving it where it is\n\n");
			json.raw("],\n     \"placed\": false}");
			continue;
		}
		// Placed. Everything after this is searched with this unit moved, so
		// the numbers stay a running total rather than a set of separate
		// what-ifs that cannot be applied together.
		opt.fixed.insert(opt.fixed.begin(), Shift{ u.start, u.end, bestDelta });
		std::printf("    placed at alignment %04x  ->  %" PRIu64 " misses (%.2f%% below baseline)\n\n",
				(u32)((u.start + bestDelta) & (span - 1)), bestMisses,
				100.0 * ((double)baseMisses - bestMisses) / baseMisses);
		json.w("],\n     \"placed\": true, \"placed_alignment\": \"0x%04x\","
				" \"running_total\": %" PRIu64 ", \"vs_baseline\": %.5f}",
				(u32)((u.start + bestDelta) & (span - 1)), bestMisses,
				((double)baseMisses - bestMisses) / baseMisses);
		bestTotal = bestMisses;
	}

	if (opt.fixed.empty())
	{
		std::printf("no placement beat the measured layout\n");
		json.raw("\n  ],\n  \"layout\": []\n}\n");
		return 0;
	}
	// Verified before anything is reported, so the text and the JSON can be
	// written in one pass and in the same order. The replay is the search's own
	// self-check: a greedy accumulator that drifted would otherwise report a
	// layout nobody can reproduce.
	Options verify = opt;
	verify.shift = Shift{};
	const Result check = run(trace, verify);

	std::printf("layout: %" PRIu64 " misses, %.2f%% fewer than baseline\n",
			bestTotal, 100.0 * ((double)baseMisses - bestTotal) / baseMisses);
	json.w("\n  ],\n  \"verified\": %s,\n  \"total\": %" PRIu64 ","
			" \"vs_baseline\": %.5f,\n  \"layout\": [",
			check.misses == bestTotal ? "true" : "false", bestTotal,
			((double)baseMisses - bestTotal) / baseMisses);

	for (const Shift& m : opt.fixed)
	{
		const Unit *u = nullptr;
		for (const Unit& c : units)
			if (c.start == m.start)
				u = &c;
		std::printf("  align %-32s to %04x (mod %04x), was %04x\n",
				u != nullptr ? unitLabel(*u).c_str() : "?",
				(u32)((m.start + m.delta) & (span - 1)), (u32)span,
				(u32)(m.start & (span - 1)));
		if (json)
		{
			json.raw(&m == &opt.fixed[0] ? "\n    {\"name\": " : ",\n    {\"name\": ");
			json.str(u != nullptr ? unitLabel(*u).c_str() : "?");
			json.w(", \"start\": \"0x%08x\", \"align_to\": \"0x%04x\","
					" \"was\": \"0x%04x\", \"modulo\": \"0x%04x\"}",
					m.start, (u32)((m.start + m.delta) & (span - 1)),
					(u32)(m.start & (span - 1)), (u32)span);
		}
	}
	json.raw("\n  ]\n}\n");

	if (check.misses != bestTotal)
	{
		std::printf("\nthe combined layout replays to %" PRIu64 " misses, not the %" PRIu64
				" the search reported: do not trust either number\n", check.misses, bestTotal);
		return 1;
	}
	std::printf("combined layout replayed from scratch: %" PRIu64 " misses, as searched\n",
			check.misses);

	std::printf("\nAlignments are what a linker script or a section attribute controls.\n"
			"Relocating a unit makes its lines new to the model, so a few hundred\n"
			"misses move from conflict to compulsory; the total is unaffected.\n");
	return 0;
}

// ---------------------------------------------------------------------------
// Storms
//
// A miss total says how much the cache cost over a window. It does not say
// whether that cost was spread evenly or arrived in a few bursts that blew a
// frame's budget, and those are different problems: an even rate is a design
// cost, a burst is a hitch somebody can feel.
//
// Two passes. The first buckets misses by time to find the bursts; the second
// replays again and attributes only what falls inside them, because which
// blocks were responsible cannot be known until the storms have been found.
// ---------------------------------------------------------------------------

constexpr u64 SH4_CLOCK = 200000000;

struct Storm
{
	u64 startCycle;
	u64 endCycle;
	u64 misses;
	u64 conflict;
	u64 frame;
	std::vector<std::pair<u32, u64>> blame;	// block id, misses
};

class StormFinder : public ReplayWatcher
{
public:
	StormFinder(u64 bucketCycles) : bucketCycles(bucketCycles) {}

	void block(u64 cycle, u32, u64 misses, u64 conflict) override
	{
		const size_t b = (size_t)(cycle / bucketCycles);
		if (b >= buckets.size())
		{
			buckets.resize(b + 1, 0);
			conflicts.resize(b + 1, 0);
			frameAt.resize(b + 1, frames);
		}
		buckets[b] += misses;
		conflicts[b] += conflict;
	}

	void frame(u64) override { frames++; }

	u64 bucketCycles;
	u64 frames = 0;
	std::vector<u64> buckets;
	std::vector<u64> conflicts;
	std::vector<u64> frameAt;	// which frame each bucket fell in
};

class StormBlamer : public ReplayWatcher
{
public:
	StormBlamer(const std::vector<Storm>& storms, const std::vector<bool>& detailed)
		: storms(storms), detailed(detailed) {}

	void block(u64 cycle, u32 blockId, u64 misses, u64) override
	{
		if (misses == 0 || next >= storms.size())
			return;
		// Storms are in cycle order and so is the replay, so this walks forward
		// rather than searching
		while (next < storms.size() && cycle >= storms[next].endCycle)
			next++;
		if (next >= storms.size() || cycle < storms[next].startCycle)
			return;
		// Every storm feeds the aggregate. The aggregate is the answer to
		// "what causes bursts"; one storm on its own is an anecdote, and on a
		// workload with a game loop it is an anecdote that repeats.
		total[blockId] += misses;
		if (detailed[next])
			blame[next][blockId] += misses;
	}

	void frame(u64) override {}

	const std::vector<Storm>& storms;
	const std::vector<bool>& detailed;
	size_t next = 0;
	std::map<u32, u64> total;
	std::map<size_t, std::map<u32, u64>> blame;
};

// Groups a blame map by function and returns it worst first, so the text and
// the JSON say the same thing rather than each grouping it their own way.
std::vector<std::pair<std::string, u64>> groupBlame(const Trace& trace,
		const std::map<u32, u64>& blame, size_t rows)
{
	// Grouped by function, not by block: a function is many blocks, and listing
	// each one separately puts the same name in the list six times while hiding
	// how much it actually cost. Generated code has no name, so it is grouped by
	// 64 KB range - the same granularity the live profile uses, so the two views
	// name the same things the same way.
	std::map<std::string, u64> grouped;
	for (const auto& r : blame)
	{
		const Block& b = trace.blockTable()[r.first];
		const Sym *sym = symbolFor(b.paddr);
		char name[48];
		if (sym != nullptr)
			std::snprintf(name, sizeof(name), "%s", sym->name.c_str());
		else
		{
			const u32 base = Shift::normalise(b.paddr) & ~0xffffu;
			std::snprintf(name, sizeof(name), "%08x-%08x", base, base + 0x10000);
		}
		grouped[name] += r.second;
	}

	std::vector<std::pair<std::string, u64>> sorted(grouped.begin(), grouped.end());
	std::sort(sorted.begin(), sorted.end(),
			[](const std::pair<std::string, u64>& a, const std::pair<std::string, u64>& b) {
				return a.second > b.second;
			});
	if (sorted.size() > rows)
		sorted.resize(rows);
	return sorted;
}

void printBlame(const Trace& trace, const std::map<u32, u64>& blame, u64 outOf,
		size_t rows, const char *indent)
{
	for (const auto& r : groupBlame(trace, blame, rows))
		std::printf("%s%-34s %8" PRIu64 " misses  %4.1f%%\n",
				indent, r.first.c_str(), r.second,
				outOf == 0 ? 0.0 : 100.0 * r.second / outOf);
}

void jsonBlame(const Trace& trace, const std::map<u32, u64>& blame, u64 outOf, size_t rows)
{
	if (!json)
		return;
	const std::vector<std::pair<std::string, u64>> grouped = groupBlame(trace, blame, rows);
	json.raw("[");
	for (size_t i = 0; i < grouped.size(); i++)
	{
		json.raw(i == 0 ? "\n" : ",\n");
		json.raw("        {\"name\": ");
		json.str(grouped[i].first.c_str());
		json.w(", \"misses\": %" PRIu64 ", \"share\": %.5f}",
				grouped[i].second,
				outOf == 0 ? 0.0 : (double)grouped[i].second / outOf);
	}
	json.raw(grouped.empty() ? "]" : "\n      ]");
}

// Where the instructions go, ranked. No cache model runs here: this answers
// "what executes" rather than "what misses", and the two lists disagree often
// enough that having only one of them is misleading.
int runProfile(Trace& trace, const Options& opt)
{
	const Trace::Profile p = trace.profile();
	const std::vector<Block>& table = trace.blockTable();

	// An SH4 instruction is two bytes, so a block's size is its instruction
	// count doubled. Attribution is by the block's start address: a block that
	// runs past the end of a function is rare and splitting it would need the
	// disassembly this tool deliberately does not do.
	std::map<std::string, u64> instrs;
	std::map<std::string, u64> calls;
	u64 total = 0;
	for (size_t id = 0; id < p.execs.size() && id < table.size(); id++)
	{
		if (p.execs[id] == 0)
			continue;
		const u64 n = p.execs[id] * (table[id].size / 2);
		total += n;
		const Sym *sym = symbolFor(table[id].paddr);
		char name[64];
		if (sym != nullptr)
			std::snprintf(name, sizeof(name), "%s", sym->name.c_str());
		else
		{
			const u32 base = Shift::normalise(table[id].paddr) & ~0xffffu;
			std::snprintf(name, sizeof(name), "%08x-%08x", base, base + 0x10000);
		}
		instrs[name] += n;
		calls[name] += p.execs[id];
	}

	std::vector<std::pair<std::string, u64>> sorted(instrs.begin(), instrs.end());
	std::sort(sorted.begin(), sorted.end(),
			[](const std::pair<std::string, u64>& a, const std::pair<std::string, u64>& b) {
				return a.second > b.second;
			});

	const double frames = p.frames == 0 ? 1.0 : (double)p.frames;
	std::printf("%" PRIu64 " frames, %" PRIu64 " instructions executed, %.0f per frame\n\n",
			p.frames, total, total / frames);
	std::printf("%-42s %14s %7s %12s\n", "where", "instr/frame", "share", "blocks/frame");
	const size_t rows = opt.topN == 0 ? 20 : opt.topN;
	for (size_t i = 0; i < sorted.size() && i < rows; i++)
		std::printf("%-42s %14.0f %6.2f%% %12.0f\n", sorted[i].first.c_str(),
				sorted[i].second / frames,
				total == 0 ? 0.0 : 100.0 * (double)sorted[i].second / (double)total,
				calls[sorted[i].first] / frames);

	if (opt.callerRows == 0)
		return 0;

	// Who reaches each of the top rows. A transition between two blocks in
	// different functions is a call edge, and its count is the weight: the
	// trace already holds the call graph, it just was never read out.
	std::printf("\ncallers\n");
	for (size_t i = 0; i < sorted.size() && i < opt.callerRows; i++)
	{
		std::map<std::string, u64> from;
		for (const auto& e : p.edges)
		{
			const u32 src = (u32)(e.first >> 32);
			const u32 dst = (u32)e.first;
			if (src >= table.size() || dst >= table.size())
				continue;
			const Sym *ds = symbolFor(table[dst].paddr);
			char dname[64];
			if (ds != nullptr)
				std::snprintf(dname, sizeof(dname), "%s", ds->name.c_str());
			else
			{
				const u32 base = Shift::normalise(table[dst].paddr) & ~0xffffu;
				std::snprintf(dname, sizeof(dname), "%08x-%08x", base, base + 0x10000);
			}
			if (sorted[i].first != dname)
				continue;
			const Sym *ss = symbolFor(table[src].paddr);
			char sname[64];
			if (ss != nullptr)
				std::snprintf(sname, sizeof(sname), "%s", ss->name.c_str());
			else
			{
				const u32 base = Shift::normalise(table[src].paddr) & ~0xffffu;
				std::snprintf(sname, sizeof(sname), "%08x-%08x", base, base + 0x10000);
			}
			if (sname == sorted[i].first)
				continue;	// internal branch, not a call in
			from[sname] += e.second;
		}
		std::vector<std::pair<std::string, u64>> fs(from.begin(), from.end());
		std::sort(fs.begin(), fs.end(),
				[](const std::pair<std::string, u64>& a, const std::pair<std::string, u64>& b) {
					return a.second > b.second;
				});
		std::printf("  %s\n", sorted[i].first.c_str());
		for (size_t j = 0; j < fs.size() && j < 5; j++)
			std::printf("      %-38s %12.0f entries/frame\n", fs[j].first.c_str(), fs[j].second / frames);
		if (fs.empty())
			std::printf("      (no cross-function entries seen)\n");
	}
	return 0;
}

int runStorms(Trace& trace, const Options& opt)
{
	if (opt.bucketCycles < 4096)
	{
		// The trace only carries a timestamp every 4096 cycles, so a finer
		// bucket would be inventing resolution the recording does not have
		std::fprintf(stderr, "--storm-bucket must be at least 4096 cycles:"
				" that is the timestamp resolution of the trace\n");
		return 1;
	}

	Model model;
	model.setMissRingSize(0);
	StormFinder finder(opt.bucketCycles);
	trace.replay(model, opt, &finder);

	// Buckets with no misses at all are still time that passed, and dropping
	// them would raise the median until nothing looked like a storm
	std::vector<u64> sorted = finder.buckets;
	if (sorted.empty())
	{
		std::printf("no blocks in the trace\n");
		return 0;
	}
	std::sort(sorted.begin(), sorted.end());
	const u64 median = sorted[sorted.size() / 2];
	const u64 p99 = sorted[(size_t)(sorted.size() * 0.99)];
	const u64 peak = sorted.back();
	u64 totalMisses = 0;
	for (u64 m : finder.buckets)
		totalMisses += m;

	// A median of zero would make any bucket with one miss a storm, so the
	// floor comes from the mean instead
	const double mean = (double)totalMisses / finder.buckets.size();
	const double threshold = std::max(opt.stormFactor * (double)median,
			opt.stormFactor * mean);

	std::printf("%zu buckets of %" PRIu64 " cycles (%.3f ms), %" PRIu64 " frames\n",
			finder.buckets.size(), opt.bucketCycles,
			1000.0 * opt.bucketCycles / SH4_CLOCK, finder.frames);
	size_t quiet = 0;
	for (u64 m : finder.buckets)
		if (m == 0)
			quiet++;
	std::printf("misses per bucket: median %" PRIu64 "  mean %.1f  p99 %" PRIu64
			"  peak %" PRIu64 "\n", median, mean, p99, peak);
	// A median of zero is not a broken statistic, it is the shape of the data,
	// and saying so stops the threshold line below from looking arbitrary
	std::printf("%.1f%% of buckets have no misses at all\n",
			100.0 * quiet / finder.buckets.size());
	std::printf("storm threshold: %.0f misses per bucket (%.1fx the larger of"
			" median and mean)\n\n", threshold, opt.stormFactor);

	// Adjacent hot buckets are one storm, not several
	std::vector<Storm> storms;
	for (size_t b = 0; b < finder.buckets.size(); b++)
	{
		if ((double)finder.buckets[b] < threshold)
			continue;
		if (!storms.empty() && storms.back().endCycle == b * opt.bucketCycles)
		{
			storms.back().endCycle = (b + 1) * opt.bucketCycles;
			storms.back().misses += finder.buckets[b];
			storms.back().conflict += finder.conflicts[b];
			continue;
		}
		storms.push_back({ b * opt.bucketCycles, (b + 1) * opt.bucketCycles,
				finder.buckets[b], finder.conflicts[b], finder.frameAt[b], {} });
	}

	u64 stormMisses = 0;
	u64 stormBuckets = 0;
	for (const Storm& s : storms)
	{
		stormMisses += s.misses;
		stormBuckets += (s.endCycle - s.startCycle) / opt.bucketCycles;
	}
	std::printf("%zu storms: %.2f%% of the time holding %.1f%% of all misses\n",
			storms.size(),
			100.0 * stormBuckets / finder.buckets.size(),
			totalMisses == 0 ? 0.0 : 100.0 * stormMisses / totalMisses);
	if (storms.empty())
	{
		// Worth saying plainly: an even rate is a finding, not a failure to find
		std::printf("\nMisses are spread evenly at this bucket size. Nothing here"
				" is a burst; the cost is the steady rate.\n");
		json.w("{\n  \"schema\": \"cachesweep/1\",\n  \"mode\": \"storms\",\n"
				"  \"bucket_cycles\": %" PRIu64 ",\n  \"misses\": %" PRIu64 ",\n"
				"  \"storms\": {\"count\": 0}\n}\n", opt.bucketCycles, totalMisses);
		return 0;
	}

	// How often, and how long. A storm every frame at the same size is a
	// periodic cost; a handful of outliers is a hitch. They read the same in a
	// total and want different fixes, so both are said.
	u64 longest = 0;
	double meanMs = 0;
	for (const Storm& s : storms)
	{
		longest = std::max(longest, s.endCycle - s.startCycle);
		meanMs += 1000.0 * (s.endCycle - s.startCycle) / SH4_CLOCK;
	}
	meanMs /= storms.size();
	std::printf("  %.2f per frame, mean %.3f ms, longest %.3f ms\n",
			finder.frames == 0 ? 0.0 : (double)storms.size() / finder.frames,
			meanMs, 1000.0 * longest / SH4_CLOCK);

	// Mark the worst few for a detailed listing; every storm still feeds the
	// aggregate below
	std::vector<size_t> order(storms.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&storms](size_t a, size_t b) {
		return storms[a].misses > storms[b].misses;
	});
	std::vector<bool> detailed(storms.size(), false);
	for (size_t i = 0; i < order.size() && i < opt.topN; i++)
		detailed[order[i]] = true;

	Model second;
	second.setMissRingSize(0);
	StormBlamer blamer(storms, detailed);
	trace.replay(second, opt, &blamer);

	std::printf("\nwhat is in the storms, across all %zu of them:\n", storms.size());
	printBlame(trace, blamer.total, stormMisses, 12, "  ");

	if (json)
	{
		json.w("{\n  \"schema\": \"cachesweep/1\",\n  \"mode\": \"storms\",\n");
		json.w("  \"bucket_cycles\": %" PRIu64 ",\n", opt.bucketCycles);
		json.w("  \"buckets\": %zu,\n", finder.buckets.size());
		json.w("  \"frames\": %" PRIu64 ",\n", finder.frames);
		json.w("  \"misses\": %" PRIu64 ",\n", totalMisses);
		json.w("  \"per_bucket\": {\"median\": %" PRIu64 ", \"mean\": %.3f,"
				" \"p99\": %" PRIu64 ", \"peak\": %" PRIu64 ", \"empty_share\": %.5f},\n",
				median, mean, p99, peak, (double)quiet / finder.buckets.size());
		json.w("  \"threshold\": %.1f,\n", threshold);
		json.w("  \"storms\": {\"count\": %zu, \"time_share\": %.5f,"
				" \"miss_share\": %.5f, \"per_frame\": %.3f,"
				" \"mean_ms\": %.4f, \"longest_ms\": %.4f},\n",
				storms.size(), (double)stormBuckets / finder.buckets.size(),
				totalMisses == 0 ? 0.0 : (double)stormMisses / totalMisses,
				finder.frames == 0 ? 0.0 : (double)storms.size() / finder.frames,
				meanMs, 1000.0 * longest / SH4_CLOCK);
		json.raw("  \"blame\": ");
		jsonBlame(trace, blamer.total, stormMisses, 24);
		json.raw(",\n  \"worst\": [");
	}

	std::printf("\nthe %zu worst individually:\n",
			std::min(opt.topN, storms.size()));
	bool printedAny = false;
	for (size_t i = 0; i < storms.size(); i++)
	{
		if (!detailed[i])
			continue;
		const Storm& s = storms[i];
		std::printf("  frame %-6" PRIu64 " cycle %" PRIu64 "  %.3f ms  %" PRIu64
				" misses (%.0f%% conflict)\n",
				s.frame, s.startCycle,
				1000.0 * (s.endCycle - s.startCycle) / SH4_CLOCK, s.misses,
				s.misses == 0 ? 0.0 : 100.0 * s.conflict / s.misses);
		auto it = blamer.blame.find(i);
		if (it != blamer.blame.end())
			printBlame(trace, it->second, s.misses, 4, "      ");

		if (json)
		{
			json.raw(printedAny ? ",\n" : "\n");
			printedAny = true;
			json.w("    {\"frame\": %" PRIu64 ", \"start_cycle\": %" PRIu64
					", \"end_cycle\": %" PRIu64 ", \"ms\": %.4f,"
					" \"misses\": %" PRIu64 ", \"conflict\": %" PRIu64 ",\n      \"blame\": ",
					s.frame, s.startCycle, s.endCycle,
					1000.0 * (s.endCycle - s.startCycle) / SH4_CLOCK, s.misses, s.conflict);
			if (it != blamer.blame.end())
				jsonBlame(trace, it->second, s.misses, 8);
			else
				json.raw("[]");
			json.raw("}");
		}
	}
	json.raw(printedAny ? "\n  ]\n}\n" : "]\n}\n");
	return 0;
}

void usage(const char *exe)
{
	std::fprintf(stderr,
		"Usage: %s trace [options]\n"
		"\n"
		"Replays a cachesim trace under a modified code layout.\n"
		"\n"
		"  --auto                    find the regions worth moving and pad before them\n"
		"  --reorder                 place the worst functions at the alignment that\n"
		"                            costs least, one after another\n"
		"  --reorder-units n         functions to place (default 4)\n"
		"  --reorder-step n          alignment granularity searched (default 512)\n"
		"  --symbols file.elf        name regions and functions from an ELF\n"
		"  --json file.json          also write the result as JSON, for diffing one\n"
		"                            experiment against the next\n"
		"  --jitmap file             name generated code from a guest JIT block map\n"
		"  --profile                 rank where the instructions go, no cache model\n"
		"  --callers n               with --profile, break down callers of the top n\n"
		"  --storms                  find bursts of misses in time and say what\n"
		"                            caused them\n"
		"  --storm-bucket n          bucket width in guest cycles (default 32768)\n"
		"  --storm-factor f          a bucket is a storm at f times the typical\n"
		"                            rate (default 4)\n"
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
	// A search prints a line per replay over several minutes; block buffering
	// would hold all of it back until the run ended, which makes a long sweep
	// look hung when redirected to a file.
	setvbuf(stdout, nullptr, _IOLBF, 0);

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
		else if (arg == "--reorder")
			opt.reorder = true;
		else if (arg == "--jitmap" && i + 1 < argc)
			opt.jitmapPath = argv[++i];
		else if (arg == "--profile")
			opt.profile = true;
		else if (arg == "--callers" && i + 1 < argc)
			opt.callerRows = strtoul(argv[++i], nullptr, 0);
		else if (arg == "--storms")
			opt.storms = true;
		else if (arg == "--storm-bucket" && i + 1 < argc)
			opt.bucketCycles = strtoull(argv[++i], nullptr, 0);
		else if (arg == "--storm-factor" && i + 1 < argc)
			opt.stormFactor = strtod(argv[++i], nullptr);
		else if (arg == "--reorder-units" && i + 1 < argc)
			opt.reorderUnits = strtoul(argv[++i], nullptr, 0);
		else if (arg == "--reorder-step" && i + 1 < argc)
			opt.reorderStep = strtoll(argv[++i], nullptr, 0);
		else if (arg == "--symbols" && i + 1 < argc)
			opt.symbolPath = argv[++i];
		else if (arg == "--json" && i + 1 < argc)
			opt.jsonPath = argv[++i];
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

	if (!opt.symbolPath.empty())
		loadElfSymbols(opt.symbolPath);
	if (!opt.jitmapPath.empty())
		loadJitMap(opt.jitmapPath);

	Trace trace;
	if (!trace.load(opt.tracePath))
		return 1;

	if (!json.open(opt.jsonPath))
		return 1;

	if (opt.storms)
	{
		const int rv = runStorms(trace, opt);
		json.close();
		return rv;
	}

	if (opt.profile)
	{
		const int rv = runProfile(trace, opt);
		json.close();
		return rv;
	}

	if (opt.reorder)
	{
		const int rv = runReorder(trace, opt);
		json.close();
		return rv;
	}

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
		if (json)
		{
			json.w("{\n  \"schema\": \"cachesweep/1\",\n  \"mode\": \"auto\",\n");
			json.w("  \"baseline\": {\"misses\": %" PRIu64 ", \"conflict\": %" PRIu64 "},\n",
					baseMisses, baseConflict);
			json.raw("  \"regions\": [");
		}
		std::printf("candidate regions, by conflict misses:\n");
		bool firstRegion = true;
		for (const Region& r : regions)
		{
			const Sym *sym = symbolFor(r.start);
			if (json)
			{
				json.raw(firstRegion ? "\n    " : ",\n    ");
				firstRegion = false;
				json.w("{\"start\": \"0x%08x\", \"end\": \"0x%08x\","
						" \"conflict\": %" PRIu64 ", \"symbol\": ",
						r.start, r.end, r.conflict);
				if (sym != nullptr)
					json.str(sym->name.c_str());
				else
					json.raw("null");
				json.raw("}");
			}
			std::printf("  %08x-%08x  %10" PRIu64 " conflict (%.1f%% of all)%s%s\n",
					r.start, r.end, r.conflict,
					baseConflict == 0 ? 0.0 : 100.0 * r.conflict / baseConflict,
					sym != nullptr ? "  from " : "",
					sym != nullptr ? sym->name.c_str() : "");
		}
		std::printf("\n");
		json.raw(firstRegion ? "],\n  \"candidates\": [" : "\n  ],\n  \"candidates\": [");
		bool firstCand = true;

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
				if (json)
				{
					json.raw(firstCand ? "\n    " : ",\n    ");
					firstCand = false;
					json.w("{\"region\": \"0x%08x\", \"pad\": %" PRId64
							", \"misses\": %" PRIu64 ", \"conflict\": %" PRIu64
							", \"vs_baseline\": %.5f}",
							r.start, delta, res.misses, res.conflict,
							baseMisses == 0 ? 0.0
									: ((double)res.misses - baseMisses) / baseMisses);
				}
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
		json.raw(firstCand ? "],\n" : "\n  ],\n");
		if (best.end != 0)
			json.w("  \"best\": {\"region\": \"0x%08x\", \"pad\": %" PRId64
					", \"misses\": %" PRIu64 ", \"vs_baseline\": %.5f}\n}\n",
					best.start, best.delta, bestMisses,
					-(double)(baseMisses - bestMisses) / baseMisses);
		else
			json.raw("  \"best\": null\n}\n");
		json.close();
		return 0;
	}

	if (!opt.sweep)
	{
		reportDetail(trace, opt);
		json.close();
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
	if (json)
	{
		json.w("{\n  \"schema\": \"cachesweep/1\",\n  \"mode\": \"sweep\",\n");
		json.w("  \"region\": {\"start\": \"0x%08x\", \"end\": \"0x%08x\"},\n",
				opt.shift.start, opt.shift.end);
		json.w("  \"baseline\": {\"misses\": %" PRIu64 ", \"conflict\": %" PRIu64
				", \"fetches\": %" PRIu64 "},\n  \"rows\": [",
				baseline.misses, baseline.conflict, baseline.fetches);
	}
	bool firstRow = true;

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
		if (json)
		{
			json.raw(firstRow ? "\n    " : ",\n    ");
			firstRow = false;
			json.w("{\"delta\": %" PRId64 ", \"misses\": %" PRIu64
					", \"conflict\": %" PRIu64 ", \"vs_baseline\": %.5f}",
					delta, r.misses, r.conflict,
					baseline.misses == 0 ? 0.0
							: ((double)r.misses - baseline.misses) / baseline.misses);
		}
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
	json.w("\n  ],\n  \"best\": {\"delta\": %" PRId64 ", \"misses\": %" PRIu64
			", \"vs_baseline\": %.5f}\n}\n",
			bestDelta, bestMisses,
			baseline.misses == 0 ? 0.0 : -(double)(baseline.misses - bestMisses) / baseline.misses);
	json.close();
	return 0;
}
