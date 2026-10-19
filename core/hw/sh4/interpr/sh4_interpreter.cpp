/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "../sh4_cache.h"
#include "debug/gdb_server.h"
#include "../sh4_cycles.h"
#include "sh4opt_counts.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

Sh4ICache icache;
Sh4OCache ocache;
Sh4Interpreter *Sh4Interpreter::Instance;

namespace sh4opt
{
constexpr u32 MAIN_RAM_BASE = 0x0c000000, MAIN_RAM_BYTES = 0x01000000;

static bool configured, requested, active, haveLast, autoWindow;
static bool interpreting;			// Sh4Interpreter::Run is the running executor
static std::atomic<bool> switchExecutor;	// the CPU was stopped so the emulator can change executor
static std::string path;
static u32 startFrame, frameCount, vblanks, recordStart, recordings, lastPc;
static u64 instructions;
static std::atomic<bool> wanted;		// set by the GUI button (or the SH4OPT_FRAMES window)
static std::atomic<u32> recorded;		// vblanks in the current recording, for the GUI
static std::vector<u32> counts;		// per 16-bit slot of main RAM
static std::vector<u16> opcodes;	// opcode seen at the first execution
static std::vector<u8> changed;		// a different opcode ran later (code overwritten)
static std::unordered_map<u64, u32> edges;	// (from << 32 | to) for every non-sequential fetch

static void configure()
{
	configured = true;
	const char *file = std::getenv("SH4OPT_COUNTS");
	if (file == nullptr || *file == 0)
		return;
	requested = true;
	path = file;
	if (const char *frames = std::getenv("SH4OPT_FRAMES"))
		autoWindow = std::sscanf(frames, "%u:%u", &startFrame, &frameCount) == 2 && frameCount > 0;
}

// Main RAM addresses are folded onto their P1 (0x8c......) alias; others are kept as fetched.
static inline u32 canonical(u32 addr)
{
	u32 phys = addr & 0x1fffffff;
	return phys - MAIN_RAM_BASE < MAIN_RAM_BYTES ? 0x80000000 | phys : addr;
}

static inline void record(u32 addr, u16 op)
{
	instructions++;
	u32 offset = (addr & 0x1fffffff) - MAIN_RAM_BASE;
	if (offset < MAIN_RAM_BYTES)
	{
		u32 slot = offset >> 1;
		if (counts[slot] == 0)
			opcodes[slot] = op;
		else if (opcodes[slot] != op)
			changed[slot] = 1;
		if (counts[slot] != 0xffffffff)
			counts[slot]++;
	}
	if (haveLast && addr != lastPc + 2)
		edges[(u64)canonical(lastPc) << 32 | canonical(addr)]++;
	lastPc = addr;
	haveLast = true;
}

// Stop the running executor; Emulator::runInternal then picks the interpreter or the dynarec.
static void requestExecutorSwitch()
{
	switchExecutor = true;
	Sh4cntx.CpuRunning = false;
	Sh4cntx.cycle_counter = 0;
}

// The first recording goes to SH4OPT_COUNTS; later ones get -2, -3... before the extension.
static std::string outputPath()
{
	if (recordings == 1)
		return path;
	size_t dot = path.find_last_of('.');
	size_t slash = path.find_last_of('/');
	std::string suffix = "-" + std::to_string(recordings);
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return path + suffix;
	return path.substr(0, dot) + suffix + path.substr(dot);
}

static void write()
{
	std::string file = outputPath();
	FILE *f = std::fopen(file.c_str(), "w");
	if (f == nullptr)
	{
		ERROR_LOG(INTERPRETER, "sh4opt: cannot write %s", file.c_str());
		return;
	}
	std::fprintf(f, "sh4opt-counts 1\nvblanks %u %u\ninstructions %llu\n",
			recordStart, vblanks, (unsigned long long)instructions);
	for (u32 slot = 0; slot < counts.size(); slot++)
		if (counts[slot] != 0)
			std::fprintf(f, "i %08x %04x %u%s\n", 0x8c000000 + slot * 2, opcodes[slot], counts[slot],
					changed[slot] ? " changed" : "");
	std::vector<std::pair<u64, u32>> sorted(edges.begin(), edges.end());
	std::sort(sorted.begin(), sorted.end());
	for (const auto& [key, count] : sorted)
		std::fprintf(f, "e %08x %08x %u\n", (u32)(key >> 32), (u32)key, count);
	std::fclose(f);
	NOTICE_LOG(INTERPRETER, "sh4opt: wrote %s (%u vblanks, %llu instructions)", file.c_str(),
			vblanks - recordStart, (unsigned long long)instructions);
}
}	// namespace sh4opt

