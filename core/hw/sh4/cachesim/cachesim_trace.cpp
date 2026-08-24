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
// Records the block execution stream, so that a different code layout can be
// re-simulated offline by tools/cachesweep instead of costing one emulator run
// per candidate. Format is described in cachesim_trace.h.
//
#include "cachesim.h"
#include "cachesim_trace.h"

#include "nowide/cstdio.hpp"

#include <cinttypes>

#include <vector>

namespace cachesim
{

static FILE *traceFile;
static std::vector<char> traceBuffer;
static std::vector<bool> defined;
static u64 lastCycle;
static u64 lastBucket;
static u64 recordsWritten;

static void putByte(u8 b)
{
	std::fputc(b, traceFile);
}

static void putVarint(u64 v)
{
	while (v >= 0x80)
	{
		putByte((u8)(v | 0x80));
		v >>= 7;
	}
	putByte((u8)v);
}

static void putEvent(u8 event)
{
	putVarint(0);
	putByte(event);
}

bool tracing()
{
	return traceFile != nullptr;
}

bool traceOpen(const std::string& path)
{
	traceClose();
	traceFile = nowide::fopen(path.c_str(), "wb");
	if (traceFile == nullptr)
	{
		WARN_LOG(SH4, "cachesim: cannot write trace %s", path.c_str());
		return false;
	}
	// The stream is a few bytes per executed block, so it is written millions of
	// times a second: without a large buffer the run is dominated by write()
	traceBuffer.resize(4 << 20);
	setvbuf(traceFile, traceBuffer.data(), _IOFBF, traceBuffer.size());

	defined.clear();
	lastCycle = 0;
	lastBucket = 0;
	recordsWritten = 0;

	std::fwrite(TRACE_MAGIC, 1, 4, traceFile);
	putVarint(TRACE_VERSION);
	putVarint(IC_SETS);
	putVarint(LINE_BYTES);
	NOTICE_LOG(SH4, "cachesim: recording trace to %s", path.c_str());
	return true;
}

void traceClose()
{
	if (traceFile == nullptr)
		return;
	putEvent(TraceEvent::End);
	std::fclose(traceFile);
	traceFile = nullptr;
	traceBuffer.clear();
	traceBuffer.shrink_to_fit();
	NOTICE_LOG(SH4, "cachesim: trace closed after %" PRIu64 " block records", recordsWritten);
}

void traceBlockExec(const BlockTrace *bt, u64 cycle)
{
	if (bt->id >= defined.size())
		defined.resize(bt->id + 1, false);
	if (!defined[bt->id])
	{
		defined[bt->id] = true;
		putEvent(TraceEvent::DefineBlock);
		putVarint(bt->id);
		putVarint(bt->vaddr);
		putVarint(bt->paddr);
		putVarint(bt->size);
		putVarint(bt->hash);
	}

	// A timestamp on every record would be most of the file. One per cycle
	// bucket is enough to find a burst in time, which is what the timestamps are
	// for, and costs a fraction of a percent.
	const u64 bucket = cycle >> TRACE_CYCLE_BUCKET_SHIFT;
	if (bucket != lastBucket)
	{
		lastBucket = bucket;
		putEvent(TraceEvent::Cycle);
		putVarint(cycle >= lastCycle ? cycle - lastCycle : 0);
		lastCycle = cycle;
	}

	putVarint((u64)bt->id + 1);
	recordsWritten++;
}

void traceEvent(u8 event, u32 a, u32 b)
{
	putEvent(event);
	switch (event)
	{
	case TraceEvent::AddressArrayWrite:
		putVarint(a);
		putVarint(b);
		break;
	default:
		break;
	}
}

} // namespace cachesim
