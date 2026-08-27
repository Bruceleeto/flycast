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
#include "g1ata.h"
#include "fatvfs.h"
#include "gdromv3.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_mem.h"
#include "serialize.h"
#include "stdclass.h"

#include <cstdio>
#include <cstring>

namespace g1ata
{

// Register addresses, as the guest sees them. The taskfile is shared with the
// GD-ROM, which is why these match the GD_* names in gdromv3.h one for one.
enum : u32
{
	ATA_ALTSTATUS   = 0x005F7018,	// R, and device control on write
	ATA_DATA        = 0x005F7080,
	ATA_FEATURES    = 0x005F7084,	// W, error on read
	ATA_SECCOUNT    = 0x005F7088,
	ATA_LBA_LOW     = 0x005F708C,
	ATA_LBA_MID     = 0x005F7090,
	ATA_LBA_HIGH    = 0x005F7094,
	ATA_DEVSEL      = 0x005F7098,
	ATA_COMMAND     = 0x005F709C,	// W, status on read
};

enum : u8
{
	CMD_READ_SECTORS      = 0x20,
	CMD_READ_SECTORS_EXT  = 0x24,
	CMD_READ_DMA_EXT      = 0x25,
	CMD_WRITE_SECTORS     = 0x30,
	CMD_WRITE_SECTORS_EXT = 0x34,
	CMD_WRITE_DMA_EXT     = 0x35,
	CMD_READ_DMA          = 0xC8,
	CMD_WRITE_DMA         = 0xCA,
	CMD_FLUSH_CACHE       = 0xE7,
	CMD_FLUSH_CACHE_EXT   = 0xEA,
	CMD_IDENTIFY          = 0xEC,
	CMD_SET_FEATURES      = 0xEF,
};

enum : u8
{
	SR_ERR  = 0x01,
	SR_DRQ  = 0x08,
	SR_DSC  = 0x10,
	SR_DRDY = 0x40,
	SR_BSY  = 0x80,
};

constexpr u32 SectorSize = 512;

static FILE *image;
static FatVfs vfs;			// used instead of `image` when a directory was attached
static u64 sectorCount;

// The taskfile. Sector count and the three LBA bytes are FIFOs two deep: a
// 48-bit command writes each one twice, most significant half first, and the
// previous value is what the spec calls the HOB. Keeping both halves is the
// whole of LBA48 support.
static struct
{
	u8 features;
	u8 secCount, secCountHob;
	u8 lbaLow, lbaLowHob;
	u8 lbaMid, lbaMidHob;
	u8 lbaHigh, lbaHighHob;
	u8 devSel;
	u8 status;
	u8 error;
	u8 command;
} tf;

// PIO transfer buffer. Sized for one sector: the guest reads or writes it a
// word at a time and we refill at each boundary, so a multi-sector command
// never needs more than this.
static u8 buffer[SectorSize];
static u32 bufPos;
static bool bufValid;
static bool bufIsWrite;

// What a data-transfer command has left to do.
static u64 curLba;
static u32 curCount;

static void raiseInterrupt()
{
	// The drive shares the GD-ROM's INTRQ line.
	asic_RaiseInterrupt(holly_GDROM_CMD);
}

static u64 readLba(bool lba48)
{
	if (lba48)
		return (u64)tf.lbaLow | ((u64)tf.lbaMid << 8) | ((u64)tf.lbaHigh << 16)
				| ((u64)tf.lbaLowHob << 24) | ((u64)tf.lbaMidHob << 32) | ((u64)tf.lbaHighHob << 40);
	// LBA28 keeps the top four bits in the device select register
	return (u64)tf.lbaLow | ((u64)tf.lbaMid << 8) | ((u64)tf.lbaHigh << 16)
			| ((u64)(tf.devSel & 0x0f) << 24);
}

static u32 readCount(bool lba48)
{
	u32 count = lba48 ? (u32)tf.secCount | ((u32)tf.secCountHob << 8) : tf.secCount;
	if (count == 0)
		// 0 means the maximum, which differs between the two forms
		count = lba48 ? 65536 : 256;
	return count;
}

static bool readSectors(u64 lba, u32 count, u8 *dst)
{
	if (lba + count > sectorCount)
		return false;
	if (vfs.valid())
	{
		for (u32 i = 0; i < count; i++)
			if (!vfs.readSector(lba + i, dst + i * SectorSize))
				return false;
		return true;
	}
	if (image == nullptr)
		return false;
	if (std::fseek(image, (long)(lba * SectorSize), SEEK_SET) != 0)
		return false;
	return std::fread(dst, 1, count * SectorSize, image) == count * SectorSize;
}

static bool writeSectors(u64 lba, u32 count, const u8 *src)
{
	if (vfs.valid())
		// A synthesized volume has nowhere to put a write: the host files are
		// the storage, and growing one through a FAT allocator is not something
		// this is trying to be. The guest sees a write-protected disk.
		return false;
	if (image == nullptr || lba + count > sectorCount)
		return false;
	if (std::fseek(image, (long)(lba * SectorSize), SEEK_SET) != 0)
		return false;
	if (std::fwrite(src, 1, count * SectorSize, image) != count * SectorSize)
		return false;
	std::fflush(image);
	return true;
}

static void setError()
{
	tf.status = SR_DRDY | SR_ERR;
	tf.error = 0x04;	// ABRT
	bufValid = false;
	raiseInterrupt();
}

// Word 0 is little-endian in the buffer but the strings are byte-swapped
// within each word, which is what every real drive does and what KOS undoes.
static void putIdentifyString(u16 *id, int word, int words, const char *str)
{
	for (int i = 0; i < words; i++)
	{
		char a = *str != '\0' ? *str++ : ' ';
		char b = *str != '\0' ? *str++ : ' ';
		id[word + i] = (u16)((u8)a << 8 | (u8)b);
	}
}

static void buildIdentify()
{
	memset(buffer, 0, sizeof(buffer));
	u16 *id = (u16 *)buffer;

	id[0] = 0x0040;					// not removable, not ATAPI
	id[1] = 0x3FFF;					// cylinders, only meaningful in CHS
	id[3] = 16;						// heads
	id[6] = 63;						// sectors per track
	putIdentifyString(id, 10, 10, "FLYCAST-IDE0001");	// serial
	putIdentifyString(id, 23, 4, "1.00");				// firmware
	putIdentifyString(id, 27, 20, "Flycast G1 ATA Disk");
	id[47] = 0x8001;				// max sectors per multiple transfer
	id[49] = 1 << 9;				// LBA supported - KOS refuses the drive without this
	id[50] = 0x4000;
	id[53] = 0x0007;				// words 64-70, 88 and 54-58 are valid
	id[59] = 0x0101;				// multiple sector transfer enabled, 1 sector
	id[63] = 0x0007;				// multiword DMA modes 0-2 supported
	id[64] = 0x0003;				// PIO modes 3 and 4
	id[80] = 0x007E;				// ATA-1 through ATA-6
	id[82] = 1 << 5;				// write cache
	id[83] = (1 << 10) | (1 << 14);	// LBA48 supported, and word 83 is valid
	id[84] = 1 << 14;
	id[86] = 1 << 10;				// LBA48 enabled
	id[87] = 1 << 14;
	id[88] = 0x0007;				// ultra DMA modes 0-2

	// LBA28 capacity saturates at its 28-bit limit; the real size is in the
	// LBA48 words, which is how a drive above 128GB reports itself.
	const u32 lba28 = (u32)std::min<u64>(sectorCount, 0x0FFFFFFF);
	id[60] = (u16)lba28;
	id[61] = (u16)(lba28 >> 16);
	id[100] = (u16)sectorCount;
	id[101] = (u16)(sectorCount >> 16);
	id[102] = (u16)(sectorCount >> 32);
	id[103] = (u16)(sectorCount >> 48);

	bufPos = 0;
	bufValid = true;
	bufIsWrite = false;
	curCount = 0;
	tf.status = SR_DRDY | SR_DSC | SR_DRQ;
	raiseInterrupt();
}

// Fills the buffer with the next sector of a PIO read, or completes the
// command when there are none left.
static void nextReadSector()
{
	if (curCount == 0)
	{
		tf.status = SR_DRDY | SR_DSC;
		bufValid = false;
		return;
	}
	if (!readSectors(curLba, 1, buffer))
	{
		setError();
		return;
	}
	curLba++;
	curCount--;
	bufPos = 0;
	bufValid = true;
	bufIsWrite = false;
	tf.status = SR_DRDY | SR_DSC | SR_DRQ;
	raiseInterrupt();
}

static void startPioRead(bool lba48)
{
	curLba = readLba(lba48);
	curCount = readCount(lba48);
	nextReadSector();
}

static void startPioWrite(bool lba48)
{
	curLba = readLba(lba48);
	curCount = readCount(lba48);
	bufPos = 0;
	bufValid = true;
	bufIsWrite = true;
	// A write command asks for the first sector without an interrupt: the
	// guest is expected to see DRQ and start feeding us.
	tf.status = SR_DRDY | SR_DSC | SR_DRQ;
}

static void execCommand(u8 cmd)
{
	tf.command = cmd;
	tf.error = 0;

	switch (cmd)
	{
	case CMD_IDENTIFY:
		buildIdentify();
		break;

	case CMD_READ_SECTORS:
		startPioRead(false);
		break;
	case CMD_READ_SECTORS_EXT:
		startPioRead(true);
		break;

	case CMD_WRITE_SECTORS:
		startPioWrite(false);
		break;
	case CMD_WRITE_SECTORS_EXT:
		startPioWrite(true);
		break;

	case CMD_READ_DMA:
	case CMD_WRITE_DMA:
	case CMD_READ_DMA_EXT:
	case CMD_WRITE_DMA_EXT:
		// The command only arms the transfer. Nothing moves until the guest
		// writes the DMA start register, which lands in dmaStart() below.
		{
			const bool lba48 = cmd == CMD_READ_DMA_EXT || cmd == CMD_WRITE_DMA_EXT;
			curLba = readLba(lba48);
			curCount = readCount(lba48);
			tf.status = SR_DRDY | SR_DSC;
			bufValid = false;
		}
		break;

	case CMD_FLUSH_CACHE:
	case CMD_FLUSH_CACHE_EXT:
		if (image != nullptr)
			std::fflush(image);
		tf.status = SR_DRDY | SR_DSC;
		raiseInterrupt();
		break;

	case CMD_SET_FEATURES:
		// Transfer mode selection and friends. Nothing here is modelled, and
		// accepting them is what a drive that supports the mode would do.
		tf.status = SR_DRDY | SR_DSC;
		raiseInterrupt();
		break;

	default:
		INFO_LOG(GDROM, "G1 ATA: unimplemented command %02x", cmd);
		setError();
		break;
	}
}

void init(const std::string& path)
{
	term();
	if (path.empty())
		return;

	// A directory is served directly; only a file gets opened as an image.
	if (vfs.init(path))
	{
		sectorCount = vfs.sectors();
		tf = {};
		tf.devSel = 0xa0;
		tf.status = SR_DRDY | SR_DSC;
		bufValid = false;
		bufPos = 0;
		curCount = 0;
		return;
	}

	image = nowide::fopen(path.c_str(), "r+b");
	if (image == nullptr)
		// Read-only is still useful: a guest that only loads data never
		// notices, and it beats refusing to attach at all.
		image = nowide::fopen(path.c_str(), "rb");
	if (image == nullptr)
		return;

	std::fseek(image, 0, SEEK_END);
	const long size = std::ftell(image);
	std::fseek(image, 0, SEEK_SET);
	if (size <= 0)
	{
		std::fclose(image);
		image = nullptr;
		return;
	}
	sectorCount = (u64)size / SectorSize;

	tf = {};
	tf.devSel = 0xa0;
	tf.status = SR_DRDY | SR_DSC;
	bufValid = false;
	bufPos = 0;
	curCount = 0;

	NOTICE_LOG(GDROM, "G1 ATA: attached %s as slave device, %llu sectors (%.1f MB)",
			path.c_str(), (unsigned long long)sectorCount, size / 1024.0 / 1024.0);
}

void term()
{
	if (image != nullptr)
	{
		std::fclose(image);
		image = nullptr;
	}
	vfs.term();
	sectorCount = 0;
}

bool present()
{
	return image != nullptr || vfs.valid();
}

bool selected()
{
	return present() && (tf.devSel & 0x10) != 0;
}

u32 readReg(u32 addr, u32 sz)
{
	switch (addr)
	{
	case ATA_DATA:
		{
			if (!bufValid || bufIsWrite)
				return 0;
			u32 data = 0;
			// KOS reads words, but honour whatever width it asks for.
			for (u32 i = 0; i < sz && bufPos < SectorSize; i++)
				data |= (u32)buffer[bufPos++] << (i * 8);
			if (bufPos >= SectorSize)
				nextReadSector();
			return data;
		}

	case ATA_ALTSTATUS:
		// Same value as the status register but without acknowledging INTRQ.
		return tf.status;

	case ATA_COMMAND:	// status
		asic_CancelInterrupt(holly_GDROM_CMD);
		return tf.status;

	case ATA_FEATURES:	// error
		return tf.error;
	case ATA_SECCOUNT:
		return tf.secCount;
	case ATA_LBA_LOW:
		return tf.lbaLow;
	case ATA_LBA_MID:
		return tf.lbaMid;
	case ATA_LBA_HIGH:
		return tf.lbaHigh;
	case ATA_DEVSEL:
		return tf.devSel;

	default:
		return 0;
	}
}

void writeReg(u32 addr, u32 data, u32 sz)
{
	switch (addr)
	{
	case ATA_DATA:
		{
			if (!bufValid || !bufIsWrite)
				break;
			for (u32 i = 0; i < sz && bufPos < SectorSize; i++)
				buffer[bufPos++] = (u8)(data >> (i * 8));
			if (bufPos >= SectorSize)
			{
				if (!writeSectors(curLba, 1, buffer))
				{
					setError();
					break;
				}
				curLba++;
				curCount--;
				bufPos = 0;
				if (curCount == 0)
				{
					tf.status = SR_DRDY | SR_DSC;
					bufValid = false;
				}
				raiseInterrupt();
			}
		}
		break;

	case ATA_FEATURES:
		tf.features = (u8)data;
		break;
	case ATA_SECCOUNT:
		tf.secCountHob = tf.secCount;
		tf.secCount = (u8)data;
		break;
	case ATA_LBA_LOW:
		tf.lbaLowHob = tf.lbaLow;
		tf.lbaLow = (u8)data;
		break;
	case ATA_LBA_MID:
		tf.lbaMidHob = tf.lbaMid;
		tf.lbaMid = (u8)data;
		break;
	case ATA_LBA_HIGH:
		tf.lbaHighHob = tf.lbaHigh;
		tf.lbaHigh = (u8)data;
		break;

	case ATA_DEVSEL:
		tf.devSel = (u8)data;
		break;

	case ATA_COMMAND:
		execCommand((u8)data);
		break;

	case ATA_ALTSTATUS:	// device control
		if (data & 0x04)
		{
			// Software reset
			tf.status = SR_DRDY | SR_DSC;
			tf.error = 1;
			bufValid = false;
			curCount = 0;
		}
		break;

	default:
		break;
	}
}

bool dmaStart()
{
	if (!selected() || curCount == 0)
		return false;

	const bool write = tf.command == CMD_WRITE_DMA || tf.command == CMD_WRITE_DMA_EXT;
	// SB_GDLEN is what the guest asked to move; a drive transfers whole
	// sectors, so the shorter of the two bounds the transfer.
	u32 count = std::min<u32>(curCount, SB_GDLEN / SectorSize);
	if (count == 0)
		count = curCount;

	u32 addr = SB_GDSTAR & 0x1fffffe0;
	bool ok = true;

	for (u32 i = 0; i < count && ok; i++)
	{
		u8 sector[SectorSize];
		if (write)
		{
			for (u32 j = 0; j < SectorSize; j += 4)
				*(u32 *)&sector[j] = ReadMem32_nommu(addr + j);
			ok = writeSectors(curLba, 1, sector);
		}
		else
		{
			ok = readSectors(curLba, 1, sector);
			if (ok)
				for (u32 j = 0; j < SectorSize; j += 4)
					WriteMem32_nommu(addr + j, *(u32 *)&sector[j]);
		}
		addr += SectorSize;
		curLba++;
		curCount--;
	}

	if (!ok)
	{
		setError();
		SB_GDST = 0;
		return true;
	}

	SB_GDSTARD = addr;
	SB_GDLEND = count * SectorSize;
	SB_GDST = 0;
	tf.status = SR_DRDY | SR_DSC;

	// Both interrupts fire: the DMA one is what KOS waits on, and the drive
	// still asserts INTRQ for the command it just finished.
	asic_RaiseInterrupt(holly_GDROM_DMA);
	raiseInterrupt();
	return true;
}

void serialize(Serializer& ser)
{
	ser << tf;
	ser << buffer;
	ser << bufPos;
	ser << bufValid;
	ser << bufIsWrite;
	ser << curLba;
	ser << curCount;
}

void deserialize(Deserializer& deser)
{
	deser >> tf;
	deser >> buffer;
	deser >> bufPos;
	deser >> bufValid;
	deser >> bufIsWrite;
	deser >> curLba;
	deser >> curCount;
}

} // namespace g1ata
