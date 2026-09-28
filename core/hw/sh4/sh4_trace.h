// Debug tracing of guest behaviour: I/O register accesses, control transfers into the BIOS,
// exceptions/interrupts and the last instructions executed before a jump to the reset vector.
// Set SH4_TRACE to 0 to compile it all out.
#pragma once
#define SH4_TRACE 1

#if SH4_TRACE
#include "types.h"
#include "sh4_core.h"
#include "sh4_opcode_list.h"
#include "hw/pvr/pvr_mem.h"
#include <unordered_map>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include "hw/sh4/sh4_mem.h"
#include "hw/mem/addrspace.h"
#include "hw/aica/aica_if.h"
#include "hw/arm7/arm7.h"

namespace gdr { void insertDisk(const std::string& path); void openLid(); }

namespace sh4trace
{

inline bool limit(u32 code, int max)
{
	static std::unordered_map<u32, int> counts;
	return ++counts[code] <= max;
}

inline bool inBios(u32 pc) {
	return (pc & 0x1fffffff) < 0x00200000;
}
inline bool inRam(u32 pc) {
	return (pc & 0x1c000000) == 0x0c000000;
}

inline void dumpRing(const char *why);
inline u64 instrCount;
inline u64 cdOpenAt;	// instr count of the first SPI CD_OPEN (disc swap test)
// SH4 reads of sound RAM against the ARM loop's output window (bleem 8c000260 stage): lr writes the good value,
// ip (lr - 0xe00) overwrites it with the XOR stream. Good: ip <= A < lr.
inline void aramRead(u32 pc, u32 a, u32 value)
{
	static u64 n;
	static u64 bad[3];
	n++;
	const u32 A = a & 0x1fffff;
	const u32 lr = aica::arm::arm_Reg[14].I & 0x1fffff;
	const u32 ip = aica::arm::arm_Reg[12].I & 0x1fffff;
	int cls = 0;						// 0 good, 1 overwritten by ip, 2 not written yet
	if (A >= 0x100100 && A < 0x1f0000)
	{
		if (A >= lr)
			cls = 2;
		else if (A < ip)
			cls = 1;
	}
	bool log = n <= 24 || (n & 2047) == 0;
	if (cls && A >= 0x100100 && ++bad[cls] <= 8)
		log = true;
	if (log && limit(0xa7a10, 400))
		WARN_LOG(SH4, "ARAMREAD #%llu %s A %06x -> %08x  sh4 pc %08x instr %llu  arm lr %06x ip %06x (A-ip %d, lr-A %d) arm7 %d",
				(unsigned long long)n, cls == 0 ? "ok  " : cls == 1 ? "OVER" : "NEW ", A, value, pc,
				(unsigned long long)instrCount, lr, ip, (int)(A - ip), (int)(lr - A), aica::arm::Arm7Enabled);
}
inline void reportMaskSource();
// Uncached accesses to hardware registers made by non-BIOS code
inline void io(bool write, u32 physAddr, u32 value, int size)
{
	const u32 pc = Sh4cntx.pc - 2;
	if (inBios(pc))
		return;
	const u32 a = physAddr & 0x1fffffff;
	if (!write && a >= 0x00800000 && a < 0x00a00000)
		aramRead(pc, a, value);
	const bool regs = (a >= 0x005f0000 && a < 0x00800000)	// holly, gd-rom, g1/g2, pvr, modem, aica regs
			|| a >= 0x1c000000;								// sh4 on-chip registers (p4)
	if (!regs)
		return;
	struct Seen { int count = 0; u32 last = 0; };
	static std::unordered_map<u32, Seen> seen;
	if (write && (a == 0x005f6910 || a == 0x005f6920 || a == 0x005f6930) && value == 0xffffffbe && limit(0x90000 | a, 1))
	{
		reportMaskSource();
		dumpRing("holly interrupt mask <- ffffffbe");
	}
	Seen& s = seen[(physAddr << 1) | write];
	s.count++;
	// holly interrupt masks and GD-DMA protection writes are always logged (up to 400)
	const bool keyWrite = write && s.count <= 400
			&& ((a >= 0x005f6910 && a < 0x005f6940) || a == 0x005f74b8);
	// first 16 accesses, then only when a read returns a new value, up to 256
	if (!keyWrite && s.count > 16 && (write || value == s.last || s.count > 256))
	{
		s.last = value;
		return;
	}
	s.last = value;
	WARN_LOG(SH4, "IO %s%d %08x %s %08x  pc %08x  #%d", write ? "W" : "R", size * 8, physAddr,
			write ? "<-" : "->", value, pc, s.count);
}

struct Entry {
	u32 pc;
	u16 op;
	u8 t;
};
constexpr int RingSize = 512;
inline Entry ring[RingSize];
inline u32 ringPos;
// non-sequential control transfers (branches, calls, exceptions), longer history than the ring
struct Jump { u32 from, to, sr, r0; };
constexpr int JumpSize = 1024;
inline Jump jumps[JumpSize];
inline u32 jumpPos;

inline void dumpRing(const char *why)
{
	WARN_LOG(SH4, "==== %s: last %d control transfers ====", why, JumpSize);
	for (int i = 0; i < JumpSize; i++)
	{
		const Jump& j = jumps[(jumpPos + i) % JumpSize];
		if (j.from == 0 && j.to == 0)
			continue;
		WARN_LOG(SH4, "  jump %08x -> %08x  sr %08x r0 %08x", j.from, j.to, j.sr, j.r0);
	}
	WARN_LOG(SH4, "==== %s: last %d instructions ====", why, RingSize);
	for (int i = 0; i < RingSize; i++)
	{
		const Entry& e = ring[(ringPos + i) % RingSize];
		if (e.pc == 0 && e.op == 0)
			continue;
		char dis[256];
		OpDesc[e.op]->Disassemble(dis, e.pc, e.op);
		WARN_LOG(SH4, "  %08x  %04x  T=%d  %s", e.pc, e.op, e.t, dis);
	}
	const Sh4Context& c = Sh4cntx;
	WARN_LOG(SH4, "  r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x",
			c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7]);
	WARN_LOG(SH4, "  r8 %08x r9 %08x r10 %08x r11 %08x r12 %08x r13 %08x r14 %08x r15 %08x",
			c.r[8], c.r[9], c.r[10], c.r[11], c.r[12], c.r[13], c.r[14], c.r[15]);
	WARN_LOG(SH4, "  sr %08x pr %08x gbr %08x vbr %08x ssr %08x spc %08x fpscr %08x mac %08x:%08x",
			c.sr.getFull(), c.pr, c.gbr, c.vbr, c.ssr, c.spc, c.fpscr.full, c.mac.h, c.mac.l);
	WARN_LOG(SH4, "==== end ====");
}


// ---- data-side tracing (buffer watch, VRAM, TMU, stale I-cache fetches) ----
inline int followJumps;
// Observe one control-flow data word, including RAM mirrors, without dumping code.
inline bool targetOverlap(u32 address, u32 size)
{
	return inRam(address) && size != 0
			&& ((0x40u - (address & 0x00ffffff)) & 0x00ffffff) < size;
}
inline void targetBlock(const char *who, u32 address, const void *source, u32 size)
{
	if (!targetOverlap(address, size))
		return;
	const u32 offset = (0x40u - (address & 0x00ffffff)) & 0x00ffffff;
	if (size - offset < 4)
		return;
	u32 value, before;
	memcpy(&value, (const u8 *)source + offset, 4);
	memcpy(&before, GetMemPtr(0x0c000040, 4), 4);
	WARN_LOG(SH4, "TARGET40 %s address %08x ram-before %08x incoming %08x pc %08x instr %llu",
			who, address + offset, before, value, Sh4cntx.pc - 2, (unsigned long long)instrCount);
}
struct IntEv { u64 n; u32 code, pc, sr; };
constexpr int IntSize = 64;
inline IntEv intRing[IntSize];
inline u32 intPos;
inline bool drive73Done;	// set by GD-ROM command 0x73, cleared by the next CD_READ
inline bool armAfterDma;	// dump history at the first interrupt after a large GD-ROM DMA completes
inline u64 dmaDoneAt;
inline void dumpRing(const char *why);

