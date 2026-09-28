#include "arm7.h"
#include "arm_mem.h"
#include "arm7_rec.h"

namespace aica::arm
{

#define CPUReadMemoryQuick(addr) (*(u32*)&aica_ram[(addr) & ARAM_MASK])
#define CPUReadByte timedRead<u8>
#define CPUReadMemory timedRead<u32>
#define CPUReadHalfWord timedRead<u16>
#define CPUReadHalfWordSigned(addr) ((s16)timedRead<u16>(addr))

#define CPUWriteMemory timedWrite<u32>
#define CPUWriteHalfWord timedWrite<u16>
#define CPUWriteByte timedWrite<u8>

#define reg arm_Reg
#define armNextPC reg[R15_ARM_NEXT].I

#define CPUUpdateTicksAccessSeq32(a) 1
#define CPUUpdateTicksAccess32(a) 1
#define CPUUpdateTicksAccess16(a) 1

alignas(8) reg_pair arm_Reg[RN_ARM_REG_COUNT];

//
// AICA ARM7DI bus timing, measured on a retail console (wren7-rtl model/TIMING.md sections 2-4, the "fixed
// costs with the 4-MCLK grid, no slots" approximation, within 1% with the DSP idle).
// Time is in MCLK (22.5792 MHz, 512 per sample). Every memory cycle, N or S, fetch or data, wave RAM or AICA
// register, starts on a 4-MCLK DSP step boundary and lasts 8 MCLK. Internal cycles last 1 MCLK. The ARM
// interrupt controller's L and M registers take 1 MCLK off the grid, and SWP's locked write takes 4.
// Not modelled yet: wave RAM slots taken by DSP MRD/MWT, playing channels, the fixed pair at steps 109/111,
// SH4 wave RAM traffic, and TEMP/EFREG port stalls.
//
static u64 armMclk;
static int swpPhase;	// 1: SWP read pending, 2: its locked write next

static inline void memCycle(u32 addr)
{
	addr &= 0x00FFFFFC;
	if (addr == 0x802D00 || addr == 0x802D04)
	{
		armMclk += 1;
		return;
	}
	u32 len = 8;
	if (swpPhase == 2)
		len = 4;
	else if (swpPhase == 1)
		swpPhase = 2;
	armMclk = ((armMclk + 3) & ~(u64)3) + len;
}

template<typename T>
static inline T timedRead(u32 addr)
{
	memCycle(addr);
	return readMem<T>(addr);
}

template<typename T>
static inline void timedWrite(u32 addr, T data)
{
	memCycle(addr);
	writeMem<T>(addr, data);
}

static void CPUSwap(u32 *a, u32 *b)
{
	u32 c = *b;
	*b = *a;
	*a = c;
}

#define N_FLAG (reg[RN_PSR_FLAGS].FLG.N)
#define Z_FLAG (reg[RN_PSR_FLAGS].FLG.Z)
#define C_FLAG (reg[RN_PSR_FLAGS].FLG.C)
#define V_FLAG (reg[RN_PSR_FLAGS].FLG.V)

bool armIrqEnable;
bool armFiqEnable;
int armMode;

bool Arm7Enabled = false;

u8 cpuBitsSet[256];

static void CPUSwitchMode(int mode, bool saveState);
static void CPUUpdateFlags();
static void CPUSoftwareInterrupt(int comment);
static void CPUUndefinedException();

//
// ARM7 interpreter
//
int arm7ClockTicks;

#if FEAT_AREC == DYNAREC_NONE

static bool condPassed(u32 cond)
{
	switch (cond)
	{
	case 0x0: return Z_FLAG;
	case 0x1: return !Z_FLAG;
	case 0x2: return C_FLAG;
	case 0x3: return !C_FLAG;
	case 0x4: return N_FLAG;
	case 0x5: return !N_FLAG;
	case 0x6: return V_FLAG;
	case 0x7: return !V_FLAG;
	case 0x8: return C_FLAG && !Z_FLAG;
	case 0x9: return !C_FLAG || Z_FLAG;
	case 0xA: return N_FLAG == V_FLAG;
	case 0xB: return N_FLAG != V_FLAG;
	case 0xC: return !Z_FLAG && N_FLAG == V_FLAG;
	case 0xD: return Z_FLAG || N_FLAG != V_FLAG;
	case 0xE: return true;
	default: return false;
	}
}

// Internal (I) cycles of an executed instruction (data sheet DDI0027D chapter 9). Sets swpPhase for SWP.
static u32 internalCycles(u32 op)
{
	if ((op & 0x0FC000F0) == 0x00000090)	// MUL, MLA: m cycles of the 2-bit Booth multiplier, unsigned early termination
	{
		u32 rs = reg[(op >> 8) & 15].I;
		u32 m = 1;
		while (m < 16 && (rs >> (2 * m - 1)) != 0)
			m++;
		return m;
	}
	if ((op & 0x0FB00FF0) == 0x01000090)	// SWP, SWPB
	{
		swpPhase = 1;
		return 1;
	}
	if ((op & 0x0E000090) == 0x00000010)	// data processing, shift amount in Rs
		return 1;
	if ((op & 0x0E000010) == 0x06000010)	// undefined
		return 1;
	if ((op & 0x0C100000) == 0x04100000)	// LDR, LDRB
		return 1;
	if ((op & 0x0E100000) == 0x08100000)	// LDM
		return 1;
	if ((op & 0x0C000000) == 0x0C000000 && (op & 0x0F000000) != 0x0F000000)	// coprocessor: undefined, no coprocessor
		return 1;
	return 0;
}

static void runInterpreter(u32 CycleCount)
{
	if (!Arm7Enabled)
		return;

	arm7ClockTicks -= CycleCount;
	while (arm7ClockTicks < 0)
	{
		const int ticks = arm7ClockTicks;
		const u64 t0 = armMclk;
		if (reg[INTR_PEND].I)
		{
			CPUFiq();
			// exception entry: S, N, S
			memCycle(armNextPC);
			memCycle(armNextPC);
			memCycle(armNextPC);
		}

		const u32 pc = armNextPC;
		reg[15].I = armNextPC + 8;
		memCycle(pc);		// this instruction's S cycle
		const u32 op = CPUReadMemoryQuick(pc);
		const u32 icycles = condPassed(op >> 28) ? internalCycles(op) : 0;

		{
			int& clockTicks = arm7ClockTicks;
			#include "arm-new.h"
		}
		swpPhase = 0;
		armMclk += icycles;
		if (armNextPC != pc + 4)
		{
			// pipeline refill: N, S
			memCycle(armNextPC);
			memCycle(armNextPC + 4);
		}
		arm7ClockTicks = ticks + (int)(armMclk - t0);
	}
}

void avoidRaceCondition()
{
	arm7ClockTicks = std::min(arm7ClockTicks, -50);
}

void run(u32 samples)
{
	for (u32 i = 0; i < samples; i++)
	{
		runInterpreter(ARM_CYCLES_PER_SAMPLE);
		timeStep();
	}
}
#endif

void staticInit()
{
	for (std::size_t i = 0; i < std::size(cpuBitsSet); i++)
	{
		int count = 0;
		for (int j = 0; j < 8; j++)
			if (i & (1 << j))
				count++;

		cpuBitsSet[i] = count;
	}
}
OnLoad _staticInit(staticInit);

void init()
{
#if FEAT_AREC != DYNAREC_NONE
	recompiler::init();
#endif
	reset();
}

void term()
{
#if FEAT_AREC != DYNAREC_NONE
	recompiler::term();
#endif
}

static void CPUSwitchMode(int mode, bool saveState)
{
	CPUUpdateCPSR();

	switch(armMode)
	{
	case 0x10:
	case 0x1F:
		reg[R13_USR].I = reg[13].I;
		reg[R14_USR].I = reg[14].I;
		reg[RN_SPSR].I = reg[RN_CPSR].I;
		break;
	case 0x11:
		CPUSwap(&reg[R8_FIQ].I, &reg[8].I);
		CPUSwap(&reg[R9_FIQ].I, &reg[9].I);
		CPUSwap(&reg[R10_FIQ].I, &reg[10].I);
		CPUSwap(&reg[R11_FIQ].I, &reg[11].I);
		CPUSwap(&reg[R12_FIQ].I, &reg[12].I);
		reg[R13_FIQ].I = reg[13].I;
		reg[R14_FIQ].I = reg[14].I;
		reg[SPSR_FIQ].I = reg[RN_SPSR].I;
		break;
	case 0x12:
		reg[R13_IRQ].I  = reg[13].I;
		reg[R14_IRQ].I  = reg[14].I;
		reg[SPSR_IRQ].I =  reg[RN_SPSR].I;
		break;
	case 0x13:
		reg[R13_SVC].I  = reg[13].I;
		reg[R14_SVC].I  = reg[14].I;
		reg[SPSR_SVC].I =  reg[RN_SPSR].I;
		break;
	case 0x17:
		reg[R13_ABT].I  = reg[13].I;
		reg[R14_ABT].I  = reg[14].I;
		reg[SPSR_ABT].I =  reg[RN_SPSR].I;
		break;
	case 0x1b:
		reg[R13_UND].I  = reg[13].I;
		reg[R14_UND].I  = reg[14].I;
		reg[SPSR_UND].I =  reg[RN_SPSR].I;
		break;
	}

	u32 CPSR = reg[RN_CPSR].I;
	u32 SPSR = reg[RN_SPSR].I;

	switch(mode)
	{
	case 0x10:
	case 0x1F:
		reg[13].I = reg[R13_USR].I;
		reg[14].I = reg[R14_USR].I;
		reg[RN_CPSR].I = SPSR;
		break;
	case 0x11:
		CPUSwap(&reg[8].I, &reg[R8_FIQ].I);
		CPUSwap(&reg[9].I, &reg[R9_FIQ].I);
		CPUSwap(&reg[10].I, &reg[R10_FIQ].I);
		CPUSwap(&reg[11].I, &reg[R11_FIQ].I);
		CPUSwap(&reg[12].I, &reg[R12_FIQ].I);
		reg[13].I = reg[R13_FIQ].I;
		reg[14].I = reg[R14_FIQ].I;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_FIQ].I;
		break;
	case 0x12:
		reg[13].I = reg[R13_IRQ].I;
		reg[14].I = reg[R14_IRQ].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_IRQ].I;
		break;
	case 0x13:
		reg[13].I = reg[R13_SVC].I;
		reg[14].I = reg[R14_SVC].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_SVC].I;
		break;
	case 0x17:
		reg[13].I = reg[R13_ABT].I;
		reg[14].I = reg[R14_ABT].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_ABT].I;
		break;
	case 0x1b:
		reg[13].I = reg[R13_UND].I;
		reg[14].I = reg[R14_UND].I;
		reg[RN_CPSR].I = SPSR;
		if(saveState)
			reg[RN_SPSR].I = CPSR;
		else
			reg[RN_SPSR].I = reg[SPSR_UND].I;
		break;
	default:
		// An illegal mode causes the processor to enter an unrecoverable state
		ERROR_LOG(AICA_ARM, "Unsupported ARM mode %02x", mode);
		Arm7Enabled = false;
		break;
	}
	armMode = mode;
	CPUUpdateFlags();
	CPUUpdateCPSR();
}

