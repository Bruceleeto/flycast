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
// Trace file format, shared by the recorder in flycast and the reader in
// tools/cachesweep.
//
//   "FCST" varint version, varint icSets, varint lineBytes
//   then a stream of varints:
//     v >= 1  block execution, block id = v - 1
//     v == 0  escape: one event byte follows, then its payload
//
// Block definitions are emitted inline the first time a block executes, so the
// reader never needs a second pass or a separate dictionary.
//
#pragma once
#include <cstdint>

namespace cachesim
{

constexpr const char *TRACE_MAGIC = "FCST";
constexpr uint32_t TRACE_VERSION = 1;
// One timestamp per bucket rather than per record: enough to locate a burst in
// time, at a fraction of the size of stamping every block.
constexpr uint32_t TRACE_CYCLE_BUCKET_SHIFT = 12;	// 4096 guest cycles

struct TraceEvent
{
	enum : uint8_t
	{
		DefineBlock = 0,	// varint id, vaddr, paddr, size, hash
		InvalidateInst = 1,
		AddressArrayWrite = 2,	// varint addr, data
		FrameBoundary = 3,
		Cycle = 4,		// varint delta since the last timestamp
		End = 5,
	};
};

} // namespace cachesim