// last writer of every system RAM word: CPU data writes (pc) or DMA (pc = 0xd0a0d0a0)
struct Writer { u32 pc; u32 value; u64 n; };
inline Writer *shadow()
{
	static Writer *s = new Writer[0x01000000 / 4]();
	return s;
}
inline void shadowWrite(u32 a, u32 value, int size, u32 pc)
{
	a &= 0x1fffffff;
	if ((a & 0x1c000000) != 0x0c000000)
		return;
	Writer& w = shadow()[(a & 0x00ffffff) >> 2];
	w = { pc, size == 4 ? value : (w.value & ~(((1u << (size * 8)) - 1) << ((a & 3) * 8)))
			| ((value & ((1u << (size * 8)) - 1)) << ((a & 3) * 8)), instrCount };
}
// last writer of every VRAM word, by storage offset (64-bit layout); path 4 = 64-bit area, 5 = 32-bit area
struct VWriter { u32 pc; u32 value; u64 n; u8 path; };
inline VWriter *vshadow()
{
	static VWriter *s = new VWriter[0x00800000 / 4]();
	return s;
}
// 32-bit path offset -> storage offset (banks interleaved every 32 bits; same as pvr_map32 for 8 MB)
inline u32 vramMap32(u32 off)
{
	off &= 0x007fffff;
	return (off & 0x00000003) | ((off & 0x003ffffc) << 1) | ((off & 0x00400000) ? 4 : 0);
}
inline u32 vramStorage(u32 phys)
{
	return (phys & 0x01000000) ? vramMap32(phys) : (phys & 0x007fffff);
}
// non-CPU transfers into VRAM: dst is a 0x04 (64-bit) or 0x05 (32-bit) path address, contiguous in that path.
// kind: 1 ch2 DMA, 2 PVR-DMA, 3 store queue; recorded in VWriter.path as kind << 4 | 4/5
inline void vramXfer(u32 kind, const char *who, u32 src, u32 dst, u32 len, u32 lmmode0, u32 lmmode1)
{
	const u8 path = (u8)((kind << 4) | ((dst & 0x01000000) ? 5 : 4));
	for (u32 i = 0; i < len; i += 4)
	{
		VWriter& w = vshadow()[vramStorage((dst + i) & 0x1fffffff) >> 2];
		w = { Sh4cntx.pc - 2, 0, instrCount, path };
	}
	if (limit(0xd0000 | kind, kind == 3 ? 40 : 400))
		WARN_LOG(SH4, "VRAMXFER %s src %08x -> %08x (storage %06x) len %x path %d LMMODE %d/%d pc %08x instr %llu", who, src, dst,
				vramStorage(dst & 0x1fffffff), len, (dst & 0x01000000) ? 32 : 64, lmmode0, lmmode1, Sh4cntx.pc - 2,
				(unsigned long long)instrCount);
}
inline void reportVShadow(u32 phys, const char *what)
{
	const u32 o = vramStorage(phys) & ~3;
	const VWriter& w = vshadow()[o >> 2];
	const u32 *v = (const u32 *)(const u32 *)&vram[o];
	WARN_LOG(SH4, "VSHADOW %-10s %08x (storage %06x) vram %08x  last write %08x by pc %08x path %d instr %llu", what, phys, o,
			v ? *v : 0, w.value, w.pc, w.path, (unsigned long long)w.n);
}
inline void reportShadow(u32 a, const char *what)
{
	a &= 0x1fffffff;
	const Writer& w = shadow()[(a & 0x00ffffff) >> 2];
	const u32 *ram = (const u32 *)GetMemPtr(0x0c000000 | (a & 0x00fffffc), 4);
	WARN_LOG(SH4, "SHADOW %-10s %08x ram %08x  last write %08x by pc %08x instr %llu", what, a, ram ? *ram : 0,
			w.value, w.pc, (unsigned long long)w.n);
}
// who produced the holly interrupt mask value ~[[8c0000ac]+0x18]
inline void reportMaskSource()
{
	reportShadow(0x0c0000ac, "[8c0000ac]");
	const u32 *pp = (const u32 *)GetMemPtr(0x0c0000ac, 4);
	if (pp == nullptr)
		return;
	const u32 p = *pp;
	for (u32 off = 0; off < 0x40; off += 4)
	{
		char what[16];
		snprintf(what, sizeof(what), "[p+%02x]", off);
		reportShadow(p + off, what);
	}
}
// low RAM (0c000000-0c0007ff, written by the wrapping GD-DMA) snapshot at DMA end, compared at decode time
inline u32 lowSnap[0x200];
inline bool lowSnapValid;
inline u32 (*ocacheAddrArray)(u32 index);	// set by sh4_interpreter.cpp
inline const u8 *(*ocacheLineData)(u32 index);
inline u32 (*icacheAddrArray)(u32 index);
inline const u8 *(*icacheLineData)(u32 index);
// I-cache line fills: when and from which pc each line was last loaded
inline u64 icFillAt[256];
inline u32 icFillPc[256];
inline u32 icFillPhys[256];
inline void icacheFill(u32 index, u32 pc, u32 physAddr)
{
	index &= 0xff;
	icFillAt[index] = instrCount;
	icFillPc[index] = pc;
	icFillPhys[index] = physAddr & ~0x1f;
	if ((physAddr & 0x1fffffff) < 0x0c000800 && (physAddr & 0x1fffffff) >= 0x0c000000 && instrCount > 1800000000u
			&& limit(0xc5000, 64))
		WARN_LOG(SH4, "ICFILL line %02x <- %08x at pc %08x instr %llu", index, physAddr & ~0x1f, pc,
				(unsigned long long)instrCount);
}
inline void dumpLowIcache(const char *why)
{
	if (icacheAddrArray == nullptr)
		return;
	for (u32 i = 0; i < 256; i++)
	{
		u32 v = icacheAddrArray(i << 5);
		u32 phys = ((v >> 10) << 10) | ((i << 5) & 0x3ff);
		if ((v & 1) && phys < 0x0c000800 && phys >= 0x0c000000)
		{
			const u32 *d = (const u32 *)icacheLineData(i);
			const u32 *r = (const u32 *)GetMemPtr(phys, 32);
			int diff = 0;
			for (int k = 0; k < 8; k++)
				diff += d[k] != r[k];
			WARN_LOG(SH4, "ICACHE %s: line %02x -> %08x valid, %d/8 words differ from ram; filled at instr %llu by pc %08x",
					why, i, phys, diff, (unsigned long long)icFillAt[i], icFillPc[i]);
			for (int k = 0; k < 8; k++)
			{
				const Writer& w = shadow()[((phys & 0xffffff) >> 2) + k];
				WARN_LOG(SH4, "   %08x icache %08x ram %08x %s  (last ram write %08x by pc %08x instr %llu)", phys + k * 4, d[k], r[k],
						d[k] == r[k] ? "same" : "DIFF", w.value, w.pc, (unsigned long long)w.n);
			}
		}
	}
}
inline void dumpLowOcache(const char *why)
{
	if (ocacheAddrArray == nullptr)
		return;
	for (u32 i = 0; i < 512; i++)
	{
		u32 v = ocacheAddrArray(i << 5);
		u32 phys = ((v >> 10) << 10) | ((i << 5) & 0x3ff);
		if ((v & 1) && phys < 0x0c000800 && phys >= 0x0c000000)
		{
			WARN_LOG(SH4, "OCACHE %s: line %03x -> %08x valid %d dirty %d", why, i, phys, v & 1, (v >> 1) & 1);
			const u32 *d = (const u32 *)ocacheLineData(i);
			const u32 *r = (const u32 *)GetMemPtr(phys, 32);
			for (int k = 0; k < 8; k++)
			{
				const Writer& w = shadow()[((phys & 0xffffff) >> 2) + k];
				WARN_LOG(SH4, "   %08x cache %08x ram %08x %s  (last cpu write %08x by pc %08x instr %llu)", phys + k * 4, d[k], r[k],
						d[k] == r[k] ? "same" : "DIFF", w.value, w.pc, (unsigned long long)w.n);
			}
		}
	}
}
inline void snapLow()
{
	const u32 *ram = (const u32 *)GetMemPtr(0x0c000000, 0x800);
	if (ram == nullptr)
		return;
	memcpy(lowSnap, ram, sizeof(lowSnap));
	lowSnapValid = true;
	dumpLowOcache("DMA done");
	dumpLowIcache("DMA done");
}
inline void compareLow(const char *why)
{
	if (!lowSnapValid)
		return;
	const u32 *ram = (const u32 *)GetMemPtr(0x0c000000, 0x800);
	int diffs = 0;
	for (u32 i = 0; i < 0x200; i++)
		if (ram[i] != lowSnap[i] && diffs++ < 64)
			reportShadow(0x0c000000 + i * 4, "lowdiff");
	WARN_LOG(SH4, "LOWRAM %s: %d words differ from the DMA-end snapshot", why, diffs);
	dumpLowOcache(why);
}
inline int armAfterStart;	// log this many interrupts after a large GD-ROM DMA starts
inline u64 dmaStartAt;
inline void dumpIntTable(const char *why)
{
	const u32 *ptr = (const u32 *)GetMemPtr(0x8c000794, 4);
	if (ptr == nullptr) return;
	const u32 table = *ptr;
	WARN_LOG(SH4, "-- dispatcher table (%s): pointer @8c000794 = %08x", why, table);
	const u32 *t = (const u32 *)GetMemPtr(table, 0x100);
	if (t == nullptr) return;
	for (int i = 0; i < 0x40; i += 8)
		WARN_LOG(SH4, "  [%02x] %08x %08x %08x %08x %08x %08x %08x %08x", i, t[i], t[i + 1], t[i + 2], t[i + 3],
				t[i + 4], t[i + 5], t[i + 6], t[i + 7]);
}
inline void interrupt(u32 code)
{
	if (armAfterStart > 0)
	{
		WARN_LOG(SH4, "interrupt %03x at pc %08x sr %08x, %llu instructions after large GD-DMA start", code,
				Sh4cntx.pc, Sh4cntx.sr.getFull(), (unsigned long long)(instrCount - dmaStartAt));
		if (armAfterStart == 8)
		{
			dumpIntTable("first interrupt after large GD-DMA start");
			dumpRing("first interrupt after large GD-DMA start");
		}
		armAfterStart--;
	}
	if (armAfterDma)
	{
		armAfterDma = false;
		WARN_LOG(SH4, "first interrupt %03x after large GD-DMA end (%llu instructions after it)", code,
				(unsigned long long)(instrCount - dmaDoneAt));
		dumpRing("first interrupt after large GD-DMA");
	}
	intRing[intPos] = { instrCount, code, Sh4cntx.pc, Sh4cntx.sr.getFull() };
	intPos = (intPos + 1) % IntSize;
}	// log this many upcoming control transfers live (set after interesting exceptions)

