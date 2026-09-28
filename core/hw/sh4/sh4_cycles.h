/*
	Copyright 2023 flyinghead

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
#pragma once
#include "types.h"
#include "sh4_opcode_list.h"
#include "sh4_if.h"
#include "sh4_sched.h"

class Sh4Cycles
{
public:
	Sh4Cycles(int cpuRatio = 1) : cpuRatio(cpuRatio) {}

	void init(Sh4Context *ctx) {
		this->ctx = ctx;
	}

	// taken: the instruction is a conditional branch that will be taken
	void executeCycles(u16 op, bool taken = false)
	{
		ctx->cycle_counter -= countCycles(op, taken);
	}

	void addCycles(int cycles) const
	{
		ctx->cycle_counter -= cycles;
	}

	void addReadAccessCycles(u32 addr, u32 size) const
	{
		ctx->cycle_counter -= readAccessCycles(addr, size);
	}

	void addWriteAccessCycles(u32 addr, u32 size) const
	{
		ctx->cycle_counter -= writeAccessCycles(addr, size);
	}

	int countCycles(u16 op, bool taken = false);

	void reset()
	{
		lastUnit = CO;
		memOps = 0;
		pipe = {};
	}

	u64 now() {
		return sh4_sched_now64() + sh4SliceLength - ctx->cycle_counter;
	}

	int readAccessCycles(u32 addr, u32 size) const {
		return readExternalAccessCycles(addr, size) * 2 * cpuRatio;
	}

	int writeAccessCycles(u32 addr, u32 size) const {
		return writeExternalAccessCycles(addr, size) * 2 * cpuRatio;
	}

private:
	// Returns the number of external cycles (100 MHz) needed for a sized read at the given address
	static int readExternalAccessCycles(u32 addr, u32 size);
	// Returns the number of external cycles (100 MHz) needed for a sized write at the given address
	static int writeExternalAccessCycles(u32 addr, u32 size);

	int legacyCycles(sh4_opcodelistentry *opcode);

	// Issue-stage model built from measured SH7091 timings (sh4_timing_data.h)
	struct Pipeline
	{
		// Resource ids: 0-15 R0-R15, 16-31 FR0-FR15, then implicit registers
		static constexpr int FirstFR = 16;
		static constexpr int FirstImplicit = 32;
		static constexpr int ResourceCount = 64;
		struct Writer {
			s16 form = -1;
			u8 out = 0;		// producer operand label
			s8 elem = -1;	// element of a vector result
			u64 cycle = 0;	// issue cycle of the producer
		};
		Writer writers[ResourceCount];
		struct Issued {
			s16 form = -1;
			u64 cycle = 0;
		};
		static constexpr int HistorySize = 16;
		Issued history[HistorySize];	// most recent first
		u64 clock = 0;			// issue cycle of the last instruction
		u64 busyUntil = 0;		// first cycle the issue stage is free again
		bool slotOpen = false;	// the last instruction left the second issue slot of its cycle free
		u8 lastClass = 0;
		s32 lastCounter = 0;	// cycle_counter after the last charge, to see external stalls
		bool counterValid = false;
	};
	Pipeline pipe;

	sh4_eu lastUnit = CO;
	const int cpuRatio;
	int memOps = 0;
	Sh4Context *ctx = nullptr;
};
