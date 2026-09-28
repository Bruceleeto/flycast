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
#include "sh4_cycles.h"
#include "sh4_timing_data.h"
#include "modules/mmu.h"
#include <algorithm>
#include <memory>
#include <unordered_map>

namespace
{
using namespace sh4timing;

// Lookup tables derived from sh4_timing_data.h
struct TimingTables
{
	s16 formOf[65536];
	u8 block[FormCount][FormCount];		// structural: B cannot issue before A + d
	std::unordered_map<u32, u8> latency;
	int maxLatency = 1;		// longest distance any rule can impose
	int maxBlock = 0;

	static u32 key(int a, int out, int b, int label, int elem) {
		return (u32)a | ((u32)out << 8) | ((u32)b << 13) | ((u32)label << 21) | ((u32)(elem + 1) << 26);
	}

	TimingTables()
	{
		static_assert(FormCount <= 256 && L_COUNT <= 32, "key packing");
		bool isTakenForm[FormCount] {};
		for (const Form& f : forms)
			if (f.taken >= 0)
				isTakenForm[f.taken] = true;
		std::fill(std::begin(formOf), std::end(formOf), -1);
		for (int i = 0; i < FormCount; i++)
		{
			if (isTakenForm[i])
				continue;
			for (u32 op = 0; op < 0x10000; op++)
				if ((op & forms[i].mask) == forms[i].match)
					formOf[op] = i;
		}
		memset(block, 0, sizeof(block));
		for (const auto& e : structural)
		{
			block[e.a][e.b] = e.d;
			maxBlock = std::max<int>(maxBlock, e.d);
		}
		for (const auto& e : sh4timing::latency)
		{
			latency[key(e.a, e.out, e.b, e.label, e.elem)] = e.d;
			maxLatency = std::max<int>(maxLatency, e.d);
		}
	}

	// Required issue distance from producer a (writing operand out, element elem) to consumer b through operand label,
	// or -1 if nothing was measured
	int lookup(int a, int out, int b, int label, int elem) const
	{
		auto it = latency.find(key(a, out, b, label, elem));
		if (it == latency.end() && elem >= 0)
			it = latency.find(key(a, out, b, label, -1));
		return it == latency.end() ? -1 : it->second;
	}
};

const TimingTables& tables()
{
	static std::unique_ptr<TimingTables> t = std::make_unique<TimingTables>();
	return *t;
}

// Measured co-issue matrix: row = first, column = second (classes 1..5)
const bool coIssue[6][6] = {
	{},
	{ false, false, true,  false, true,  true },	// C1: load/store, FP move
	{ false, true,  false, false, true,  true },	// C2: integer ALU
	{},												// C3: always alone
	{ false, true,  true,  false, true,  true },	// C4: compare, mov, nop, not-taken branch
	{ false, true,  true,  false, true,  false },	// C5: FP arithmetic
};

// Maps an operand of a form to the resources it names. elem receives the element index within a vector operand.
int resolve(const Form& f, u8 label, u16 op, int res[4], s8 elem[4])
{
	constexpr int FR = 16;
	auto n = [&]() { return (op >> f.nshift) & ((1 << f.nwidth) - 1); };
	auto m = [&]() { return (op >> f.mshift) & ((1 << f.mwidth) - 1); };
	int base, count = 1;
	switch (label)
	{
	case L_RN:
		if (f.nwidth != 4)
			return 0;
		base = n();
		break;
	case L_RM:
		if (f.mwidth != 4)
			return 0;
		base = m();
		break;
	case L_R0:
		base = 0;
		break;
	case L_FRN:
		base = FR + n();
		break;
	case L_FRM:
		base = FR + m();
		break;
	case L_FR0:
		base = FR;
		break;
	case L_FVN:
		base = FR + n() * 4;
		count = 4;
		break;
	case L_FVM:
		base = FR + m() * 4;
		count = 4;
		break;
	case L_DRN:
		base = FR + n() * 2;
		count = 2;
		break;
	case L_FIPRN:
		base = FR + n() * 4 + 3;
		break;
	default:
		base = 32 + label - L_T;
		break;
	}
	for (int i = 0; i < count; i++)
	{
		res[i] = base + i;
		elem[i] = count > 1 ? i : -1;
	}
	return count;
}

}	// namespace