constexpr u32 WatchLo = 0x0c000100, WatchHi = 0x0c000800;	// low RAM: interrupt dispatcher, its table and pointer
struct WatchEv { u64 n; u32 pc, addr, value; u8 size; u8 cached; };
constexpr int WatchSize = 256;
inline WatchEv watchRing[WatchSize];
inline u32 watchPos;
// instruction ring snapshot taken at the most recent write to each watched byte
inline Entry watchSnap[WatchHi - WatchLo][64];
inline u64 watchSnapN[WatchHi - WatchLo];

struct AccessEv { u64 n; u32 pc, addr, value; u8 size, write, cached; };
constexpr int VramSize = 256;
inline AccessEv vramRing[VramSize];
inline u32 vramPos;
inline u64 vramCount[2][2];	// [write][cached]

struct TmuEv { u64 n, cycles; u32 pc, ch, value; };
constexpr int TmuSize = 256;
inline TmuEv tmuRing[TmuSize];
inline u32 tmuPos;
inline u64 tmuReads;

struct StaleEv { u64 n; u32 pc, phys; u16 cached, ram; };
constexpr int StaleSize = 256;
inline StaleEv staleRing[StaleSize];
inline u32 staleRingPos;
inline u64 staleCount;

// every data access made through the operand cache (cached or not), after address translation
inline void data(bool write, u32 physAddr, u32 value, int size, bool cached)
{
	const u32 pc = Sh4cntx.pc - 2;
	const u32 a = physAddr & 0x1fffffff;
	if (inRam(a) && (a & 0x00ffffff) < 0x44 && (a & 0x00ffffff) + size > 0x40
			&& (write || lowSnapValid))
		WARN_LOG(SH4, "TARGET40 CPU %s%d address %08x value %08x pc %08x %s instr %llu",
				write ? "W" : "R", size * 8, physAddr, value, pc, cached ? "cached" : "uncached",
				(unsigned long long)instrCount);
	// the word bleem's GD command-list engine reads as its end marker (VA 0 -> PA 0c400000 via UTLB[0])
	if ((a & 0x1ffffffc) == 0x0c400000 && instrCount > 2081000000ull && limit(0xc4000, 300))
		WARN_LOG(SH4, "WORD0 CPU %s%d %08x %s %08x pc %08x %s instr %llu", write ? "W" : "R", size * 8, physAddr,
				write ? "<-" : "->", value, pc, cached ? "cached" : "uncached", (unsigned long long)instrCount);
	// bleem's GD driver state pointers (8c031c8c..8c031c9f): who overwrites them after the 0x73 exchange
	if (write && (a & 0x1fffffe0) == 0x0c031c80 && instrCount > 2114000000ull && limit(0xc1c80, 300))
		WARN_LOG(SH4, "STATEPTR CPU W%d %08x <- %08x pc %08x %s instr %llu", size * 8, physAddr, value, pc,
				cached ? "cached" : "uncached", (unsigned long long)instrCount);
	if (write)
	{
		if (size >= 4)
			for (int i = 0; i < size; i += 4)
				shadowWrite(a + i, i == 0 ? value : 0, 4, pc);
		else
			shadowWrite(a, value, size, pc);
	}
	// reads of BIOS ROM / flash from outside the BIOS (checksums, serials, hardware fingerprints)
	// (after the BIOS boot; flash reads also count from BIOS code, e.g. the flashrom syscall)
	if (!write && a < 0x00220000 && instrCount > 5000000u && (a >= 0x00200000 || !inBios(pc)))
	{
		static std::unordered_map<u64, u32> romSeen;
		u32& n = romSeen[((u64)pc << 32) | (a >> 10)];
		if (n++ == 0 && limit(0xa2000, 400))
			WARN_LOG(SH4, "ROMREAD %s %08x (%d bytes) -> %08x  pc %08x  %s  instr %llu", a < 0x00200000 ? "bios" : "flash", a, size,
					value, pc, cached ? "cached" : "uncached", (unsigned long long)instrCount);
	}
	// every access to the dispatcher handler table after bleem restores its low-memory image
	if (a + size > 0x0c0001c8 && a < 0x0c0002c8 && instrCount > 1838000000u && limit(0xa1000 | write, 256))
		WARN_LOG(SH4, "TABLE %s%d %08x (entry %02x) %s %08x  pc %08x  %s  instr %llu", write ? "W" : "R", size * 8, a,
				(a - 0x0c0001c8) / 4, write ? "<-" : "->", value, pc, cached ? "cached" : "uncached",
				(unsigned long long)instrCount);
	if (write && a + size > WatchLo && a < WatchHi)
	{
		if (((a <= 0x0c00022c && a + size > 0x0c00022c) || (a <= 0x0c000794 && a + size > 0x0c000794)
				|| (a <= 0x0c000228 && a + size > 0x0c000228)) && limit(0xa0000 | (a & 0xfff), 64))
			WARN_LOG(SH4, "WATCH W%d %08x <- %08x  pc %08x  %s  instr %llu", size * 8, a, value, pc,
					cached ? "cached" : "uncached", (unsigned long long)instrCount);
		watchRing[watchPos] = { instrCount, pc, a, value, (u8)size, (u8)cached };
		watchPos = (watchPos + 1) % WatchSize;
		for (u32 b = std::max(a, WatchLo); b < std::min(a + size, WatchHi); b++)
		{
			for (int i = 0; i < 64; i++)
				watchSnap[b - WatchLo][i] = ring[(ringPos + RingSize - 64 + i) % RingSize];
			watchSnapN[b - WatchLo] = instrCount;
		}
	}
	// stray stores into VRAM from bleem's generated-code stream (8c14f000..8d120000): exact op and registers
	if (write && (a & 0x1c000000) == 0x04000000 && pc >= 0x8c14f000 && pc < 0x8d120000 && limit(0xd1000, 500))
	{
		const Entry& e = ring[(ringPos + RingSize - 1) % RingSize];
		char dis[128];
		OpDesc[e.op]->Disassemble(dis, e.pc, e.op);
		const Sh4Context& c = Sh4cntx;
		WARN_LOG(SH4, "STRAY W%d phys %08x <- %08x %s pc %08x op %04x %-26s sr %08x | %08x %08x %08x %08x %08x %08x %08x %08x "
				"%08x %08x %08x %08x %08x %08x %08x %08x instr %llu", size * 8, physAddr, value, cached ? "cached" : "uncached",
				pc, e.op, dis, c.sr.getFull(), c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.r[8], c.r[9],
				c.r[10], c.r[11], c.r[12], c.r[13], c.r[14], c.r[15], (unsigned long long)instrCount);
	}
	if (write && (a & 0x1c000000) == 0x04000000)
		for (int i = 0; i < size; i += 4)
		{
			VWriter& w = vshadow()[vramStorage(a + i) >> 2];
			w = { pc, i == 0 ? value : 0, instrCount, (u8)((a & 0x01000000) ? 5 : 4) };
		}
	if ((a & 0x1c000000) == 0x04000000 && !inBios(pc))
	{
		vramCount[write][cached]++;
		vramRing[vramPos] = { instrCount, pc, a, value, (u8)size, (u8)write, (u8)cached };
		vramPos = (vramPos + 1) % VramSize;
	}
}

