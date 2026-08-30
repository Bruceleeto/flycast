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
#include "jitdump.h"
#include "cfg/option.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"

#include "nowide/cstdio.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace jitdump
{

bool g_active = false;

// One record per entry into the region. Fixed size and fully packed: the
// offline reader mmaps the file and casts.
struct Record
{
	u32 target;		// where execution landed, inside the region
	u32 from;		// the instruction it came from, outside it
	u64 cycle;		// SH4 scheduler cycle
};
static_assert(sizeof(Record) == 16, "the entry file layout is a contract");

enum class Phase
{
	Off,		// not requested, or the region could not be parsed
	Waiting,	// counting off the warm-up frames
	Armed,		// logging
};

struct State
{
	Phase phase = Phase::Off;
	std::string prefix;
	bool logEntries = true;

	u32 base = 0;
	u32 size = 0;

	u32 frames = 0;
	u32 skipFrames = 0;
	u32 snapshotFrames = 0;		// interval dumps, 0 for one dump at the end
	u32 snapshotIndex = 0;
	u64 maxEntries = 0;

	FILE *entries = nullptr;
	std::vector<Record> buffer;
	u64 entriesWritten = 0;
	bool entriesFull = false;
	u32 prevPc = 0;
	bool prevInRegion = false;

	bool dumped = false;
};

static State& state()
{
	static State st;
	return st;
}

//
// Guest memory. Only area 3 (main RAM) can be read in bulk; anything else falls
// back to the slow path, which is fine because a dump happens at most a handful
// of times in a run.
//
static bool isRam(u32 addr) {
	return ((addr >> 26) & 7) == 3;
}

static void readGuest(u32 addr, u8 *dst, u32 len)
{
	const u32 offset = addr & settings.platform.ram_mask;
	if (isRam(addr) && (u64)offset + len <= settings.platform.ram_size)
	{
		std::memcpy(dst, &mem_b[offset], len);
		return;
	}
	for (u32 i = 0; i < len; i++)
		dst[i] = addrspace::read8(addr + i);
}

//
// Output
//
static void flushEntries()
{
	State& st = state();
	if (st.entries == nullptr || st.buffer.empty())
		return;
	std::fwrite(st.buffer.data(), sizeof(Record), st.buffer.size(), st.entries);
	st.buffer.clear();
}

static void openEntries()
{
	State& st = state();
	if (!st.logEntries)
		return;
	const std::string path = st.prefix + ".entries";
	st.entries = nowide::fopen(path.c_str(), "wb");
	if (st.entries == nullptr) {
		ERROR_LOG(SH4, "jitdump: cannot write %s", path.c_str());
		st.logEntries = false;
		return;
	}
	st.buffer.reserve(64 * 1024);
	NOTICE_LOG(SH4, "jitdump: logging region entries to %s", path.c_str());
}

static void recordEntry(u32 target, u32 from)
{
	State& st = state();
	if (st.entriesFull)
		return;
	st.buffer.push_back({ target, from, sh4_sched_now64() });
	st.entriesWritten++;
	if (st.buffer.size() == st.buffer.capacity())
		flushEntries();
	if (st.maxEntries != 0 && st.entriesWritten >= st.maxEntries)
	{
		flushEntries();
		st.entriesFull = true;
		// Loud, because everything after this point is missing from the log and
		// the offline hotness numbers are then a prefix of the run, not all of it
		NOTICE_LOG(SH4, "jitdump: entry limit of %" PRIu64 " records reached,"
				" no more entries will be logged", st.maxEntries);
	}
}

static void writeDump(const std::string& path)
{
	State& st = state();
	if (st.size == 0)
		return;
	FILE *f = nowide::fopen(path.c_str(), "wb");
	if (f == nullptr) {
		ERROR_LOG(SH4, "jitdump: cannot write %s", path.c_str());
		return;
	}
	// 32 byte header, then the raw bytes. See docs/jitdump.md.
	u8 header[32];
	std::memset(header, 0, sizeof(header));
	std::memcpy(header, "SH4DUMP1", 8);
	const u64 cycle = sh4_sched_now64();
	std::memcpy(header + 8, &st.base, 4);
	std::memcpy(header + 12, &st.size, 4);
	std::memcpy(header + 24, &cycle, 8);
	std::fwrite(header, 1, sizeof(header), f);

	std::vector<u8> bytes(st.size);
	readGuest(st.base, bytes.data(), st.size);
	std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
	NOTICE_LOG(SH4, "jitdump: wrote %s (%u bytes at %08x, cycle %" PRIu64 ")",
			path.c_str(), st.size, st.base, cycle);
}

//
// Feeds
//
void traceFetch(u32 pc)
{
	State& st = state();
	// An entry is a fetch inside the region whose predecessor was outside it:
	// that is a branch in, whatever instruction made it.
	const bool inside = pc - st.base < st.size;
	if (inside && !st.prevInRegion)
		recordEntry(pc, st.prevPc);
	st.prevInRegion = inside;
	st.prevPc = pc;
}

void frameBoundary()
{
	State& st = state();
	if (st.phase == Phase::Off)
		return;
	st.frames++;

	if (st.phase == Phase::Waiting)
	{
		if (st.frames < st.skipFrames)
			return;
		st.phase = Phase::Armed;
		st.prevInRegion = false;
		g_active = st.logEntries;
		NOTICE_LOG(SH4, "jitdump: armed on %08x..%08x (%u KB)",
				st.base, st.base + st.size, st.size / 1024);
		openEntries();
		return;
	}
	if (st.snapshotFrames != 0 && st.frames % st.snapshotFrames == 0)
	{
		char suffix[32];
		snprintf(suffix, sizeof(suffix), ".%04u.bin", st.snapshotIndex++);
		writeDump(st.prefix + suffix);
	}
}

//
// Control
//
void init()
{
	State& st = state();
	if (st.entries != nullptr)
		std::fclose(st.entries);
	st = State();
	g_active = false;
	if (!config::JitDump)
		return;

	// -jitdump-region 0xADDR:SIZE. Required: what is and is not generated code
	// is not something flycast can know, and guessing it produced a worse
	// answer than being told.
	const std::string region = (std::string)config::JitDumpRegion;
	const size_t colon = region.find(':');
	if (colon != std::string::npos)
	{
		st.base = (u32)strtoul(region.substr(0, colon).c_str(), nullptr, 0);
		st.size = (u32)strtoul(region.substr(colon + 1).c_str(), nullptr, 0);
	}
	if (st.size == 0) {
		ERROR_LOG(SH4, "jitdump: -jitdump-region is required, as 0xADDR:SIZE"
				" (got '%s'). Nothing will be dumped.", region.c_str());
		st.phase = Phase::Off;
		return;
	}

	st.prefix = (std::string)config::JitDumpOut;
	if (st.prefix.empty())
		st.prefix = "jitdump";
	st.logEntries = config::JitDumpEntries;
	st.skipFrames = (u32)std::max(0, (int)config::JitDumpSkipFrames);
	st.snapshotFrames = (u32)std::max(0, (int)config::JitDumpInterval);
	st.maxEntries = (u64)std::max(0, (int)config::JitDumpMaxEntries);
	st.phase = Phase::Waiting;

	NOTICE_LOG(SH4, "jitdump: enabled, writing %s.bin%s", st.prefix.c_str(),
			st.logEntries ? " and .entries" : "");
	if (st.logEntries && config::DynarecEnabled)
		// The entry log is fed per instruction by the interpreter. With the
		// dynarec on, nothing feeds it and the file would be silently empty.
		WARN_LOG(SH4, "jitdump: entry logging needs the interpreter."
				" Run with -config Dynarec.Enabled=no or -jitdump-no-entries.");
}

void finish()
{
	State& st = state();
	if (st.phase == Phase::Off || st.dumped)
		return;
	st.dumped = true;
	if (st.phase == Phase::Waiting) {
		WARN_LOG(SH4, "jitdump: the run ended during the %u frame warm-up,"
				" nothing was dumped", st.skipFrames);
		return;
	}
	writeDump(st.prefix + ".bin");
	flushEntries();
	if (st.entries != nullptr) {
		std::fclose(st.entries);
		st.entries = nullptr;
	}
	if (st.logEntries)
	{
		NOTICE_LOG(SH4, "jitdump: %" PRIu64 " region entries logged%s",
				st.entriesWritten, st.entriesFull ? " (limit reached)" : "");
		if (st.entriesWritten == 0)
			// Nothing branched into it, so either the region is wrong or the
			// dynarec is on. Silence here would look like a valid measurement.
			WARN_LOG(SH4, "jitdump: nothing ever entered %08x..%08x."
					" Check the region and that the interpreter is in use.",
					st.base, st.base + st.size);
	}
	g_active = false;
}

void term()
{
	State& st = state();
	flushEntries();
	if (st.entries != nullptr) {
		std::fclose(st.entries);
		st.entries = nullptr;
	}
	st.phase = Phase::Off;
	g_active = false;
}

}
