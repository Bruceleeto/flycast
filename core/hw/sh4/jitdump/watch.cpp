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
#include "watch.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
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

namespace watch
{

bool g_write = false;
bool g_jump = false;

// One store into the watched range.
struct WriteRecord
{
	u32 pc;			// the instruction that stored, guest virtual
	u32 addr;		// where it stored, as the guest gave it
	u32 size;		// 1, 2, 4 or 8 bytes
	u32 flags;		// reserved, always 0
	u64 value;		// zero extended
	u64 cycle;
};
static_assert(sizeof(WriteRecord) == 32, "the write log layout is a contract");

// One fetch that left the watched range.
struct JumpRecord
{
	u32 from;		// last instruction fetched inside the range
	u32 to;			// where it went
	u64 cycle;
};
static_assert(sizeof(JumpRecord) == 16, "the jump log layout is a contract");

struct Range
{
	u32 base = 0;
	u32 size = 0;

	bool contains(u32 addr) const
	{
		if (addr - base < size)
			return true;
		// Same location through another mirror: P0/P1/P2/P3 differ only in the
		// top three bits, and generated code is commonly written through one
		// mapping and run through another.
		return (addr & 0x1fffffff) - (base & 0x1fffffff) < size;
	}
};

template <typename Rec>
struct Log
{
	FILE *file = nullptr;
	std::vector<Rec> buffer;
	u64 written = 0;
	bool full = false;

	void open(const std::string& path, const char *magic, const Range& range)
	{
		file = nowide::fopen(path.c_str(), "wb");
		if (file == nullptr) {
			ERROR_LOG(SH4, "watch: cannot write %s", path.c_str());
			return;
		}
		// Same 32 byte header as a jitdump .bin, so one reader handles both
		u8 header[32];
		std::memset(header, 0, sizeof(header));
		std::memcpy(header, magic, 8);
		const u64 cycle = sh4_sched_now64();
		std::memcpy(header + 8, &range.base, 4);
		std::memcpy(header + 12, &range.size, 4);
		std::memcpy(header + 24, &cycle, 8);
		std::fwrite(header, 1, sizeof(header), file);
		buffer.reserve(16 * 1024);
		NOTICE_LOG(SH4, "watch: %08x..%08x logging to %s",
				range.base, range.base + range.size, path.c_str());
	}

	void flush()
	{
		if (file == nullptr || buffer.empty())
			return;
		std::fwrite(buffer.data(), sizeof(Rec), buffer.size(), file);
		buffer.clear();
	}

	// Returns false once the cap is reached, so the caller can disarm
	bool push(const Rec& rec, u64 max, const char *what)
	{
		if (file == nullptr)
			return true;
		buffer.push_back(rec);
		written++;
		if (buffer.size() == buffer.capacity())
			flush();
		if (max != 0 && written >= max)
		{
			flush();
			full = true;
			NOTICE_LOG(SH4, "watch: %s limit of %" PRIu64 " records reached,"
					" no more will be logged", what, max);
			return false;
		}
		return true;
	}