inline void word0Block(const char *who, u32 dst, u32 size)
{
	const u32 a = dst & 0x1fffffff;
	if (a <= 0x0c400000 && a + size > 0x0c400000 && instrCount > 2081000000ull && limit(0xc4002, 100))
		WARN_LOG(SH4, "WORD0 %s block write %08x..%08x pc %08x instr %llu", who, dst, dst + size, Sh4cntx.pc - 2,
				(unsigned long long)instrCount);
}
inline void dmaWrite(const char *who, u32 dst, u32 size)
{
	const u32 a = dst & 0x1fffffff;
	if ((a & 0x1fffffff) <= 0x0c400000 && (a & 0x1fffffff) + size > 0x0c400000 && limit(0xc4001, 50))
		WARN_LOG(SH4, "WORD0 %s DMA write %08x..%08x instr %llu", who, dst, dst + size, (unsigned long long)instrCount);
	if ((a & 0x1fffffff) <= 0x0c031c8c && (a & 0x1fffffff) + size > 0x0c031c8c && limit(0xc1c81, 50))
		WARN_LOG(SH4, "STATEPTR %s DMA write %08x..%08x instr %llu", who, dst, dst + size, (unsigned long long)instrCount);
	for (u32 i = 0; i < size; i += 4)
		shadowWrite(a + i, 0, 4, 0xd0a0d0a0);
	if ((a & 0x1c000000) == 0x0c000000 && ((a & 0x00ffffff) < 0x800 || ((a + size) & 0x00ffffff) < ((a & 0x00ffffff)))
			&& limit(0xb0000, 64))
		WARN_LOG(SH4, "WATCH %s DMA write %08x..%08x (%x bytes)  instr %llu", who, dst, dst + size, size,
				(unsigned long long)instrCount);
}

inline void tmu(u32 ch, u32 value, u64 cycles)
{
	const u32 pc = Sh4cntx.pc - 2;
	if (inBios(pc))
		return;
	tmuReads++;
	tmuRing[tmuPos] = { instrCount, cycles, pc, ch, value };
	tmuPos = (tmuPos + 1) % TmuSize;
}

// instruction fetch that hit the I-cache: record it if RAM no longer holds the same opcode
inline void icacheHit(u32 pc, u32 physAddr, u16 cachedOp, u16 ramOp)
{
	if (cachedOp == ramOp)
		return;
	staleCount++;
	{
		// first stale fetch from a line: when was it filled, who wrote the ram behind it
		static u32 lastLine = ~0u;
		u32 line = physAddr & ~0x1f;
		if (line != lastLine && limit(0xc5001, 48))
		{
			u32 idx = (pc >> 5) & 0xff;
			WARN_LOG(SH4, "STALE LINE %08x (pc %08x): icache line %02x filled at instr %llu by pc %08x from %08x",
					line, pc, idx, (unsigned long long)icFillAt[idx], icFillPc[idx], icFillPhys[idx]);
			reportShadow(physAddr, "staleram");
		}
		lastLine = line;
	}
	staleRing[staleRingPos] = { instrCount, pc, physAddr, cachedOp, ramOp };
	staleRingPos = (staleRingPos + 1) % StaleSize;
	if (limit(0x50000, 64))
		WARN_LOG(SH4, "stale I-cache fetch pc %08x phys %08x cached %04x ram %04x (instr %llu)",
				pc, physAddr, cachedOp, ramOp, (unsigned long long)instrCount);
}