bool sh4opt_counting_requested()
{
	if (!sh4opt::configured)
		sh4opt::configure();
	return sh4opt::requested;
}

void sh4opt_toggle_recording()
{
	sh4opt::wanted = !sh4opt::wanted;
}

bool sh4opt_is_recording()
{
	return sh4opt::wanted;
}

unsigned sh4opt_recorded_vblanks()
{
	return sh4opt::recorded;
}

bool sh4opt_needs_interpreter()
{
	return sh4opt_counting_requested() && (sh4opt::wanted || sh4opt::active);
}

bool sh4opt_take_executor_switch()
{
	return sh4opt::switchExecutor.exchange(false);
}

// Runs on the emulation thread, so starting and stopping never race the interpreter.
void sh4opt_vblank()
{
	using namespace sh4opt;
	if (!sh4opt_counting_requested())
		return;
	vblanks++;
	if (autoWindow && vblanks == startFrame)
		wanted = true;
	bool windowEnd = autoWindow && active && vblanks == recordStart + frameCount;
	if (windowEnd)
		wanted = false;
	if (!active && wanted)
	{
		if (!interpreting)
		{
			// Counting starts at the first vblank in the interpreter.
			requestExecutorSwitch();
			return;
		}
		counts.assign(MAIN_RAM_BYTES / 2, 0);
		opcodes.assign(MAIN_RAM_BYTES / 2, 0);
		changed.assign(MAIN_RAM_BYTES / 2, 0);
		edges.clear();
		instructions = 0;
		recordStart = vblanks;
		recorded = 0;
		active = true;
		haveLast = false;
		NOTICE_LOG(INTERPRETER, "sh4opt: recording started at vblank %u", vblanks);
	}
	else if (active && !wanted)
	{
		active = false;
		recordings++;
		write();
		const char *quit = std::getenv("SH4OPT_EXIT");
		if (windowEnd && quit != nullptr && *quit == '1')
			std::_Exit(0);
		requestExecutorSwitch();	// back to the configured executor
	}
	else if (active)
		recorded = vblanks - recordStart;
}

void Sh4Interpreter::ExecuteOpcode(u16 op)
{
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);
	OpPtr[op](ctx, op);
	sh4cycles.executeCycles(op);
}

u16 Sh4Interpreter::ReadNexOp()
{
	u32 addr = ctx->pc;
	if (!mmu_enabled() && (addr & 1))
		// address error
		throw SH4ThrownException(addr, Sh4Ex_AddressErrorRead);

	ctx->pc = addr + 2;

	u16 op = IReadMem16(addr);
	if (sh4opt::active)
		sh4opt::record(addr, op);
	return op;
}

void Sh4Interpreter::Run()
{
	Instance = this;
	ctx->restoreHostRoundingMode();
	sh4opt::interpreting = true;

	try {
		do
		{
			try {
				do
				{
					u32 op = ReadNexOp();

					ExecuteOpcode(op);
				} while (ctx->cycle_counter > 0);
				ctx->cycle_counter += SH4_TIMESLICE;
				UpdateSystem_INTC();
			} catch (const SH4ThrownException& ex) {
				Do_Exception(ex.epc, ex.expEvn);
				// an exception requires the instruction pipeline to drain, so approx 5 cycles
				sh4cycles.addCycles(5 * CPU_RATIO);
			}
		} while (ctx->CpuRunning);
	} catch (const debugger::Stop&) {
	}

	ctx->CpuRunning = false;
	Instance = nullptr;
	sh4opt::interpreting = false;
}