int Sh4Cycles::legacyCycles(sh4_opcodelistentry *opcode)
{
	if (lastUnit == CO
			|| opcode->unit == CO
			|| (lastUnit == opcode->unit && lastUnit != MT))
	{
		// cannot run in parallel
		lastUnit = opcode->unit;
		return opcode->IssueCycles;
	}
	// can run in parallel
	lastUnit = CO;
	return 0;
}

int Sh4Cycles::countCycles(u16 op, bool taken)
{
	sh4_opcodelistentry *opcode = OpDesc[op];
	int cycles = 0;
#ifndef STRICT_MODE
	static const bool isMemOp[45] {
		false,
		false,
		true,	// all mem moves, ldtlb, sts.l FPUL/FPSCR, @-Rn, lds.l @Rn+,FPUL
		true,	// gbr-based load/store
		false,
		true,	// tst.b #<imm8>, @(R0,GBR)
		true,	// and/or/xor.b #<imm8>, @(R0,GBR)
		true,	// tas.b @Rn
		false,
		false,
		false,
		false,
		true,	// movca.l R0, @Rn
		false,
		false,
		false,
		false,
		true,	// ldc.l @Rn+, VBR/SPC/SSR/Rn_Bank/DBR
		true,	// ldc.l @Rn+, GBR/SGR
		true,	// ldc.l @Rn+, SR
		false,
		false,
		true,	// stc.l DBR/SR/GBR/VBR/SSR/SPC/Rn_Bank, @-Rn
		true,	// stc.l SGR, @-Rn
		false,
		true,	// lds.l @Rn+, PR
		false,
		true,	// sts.l PR, @-Rn
		false,
		true,	// lds.l @Rn+, MACH/MACL
		false,
		true,	// sts.l MACH/MACL, @-Rn
		false,
		true,	// lds.l @Rn+,FPSCR
		false,
		true,	// mac.wl @Rm+,@Rn+
	};
	if (isMemOp[opcode->ex_type])
	{
		if (++memOps < 4)
			cycles = mmu_enabled() ? 5 : 2;
	}
	// TODO only for mem read?
#endif

	// Cycles charged by others (memory accesses, exceptions) since the last instruction: the pipeline was frozen
	if (ctx != nullptr && pipe.counterValid)
	{
		int external = (pipe.lastCounter - ctx->cycle_counter) / cpuRatio;
		if (external > 0)
		{
			pipe.busyUntil += external;
			pipe.clock += external;
			pipe.slotOpen = false;
		}
	}

	const TimingTables& t = tables();
	int f = t.formOf[op];
	if (f >= 0 && taken && forms[f].taken >= 0)
		f = forms[f].taken;

	int issued;
	if (f < 0)
	{
		// Not measured (sleep, trapa, illegal): issue alone
		int occupancy = std::max(1, legacyCycles(opcode));
		pipe.clock = pipe.busyUntil;
		pipe.busyUntil += occupancy;
		pipe.slotOpen = false;
		std::copy_backward(std::begin(pipe.history), std::end(pipe.history) - 1, std::end(pipe.history));
		pipe.history[0] = { -1, pipe.clock };
		issued = occupancy;
	}
	else
	{
		const Form& form = forms[f];
		const int occupancy = std::max<int>(1, form.cost);
		int res[4];
		s8 elem[4];

		// Data hazards
		u64 need = 0;
		for (int i = 0; i < form.nops; i++)
		{
			const Operand& o = form.ops[i];
			int count = resolve(form, o.label, op, res, elem);
			for (int j = 0; j < count; j++)
			{
				const Pipeline::Writer& w = pipe.writers[res[j]];
				if (w.form < 0 || w.cycle + t.maxLatency <= pipe.clock)
					continue;
				int d = t.lookup(w.form, w.out, f, o.label, w.elem);
				if (d < 0)
					// nothing measured: an explicit register is still never used in the cycle it is written
					d = res[j] < Pipeline::FirstImplicit ? 1 : 0;
				need = std::max(need, w.cycle + d);
			}
		}
		// Structural hazards
		for (const Pipeline::Issued& h : pipe.history)
		{
			if (h.cycle + t.maxBlock <= pipe.clock)
				break;	// most recent first: the rest are older
			if (h.form >= 0 && t.block[h.form][f] != 0)
				need = std::max(need, h.cycle + t.block[h.form][f]);
		}

		u64 issue;
		bool paired = false;
		if (pipe.slotOpen && occupancy == 1 && coIssue[pipe.lastClass][form.cls] && need <= pipe.clock)
		{
			issue = pipe.clock;
			paired = true;
		}
		else {
			issue = std::max(pipe.busyUntil, need);
		}
		u64 end = issue + occupancy;
		issued = end > pipe.busyUntil ? (int)(end - pipe.busyUntil) : 0;
		pipe.busyUntil = std::max(pipe.busyUntil, end);
		pipe.clock = issue;
		pipe.slotOpen = !paired && occupancy == 1 && form.cls != 0 && form.cls != 3;
		pipe.lastClass = form.cls;

		for (int i = 0; i < form.nops; i++)
		{
			const Operand& o = form.ops[i];
			if (!(o.rw & 2))
				continue;
			int count = resolve(form, o.label, op, res, elem);
			for (int j = 0; j < count; j++)
				pipe.writers[res[j]] = { (s16)f, o.label, elem[j], issue };
		}
		std::copy_backward(std::begin(pipe.history), std::end(pipe.history) - 1, std::end(pipe.history));
		pipe.history[0] = { (s16)f, issue };
	}
	cycles += issued;

	cycles *= cpuRatio;
	if (ctx != nullptr)
	{
		pipe.lastCounter = ctx->cycle_counter - cycles;
		pipe.counterValid = true;
	}
	return cycles;
}