void CPUUpdateCPSR()
{
	reg_pair CPSR;

	CPSR.I = reg[RN_CPSR].I & 0x40;

	CPSR.PSR.NZCV = reg[RN_PSR_FLAGS].FLG.NZCV;

	if (!armFiqEnable)
		CPSR.I |= 0x40;
	if(!armIrqEnable)
		CPSR.I |= 0x80;

	CPSR.PSR.M = armMode;
	
	reg[RN_CPSR].I = CPSR.I;
}

static void CPUUpdateFlags()
{
	u32 CPSR = reg[RN_CPSR].I;

	reg[RN_PSR_FLAGS].FLG.NZCV = reg[RN_CPSR].PSR.NZCV;

	armIrqEnable = (CPSR & 0x80) ? false : true;
	armFiqEnable = (CPSR & 0x40) ? false : true;
	update_armintc();
}

static void CPUSoftwareInterrupt(int comment)
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x13, true);
	reg[14].I = PC;
	
	armIrqEnable = false;
	armNextPC = 0x08;
}

static void CPUUndefinedException()
{
	WARN_LOG(AICA_ARM, "arm7: CPUUndefinedException(). SOMETHING WENT WRONG");
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x1b, true);
	reg[14].I = PC;
	armIrqEnable = false;
	armNextPC = 0x04;
}