void Sh4Interpreter::Start()
{
	ctx->CpuRunning = true;
}

void Sh4Interpreter::Stop()
{
	ctx->CpuRunning = false;
	ctx->cycle_counter = 0;
}

void Sh4Interpreter::Step()
{
	Instance = this;

	ctx->restoreHostRoundingMode();
	try {
		u32 op = ReadNexOp();
		ExecuteOpcode(op);
	} catch (const SH4ThrownException& ex) {
		Do_Exception(ex.epc, ex.expEvn);
		// an exception requires the instruction pipeline to drain, so approx 5 cycles
		sh4cycles.addCycles(5 * CPU_RATIO);
	} catch (const debugger::Stop&) {
	}
	Instance = nullptr;
}

void Sh4Interpreter::Reset(bool hard)
{
	verify(!ctx->CpuRunning);

	if (hard)
	{
		int schedNext = ctx->sh4_sched_next;
		memset(ctx, 0, sizeof(*ctx));
		ctx->sh4_sched_next = schedNext;
	}
	ctx->pc = 0xA0000000;

	memset(ctx->r, 0, sizeof(ctx->r));
	memset(ctx->r_bank, 0, sizeof(ctx->r_bank));

	ctx->gbr = ctx->ssr = ctx->spc = ctx->sgr = ctx->dbr = ctx->vbr = 0;
	ctx->mac.full = ctx->pr = ctx->fpul = 0;

	ctx->sr.setFull(0x700000F0);
	ctx->old_sr.status = ctx->sr.status;
	UpdateSR();

	ctx->fpscr.full = 0x00040001;
	ctx->old_fpscr = ctx->fpscr;

	icache.Reset(hard);
	ocache.Reset(hard);
	sh4cycles.reset();
	ctx->cycle_counter = SH4_TIMESLICE;

	INFO_LOG(INTERPRETER, "Sh4 Reset");
}

bool Sh4Interpreter::IsCpuRunning()
{
	return ctx->CpuRunning;
}

//TODO : Check for valid delayslot instruction
void Sh4Interpreter::ExecuteDelayslot()
{
	try {
		u32 op = ReadNexOp();

		ExecuteOpcode(op);
	} catch (SH4ThrownException& ex) {
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

void Sh4Interpreter::ExecuteDelayslot_RTE()
{
	try {
		// In an RTE delay slot, status register (SR) bits are referenced as follows.
		// In instruction access, the MD bit is used before modification, and in data access,
		// the MD bit is accessed after modification.
		// The other bits—S, T, M, Q, FD, BL, and RB—after modification are used for delay slot
		// instruction execution. The STC and STC.L SR instructions access all SR bits after modification.
		u32 op = ReadNexOp();
		// Now restore all SR bits
		ctx->sr.setFull(ctx->ssr);
		// And execute
		ExecuteOpcode(op);
	} catch (const SH4ThrownException&) {
		throw FlycastException("Fatal: SH4 exception in RTE delay slot");
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

// every SH4_TIMESLICE cycles
int UpdateSystem_INTC()
{
	Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next < 0)
		sh4_sched_tick(SH4_TIMESLICE);
	if (Sh4cntx.interrupt_pend)
		return UpdateINTC();
	else
		return 0;
}

void Sh4Interpreter::Init()
{
	ctx = &p_sh4rcb->cntx;
	memset(ctx, 0, sizeof(*ctx));
	sh4cycles.init(ctx);
	icache.init(ctx);
	ocache.init(ctx);
}

void Sh4Interpreter::Term()
{
	Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term");
}

Sh4Executor *Get_Sh4Interpreter()
{
	return new Sh4Interpreter();
}
