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
// SH7091 performance counters, answered from the cache and pipeline models.
//
// The point of these is validation. A guest that reads PMCR on a Dreamcast gets
// what the silicon counted; the same guest reading PMCR here gets what cachesim
// and pipesim believe. Running one unmodified binary in both places and diffing
// the output is a far stronger check than correlating two different tools, and
// it is the only way to compare per-event rather than per-frame.
//
// Events with no model behind them read zero, and say so once in the log rather
// than quietly returning a plausible number. A zero is obviously missing; a
// guess is not.
//
#pragma once
#include "types.h"

namespace pmcr
{

// 0xFF000084 / 0xFF000088, 16-bit control
bool isControlAddr(u32 addr);
u16 readControl(u32 addr);
void writeControl(u32 addr, u16 value);

// 0xFF100004..0xFF100010, 32-bit counter halves
bool isCounterAddr(u32 addr);
u32 readCounter(u32 addr);

void reset();

// Guest cycles the SH4 spent halted in `sleep`. PMCR's elapsed-time counter
// stops when the CPU halts - measured on hardware by validation/pmcrclock,
// which reads 200.00MHz in a busy loop and 0.78MHz across a thd_sleep - so
// these have to come off the elapsed-time event or flycast reports wall time
// where hardware reports awake time. On texture2d that is a 31% difference and
// it looked for a long while like a frame pacing bug.
void noteSleep(u64 cycles);

} // namespace pmcr
