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
#include <algorithm>

static int UpdateSystemSlice(int slice);
#include "../sh4_trace.h"

Sh4ICache icache;
Sh4OCache ocache;
#if SH4_TRACE
static const bool ocacheTraceHook = (sh4trace::ocacheAddrArray = [](u32 a) { return ocache.ReadAddressArray(a); },
		sh4trace::ocacheLineData = [](u32 i) { return ocache.lineData(i); },
		sh4trace::icacheAddrArray = [](u32 a) { return icache.ReadAddressArray(a); },
		sh4trace::icacheLineData = [](u32 i) { return icache.lineData(i); }, true);
#endif
Sh4Interpreter *Sh4Interpreter::Instance;

void Sh4Interpreter::ExecuteOpcode(u16 op)
{
#if SH4_TRACE
	sh4trace::step(ctx->pc - 2, op);
#endif
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);
	// Count the instruction before running it so that a branch issues before its delay slot
	bool taken = false;
	if ((op & 0xf900) == 0x8900)	// bt, bf, bt/s, bf/s
		taken = ctx->sr.T == ((op & 0x0200) == 0 ? 1u : 0u);
	sh4cycles.executeCycles(op, taken);
	OpPtr[op](ctx, op);
}

u16 Sh4Interpreter::ReadNexOp()
{
	u32 addr = ctx->pc;
	if (!mmu_enabled() && (addr & 1))
		// address error
		throw SH4ThrownException(addr, Sh4Ex_AddressErrorRead);

	ctx->pc = addr + 2;

	return IReadMem16(addr);
}

void Sh4Interpreter::Run()
{
	Instance = this;
	ctx->restoreHostRoundingMode();

	try {
		do
		{
			try {
				do
				{
					u32 op = ReadNexOp();

					ExecuteOpcode(op);
				} while (ctx->cycle_counter > 0);
				ctx->cycle_counter += sh4SliceLength;
				UpdateSystemSlice(sh4SliceLength);
				// end the next slice when the next scheduled event is due
				const int next = std::clamp(Sh4cntx.sh4_sched_next, 1, SH4_TIMESLICE);
				ctx->cycle_counter += next - sh4SliceLength;
				sh4SliceLength = next;
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
	sh4SliceLength = SH4_TIMESLICE;

	INFO_LOG(INTERPRETER, "Sh4 Reset");
}

bool Sh4Interpreter::IsCpuRunning()
{
	return ctx->CpuRunning;
}

// SH7750 HW manual 5.6.3 (10): instructions that raise a slot illegal instruction exception
// when decoded in a delay slot. Undefined opcodes already throw Sh4Ex_IllegalInstr, which
// AdjustDelaySlotException converts. Privileged instructions in user mode aren't checked here.
static bool isIllegalInDelaySlot(u16 op)
{
	switch (op >> 12)
	{
	case 0x0:
		return (op & 0xf0ff) == 0x0023	// braf
			|| (op & 0xf0ff) == 0x0003	// bsrf
			|| op == 0x000b				// rts
			|| op == 0x002b;			// rte
	case 0x4:
		return (op & 0xf0ff) == 0x402b	// jmp
			|| (op & 0xf0ff) == 0x400b	// jsr
			|| (op & 0xf0ff) == 0x400e	// ldc Rm,SR
			|| (op & 0xf0ff) == 0x4007;	// ldc.l @Rm+,SR
	case 0x8:
		return (op & 0xf900) == 0x8900;	// bt, bf, bt/s, bf/s
	case 0x9:	// mov.w @(disp,PC),Rn
	case 0xa:	// bra
	case 0xb:	// bsr
	case 0xd:	// mov.l @(disp,PC),Rn
		return true;
	case 0xc:
		return (op & 0xff00) == 0xc300	// trapa
			|| (op & 0xff00) == 0xc700;	// mova
	default:
		return false;
	}
}

void Sh4Interpreter::ExecuteDelayslot()
{
	try {
		u32 op = ReadNexOp();

		if (isIllegalInDelaySlot(op))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_IllegalInstr);
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

// at the end of each interpreter slice
static int UpdateSystemSlice(int slice)
{
	Sh4cntx.sh4_sched_next -= slice;
	if (Sh4cntx.sh4_sched_next < 0)
		sh4_sched_tick(slice);
	if (Sh4cntx.interrupt_pend)
		return UpdateINTC();
	else
		return 0;
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
