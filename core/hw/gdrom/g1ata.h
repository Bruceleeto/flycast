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
// The ATA drive some Dreamcasts have on the G1 bus in place of, or alongside,
// the GD-ROM. It is the ATA *slave*; the GD-ROM is the master, and the two
// share one register file, so which of them a register access belongs to is
// decided by the device select register. gdromv3.cpp owns that register and
// routes here when the slave is selected.
//
// This exists because homebrew developed against a console with an IDE mod
// reads its data from /ide, and had no way to run under flycast at all: the
// image had to be repacked as a disc, which changes the very I/O path a
// benchmark is trying to hold constant.
//
// Backed by a flat image file, so the guest's filesystem is the guest's
// business - KOS mounts the FAT, this only serves sectors.
//
#pragma once
#include "types.h"

#include <string>

class Serializer;
class Deserializer;

namespace g1ata
{

// Attaches the image at `path` as the slave device. Missing file is not an
// error: the guest then sees an empty bus and carries on, exactly as it does
// on a console with no drive fitted.
void init(const std::string& path);
void term();

// True once an image is attached. When false every access below is left to
// the GD-ROM, so a normal disc boot behaves as it always did.
bool present();

// True when the guest has the slave selected and there is a drive to talk to.
bool selected();

u32 readReg(u32 addr, u32 sz);
void writeReg(u32 addr, u32 data, u32 sz);

// Called when the guest starts a G1 DMA. Returns false if the transfer is not
// ours, leaving the GD-ROM to handle it.
bool dmaStart();

void serialize(Serializer& ser);
void deserialize(Deserializer& deser);

} // namespace g1ata
