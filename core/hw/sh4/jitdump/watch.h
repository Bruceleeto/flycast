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
// Two guest watchpoints, in the same spirit as jitdump: dumb, generic, and
// leaving every interpretation to the offline side.
//
//   -watch-write ADDR:SIZE    every SH4 store into the range, with the PC that
//                             made it. Names the writer of a byte outright,
//                             instead of narrowing it down by elimination.
//   -watch-badjump ADDR:SIZE  every fetch that leaves the range other than by
//                             falling off the end of it: the jump out, at the
//                             instant it happens.
//
// Both are fed by the interpreter, which is the only executor that sees single
// stores and single fetches, so asking for either selects it.
//
// A range matches through any mirror: an address is in the range if it is, or
// if it is once the top three bits of both are dropped. Guests that generate
// code routinely write it through one mapping and execute it through another,
// and a watch that missed those would report "nobody wrote it".
//
// See docs/jitdump.md for the file layout.
//
#pragma once
#include "types.h"

namespace watch
{

// Hot paths. Both are plain variable reads: the store hook is only installed
// while the first is set, and the interpreter tests the second per instruction.
extern bool g_write;
inline bool writeArmed() { return g_write; }
extern bool g_jump;
inline bool jumpArmed() { return g_jump; }

void init();
// Wraps the current SH4 store handlers. Called at the end of SetMemoryHandlers,
// since that reassigns them whenever the MMU is turned on or off.
void installHandlers();
// Per-instruction fetch feed, from the interpreter. Only called when jumpArmed().
void traceFetch(u32 pc);
// Flushes and closes the logs. Idempotent.
void finish();
void term();

}