inline void dumpData(const char *why)
{
	WARN_LOG(SH4, "==== %s: data trace at instr %llu ====", why, (unsigned long long)instrCount);
	WARN_LOG(SH4, "-- writes to %08x-%08x (last %d)", WatchLo, WatchHi, WatchSize);
	for (int i = 0; i < WatchSize; i++)
	{
		const WatchEv& e = watchRing[(watchPos + i) % WatchSize];
		if (e.n == 0) continue;
		WARN_LOG(SH4, "  W%d %08x <- %08x  pc %08x  %s  instr %llu", e.size * 8, e.addr, e.value, e.pc,
				e.cached ? "cached" : "uncached", (unsigned long long)e.n);
	}
	WARN_LOG(SH4, "-- buffer contents (phys, via cache-coherent read)");
	for (u32 a = 0x0c0001c0; a < 0x0c000240; a += 16)
	{
		u8 *p = GetMemPtr(a | 0x80000000, 16);
		if (p == nullptr) break;
		WARN_LOG(SH4, "  %08x: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x", a,
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}
	for (u32 b : { 0x12cu, 0x694u })
	{
		if (watchSnapN[b] == 0) continue;
		WARN_LOG(SH4, "-- last 64 instructions before the last write to %08x (instr %llu)", WatchLo + b,
				(unsigned long long)watchSnapN[b]);
		for (const Entry& e : watchSnap[b])
		{
			if (e.pc == 0 && e.op == 0) continue;
			char dis[256];
			OpDesc[e.op]->Disassemble(dis, e.pc, e.op);
			WARN_LOG(SH4, "    %08x  %04x  T=%d  %s", e.pc, e.op, e.t, dis);
		}
	}
	WARN_LOG(SH4, "-- VRAM accesses: read uncached %llu cached %llu, write uncached %llu cached %llu (last %d below)",
			(unsigned long long)vramCount[0][0], (unsigned long long)vramCount[0][1],
			(unsigned long long)vramCount[1][0], (unsigned long long)vramCount[1][1], VramSize);
	for (int i = 0; i < VramSize; i++)
	{
		const AccessEv& e = vramRing[(vramPos + i) % VramSize];
		if (e.n == 0) continue;
		WARN_LOG(SH4, "  %s%d %08x %s %08x  pc %08x  %s  instr %llu", e.write ? "W" : "R", e.size * 8, e.addr,
				e.write ? "<-" : "->", e.value, e.pc, e.cached ? "cached" : "uncached", (unsigned long long)e.n);
	}
	WARN_LOG(SH4, "-- TMU TCNT reads: %llu (last %d below)", (unsigned long long)tmuReads, TmuSize);
	for (int i = 0; i < TmuSize; i++)
	{
		const TmuEv& e = tmuRing[(tmuPos + i) % TmuSize];
		if (e.n == 0) continue;
		WARN_LOG(SH4, "  TCNT%d -> %08x  pc %08x  cycle %llu  instr %llu", e.ch, e.value, e.pc,
				(unsigned long long)e.cycles, (unsigned long long)e.n);
	}
	WARN_LOG(SH4, "-- stale I-cache fetches: %llu (last %d below)", (unsigned long long)staleCount, StaleSize);
	for (int i = 0; i < StaleSize; i++)
	{
		const StaleEv& e = staleRing[(staleRingPos + i) % StaleSize];
		if (e.n == 0) continue;
		WARN_LOG(SH4, "  pc %08x phys %08x cached %04x ram %04x  instr %llu", e.pc, e.phys, e.cached, e.ram,
				(unsigned long long)e.n);
	}
	WARN_LOG(SH4, "==== end data trace ====");
}

// ---- stage snapshots and fetched-instruction capture (for offline disassembly) ----
// Output goes to $BLEEM_DUMP_DIR (default dumps/capture, relative to the working directory).
// Each snapshot NN_tag/ holds: ram.bin (16 MB physical RAM 0c000000, dirty O-cache lines NOT merged),
// icache.bin / ocache.bin (per line: u32 address-array word, then 32 data bytes), regs.txt.
inline const char *dumpDir()
{
	static std::string dir = [] {
		const char *e = getenv("BLEEM_DUMP_DIR");
		std::string d = e != nullptr ? e : "dumps/capture";
		std::string cmd = "mkdir -p '" + d + "'";
		if (system(cmd.c_str()) != 0)
			WARN_LOG(SH4, "SNAP cannot create %s", d.c_str());
		return d;
	}();
	return dir.c_str();
}
inline FILE *fetchLog;	// fetched instruction trace (after I-cache), open while fetchLeft > 0
inline u64 fetchLeft;
inline void snapshot(const char *tag, u32 pc, u32 lastPc)
{
	static int seq;
	if (seq >= 40)
		return;
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/%02d_%s", dumpDir(), seq++, tag);
	std::string cmd = std::string("mkdir -p '") + dir + "'";
	if (system(cmd.c_str()) != 0)
		return;
	std::string base(dir);
	if (FILE *f = fopen((base + "/ram.bin").c_str(), "wb"))
	{
		fwrite(GetMemPtr(0x0c000000, 0x01000000), 1, 0x01000000, f);
		fclose(f);
	}
	if (FILE *f = fopen((base + "/vram.bin").c_str(), "wb"))	// storage (64-bit path) layout
	{
		fwrite(&vram[0], 1, 0x00800000, f);
		fclose(f);
	}
	if (FILE *f = fopen((base + "/aica_ram.bin").c_str(), "wb"))
	{
		fwrite(&aica::aica_ram[0], 1, ARAM_SIZE, f);
		fclose(f);
	}
	for (int c = 0; c < 2; c++)
	{
		auto addrArray = c ? ocacheAddrArray : icacheAddrArray;
		auto lineData = c ? ocacheLineData : icacheLineData;
		if (addrArray == nullptr)
			continue;
		FILE *f = fopen((base + (c ? "/ocache.bin" : "/icache.bin")).c_str(), "wb");
		if (f == nullptr)
			continue;
		for (u32 i = 0; i < (c ? 512u : 256u); i++)
		{
			u32 v = addrArray(i << 5);
			fwrite(&v, 4, 1, f);
			fwrite(lineData(i), 1, 32, f);
		}
		fclose(f);
	}
	FILE *f = fopen((base + "/regs.txt").c_str(), "w");
	if (f == nullptr)
		return;
	const Sh4Context& c = Sh4cntx;
	fprintf(f, "tag %s\ninstr %llu\npc %08x\nlastpc %08x\n", tag, (unsigned long long)instrCount, pc, lastPc);
	for (int i = 0; i < 16; i++)
		fprintf(f, "r%d %08x\n", i, c.r[i]);
	for (int i = 0; i < 8; i++)
		fprintf(f, "r%d_bank %08x\n", i, c.r_bank[i]);
	fprintf(f, "sr %08x\ngbr %08x\nvbr %08x\nssr %08x\nspc %08x\nsgr %08x\ndbr %08x\npr %08x\nmach %08x\nmacl %08x\nfpscr %08x\nfpul %08x\n",
			c.sr.getFull(), c.gbr, c.vbr, c.ssr, c.spc, c.sgr, c.dbr, c.pr, c.mac.h, c.mac.l, c.fpscr.full, c.fpul);
	const u32 *fr = (const u32 *)c.fr, *xf = (const u32 *)c.xf;
	for (int i = 0; i < 16; i++)
		fprintf(f, "fr%d %08x\n", i, fr[i]);
	for (int i = 0; i < 16; i++)
		fprintf(f, "xf%d %08x\n", i, xf[i]);
	static const struct { const char *name; u32 addr; } regs[] = {
		{ "PTEH", 0xff000000 }, { "PTEL", 0xff000004 }, { "TTB", 0xff000008 }, { "TEA", 0xff00000c },
		{ "MMUCR", 0xff000010 }, { "CCR", 0xff00001c }, { "TRA", 0xff000020 }, { "EXPEVT", 0xff000024 },
		{ "INTEVT", 0xff000028 }, { "REG2C", 0xff00002c }, { "PTEA", 0xff000034 }, { "QACR0", 0xff000038 },
		{ "QACR1", 0xff00003c }, { "ISTNRM", 0xa05f6900 }, { "ISTEXT", 0xa05f6904 }, { "ISTERR", 0xa05f6908 },
		{ "IML2NRM", 0xa05f6910 }, { "IML4NRM", 0xa05f6920 }, { "IML6NRM", 0xa05f6930 },
		{ "IPRA", 0xffd00004 }, { "IPRB", 0xffd00008 }, { "IPRC", 0xffd0000c },
	};
	for (const auto& r : regs)
		fprintf(f, "%s %08x\n", r.name, addrspace::read32(r.addr));
	fclose(f);
	WARN_LOG(SH4, "SNAP %s written at instr %llu pc %08x (from %08x)", dir, (unsigned long long)instrCount, pc, lastPc);
}
// start writing every fetched instruction with its registers
inline void startFetchLog(const char *why, u64 count)
{
	if (fetchLog == nullptr)
	{
		std::string path = std::string(dumpDir()) + "/fetch_trace.txt";
		fetchLog = fopen(path.c_str(), "w");
		if (fetchLog == nullptr)
			return;
		fprintf(fetchLog, "# columns: instr pc op(fetched) ram-op(at pc's physical address; '----' if not RAM) "
				"disassembly | r0-r15 sr before execution\n");
	}
	fprintf(fetchLog, "# start: %s at instr %llu, %llu instructions\n", why, (unsigned long long)instrCount,
			(unsigned long long)count);
	fetchLeft = std::max(fetchLeft, count);
}
inline void fetchStep(u32 pc, u16 op)
{
	if (fetchLog == nullptr || fetchLeft == 0)
		return;
	char ramOp[8] = "----";
	if (inRam(pc) && (pc >> 29) != 7)
		if (const u16 *p = (const u16 *)GetMemPtr(0x0c000000 | (pc & 0x00fffffe), 2))
			snprintf(ramOp, sizeof(ramOp), "%04x", *p);
	char dis[256];
	OpDesc[op]->Disassemble(dis, pc, op);
	const Sh4Context& c = Sh4cntx;
	fprintf(fetchLog, "%llu %08x %04x %s %-28s | %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x sr %08x\n",
			(unsigned long long)instrCount, pc, op, ramOp, dis, c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7],
			c.r[8], c.r[9], c.r[10], c.r[11], c.r[12], c.r[13], c.r[14], c.r[15], c.sr.getFull());
	if (--fetchLeft == 0)
	{
		fprintf(fetchLog, "# stop at instr %llu\n", (unsigned long long)instrCount);
		fflush(fetchLog);
	}
}
// XF12..15 (the dispatcher's key source) from power-on: log every change with the instruction ring
inline void xfKeyWatch(u32 pc, u32 lastPc)
{
	static u32 last[4];
	const u32 *xf = (const u32 *)Sh4cntx.xf;
	if (memcmp(last, xf + 12, 16) == 0)
		return;
	if (limit(0xc6000, 64))
		WARN_LOG(SH4, "XFORIGIN at pc %08x (prev %08x) instr %llu: xf12-15 %08x %08x %08x %08x -> %08x %08x %08x %08x fpscr %08x",
				pc, lastPc, (unsigned long long)instrCount, last[0], last[1], last[2], last[3], xf[12], xf[13], xf[14], xf[15],
				Sh4cntx.fpscr.full);
	if ((xf[13] == 0x3fe66666 || xf[14] == 0x41840000) && last[1] != 0x3fe66666 && last[2] != 0x41840000 && limit(0xc6001, 2))
	{
		dumpRing("XF13/XF14 key value first appears");
		snapshot("xfkey_set", pc, lastPc);
	}
	memcpy(last, xf + 12, 16);
}