void reset()
{
	INFO_LOG(AICA_ARM, "AICA ARM Reset");
#if FEAT_AREC != DYNAREC_NONE
	recompiler::flush();
#endif
	aica_interr = false;
	aica_reg_L = 0;
	e68k_out = false;
	e68k_reg_L = 0;
	e68k_reg_M = 0;

	Arm7Enabled = false;
	// clean registers
	memset(&arm_Reg[0], 0, sizeof(arm_Reg));

	armMode = 0x13;

	reg[13].I = 0x03007F00;
	reg[15].I = 0x0000000;
	reg[RN_CPSR].I = 0x00000000;
	reg[R13_IRQ].I = 0x03007FA0;
	reg[R13_SVC].I = 0x03007FE0;
	armIrqEnable = true;      
	armFiqEnable = false;
	update_armintc();

	C_FLAG = V_FLAG = N_FLAG = Z_FLAG = false;

	// disable FIQ
	reg[RN_CPSR].I |= 0x40;

	CPUUpdateCPSR();

	armNextPC = reg[15].I;
	reg[15].I += 4;
}

void CPUFiq()
{
	u32 PC = reg[R15_ARM_NEXT].I+4;
	CPUSwitchMode(0x11, true);
	reg[14].I = PC;
	armIrqEnable = false;
	armFiqEnable = false;
	update_armintc();

	armNextPC = 0x1c;
}

