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
#include "cachesim_symbols.h"
#include "cachesim.h"

#include "nowide/cstdio.hpp"
#include "stdclass.h"

extern "C" {
#include <elf/elf.h>
}

#include <algorithm>
#include <vector>

namespace cachesim
{

namespace
{

struct Symbol
{
	u32 start;	// region bits only, so P0/P1/P2 views of the same code match
	u32 end;
	std::string name;
};

struct Segment
{
	u32 vaddr;
	u32 size;
	size_t fileOffset;
};

std::vector<Symbol> symbols;
std::vector<Segment> segments;
std::vector<u8> image;
std::string source;

constexpr u32 regionBits(u32 addr) { return addr & 0x1fffffff; }

// Bytes of the binary at a guest address, or nullptr if that address is not
// backed by the file.
const u8 *imageAt(u32 vaddr, u32 size)
{
	const u32 addr = regionBits(vaddr);
	for (const Segment& seg : segments)
	{
		const u32 start = regionBits(seg.vaddr);
		if (addr < start || addr + size > start + seg.size)
			continue;
		const size_t offset = seg.fileOffset + (addr - start);
		if (offset + size > image.size())
			return nullptr;
		return image.data() + offset;
	}
	return nullptr;
}

bool readSymbolTable(const elf_t& elf)
{
	for (size_t i = 0; i < elf_getNumSections(&elf); i++)
	{
		if (elf_getSectionType(&elf, i) != SHT_SYMTAB)
			continue;
		const size_t entSize = elf_getSectionEntrySize(&elf, i);
		if (entSize < sizeof(Elf32_Sym))
			continue;
		const size_t count = elf_getSectionSize(&elf, i) / entSize;
		const auto *syms = (const Elf32_Sym *)elf_getSection(&elf, i);
		const char *strings = elf_getStringTable(&elf, elf_getSectionLink(&elf, i));
		if (syms == nullptr || strings == nullptr)
			continue;

		for (size_t s = 0; s < count; s++)
		{
			const Elf32_Sym& sym = syms[s];
			// Only things with an extent: a zero-sized symbol cannot own an
			// address range, and naming a range is the whole point here
			if (sym.st_size == 0 || sym.st_value == 0)
				continue;
			if (ELF32_ST_TYPE(sym.st_info) != STT_FUNC)
				continue;
			const char *name = strings + sym.st_name;
			if (name == nullptr || *name == '\0')
				continue;
			symbols.push_back({ regionBits(sym.st_value),
					regionBits(sym.st_value) + sym.st_size, name });
		}
	}
	std::sort(symbols.begin(), symbols.end(),
			[](const Symbol& a, const Symbol& b) { return a.start < b.start; });
	return !symbols.empty();
}

void readSegments(const elf_t& elf)
{
	for (size_t i = 0; i < elf_getNumProgramHeaders(&elf); i++)
	{
		if (elf_getProgramHeaderType(&elf, i) != PT_LOAD)
			continue;
		segments.push_back({ (u32)elf_getProgramHeaderVaddr(&elf, i),
				(u32)elf_getProgramHeaderFileSize(&elf, i),
				elf_getProgramHeaderOffset(&elf, i) });
	}
}

} // anonymous namespace

bool loadSymbols(const std::string& path)
{
	symbols.clear();
	segments.clear();
	image.clear();
	source.clear();

	FILE *f = nowide::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	image.resize(size > 0 ? size : 0);
	const size_t read = std::fread(image.data(), 1, image.size(), f);
	std::fclose(f);
	image.resize(read);

	elf_t elf;
	if (elf_newFile(image.data(), image.size(), &elf) != 0)
	{
		WARN_LOG(SH4, "cachesim: %s is not an ELF, no symbols loaded", path.c_str());
		image.clear();
		return false;
	}
	readSegments(elf);
	if (!readSymbolTable(elf))
	{
		WARN_LOG(SH4, "cachesim: %s has no symbol table, no names available", path.c_str());
		image.clear();
		segments.clear();
		return false;
	}
	source = path;
	NOTICE_LOG(SH4, "cachesim: loaded %zu symbols from %s", symbols.size(), path.c_str());
	return true;
}

void discoverSymbols(const std::string& contentPath)
{
	if (contentPath.empty())
		return;
	// A disc image carries no symbols, so look for the ELF it was built from
	// sitting beside it under the same name
	const size_t dot = contentPath.find_last_of('.');
	const std::string base = dot == std::string::npos ? contentPath : contentPath.substr(0, dot);
	for (const char *ext : { ".elf", ".ELF" })
		if (loadSymbols(base + ext))
			return;
}

bool symbolsLoaded()
{
	return !symbols.empty();
}

const std::string& symbolSource()
{
	return source;
}

const char *symbolFor(u32 addr)
{
	if (symbols.empty())
		return nullptr;
	const u32 a = regionBits(addr);
	// First symbol starting after a, then step back one
	auto it = std::upper_bound(symbols.begin(), symbols.end(), a,
			[](u32 value, const Symbol& s) { return value < s.start; });
	if (it == symbols.begin())
		return nullptr;
	--it;
	return a < it->end ? it->name.c_str() : nullptr;
}

SymbolVerification verifySymbols()
{
	SymbolVerification result { 0, 0, false };
	if (symbols.empty())
		return result;

	// Hash the binary's bytes where blocks actually executed. A stale ELF left
	// beside a disc names every row confidently and wrongly, and .text moves
	// whenever the guest is rebuilt, so this is a gate rather than a note.
	for (const BlockTrace& block : blocks())
	{
		const u8 *bytes = imageAt(block.vaddr, block.size);
		if (bytes == nullptr)
			// Generated code, or RAM the binary never occupied: nothing to
			// check against, and not evidence either way
			continue;
		result.checked++;
		if (hashCode(bytes, block.size) == block.hash)
			result.matched++;
	}
	result.trusted = result.checked >= 32 && result.matched * 10 >= result.checked * 9;
	return result;
}

} // namespace cachesim