// Sound RAM table the post-DMA loader stage reads through a0900100, and the ARM7 run state
inline void aicaWatch(u32 pc)
{
	static u32 last[8];
	static bool lastArm;
	static bool init;
	const u32 *w = (const u32 *)&aica::aica_ram[0x100100];
	if (!init)
	{
		memcpy(last, w, sizeof(last));
		lastArm = aica::arm::Arm7Enabled;
		init = true;
	}
	if (aica::arm::Arm7Enabled != lastArm && limit(0xa7a00, 64))
		WARN_LOG(SH4, "ARM7WATCH arm7 %s at sh4 pc %08x instr %llu arm pc %08x",
				aica::arm::Arm7Enabled ? "RUNNING" : "held in reset", pc, (unsigned long long)instrCount,
				aica::arm::arm_Reg[15].I);
	// the ARM being released after the large GD-DMA load (bleem starts its ARM-side stage here)
	if (aica::arm::Arm7Enabled && !lastArm && dmaDoneAt != 0 && limit(0xc7008, 1))
		snapshot("arm_start", pc, 0);
	lastArm = aica::arm::Arm7Enabled;
	if (memcmp(last, w, sizeof(last)) != 0)
	{
		if (limit(0xa7a01, 64))
			WARN_LOG(SH4, "ARAMWATCH 00100100 at sh4 pc %08x instr %llu arm7 %d arm pc %08x: %08x %08x %08x %08x -> %08x %08x %08x %08x",
					pc, (unsigned long long)instrCount, aica::arm::Arm7Enabled, aica::arm::arm_Reg[15].I,
					last[0], last[1], last[2], last[3], w[0], w[1], w[2], w[3]);
		memcpy(last, w, sizeof(last));
	}
}

