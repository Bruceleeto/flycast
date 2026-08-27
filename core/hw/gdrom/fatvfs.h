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
// A read-only FAT32 volume synthesized from a host directory, served a sector
// at a time to the G1 ATA drive.
//
// The alternative is packing the directory into an image file, which means a
// second copy of every asset and a repack step between editing a file and
// running. Nothing here is copied: the boot sector, FATs and directories are a
// few hundred KB of generated metadata, and a read of a file's sectors is a
// read of the host file at the matching offset.
//
// Read-only on purpose. A guest that only loads its data never notices, and
// writing would mean either growing host files through a FAT allocator or
// silently dropping the write - both worse than saying no.
//
#pragma once
#include "types.h"

#include <string>
#include <vector>

class FatVfs
{
public:
	// Builds the volume from `root`. Returns false if the directory is missing
	// or unreadable, in which case nothing is attached.
	bool init(const std::string& root);
	void term();

	bool valid() const { return totalSectors != 0; }
	u64 sectors() const { return totalSectors; }

	// Fills `buf` with 512 bytes. Sectors outside anything mapped read as
	// zeroes, which is what unwritten areas of a real disk hold.
	bool readSector(u64 lba, u8 *buf);

private:
	struct Entry
	{
		std::string hostPath;	// empty for a directory
		u64 size;
		u32 firstCluster;
		u32 clusters;
	};

	// Where a data cluster's contents come from: a host file, or a block of
	// generated directory entries.
	struct ClusterSource
	{
		int fileIndex;		// -1 when this is directory metadata
		u64 offset;			// into the host file, or into dirBytes
	};

	u32 scanDirectory(const std::string& path, u32 parentCluster, int depth);
	u32 allocClusters(u32 count);

	std::vector<Entry> files;
	std::vector<u8> dirBytes;			// every directory's entries, concatenated
	std::vector<ClusterSource> clusterMap;
	std::vector<u32> fat;

	u64 totalSectors = 0;
	u32 partitionStart = 0;
	u32 reservedSectors = 0;
	u32 fatSectors = 0;
	u32 dataStartSector = 0;
	u32 sectorsPerCluster = 0;
	u32 clusterCount = 0;
	u32 rootCluster = 2;
	u32 nextCluster = 2;
};
