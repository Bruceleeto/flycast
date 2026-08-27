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
#include "fatvfs.h"
#include "stdclass.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace
{

constexpr u32 SectorSize = 512;
constexpr u32 SectorsPerCluster = 4;		// 2KB clusters
constexpr u32 ReservedSectors = 32;
constexpr u32 PartitionStart = 2048;
constexpr u32 NumFats = 2;
// FAT32 is only well defined above this many clusters; below it, drivers are
// entitled to read the volume as FAT16. The volume is synthetic, so padding out
// to reach it costs nothing but a larger reported size.
constexpr u32 MinFat32Clusters = 65536 + 16;

constexpr u32 AttrReadOnly  = 0x01;
constexpr u32 AttrDirectory = 0x10;
constexpr u32 AttrLongName  = 0x0f;

void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

u8 shortNameChecksum(const u8 *shortName)
{
	u8 sum = 0;
	for (int i = 0; i < 11; i++)
		sum = (u8)(((sum & 1) << 7) + (sum >> 1) + shortName[i]);
	return sum;
}

// 8.3 name for `name`, uniquified with ~N. Long names get a full LFN chain as
// well, so this only has to be unique and legal, not pretty.
void makeShortName(const std::string& name, int ordinal, u8 out[11])
{
	memset(out, ' ', 11);
	std::string base = name;
	std::string ext;
	const size_t dot = name.find_last_of('.');
	if (dot != std::string::npos && dot != 0)
	{
		base = name.substr(0, dot);
		ext = name.substr(dot + 1);
	}

	auto legal = [](char c) -> char {
		if (c >= 'a' && c <= 'z')
			return (char)(c - 'a' + 'A');
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			return c;
		if (strchr("$%'-_@~`!(){}^#&", c) != nullptr)
			return c;
		return '_';
	};

	std::string tail;
	if (ordinal > 0)
		tail = "~" + std::to_string(ordinal);

	const size_t baseLen = std::min<size_t>(base.size(), 8 - tail.size());
	size_t o = 0;
	for (size_t i = 0; i < baseLen; i++)
		out[o++] = (u8)legal(base[i]);
	for (char c : tail)
		out[o++] = (u8)c;

	for (size_t i = 0; i < std::min<size_t>(ext.size(), 3); i++)
		out[8 + i] = (u8)legal(ext[i]);
}

// Only when the name genuinely does not fit 8.3. Case does not count: a
// lowercase name that otherwise fits is stored uppercase with the lowercase
// flags set, which every driver reads correctly and a driver with no long-name
// support can still find. Forcing an LFN for case alone renames `game.bin` to
// GAME~1.BIN in the short entry, and anything not parsing long names then
// fails to open it.
bool needsLongName(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
	const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
	if (base.empty() || base.size() > 8 || ext.size() > 3)
		return true;
	if (name.find_last_of('.') != name.find_first_of('.'))
		return true;		// more than one dot has no 8.3 form
	for (char c : name)
	{
		if (c == '.')
			continue;
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
				|| (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c) != nullptr;
		if (!ok)
			return true;
	}
	return false;
}

// The 0x08/0x10 flags in the reserved byte say the stored uppercase name is
// really all lowercase.
u8 lowercaseFlags(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
	const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
	auto allLower = [](const std::string& s) {
		bool anyAlpha = false;
		for (char c : s) {
			if (c >= 'A' && c <= 'Z')
				return false;
			if (c >= 'a' && c <= 'z')
				anyAlpha = true;
		}
		return anyAlpha;
	};
	u8 flags = 0;
	if (allLower(base))
		flags |= 0x08;
	if (allLower(ext))
		flags |= 0x10;
	return flags;
}

// One 32-byte directory entry.
void appendEntry(std::vector<u8>& dir, const u8 shortName[11], u8 attr,
		u32 cluster, u64 size, u8 ntRes = 0)
{
	const size_t at = dir.size();
	dir.resize(at + 32, 0);
	u8 *e = &dir[at];
	memcpy(e, shortName, 11);
	e[11] = attr;
	e[12] = ntRes;
	put16(e + 14, 0);					// creation time
	put16(e + 16, 0x21);				// creation date, 1980-01-01
	put16(e + 18, 0x21);
	put16(e + 20, (u16)(cluster >> 16));
	put16(e + 22, 0);					// write time
	put16(e + 24, 0x21);				// write date
	put16(e + 26, (u16)cluster);
	put32(e + 28, (u32)size);
}

void appendLongName(std::vector<u8>& dir, const std::string& name, u8 checksum)
{
	// UTF-16, 13 code units per entry, stored last chunk first.
	std::vector<u16> wide;
	for (char c : name)
		wide.push_back((u8)c);
	wide.push_back(0);
	while (wide.size() % 13 != 0)
		wide.push_back(0xFFFF);

	const int chunks = (int)(wide.size() / 13);
	for (int chunk = chunks - 1; chunk >= 0; chunk--)
	{
		const size_t at = dir.size();
		dir.resize(at + 32, 0);
		u8 *e = &dir[at];
		e[0] = (u8)(chunk + 1) | (chunk == chunks - 1 ? 0x40 : 0);
		e[11] = AttrLongName;
		e[12] = 0;
		e[13] = checksum;
		put16(e + 26, 0);
		static const int slots[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
		for (int i = 0; i < 13; i++)
			put16(e + slots[i], wide[chunk * 13 + i]);
	}
}

} // anonymous namespace

u32 FatVfs::allocClusters(u32 count)
{
	const u32 first = nextCluster;
	nextCluster += count;
	return first;
}

// Walks `path`, appending this directory's entries to dirBytes and recursing.
// Returns the first cluster of the directory.
u32 FatVfs::scanDirectory(const std::string& path, u32 parentCluster, int depth)
{
	if (depth > 8)
		return 0;

	DIR *dir = opendir(path.c_str());
	if (dir == nullptr)
		return 0;

	// Collect first so the cluster numbers can be assigned in order.
	struct Child { std::string name; std::string path; bool isDir; u64 size; };
	std::vector<Child> children;
	while (dirent *de = readdir(dir))
	{
		const std::string name = de->d_name;
		if (name == "." || name == "..")
			continue;
		const std::string child = path + "/" + name;
		struct stat st;
		if (stat(child.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			children.push_back({ name, child, true, 0 });
		else if (S_ISREG(st.st_mode))
			children.push_back({ name, child, false, (u64)st.st_size });
	}
	closedir(dir);
	std::sort(children.begin(), children.end(),
			[](const Child& a, const Child& b) { return a.name < b.name; });

	// Size this directory before recursing. Its clusters have to be contiguous,
	// so they must all be taken before the children start allocating - working
	// it out afterwards would hand back clusters the children already own.
	// The entry count follows from the names alone: one entry each, plus the
	// long-name slots, plus dot and dot-dot below the root.
	const u32 clusterBytes = SectorsPerCluster * SectorSize;
	u32 entryCount = parentCluster != 0 ? 2 : 0;
	for (const Child& c : children)
	{
		entryCount++;
		if (needsLongName(c.name))
			entryCount += (u32)((c.name.size() + 1 + 12) / 13);
	}
	const u32 needed = std::max<u32>(1, (entryCount * 32 + clusterBytes - 1) / clusterBytes);
	const u32 myCluster = allocClusters(needed);

	std::vector<u8> entries;
	if (parentCluster != 0)
	{
		u8 dot[11];
		memset(dot, ' ', 11);
		dot[0] = '.';
		appendEntry(entries, dot, AttrDirectory, myCluster, 0);
		dot[1] = '.';
		// ".." points at the root as cluster 0 by convention, not as cluster 2.
		appendEntry(entries, dot, AttrDirectory,
				parentCluster == rootCluster ? 0 : parentCluster, 0);
	}

	int ordinal = 0;
	for (const Child& c : children)
	{
		u8 shortName[11];
		const bool lfn = needsLongName(c.name);
		makeShortName(c.name, lfn ? ++ordinal : 0, shortName);
		if (lfn)
			appendLongName(entries, c.name, shortNameChecksum(shortName));

		if (c.isDir)
		{
			const u32 sub = scanDirectory(c.path, myCluster, depth + 1);
			appendEntry(entries, shortName, AttrDirectory, sub, 0,
					lfn ? 0 : lowercaseFlags(c.name));
		}
		else
		{
			const u32 clusters = (u32)((c.size + SectorsPerCluster * SectorSize - 1)
					/ (SectorsPerCluster * SectorSize));
			const u32 first = clusters == 0 ? 0 : allocClusters(clusters);
			files.push_back({ c.path, c.size, first, clusters });
			appendEntry(entries, shortName, AttrReadOnly, first, c.size,
					lfn ? 0 : lowercaseFlags(c.name));
		}
	}

	// Park the entries in dirBytes and map this directory's clusters to them.
	verify(entries.size() <= (size_t)needed * clusterBytes);
	const u64 at = dirBytes.size();
	dirBytes.resize(at + (u64)needed * clusterBytes, 0);
	memcpy(&dirBytes[at], entries.data(), entries.size());

	if (clusterMap.size() < nextCluster)
		clusterMap.resize(nextCluster, { -1, ~0ull });
	for (u32 i = 0; i < needed; i++)
		clusterMap[myCluster + i] = { -1, at + (u64)i * clusterBytes };

	return myCluster;
}

bool FatVfs::init(const std::string& root)
{
	term();

	struct stat st;
	if (stat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
		return false;

	sectorsPerCluster = SectorsPerCluster;
	partitionStart = PartitionStart;
	reservedSectors = ReservedSectors;
	nextCluster = rootCluster;

	clusterMap.assign(rootCluster, { -1, ~0ull });
	if (scanDirectory(root, 0, 0) == 0)
		return false;

	// Everything is allocated: size the volume around it.
	clusterCount = std::max(nextCluster, MinFat32Clusters);
	fatSectors = (clusterCount + 2) * 4;
	fatSectors = (fatSectors + SectorSize - 1) / SectorSize;
	dataStartSector = partitionStart + reservedSectors + NumFats * fatSectors;
	totalSectors = (u64)dataStartSector + (u64)clusterCount * sectorsPerCluster;

	// Build the FAT. Every file and directory is laid out contiguously, so a
	// chain is just a run of consecutive numbers.
	fat.assign(clusterCount + 2, 0);
	fat[0] = 0x0FFFFFF8;
	fat[1] = 0x0FFFFFFF;
	for (u32 c = rootCluster; c < nextCluster; c++)
		fat[c] = c + 1;
	// Map the file clusters first: the directory pass below decides where a
	// chain ends by looking at what each cluster holds, and an unmapped cluster
	// is indistinguishable from the end of a directory.
	if (clusterMap.size() < nextCluster)
		clusterMap.resize(nextCluster, { -1, ~0ull });
	for (size_t i = 0; i < files.size(); i++)
	{
		const Entry& f = files[i];
		for (u32 c = 0; c < f.clusters; c++)
			clusterMap[f.firstCluster + c] = { (int)i, (u64)c * sectorsPerCluster * SectorSize };
	}

	// Terminate each file's last cluster.
	for (const Entry& f : files)
		if (f.clusters != 0)
			fat[f.firstCluster + f.clusters - 1] = 0x0FFFFFFF;
	// And each directory's. A directory's clusters are the ones mapped to
	// dirBytes; the run ends where the next source differs.
	for (u32 c = rootCluster; c < nextCluster; c++)
	{
		if (clusterMap[c].fileIndex != -1)
			continue;
		const bool lastOfRun = c + 1 >= nextCluster
				|| clusterMap[c + 1].fileIndex != -1
				|| clusterMap[c + 1].offset != clusterMap[c].offset + sectorsPerCluster * SectorSize;
		if (lastOfRun)
			fat[c] = 0x0FFFFFFF;
	}

	NOTICE_LOG(GDROM, "G1 ATA: serving %s as a %llu MB FAT32 volume, %zu files",
			root.c_str(), (unsigned long long)(totalSectors * SectorSize / 1024 / 1024),
			files.size());
	return true;
}

void FatVfs::term()
{
	files.clear();
	dirBytes.clear();
	clusterMap.clear();
	fat.clear();
	totalSectors = 0;
	nextCluster = rootCluster;
}

bool FatVfs::readSector(u64 lba, u8 *buf)
{
	memset(buf, 0, SectorSize);
	if (lba >= totalSectors)
		return false;

	// Master boot record: one partition covering the volume.
	if (lba == 0)
	{
		u8 *p = buf + 446;
		p[0] = 0x00;			// not bootable
		p[1] = 0x01; p[2] = 0x01; p[3] = 0x00;		// CHS start, not used
		p[4] = 0x0c;			// FAT32 LBA
		p[5] = 0xfe; p[6] = 0xff; p[7] = 0xff;		// CHS end, saturated
		put32(p + 8, partitionStart);
		put32(p + 12, (u32)(totalSectors - partitionStart));
		buf[510] = 0x55;
		buf[511] = 0xAA;
		return true;
	}
	if (lba < partitionStart)
		return true;			// gap before the partition

	const u64 rel = lba - partitionStart;

	// Boot sector, and its backup at sector 6.
	if (rel == 0 || rel == 6)
	{
		buf[0] = 0xEB; buf[1] = 0x58; buf[2] = 0x90;		// jmp, as a real one has
		memcpy(buf + 3, "MSWIN4.1", 8);
		put16(buf + 11, SectorSize);
		buf[13] = (u8)sectorsPerCluster;
		put16(buf + 14, (u16)reservedSectors);
		buf[16] = NumFats;
		put16(buf + 17, 0);			// root entries, always 0 on FAT32
		put16(buf + 19, 0);			// small sector count, unused
		buf[21] = 0xF8;				// fixed disk
		put16(buf + 22, 0);			// FAT16 fat size, unused
		put16(buf + 24, 63);		// sectors per track
		put16(buf + 26, 16);		// heads
		put32(buf + 28, partitionStart);
		put32(buf + 32, (u32)(totalSectors - partitionStart));
		put32(buf + 36, fatSectors);
		put16(buf + 40, 0);			// flags: FAT mirroring on
		put16(buf + 42, 0);			// version
		put32(buf + 44, rootCluster);
		put16(buf + 48, 1);			// FSInfo sector
		put16(buf + 50, 6);			// backup boot sector
		buf[64] = 0x80;				// drive number
		buf[66] = 0x29;				// extended boot signature
		put32(buf + 67, 0x464C5943);	// volume id
		memcpy(buf + 71, "IDE        ", 11);
		memcpy(buf + 82, "FAT32   ", 8);
		buf[510] = 0x55;
		buf[511] = 0xAA;
		return true;
	}
	if (rel == 1 || rel == 7)
	{
		put32(buf + 0, 0x41615252);
		put32(buf + 484, 0x61417272);
		put32(buf + 488, 0xFFFFFFFF);	// free count, unknown
		put32(buf + 492, 0xFFFFFFFF);	// next free, unknown
		buf[510] = 0x55;
		buf[511] = 0xAA;
		return true;
	}

	// The two FAT copies.
	const u64 fatStart = reservedSectors;
	if (rel >= fatStart && rel < fatStart + (u64)NumFats * fatSectors)
	{
		const u64 offset = ((rel - fatStart) % fatSectors) * SectorSize;
		for (u32 i = 0; i < SectorSize / 4; i++)
		{
			const u64 index = offset / 4 + i;
			if (index < fat.size())
				put32(buf + i * 4, fat[index]);
		}
		return true;
	}

	// Data region.
	if (lba < dataStartSector)
		return true;
	const u64 dataSector = lba - dataStartSector;
	const u32 cluster = (u32)(dataSector / sectorsPerCluster) + rootCluster;
	const u32 inCluster = (u32)(dataSector % sectorsPerCluster);
	if (cluster >= clusterMap.size())
		return true;

	const ClusterSource& src = clusterMap[cluster];
	if (src.offset == ~0ull)
		return true;

	if (src.fileIndex == -1)
	{
		const u64 at = src.offset + inCluster * SectorSize;
		if (at < dirBytes.size())
			memcpy(buf, &dirBytes[at], std::min<u64>(SectorSize, dirBytes.size() - at));
		return true;
	}

	const Entry& f = files[src.fileIndex];
	const u64 at = src.offset + inCluster * SectorSize;
	if (at >= f.size)
		return true;
	FILE *fp = nowide::fopen(f.hostPath.c_str(), "rb");
	if (fp == nullptr)
		return true;
	std::fseek(fp, (long)at, SEEK_SET);
	std::fread(buf, 1, std::min<u64>(SectorSize, f.size - at), fp);
	std::fclose(fp);
	return true;
}