	void close()
	{
		flush();
		if (file != nullptr) {
			std::fclose(file);
			file = nullptr;
		}
	}
};

struct State
{
	std::string prefix;
	u64 maxRecords = 0;
	Range writeRange;
	Range jumpRange;
	Log<WriteRecord> writes;
	Log<JumpRecord> jumps;
	bool prevInJump = false;
	u32 prevPc = 0;
	bool finished = false;
};

static State& state()
{
	static State st;
	return st;
}

//
// Write watch
//
// The handlers are wrapped rather than the opcodes patched: every interpreter
// store goes through these four pointers, so nothing can slip past, and when
// the watch is off there is not even a branch to skip.
//
static WriteMem8Func origWrite8;
static WriteMem16Func origWrite16;
static WriteMem32Func origWrite32;
static WriteMem64Func origWrite64;

static void noteWrite(u32 addr, u32 size, u64 value)
{
	State& st = state();
	if (!st.writeRange.contains(addr))
		return;
	// Sh4cntx.pc has already been advanced past the instruction being executed
	const u32 pc = Sh4cntx.pc - 2;
	if (!st.writes.push({ pc, addr, size, 0, value, sh4_sched_now64() },
			st.maxRecords, "write"))
		g_write = false;
}

static void DYNACALL watchWrite8(u32 addr, u8 data) {
	noteWrite(addr, 1, data);
	origWrite8(addr, data);
}
static void DYNACALL watchWrite16(u32 addr, u16 data) {
	noteWrite(addr, 2, data);
	origWrite16(addr, data);
}
static void DYNACALL watchWrite32(u32 addr, u32 data) {
	noteWrite(addr, 4, data);
	origWrite32(addr, data);
}
static void DYNACALL watchWrite64(u32 addr, u64 data) {
	noteWrite(addr, 8, data);
	origWrite64(addr, data);
}

void installHandlers()
{
	if (!g_write)
		return;
	// SetMemoryHandlers has just reassigned these, so whatever they point at
	// now is the real handler for the current MMU state
	origWrite8 = WriteMem8;
	origWrite16 = WriteMem16;
	origWrite32 = WriteMem32;
	origWrite64 = WriteMem64;
	WriteMem8 = &watchWrite8;
	WriteMem16 = &watchWrite16;
	WriteMem32 = &watchWrite32;
	WriteMem64 = &watchWrite64;
}

//
// Bad jump watch
//
void traceFetch(u32 pc)
{
	State& st = state();
	const bool inside = st.jumpRange.contains(pc);
	// Leaving by falling off the end is not a jump, so a discontinuity is what
	// is being looked for: anything that is not the next instruction along.
	if (st.prevInJump && !inside && pc != st.prevPc + 2)
	{
		if (!st.jumps.push({ st.prevPc, pc, sh4_sched_now64() },
				st.maxRecords, "jump"))
			g_jump = false;
	}
	st.prevInJump = inside;
	st.prevPc = pc;
}

//
// Control
//
static bool parseRange(const std::string& text, Range& range, const char *what)
{
	if (text.empty())
		return false;
	const size_t colon = text.find(':');
	if (colon != std::string::npos)
	{
		range.base = (u32)strtoul(text.substr(0, colon).c_str(), nullptr, 0);
		range.size = (u32)strtoul(text.substr(colon + 1).c_str(), nullptr, 0);
	}
	if (range.size == 0) {
		ERROR_LOG(SH4, "watch: cannot parse %s range '%s', expected 0xADDR:SIZE",
				what, text.c_str());
		range = Range();
		return false;
	}
	return true;
}

void init()
{
	State& st = state();
	st.writes.close();
	st.jumps.close();
	st = State();
	g_write = false;
	g_jump = false;

	st.prefix = (std::string)config::WatchOut;
	if (st.prefix.empty())
		st.prefix = "watch";
	st.maxRecords = (u64)std::max(0, (int)config::WatchMaxRecords);

	g_write = parseRange((std::string)config::WatchWrite, st.writeRange, "write");
	g_jump = parseRange((std::string)config::WatchBadJump, st.jumpRange, "badjump");
	if (!g_write && !g_jump)
		return;

	if (g_write)
	{
		st.writes.open(st.prefix + ".writes", "SH4WRIT1", st.writeRange);
		// The handlers are already set by the time the options are read, and
		// they are only reassigned when the MMU changes, so wrap them now
		installHandlers();
	}
	if (g_jump)
		st.jumps.open(st.prefix + ".jumps", "SH4JUMP1", st.jumpRange);
	if (config::DynarecEnabled)
		// Neither hook exists in compiled code, so both logs would be empty
		WARN_LOG(SH4, "watch: the watchpoints need the interpreter."
				" Run with -config config:Dynarec.Enabled=no.");
}

void finish()
{
	State& st = state();
	if (st.finished)
		return;
	st.finished = true;
	if (st.writes.file != nullptr)
		NOTICE_LOG(SH4, "watch: %" PRIu64 " stores into %08x..%08x%s",
				st.writes.written, st.writeRange.base,
				st.writeRange.base + st.writeRange.size,
				st.writes.full ? " (limit reached)" : "");
	if (st.jumps.file != nullptr)
		NOTICE_LOG(SH4, "watch: %" PRIu64 " jumps out of %08x..%08x%s",
				st.jumps.written, st.jumpRange.base,
				st.jumpRange.base + st.jumpRange.size,
				st.jumps.full ? " (limit reached)" : "");
	st.writes.close();
	st.jumps.close();
	g_write = false;
	g_jump = false;
}

void term()
{
	State& st = state();
	st.writes.close();
	st.jumps.close();
	g_write = false;
	g_jump = false;
}

}