// TODO additional wait cycles depending on area?:
// Area       Wait cycles (not including external wait)
// 0          3
// 1 VRAM     3
// 2 reserved 3
// 3 SDRAM    0         CAS latency 3
// 4 TA,YUV   1
// 5 G2 ext   3
// 6 reserved 3

int Sh4Cycles::readExternalAccessCycles(u32 addr, u32 size)
{
	if ((addr & 0xfc000000) == 0xe0000000)
		// store queues
		return 0;

	addr &= 0x1fffffff;
	switch (addr >> 26)
	{
	case 0:
		if (!settings.platform.isAtomiswave())
		{
			// Dreamcast, Naomi
			if (addr < 0x00200000)
			{
				// system rom
				switch (size)
				{
				case 1:
					return 44;
				case 2:
					return 63;
				case 4:
					return 99;
				case 32:
				default:
					return 618;
				}
			}
			if (addr < 0x00200000 + settings.platform.flash_size)
			{
				// flash
				switch (size)
				{
				case 1:
					return 41;
				case 2:
					return 55;
				case 4:
					return 83;
				case 32:
				default:
					return 489;
				}
			}
		}
		else
		{
			// Atomiswave
			if (addr < 0x00020000 || (addr >= 0x00200000 && addr < 0x00200000 + settings.platform.flash_size))
			{
				// flash
				switch (size)
				{
				case 1:
					return 41;
				case 2:
					return 55;
				case 4:
					return 83;
				case 32:
				default:
					return 489;
				}
			}
		}
		addr &= 0x01ffffff;
		if (addr >= 0x005f6800 && addr <= 0x005f69ff)
		{
			// holly system control regs
			if (size != 4)
				INFO_LOG(SH4, "holly system reg: Invalid read size %d @ %07x", size, addr);
			return 5;
		}
		if (addr >= 0x005f6c00 && addr <= 0x005f6cff)
		{
			// maple regs
			if (size != 4)
				INFO_LOG(SH4, "maple reg: Invalid read size %d @ %07x", size, addr);
			return 22;
		}
		if (addr >= 0x005f7000 && addr <= 0x005f70ff)
		{
			if (settings.platform.isArcade())
				// naomi/aw cart
				return 20; // ???
			else
			{
				// gd-rom
				if (size > 2)
					INFO_LOG(SH4, "gd-rom: Invalid read size %d @ %07x", size, addr);
				return 39;
			}
		}
		if (addr >= 0x005f7400 && addr <= 0x005f74ff)
		{
			// G1 I/F control regs
			if (settings.platform.isConsole())
			{
				if (size != 4) // unknown for aw/naomi
					INFO_LOG(SH4, "G1 I/F: Invalid read size %d @ %07x", size, addr);
			}
			else
			{
				// unknown for aw/naomi. seeing size 1 and 4 at least
			}
			return 24;
		}
		if (addr >= 0x005f7800 && addr <= 0x005f78ff)
		{
			// G2 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G2 I/F: Invalid read size %d @ %07x", size, addr);
			return 38;
		}
		if (addr >= 0x005f7c00 && addr <= 0x005f7cff)
		{
			// PVR I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "PVR I/F: Invalid read size %d @ %07x", size, addr);
			return 24;
		}
		if (addr >= 0x005f8000 && addr <= 0x005f9fff)
		{
			// TA/PVR core control regs, Palette RAM, fog table
			if (size != 4)
				// TODO 32-byte access allowed for palette and fog tables?
				INFO_LOG(SH4, "PVR/TA core: Invalid read size %d @ %07x", size, addr);
			return 34;
		}
		if (addr >= 0x00600000 && addr <= 0x006007ff)
		{
			if (settings.platform.isConsole())
				// AW registers
				return 20; // ???
			else
			{
				// modem
				if (size != 1)
					INFO_LOG(SH4, "modem: Invalid read size %d @ %07x", size, addr);
				return 67;
			}
		}
		if (addr >= 0x00700000 && addr <= 0x00ffffff)
		{
			// aica regs and ram
			if (size < 4)
				INFO_LOG(SH4, "aica: Invalid read size %d @ %07x", size, addr);
			// measured: AICA register, RTC or wave RAM read over G2, 252 Icyc back to back (bsc-memmap.md 4.7);
			// bleem's expander loop (hwtest/sh4loop) confirms it: +232 per word against a main RAM stream
			return 126;
		}
		if (addr >= 0x01000000 && addr <= 0x01ffffff)
		{
			// G2 external area
			switch (size)
			{
			case 1:
			case 2:
				return 56;
			case 4:
				return 60;
			case 32:
			default:
				return 84;
			}
		}
		break;

	case 1:
		// VRAM
		switch (size)
		{
		case 1:
			INFO_LOG(SH4, "vram: Invalid read size 1 @ %07x", addr);
			return 41;
		case 2:
		case 4:
			return 41;
		case 32:
		default:
			return 61;
		}

	case 2:
		// Area 2
		INFO_LOG(SH4, "Invalid read from area 2 @ %07x", addr);
		return 60;

	case 3:
		// System RAM. Measured on the console (shrike4-rtl bsc-memmap.md 4.7): uncached read B/W/L 16 Icyc,
		// 64-bit FMOV read 30 (two longword cycles), line fill 16. Row misses (26) are not modelled.
		return size == 8 ? 15 : 8;

	case 4:
		// TA FIFO
		if (size != 32)
			INFO_LOG(SH4, "Invalid read size %d from area 4 (TA FIFO) @ %07x", size, addr);
		if ((addr >= 0x11000000 && addr <= 0x11ffffff) || (addr >= 0x13000000 && addr <= 0x13ffffff))
			// VRAM (64 bits)
			return 61;	// undocumented
		break;

	case 5:
		// Ext device
		switch (size)
		{
		case 1:
		case 2:
			return 56;
		case 4:
			return 60;
		case 32:
		default:
			return 84;
		}

	case 6:
		// Area 6
		INFO_LOG(SH4, "Invalid read from area 6 @ %07x", addr);
		return 60;

	case 7:
		// SH4 registers
		return 0;
	}

	INFO_LOG(SH4, "Unmapped read @ %08x", addr);
	return 60;
}


