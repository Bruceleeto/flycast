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
// Guest code dumper.
//
// Two dumb, generic jobs, and deliberately nothing else:
//
//   1. write a range of guest memory to a file, and
//   2. log every entry into that range from outside it.
//
// It has no notion of a JIT, a block, or a guest architecture above the SH4.
// Which pages hold generated code, where the blocks begin and end, which PS1
// instructions they came from - all of that is reconstructed offline from the
// dump plus a disassembler, so the same analysis runs unchanged on a guest that
// generates code (bleem) and on one that does not.
//
// The range is given, never guessed. Deciding for itself which pages held
// generated code was tried and removed: it is the caller who has the
// disassembly and knows the addresses, and a heuristic here just produces a
// confident wrong answer.
//
// Output is two files, little-endian, no strings, no aggregation. See
// docs/jitdump.md for the byte layout - the offline reader is written to that
// document, so change them together.
//
#pragma once
#include "types.h"

namespace jitdump
{

// Hot path: the interpreter tests this once per instruction, so it is a plain
// variable read rather than a call. True only while entries are being logged.
extern bool g_active;
inline bool active() { return g_active; }

// Applies the config options. Called from Emulator::init().
void init();
// Per-instruction fetch feed, from the interpreter. Only called when active().
void traceFetch(u32 pc);
// Frame boundary: drives the warm-up and the interval snapshots.
void frameBoundary();
// Writes the region dump and closes the entry log. Must run while the game is
// still loaded, so that guest RAM is still there to read. Idempotent.
void finish();
void term();

}
