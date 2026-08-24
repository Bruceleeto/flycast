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
// Optional symbol names for the cache report.
//
// Names are decoration and never a dependency: with no symbols every view still
// works on raw addresses, which is the only thing available for a retail disc
// or for code a guest generated at runtime.
//
// A disc image carries no symbols, so they come from the ELF the image was
// built from, found beside the content or named explicitly. That ELF is
// verified against the code actually executed - by hashing its bytes at the
// addresses of blocks that ran - because a stale sidecar left next to a disc
// would otherwise put confident, wrong function names on every row.
//
#pragma once
#include "types.h"

#include <string>

namespace cachesim
{

struct SymbolVerification
{
	size_t checked;		// blocks that fell inside a loadable ELF segment
	size_t matched;		// of those, blocks whose bytes hashed identically
	bool trusted;		// enough checked, and enough of them matched
};

// Explicit file, or found beside the content. Both accept an ELF; anything
// else is reported and ignored.
bool loadSymbols(const std::string& path);
void discoverSymbols(const std::string& contentPath);

bool symbolsLoaded();
const std::string& symbolSource();
// nullptr when unknown, which is the normal case for generated code
const char *symbolFor(u32 addr);
// Runs the hash check against the blocks executed so far. Cheap enough to call
// when a report is written, which is the point where names would be believed.
SymbolVerification verifySymbols();

} // namespace cachesim