// Called for every instruction, before it executes
inline void step(u32 pc, u16 op)
{
	static u32 lastPc;
	static u64 count;
	static int resets;
	static std::unordered_map<u32, int> biosTargets;

	fetchStep(pc, op);
	xfKeyWatch(pc, lastPc);
	aicaWatch(pc);
	// stage transitions after the large GD-DMA: snapshot and capture the fetched instructions
	if (dmaDoneAt != 0)
	{
		if (pc == 0x8c000600 && lastPc != 0x8c0005fe && limit(0xc7000, 1))
		{
			snapshot("dispatcher_entry", pc, lastPc);
			startFetchLog("dispatcher entry 8c000600", 60000);
		}
		if (lastPc == 0xac000010 && pc == 0xac000012 && limit(0xc7001, 2))
			snapshot("decode_done", pc, lastPc);
		if (inBios(lastPc) && (pc & 0x1fffffff) == 0x0c000018 && limit(0xc7002, 3))
		{
			snapshot("bios_to_8c000018", pc, lastPc);
			startFetchLog("BIOS -> 8c000018", 20000);
		}
	}
	// first jumps into code running from VRAM (area 1) after the large GD-DMA
	if (dmaDoneAt != 0 && (pc & 0x1c000000) == 0x04000000 && (lastPc & 0x1c000000) != 0x04000000
			&& limit(0xc7006, 3))
	{
		WARN_LOG(SH4, "VRAMCODE enter %08x from %08x instr %llu sr %08x", pc, lastPc, (unsigned long long)instrCount,
				Sh4cntx.sr.getFull());
		for (u32 a = pc & ~0x1f; a < (pc & ~0x1f) + 0x40; a += 4)
			reportVShadow(a & 0x1fffffff, "code");
		// who filled the VRAM code and the data it copies (a5162a48, 0x38948 words in the first capture)
		std::map<std::pair<u32, u32>, std::pair<u32, std::pair<u64, u64>>> by;	// (path, pc) -> words, first/last instr
		for (u32 a = 0x05162000; a < 0x05162a48 + 0x38948 * 4; a += 4)
		{
			const VWriter& w = vshadow()[vramStorage(a) >> 2];
			auto& e = by[{ w.path, w.pc }];
			if (e.first++ == 0)
				e.second = { w.n, w.n };
			e.second.first = std::min(e.second.first, w.n);
			e.second.second = std::max(e.second.second, w.n);
		}
		for (auto& [k, e] : by)
			WARN_LOG(SH4, "VRAMCODE writer path %02x pc %08x words %u instr %llu..%llu", k.first, k.second, e.first,
					(unsigned long long)e.second.first, (unsigned long long)e.second.second);
		for (u32 a = 0x05162a48; a < 0x05162a48 + 0x38948 * 4; a += 0x8000)
			reportVShadow(a, "data");
		dumpRing("enter VRAM code");
		snapshot("vram_entry", pc, lastPc);
		startFetchLog("VRAM code entry", 200000);
	}
	// entry to the LZ decompressor (8c0daa9c jsr) whose output header yields the misaligned 0e8003f2:
	// who last wrote its compressed input (header 8c037800: size, offset; bit stream read backwards)
	if (lastPc == 0x8c0daa9e && pc == 0x8c0da880 && limit(0xc7005, 2))
	{
		const u32 *hdr = (const u32 *)GetMemPtr(0x0c037800, 8);
		const u32 end = 0x0c037800 + 8 + (hdr ? std::min(hdr[1], 0x100000u) : 0x20000);
		WARN_LOG(SH4, "DECOMP entry instr %llu r10 %08x r11 %08x  RAM hdr %08x %08x (O-cache may differ)",
				(unsigned long long)instrCount, Sh4cntx.r[10], Sh4cntx.r[11], hdr ? hdr[0] : 0, hdr ? hdr[1] : 0);
		std::map<u32, std::pair<u32, std::pair<u64, u64>>> by;	// pc -> words, first/last instr
		for (u32 a = 0x0c037800; a < end; a += 4)
		{
			const Writer& w = shadow()[(a & 0x00ffffff) >> 2];
			auto& e = by[w.pc];
			if (e.first++ == 0)
				e.second = { w.n, w.n };
			e.second.first = std::min(e.second.first, w.n);
			e.second.second = std::max(e.second.second, w.n);
		}
		for (auto& [wpc, e] : by)
			WARN_LOG(SH4, "DECOMP src writer pc %08x words %u instr %llu..%llu", wpc, e.first,
					(unsigned long long)e.second.first, (unsigned long long)e.second.second);
		for (u32 a = 0x0c037800; a < 0x0c037820; a += 4)
			reportShadow(a, "decomp src");
		reportShadow(end - 4, "decomp end");
		snapshot("decomp_entry", pc, lastPc);
	}
	// the same decompressor in bleemcast! GT (8c0de760, jsr at 8c0de97c) and Tekken 3 (8c0d31e0, jsr at 8c0d33fc,
	// found by matching GT's code bytes)
	if (((lastPc == 0x8c0de97e && pc == 0x8c0de760) || (lastPc == 0x8c0d33fe && pc == 0x8c0d31e0)) && limit(0xc700a, 2))
	{
		WARN_LOG(SH4, "DECOMP entry (GT/Tekken) instr %llu r10 %08x r11 %08x", (unsigned long long)instrCount,
				Sh4cntx.r[10], Sh4cntx.r[11]);
		snapshot("decomp_entry", pc, lastPc);
	}
	{
		static u32 snapVbr;
		if (Sh4cntx.vbr != snapVbr && limit(0xc7003, 12))
		{
			char tag[32];
			snprintf(tag, sizeof(tag), "vbr_%08x", Sh4cntx.vbr);
			snapshot(tag, pc, lastPc);
		}
		snapVbr = Sh4cntx.vbr;
	}

	if (inBios(pc) && !inBios(lastPc) && lastPc != 0)
	{
		int& n = biosTargets[pc & 0x1fffffff];
		if (++n <= 8)
			WARN_LOG(SH4, "jump into BIOS %08x from %08x (pr %08x r4 %08x r5 %08x r6 %08x r7 %08x) #%d",
					pc, lastPc, Sh4cntx.pc, Sh4cntx.r[4], Sh4cntx.r[5], Sh4cntx.r[6], Sh4cntx.r[7], n);
		if (n == 1 && biosTargets.size() <= 6)
			dumpRing("first jump into BIOS at this address");
	}
	if (!inBios(pc) && inBios(lastPc))
	{
		static std::unordered_map<u32, int> back;
		if (++back[pc] <= 4)
			WARN_LOG(SH4, "BIOS %08x -> game code %08x", lastPc, pc);
	}
	if ((pc & 0x1fffffff) == 0 && lastPc != 0 && ++resets <= 8)
	{
		WARN_LOG(SH4, "reset vector reached from %08x", lastPc);
		dumpRing("reset vector");
		// bleem enters the BIOS through a manual reset after its last loader stage: follow it,
		// and only stop if it keeps resetting (a real restart loop)
		static int lateResets;
		if (instrCount > 100000000u)
		{
			followJumps = 3000;
			WARN_LOG(SH4, "late reset #%d: sr %08x vbr %08x fpscr %08x r15 %08x expevt %08x", lateResets + 1,
					Sh4cntx.sr.getFull(), Sh4cntx.vbr, Sh4cntx.fpscr.full, Sh4cntx.r[15], addrspace::read32(0xff000024));
		}
		if (instrCount > 100000000u && ++lateResets >= 5)
		{
			WARN_LOG(SH4, "bleem reset detected 3 times, exiting");
			fflush(stdout);
			fflush(stderr);
			_exit(0);
		}
	}
	// who sets the XF12..15 key constants (and FPSCR bank/size/precision changes)
	if (instrCount > 1700000000u)
	{
		static u32 lastXf[4], lastFpscr;
		const u32 *xf = (const u32 *)Sh4cntx.xf;
		if ((memcmp(lastXf, xf + 12, 16) || ((Sh4cntx.fpscr.full ^ lastFpscr) & 0x00380000)) && limit(0xc3002, 200))
			WARN_LOG(SH4, "XFKEY at pc %08x (prev %08x): xf12-15 %08x %08x %08x %08x fpscr %08x -> %08x instr %llu", pc, lastPc,
					xf[12], xf[13], xf[14], xf[15], lastFpscr, Sh4cntx.fpscr.full, (unsigned long long)instrCount);
		memcpy(lastXf, xf + 12, 16);
		lastFpscr = Sh4cntx.fpscr.full;
	}
	// bleem's spin loop during the large GD-DMA: sample the DR14 counter
	if (pc == 0x8c00638c && instrCount > 1870000000u)
	{
		static u64 lastSample, visits;
		visits++;
		if (instrCount - lastSample > 4000000u && limit(0xc3004, 120))
		{
			const u32 *fr = (const u32 *)Sh4cntx.fr;
			u64 bits = ((u64)fr[14] << 32) | fr[15];
			double dr14;
			memcpy(&dr14, &bits, 8);
			WARN_LOG(SH4, "SPIN visit %llu at instr %llu: DR14 %.1f fpscr %08x sr %08x gdlend %08x", (unsigned long long)visits,
					(unsigned long long)instrCount, dr14, Sh4cntx.fpscr.full, Sh4cntx.sr.getFull(), addrspace::read32(0xa05f74f8));
			lastSample = instrCount;
		}
	}
	// every FPSCR change (enable/flag/cause bits too) shortly before the dispatcher
	if (instrCount > 2099000000u)
	{
		static u32 lastFull;
		if (Sh4cntx.fpscr.full != lastFull && limit(0xc3003, 300))
			WARN_LOG(SH4, "FPSCR %08x -> %08x at pc %08x (prev %08x) instr %llu", lastFull, Sh4cntx.fpscr.full, pc, lastPc,
					(unsigned long long)instrCount);
		lastFull = Sh4cntx.fpscr.full;
	}
	// FPU state at interrupt entry into the DMA'd dispatcher and at the decode loop
	if (((pc == 0x8c000600 && lastPc != 0x8c0005fe) || (pc == 0xac000000 && lastPc != 0xac000010))
			&& instrCount > 1800000000u && limit(0xc3001, 8))
	{
		WARN_LOG(SH4, "FPU @%08x (from %08x): fpscr %08x fpul %08x sr %08x ssr %08x spc %08x", pc, lastPc,
				Sh4cntx.fpscr.full, Sh4cntx.fpul, Sh4cntx.sr.getFull(), Sh4cntx.ssr, Sh4cntx.spc);
		dumpLowIcache(pc == 0x8c000600 ? "dispatcher entry" : "decode entry");
		{
			const u32 *fr = (const u32 *)Sh4cntx.fr, *xf = (const u32 *)Sh4cntx.xf;
			const float *frf = (const float *)Sh4cntx.fr;
			WARN_LOG(SH4, "  FR0-7  %08x %08x %08x %08x %08x %08x %08x %08x", fr[0], fr[1], fr[2], fr[3], fr[4], fr[5], fr[6], fr[7]);
			WARN_LOG(SH4, "  FR8-15 %08x %08x %08x %08x %08x %08x %08x %08x  (FR12 %f FR14 %f)", fr[8], fr[9], fr[10], fr[11],
					fr[12], fr[13], fr[14], fr[15], frf[12], frf[14]);
			WARN_LOG(SH4, "  XF0-15 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
					xf[0], xf[1], xf[2], xf[3], xf[4], xf[5], xf[6], xf[7], xf[8], xf[9], xf[10], xf[11], xf[12], xf[13], xf[14], xf[15]);
			static const u32 lits[] = { 0x8c006400, 0x8c006404, 0x8c006408, 0x8c00640c, 0x8c006410, 0x8c00652c, 0x8c006530,
					0x8c006538, 0x8c00653c };
			for (u32 a : lits)
				WARN_LOG(SH4, "  lit [%08x] = %08x (%f)", a, addrspace::read32(a), *(const float *)GetMemPtr(a, 4));
			WARN_LOG(SH4, "  stack [r15 %08x] = %08x %08x %08x %08x", Sh4cntx.r[15], addrspace::read32(Sh4cntx.r[15]),
					addrspace::read32(Sh4cntx.r[15] + 4), addrspace::read32(Sh4cntx.r[15] + 8), addrspace::read32(Sh4cntx.r[15] + 12));
		}
		for (int b = 0; b < 2; b++)
		{
			const u32 *f = b ? (const u32 *)Sh4cntx.xf : (const u32 *)Sh4cntx.fr;
			WARN_LOG(SH4, "  %s0-7  %08x %08x %08x %08x %08x %08x %08x %08x", b ? "XF" : "FR", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
			WARN_LOG(SH4, "  %s8-15 %08x %08x %08x %08x %08x %08x %08x %08x", b ? "XF" : "FR", f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
		}
	}
	// entry into the decode loop the large GD-DMA writes over the low vectors: dump how its keys were set up
	if (pc == 0xac000000 && lastPc != 0xac000010 && instrCount > 1800000000u && limit(0xc3000, 2))
	{
		WARN_LOG(SH4, "entering ac000000 from %08x  r3 %08x r4 %08x  instr %llu", lastPc, Sh4cntx.r[3], Sh4cntx.r[4],
				(unsigned long long)instrCount);
		compareLow("entering ac000000");
		dumpRing("entering ac000000");
	}
	// bleem check loop at 8c006120: dump on its pass (8c00618a) and fail (8c0061c4) exits
	instrCount++;
	{
		static u32 lastVbr;
		if (Sh4cntx.vbr != lastVbr)
		{
			if (limit(0xc0000, 32))
				WARN_LOG(SH4, "VBR %08x -> %08x at pc %08x (prev pc %08x) sr %08x instr %llu", lastVbr, Sh4cntx.vbr, pc,
						lastPc, Sh4cntx.sr.getFull(), (unsigned long long)instrCount);
			if (lastVbr == 0x8c010000 && Sh4cntx.vbr == 0x8c000000 && limit(0xc1000, 2))
				dumpRing("VBR switched from 8c010000 back to 8c000000");
			lastVbr = Sh4cntx.vbr;
		}
		// interrupt mask / block changes once bleem's loader is running its final stage
		static u32 lastImask;
		const u32 imask = Sh4cntx.sr.getFull() & 0x100000f0;
		if (imask != lastImask)
		{
			if (instrCount > 1830000000u && limit(0xc2000, 128))
				WARN_LOG(SH4, "SR imask/bl %08x -> %08x at pc %08x (prev pc %08x) vbr %08x instr %llu", lastImask, imask, pc,
						lastPc, Sh4cntx.vbr, (unsigned long long)instrCount);
			lastImask = imask;
		}
	}
	// unattended disc swap after bleem's CD_OPEN, like the GUI: open the lid, then insert BLEEM_SWAP
	if (cdOpenAt != 0 && getenv("BLEEM_SWAP") != nullptr)
	{
		static int swapStep;
		if (swapStep == 0 && instrCount >= cdOpenAt + 100000000)
		{
			swapStep = 1;
			WARN_LOG(SH4, "SWAP: opening lid at instr %llu", (unsigned long long)instrCount);
			gdr::openLid();
		}
		else if (swapStep == 1 && instrCount >= cdOpenAt + 400000000)
		{
			swapStep = 2;
			WARN_LOG(SH4, "SWAP: inserting %s at instr %llu", getenv("BLEEM_SWAP"), (unsigned long long)instrCount);
			gdr::insertDisk(getenv("BLEEM_SWAP"));
		}
	}
	// waiting for the GD job list to drain (8c0318f4) long after the swap
	if (cdOpenAt != 0 && instrCount > cdOpenAt + 600000000 && pc == 0x8c0318f6 && limit(0x5a000, 1))
	{
		WARN_LOG(SH4, "SWAP: job list still busy: [8c031c8c..9f] %08x %08x %08x %08x %08x", ReadMem32_nommu(0x8c031c8c),
				ReadMem32_nommu(0x8c031c90), ReadMem32_nommu(0x8c031c94), ReadMem32_nommu(0x8c031c98), ReadMem32_nommu(0x8c031c9c));
		dumpRing("job list busy after swap");
		snapshot("swap_stuck", pc, lastPc);
	}
	if (lastPc == 0x8c00614c && pc == 0x8c0061c4 && limit(0x40000, 3))
	{
		dumpRing("check loop early exit (8c00614c -> 8c0061c4)");
		dumpData("check loop early exit");
	}
	if (++count % 200000000 == 0)
	{
		WARN_LOG(SH4, "sample: %llu instructions, pc %08x sr %08x pr %08x", (unsigned long long)count, pc,
				Sh4cntx.sr.getFull(), Sh4cntx.pr);
		// stuck with exceptions/interrupts blocked: nothing can ever get it out, so stop the run
		static int blockedSamples;
		blockedSamples = Sh4cntx.sr.BL ? blockedSamples + 1 : 0;
		if (blockedSamples >= 3)
		{
			dumpRing("hung with SR.BL set");
			WARN_LOG(SH4, "-- last %d interrupts", IntSize);
			for (int i = 0; i < IntSize; i++)
			{
				const IntEv& e = intRing[(intPos + i) % IntSize];
				if (e.n == 0) continue;
				WARN_LOG(SH4, "  int %03x at pc %08x sr %08x instr %llu", e.code, e.pc, e.sr, (unsigned long long)e.n);
			}
			static const struct { const char *name; u32 addr; } regs[] = {
				{ "ISTNRM", 0xa05f6900 }, { "ISTEXT", 0xa05f6904 }, { "ISTERR", 0xa05f6908 },
				{ "IML2NRM", 0xa05f6910 }, { "IML2EXT", 0xa05f6914 }, { "IML2ERR", 0xa05f6918 },
				{ "IML4NRM", 0xa05f6920 }, { "IML4EXT", 0xa05f6924 }, { "IML4ERR", 0xa05f6928 },
				{ "IML6NRM", 0xa05f6930 }, { "IML6EXT", 0xa05f6934 }, { "IML6ERR", 0xa05f6938 },
				{ "GDST", 0xa05f7418 }, { "GDLEN", 0xa05f74f8 }, { "GDSTARD", 0xa05f74f4 },
				{ "INTEVT", 0xff000028 }, { "ICR", 0xffd00000 }, { "IPRA", 0xffd00004 }, { "IPRB", 0xffd00008 },
				{ "IPRC", 0xffd0000c }, { "TSTR", 0xffd80004 }, { "TCR0", 0xffd80010 }, { "TCR1", 0xffd8001c },
				{ "TCR2", 0xffd80028 }, { "CHCR2", 0xffa0002c }, { "DMAOR", 0xffa00040 },
			};
			for (const auto& r : regs)
				WARN_LOG(SH4, "  %-8s %08x = %08x", r.name, r.addr, addrspace::read32(r.addr));
			dumpData("hang");
			dumpIntTable("hang");
			for (u32 base : { 0x0c000000u, 0x0c000600u, 0x0cfff800u })
				for (u32 a = base; a < base + 0x40; a += 16)
				{
					const u8 *p = GetMemPtr(a | 0x80000000, 16);
					if (p == nullptr) break;
					WARN_LOG(SH4, "  ram %08x: %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x", a,
							p[1], p[0], p[3], p[2], p[5], p[4], p[7], p[6], p[9], p[8], p[11], p[10], p[13], p[12], p[15], p[14]);
				}
			WARN_LOG(SH4, "hang detected, exiting");
			fflush(stdout);
			fflush(stderr);
			_exit(0);
		}
	}

	if (pc != lastPc + 2 && pc != lastPc + 4)
	{
		// skip tight loops: same transfer as the previous one
		const Jump& prev = jumps[(jumpPos + JumpSize - 1) % JumpSize];
		if (prev.from != lastPc || prev.to != pc)
		{
			if (followJumps > 0)
			{
				followJumps--;
				WARN_LOG(SH4, "  follow: %08x -> %08x  sr %08x r0 %08x r1 %08x r4 %08x spc %08x ssr %08x", lastPc, pc,
						Sh4cntx.sr.getFull(), Sh4cntx.r[0], Sh4cntx.r[1], Sh4cntx.r[4], Sh4cntx.spc, Sh4cntx.ssr);
			}
			jumps[jumpPos] = { lastPc, pc, Sh4cntx.sr.getFull(), Sh4cntx.r[0] };
			jumpPos = (jumpPos + 1) % JumpSize;
		}
	}
	if (!inBios(pc))
	{
		ring[ringPos] = { pc, op, (u8)Sh4cntx.sr.T };
		ringPos = (ringPos + 1) % RingSize;
	}
	lastPc = pc;
}

}	// namespace sh4trace
#endif