int Sh4Cycles::writeExternalAccessCycles(u32 addr, u32 size)
{
	if ((addr & 0xfc000000) == 0xe0000000)
		// store queues
		return 0;

	addr &= 0x1fffffff;
	switch (addr >> 26)
	{
	case 0:
		if (!settings.platform.isAtomiswave())
		{
			if (addr < 0x00200000)
			{
				// system rom
				INFO_LOG(SH4, "Invalid write to rom @ %07x", addr);
				return 99;
			}
			if (addr < 0x00200000 + settings.platform.flash_size)
			{
				// flash
				if (size != 1)
					INFO_LOG(SH4, "flashrom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
		}
		else
		{
			if (addr < 0x00020000)
			{
				// flash
				if (size != 1)
					INFO_LOG(SH4, "flashrom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
			if (addr >= 0x00200000 && addr < 0x00200000 + settings.platform.flash_size)
			{
				// nvmem
				return 14; // ????
			}
		}
		addr &= 0x01ffffff;
		if (addr >= 0x005f6800 && addr <= 0x005f69ff)
		{
			// holly system control regs
			if (size != 4)
				INFO_LOG(SH4, "holly system reg: Invalid write size %d @ %07x", size, addr);
			return 5;
		}
		if (addr >= 0x005f6c00 && addr <= 0x005f6cff)
		{
			// maple regs
			if (size != 4)
				INFO_LOG(SH4, "maple reg: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7000 && addr <= 0x005f70ff)
		{
			if (settings.platform.isArcade())
				// naomi/aw cart
				return 14; // ???
			else
			{
				// gd-rom
				if (size > 2)
					INFO_LOG(SH4, "gd-rom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
		}
		if (addr >= 0x005f7400 && addr <= 0x005f74ff)
		{
			// G1 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G1 I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7800 && addr <= 0x005f78ff)
		{
			// G2 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G2 I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7c00 && addr <= 0x005f7cff)
		{
			// PVR I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "PVR I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f8000 && addr <= 0x005f9fff)
		{
			// TA/PVR core control regs, Palette RAM, fog table
			if (size != 4)
				// TODO 32-byte access allowed for palette and fog tables?
				INFO_LOG(SH4, "PVR/TA core: Invalid write size %d @ %07x", size, addr);
			return 14;
		}
		if (addr >= 0x00600000 && addr <= 0x006007ff)
		{
			if (settings.platform.isAtomiswave())
				// AW registers
				return 14; // ???
			else
			{
				// modem
				if (size != 1)
					INFO_LOG(SH4, "modem: Invalid write size %d @ %07x", size, addr);
				return 44;
			}
		}
		if (addr >= 0x00700000 && addr <= 0x00ffffff)
		{
			// aica regs and ram
			if (size < 4)
				INFO_LOG(SH4, "aica: Invalid read size %d @ %07x", size, addr);
			return 12 * size / 4;	// undocumented
		}
		if (addr >= 0x01000000 && addr <= 0x01ffffff)
		{
			// G2 external area
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return 28;
			case 32:
			default:
				return 52;
			}
		}
		break;

	case 1:
		// VRAM
		switch (size)
		{
		case 1:
			INFO_LOG(SH4, "vram: Invalid write size 1 @ %07x", addr);
			return 12;
		case 2:
		case 4:
			return 12;
		case 32:
		default:
			return 38;
		}

	case 2:
		// Area 2
		INFO_LOG(SH4, "Invalid read to area 2 @ %07x", addr);
		return 12;

	case 3:
		// System RAM. Measured (bsc-memmap.md 4.7): write B/W/L 10 Icyc back to back, 64-bit FMOV write 20
		// (two writes), 32-byte burst (write-back, SQ) 12.
		return size == 8 ? 10 : size == 32 ? 6 : 5;

	case 4:
		// TA FIFO
		if (size != 32)
			INFO_LOG(SH4, "Invalid write size %d to area 4 (TA FIFO) @ %07x", size, addr);
		if ((addr >= 0x10000000 && addr <= 0x107fffff) || (addr >= 0x12000000 && addr <= 0x127fffff))
			// TA polygon data
			return 7;	// undocumented
		if ((addr >= 0x10800000 && addr <= 0x10ffffff) || (addr >= 0x12800000 && addr <= 0x12ffffff))
			// YUV converter
			return 9;	// 858 cycles for 3072 bytes (YUV420)
		if ((addr >= 0x11000000 && addr <= 0x11ffffff) || (addr >= 0x13000000 && addr <= 0x13ffffff))
			// VRAM (64 bits)
			return 5;	// 8 for 32-bit access (LMMODE0/1)
		break;

	case 5:
		// Ext device
		switch (size)
		{
		case 1:
		case 2:
		case 4:
			return 28;
		case 32:
		default:
			return 52;
		}

	case 6:
		// Area 6
		INFO_LOG(SH4, "Invalid write to area 6 @ %07x", addr);
		return 14;

	case 7:
		// SH4 registers
		return 0;
	}

	INFO_LOG(SH4, "Unmapped read @ %08x", addr);
	return 14;
}