/*
	--Seems like aica has 3 interrupt controllers actualy (damn lazy sega ..)
	The "normal" one (the one that exists on scsp) , one to emulate the 68k intc , and , 
	of course , the arm7 one

	The output of the sci* bits is input to the e68k , and the output of e68k is inputed into the FIQ
	pin on arm7
*/


void enable(bool enabled)
{
	if(!Arm7Enabled && enabled)
		reset();
	
	Arm7Enabled=enabled;
}

void update_armintc()
{
	reg[INTR_PEND].I=e68k_out && armFiqEnable;
}

#if FEAT_AREC != DYNAREC_NONE
//
// Used by ARM7 Recompiler
//
namespace recompiler {

//Emulate a single arm op, passed in opcode

void DYNACALL interpret(u32 opcode)
{
	u32 clockTicks = 0;

#define NO_OPCODE_READ
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "arm-new.h"
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif
#undef NO_OPCODE_READ

	reg[CYCL_CNT].I -= clockTicks;
}

template<u32 Pd>
void DYNACALL MSR_do(u32 v, u32 mask)
{
	if (Pd)
	{
		if(armMode > 0x10 && armMode < 0x1f) /* !=0x10 ?*/
		{
			u32 newValue = reg[RN_SPSR].I;
			if (mask & 1)
				newValue = (newValue & 0xFFFFFF00) | (v & 0x000000FF);
			if (mask & 2)
				newValue = (newValue & 0xFFFF00FF) | (v & 0x0000FF00);
			if (mask & 4)
				newValue = (newValue & 0xFF00FFFF) | (v & 0x00FF0000);
			if (mask & 8)
				newValue = (newValue & 0x00FFFFFF) | (v & 0xFF000000);
			reg[RN_SPSR].I = newValue;
		}
	}
	else
	{
		CPUUpdateCPSR();
	
		u32 newValue = reg[RN_CPSR].I;
		if(armMode > 0x10)
		{
			if (mask & 1)
				newValue = (newValue & 0xFFFFFF00) | (v & 0x000000FF);
			if (mask & 2)
				newValue = (newValue & 0xFFFF00FF) | (v & 0x0000FF00);
			if (mask & 4)
				newValue = (newValue & 0xFF00FFFF) | (v & 0x00FF0000);
		}
		if (mask & 8)
			newValue = (newValue & 0x00FFFFFF) | (v & 0xFF000000);
		newValue |= 0x10;
		if(armMode > 0x10)
		{
			CPUSwitchMode(newValue & 0x1f, false);
		}
		reg[RN_CPSR].I = newValue;
		CPUUpdateFlags();
	}
}
template void DYNACALL MSR_do<0>(u32 v, u32 mask);
template void DYNACALL MSR_do<1>(u32 v, u32 mask);

} // namespace recompiler
#endif	// FEAT_AREC != DYNAREC_NONE

} // namespace aica::arm
