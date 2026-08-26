#include <algorithm>
#include <climits>
#include "pipesim.h"
#include "pipesim_optable.h"
#include <vector>
#include <cstring>
#include <cstdlib>

namespace pipesim
{

// ---------------------------------------------------------------------------
// Pipeline stages, figure 8.1.
// ---------------------------------------------------------------------------
enum Stage : u8
{
	ST_I, ST_D, ST_EX, ST_SX, ST_F0, ST_F1, ST_F2, ST_F3,
	ST_NA, ST_MA, ST_S, ST_FS,
	ST_f1,	// can overlap another f1, but not another F1
	ST_d,	// register read only
	ST_COUNT
};

// Step flags, figure 8.2's legend.
enum : u16
{
	F_LOCK    = 0x0010,	// locks the stage
	F_LOCKP   = 0x0020,	// locks, but no operation is executed
	F_RESULT  = 0x0040,	// result becomes available here
	KICK_SHIFT = 7,
	STAGE_MASK = 0x000f,
};

#define S(st)            ((u16)(ST_##st))
#define SL(st)           ((u16)(ST_##st | F_LOCK))
#define SLP(st)          ((u16)(ST_##st | F_LOCKP))
#define SR(st)           ((u16)(ST_##st | F_RESULT))
#define SK(st, n)        ((u16)(ST_##st | ((n) << KICK_SHIFT)))
#define SLK(st, n)       ((u16)(ST_##st | F_LOCK | ((n) << KICK_SHIFT)))
#define SLPK(st, n)      ((u16)(ST_##st | F_LOCKP | ((n) << KICK_SHIFT)))
#define SRK(st, n)       ((u16)(ST_##st | F_RESULT | ((n) << KICK_SHIFT)))
#define END              0xffff

// A pattern is a list of sequences; a sequence is a list of steps ended by END.
// Sequences after the first are started by a "kick" from an earlier one.
struct Pattern
{
	const u16 *seq[8];
	u8 nseq;
	// Index of the first sequence that runs on the FPU side in the background.
	// Figure 8.2 annotates patterns 34 and 35 with (CPU) and (FPU): a
	// fixed-point multiply issues on the CPU side in two cycles, and the
	// multiplier array then runs as f1 f1 f1 f1 F2 FS without holding up the
	// instructions behind it. Those sequences carry the result's latency but
	// must not occupy issue slots. 0xff means the pattern has no such chain.
	u8 bgFrom;
};

#define P1(a)                   { { a }, 1, 0xff }
#define P2(a, b)                { { a, b }, 2, 0xff }
#define P3(a, b, c)             { { a, b, c }, 3, 0xff }
#define P4(a, b, c, d)          { { a, b, c, d }, 4, 0xff }
#define P5(a, b, c, d, e)       { { a, b, c, d, e }, 5, 0xff }
#define P6(a, b, c, d, e, f)    { { a, b, c, d, e, f }, 6, 0xff }
// Patterns 34 and 35: sequences 2 onwards are the (FPU) half.
#define PMUL(a, b, c, d, e, f)  { { a, b, c, d, e, f }, 6, 2 }

// figure 8.2. Numbering is the manual's, and is the same numbering flycast's
// sh4_opcodelistentry::ex_type already uses.
static const u16 p1_0[]  = { S(I), S(D), S(EX), S(NA), S(S), END };
static const u16 p2_0[]  = { S(I), S(D), S(EX), S(MA), S(S), END };
static const u16 p3_0[]  = { S(I), S(D), S(SX), S(MA), S(S), END };
static const u16 p4_0[]  = { S(I), SL(D), SK(EX,1), S(NA), SR(S), END };
static const u16 p4_1[]  = { SL(D), S(EX), S(NA), S(S), END };
static const u16 p5_0[]  = { S(I), SL(D), SK(SX,1), S(MA), S(S), END };
static const u16 p5_1[]  = { SL(D), SK(SX,2), S(NA), S(S), END };
static const u16 p5_2[]  = { SL(D), S(SX), S(MA), S(S), END };
static const u16 p6_0[]  = { S(I), SL(D), SK(SX,1), S(MA), S(S), END };
static const u16 p6_1[]  = { SL(D), SK(SX,2), S(NA), S(S), END };
static const u16 p6_2[]  = { SL(D), SK(SX,3), S(NA), S(S), END };
static const u16 p6_3[]  = { SL(D), S(SX), S(MA), S(S), END };
static const u16 p7_0[]  = { S(I), SL(D), SK(EX,1), S(MA), SR(S), END };
static const u16 p7_1[]  = { SL(D), SK(EX,2), S(NA), S(S), END };
static const u16 p7_2[]  = { SL(D), SK(EX,3), S(NA), S(S), END };
static const u16 p7_3[]  = { SL(D), SK(EX,4), S(NA), S(S), END };
static const u16 p7_4[]  = { SL(D), S(EX), S(MA), S(S), END };
static const u16 p8_0[]  = { S(I), S(D), SK(EX,1), S(NA), S(S), END };
static const u16 p8_1[]  = { S(D), SK(EX,1), S(NA), S(S), END };
static const u16 p8_2[]  = { S(D), SK(EX,2), S(NA), S(S), END };
static const u16 p8_3[]  = { S(D), SK(EX,3), S(NA), S(S), END };
static const u16 p8_4[]  = { S(D), S(EX), S(NA), S(S), END };
static const u16 p10_0[] = { S(I), S(D), S(EX), S(MA), SK(S,1), END };
static const u16 p10_1[] = { SLP(MA), END };
static const u16 p11_1[] = { SLP(MA), SLP(MA), SLP(MA), SLP(MA), END };
static const u16 p12_1[] = { SLP(MA), SLP(MA), SLP(MA), SLP(MA), SLP(MA), SLP(MA), END };
static const u16 p13_0[] = { S(I), SLK(D,1), S(EX), S(NA), S(S), END };
static const u16 p13_1[] = { SL(D), SK(EX,2), S(NA), S(S), END };
static const u16 p13_2[] = { SL(D), SK(EX,3), S(NA), S(S), END };
static const u16 p13_3[] = { SL(D), SK(EX,4), S(NA), S(S), END };
static const u16 p13_4[] = { SL(D), SK(EX,5), S(NA), S(S), END };
static const u16 p13_5[] = { SL(D), SK(EX,6), S(NA), S(S), END };
static const u16 p13_6[] = { SL(D), S(EX), S(NA), S(S), END };
static const u16 p14_0[] = { S(I), S(D), S(EX), SK(NA,1), SR(S), END };
static const u16 p14_1[] = { SLP(SX), SLP(SX), END };
static const u16 p15_0[] = { S(I), SL(D), SK(EX,1), S(NA), SR(S), END };
static const u16 p15_1[] = { SLP(D), SLPK(SX,2), END };
static const u16 p15_2[] = { SLP(D), SLP(SX), END };
static const u16 p16_0[] = { S(I), SLK(D,1), S(EX), S(NA), SR(S), END };
static const u16 p16_1[] = { SLP(D), SLPK(SX,2), END };
static const u16 p16_2[] = { SLP(D), SLPK(SX,3), END };
static const u16 p16_3[] = { SLP(D), SLP(SX), END };
static const u16 p17_0[] = { S(I), S(D), S(EX), SK(MA,1), SR(S), END };
static const u16 p17_1[] = { SLP(SX), SLP(SX), END };
static const u16 p18_0[] = { S(I), SLK(D,1), S(EX), S(MA), SR(S), END };
static const u16 p19_0[] = { S(I), SLK(D,1), S(EX), S(MA), SR(S), END };
static const u16 p20_0[] = { S(I), SL(D), SK(SX,1), S(NA), S(S), END };
static const u16 p20_1[] = { SL(D), S(SX), S(NA), SR(S), END };
static const u16 p21_0[] = { S(I), SL(D), SK(SX,1), S(NA), S(S), END };
static const u16 p21_1[] = { SL(D), SK(SX,2), S(NA), S(S), END };
static const u16 p21_2[] = { SL(D), S(SX), S(NA), SR(S), END };
static const u16 p22_0[] = { S(I), SL(D), (u16)(SK(SX,1) | F_RESULT), S(NA), S(S), END };
static const u16 p22_1[] = { SL(D), S(SX), S(MA), S(S), END };
static const u16 p23_0[] = { S(I), SL(D), (u16)(SK(SX,1) | F_RESULT), S(NA), S(S), END };
static const u16 p23_1[] = { SL(D), SK(SX,2), S(NA), S(S), END };
static const u16 p23_2[] = { SL(D), S(SX), S(MA), S(S), END };
static const u16 p24_0[] = { S(I), S(D), SK(EX,1), S(NA), SR(S), END };
static const u16 p24_1[] = { SLP(D), SLP(SX), SLP(SX), END };
static const u16 p25_0[] = { S(I), SL(D), SK(EX,1), S(MA), SR(S), END };
static const u16 p26_0[] = { S(I), SL(D), SK(SX,1), S(NA), S(S), END };
static const u16 p26_1[] = { SL(D), S(SX), S(NA), SR(S), END };
static const u16 p27_0[] = { S(I), SL(D), SK(SX,1), S(NA), S(S), END };
static const u16 p27_1[] = { SL(D), S(SX), S(MA), SR(S), END };
static const u16 p28_0[] = { S(I), S(D), S(EX), SK(NA,1), S(S), END };
static const u16 p28_1[] = { SLP(F1), SL(F1), S(F2), SR(FS), END };
static const u16 p29_0[] = { S(I), S(D), S(EX), SK(MA,1), S(S), END };
static const u16 p29_1[] = { SLP(F1), S(F1), S(F2), SR(FS), END };
static const u16 p30_0[] = { S(I), S(D), S(EX), S(NA), S(S), END };
static const u16 p31_0[] = { S(I), S(D), S(EX), S(MA), S(S), END };
static const u16 p32_0[] = { S(I), S(D), S(EX), SK(NA,1), SR(S), END };
static const u16 p32_1[] = { SLP(F1), SLP(F1), SLP(F1), END };
static const u16 p33_0[] = { S(I), S(D), S(EX), SK(MA,1), SR(S), END };
static const u16 p34_0[] = { S(I), S(D), SK(EX,1), S(NA), S(S), END };
static const u16 p34_1[] = { (u16)(SL(D) | (2 << KICK_SHIFT)), SK(EX,3), SK(NA,4), SK(S,5), END };
static const u16 p34_f[] = { S(f1), END };
// The multiplier result lands at F2, not FS. Table 8.3 note 8 gives MUL* a
// latency of 4 to MACL; with the result at FS the model charged 5, and a
// mul.l/sts macl pair came out at 9 cycles against 7 measured.
static const u16 p34_5[] = { S(f1), SR(F2), S(FS), END };
// MAC.L / MAC.W. Table 8.3 note 7 gives their latency per destination -
// L1 for Rm, L2 for Rn, L3 for MACH, L4 for MACL - and exception 5 says that
// consecutive MACs have their latency reduced to 2 cycles, which is the
// accumulator forwarding path. Hardware measures a dependent chain at 2.00
// and the issue rate at 2.18.
//
// This model carries one latency per opcode, so the early result - the address
// write-back that a chain of MACs actually depends on - is marked in the
// pattern rather than derived from the latency field.
static const u16 p35_0[] = { S(I), SL(D), (u16)(SK(EX,1) | F_RESULT), S(MA), S(S), END };
static const u16 p35_1[] = { (u16)(SL(D) | (2 << KICK_SHIFT)), SK(EX,3), SK(MA,4), SK(S,5), END };
static const u16 p36_0[] = { S(I), S(D), S(F1), S(F2), S(FS), END };
static const u16 p38_0[] = { S(I), S(D), SLK(F1,1), S(F2), S(FS), END };
static const u16 p38_1[] = { S(d), SL(F1), S(F2), SR(FS), END };
static const u16 p39_0[] = { S(I), S(D), SLK(F1,1), S(F2), S(FS), END };
static const u16 p39_1[] = { S(d), SLK(F1,2), S(F2), S(FS), END };
static const u16 p39_2[] = { S(d), SLK(F1,3), S(F2), S(FS), END };
static const u16 p39_3[] = { S(d), SLK(F1,4), S(F2), S(FS), END };
static const u16 p39_4[] = { S(d), SL(F1), SK(F2,5), S(FS), END };
static const u16 p39_5[] = { SL(F1), S(F2), SR(FS), END };
static const u16 p40_0[] = { S(I), SL(D), SLK(F1,1), S(F2), S(FS), END };
static const u16 p40_1[] = { SL(D), SL(F1), S(F2), SR(FS), END };
static const u16 p42_0[] = { S(I), S(D), S(F0), S(F1), S(F2), S(FS), END };
static const u16 p43_0[] = { S(I), S(D), SK(F0,1), S(F1), S(F2), S(FS), END };
static const u16 p43_1[] = { S(d), SK(F0,2), S(F1), S(F2), S(FS), END };
static const u16 p43_2[] = { S(d), SK(F0,3), S(F1), S(F2), S(FS), END };
static const u16 p43_3[] = { S(d), S(F0), S(F1), S(F2), SR(FS), END };

// Patterns 37 and 41 (FDIV/FSQRT) hold the F3 stage for a number of cycles that
// depends on which operation it is, so one pattern number covers two shapes.
// Figure 8.2 writes them as "F3 2 10 F1 11 1" and so on; spelled out, the
// middle sequence occupies F3 and locks it, and the count is the difference.
//
//   pattern 37, single precision:  FDIV 10 locks, FSQRT 9
//   pattern 41, double precision:  FDIV 21 locks, FSQRT 20
//
// Leaving these unimplemented is what made the model report FDIV and FSQRT as
// costing a single cycle with no stage lock: they fell through to the
// unknown-opcode path, and every block containing one came out 6 to 12 cycles
// too fast.
#define F3LOCK   SL(F3)
#define F3x2     F3LOCK, F3LOCK
#define F3x4     F3x2, F3x2
#define F3x8     F3x4, F3x4

static const u16 p37_0[]  = { S(I), S(D), SK(F1,1), S(F2), S(FS), END };
static const u16 p37_2[]  = { SL(F1), S(F2), SR(FS), END };
// FDIV: ten F3 locks, the last of which kicks the final sequence.
static const u16 p37d_1[] = { F3x8, F3LOCK, (u16)(F3LOCK | (2 << KICK_SHIFT)), END };
// FSQRT: nine.
static const u16 p37s_1[] = { F3x8, (u16)(F3LOCK | (2 << KICK_SHIFT)), END };

static const u16 p41_0[]  = { S(I), S(D), SK(F1,1), S(F2), S(FS), END };
static const u16 p41_1[]  = { (u16)(S(d) | (2 << KICK_SHIFT)), S(F1), S(F2), END };
static const u16 p41d_2[] = { F3x8, F3x8, F3x4, (u16)(F3LOCK | (3 << KICK_SHIFT)), F3LOCK, END };
static const u16 p41s_2[] = { F3x8, F3x8, F3x2, F3LOCK, (u16)(F3LOCK | (3 << KICK_SHIFT)), F3LOCK, END };
static const u16 p41_3[]  = { SL(F1), SK(F2,4), S(F3), END };
static const u16 p41_4[]  = { SL(F1), SK(F2,5), S(F3), END };
static const u16 p41_5[]  = { SL(F1), S(F2), SR(F3), END };

// FSRRA: three-deep F0 chain. Hitachi's SH7091 training material gives pitch 3
// and latency 6, and hardware measures 3.00 / 6.16.
static const u16 p45_0[] = { S(I), S(D), SK(F0,1), S(F1), S(F2), S(FS), END };
static const u16 p45_1[] = { S(d), SK(F0,2), S(F1), S(F2), END };
static const u16 p45_2[] = { S(d), S(F0), S(F1), S(F2), SR(FS), END };
static const Pattern patternFsrra = P3(p45_0, p45_1, p45_2);

static const Pattern pattern37Fdiv  = P3(p37_0, p37d_1, p37_2);
static const Pattern pattern37Fsqrt = P3(p37_0, p37s_1, p37_2);
static const Pattern pattern41Fdiv  = { { p41_0, p41_1, p41d_2, p41_3, p41_4, p41_5 }, 6, 0xff };
static const Pattern pattern41Fsqrt = { { p41_0, p41_1, p41s_2, p41_3, p41_4, p41_5 }, 6, 0xff };

// FSQRT is 0xF_6D; FDIV is 0xF__3.
static bool isFsqrt(u16 op) { return (op & 0xF0FF) == 0xF06D; }

static const Pattern patterns[45] = {
	{ { nullptr }, 0, 0xff },										// 0 unused
	P1(p1_0), P1(p2_0), P1(p3_0), P2(p4_0, p4_1),
	P3(p5_0, p5_1, p5_2), P4(p6_0, p6_1, p6_2, p6_3),
	P5(p7_0, p7_1, p7_2, p7_3, p7_4),
	P5(p8_0, p8_1, p8_2, p8_3, p8_4),
	P4(p8_0, p8_1, p8_2, p8_4),										// 9
	P2(p10_0, p10_1), P2(p10_0, p11_1), P2(p10_0, p12_1),
	{ { p13_0, p13_1, p13_2, p13_3, p13_4, p13_5, p13_6 }, 7, 0xff },	// 13
	P2(p14_0, p14_1), P3(p15_0, p15_1, p15_2),
	P4(p16_0, p16_1, p16_2, p16_3),
	P2(p17_0, p17_1), P3(p18_0, p15_1, p15_2),
	P4(p19_0, p16_1, p16_2, p16_3),
	P2(p20_0, p20_1), P3(p21_0, p21_1, p21_2), P2(p22_0, p22_1),
	P3(p23_0, p23_1, p23_2), P2(p24_0, p24_1), P2(p25_0, p24_1),
	P2(p26_0, p26_1), P2(p27_0, p27_1), P2(p28_0, p28_1),
	P2(p29_0, p29_1), P1(p30_0), P1(p31_0), P2(p32_0, p32_1),
	P2(p33_0, p32_1),
	PMUL(p34_0, p34_1, p34_f, p34_f, p34_f, p34_5),					// 34
	PMUL(p35_0, p35_1, p34_f, p34_f, p34_f, p34_5),					// 35
	P1(p36_0),														// 36
	{ { nullptr }, 0, 0xff },										// 37 built
	P2(p38_0, p38_1),
	P6(p39_0, p39_1, p39_2, p39_3, p39_4, p39_5),
	P2(p40_0, p40_1),
	{ { nullptr }, 0, 0xff },										// 41 built
	P1(p42_0), P4(p43_0, p43_1, p43_2, p43_3),
	P1(p36_0),														// 44 FSCA/FSRRA - see notes
};

// ---------------------------------------------------------------------------
// Parallel-executability, table 8.2.
// ---------------------------------------------------------------------------
// Multiplies cost 6 cycles here against roughly 2 measured on hardware. See
// docs/cachesim/pipesim_notes.md - the cause is localised but not fixed, and
// it is the largest remaining error in the model.

static bool isParallel(sh4_eu a, sh4_eu b)
{
	if (a == MT && b == MT)
		return true;
	if (a == CO || b == CO)
		return false;
	return a != b;
}

// ---------------------------------------------------------------------------
// Register identity. The optable stores symbolic slots; these resolve them
// against an actual opcode so that dependencies compare real registers.
// ---------------------------------------------------------------------------
enum : int
{
	REG_R0 = 0,		// r0..r15    0..15
	REG_FR0 = 16,	// fr0..fr15  16..31
	REG_GBR = 32, REG_SR, REG_SSR, REG_SPC, REG_VBR, REG_DBR, REG_SGR,
	REG_PR, REG_MACH, REG_MACL, REG_FPUL, REG_FPSCR, REG_T, REG_XMTRX,
	REG_RBANK0 = 48,	// rb0..rb7  48..55
};

static u64 resolveSyms(u32 syms, u16 op)
{
	const int n = (op >> 8) & 0xf;
	const int m = (op >> 4) & 0xf;
	u64 rv = 0;
	if (syms & SYM_RN)    rv |= 1ull << (REG_R0 + n);
	if (syms & SYM_RM)    rv |= 1ull << (REG_R0 + m);
	if (syms & SYM_R0)    rv |= 1ull << REG_R0;
	if (syms & SYM_FN)    rv |= 1ull << (REG_FR0 + n);
	if (syms & SYM_FM)    rv |= 1ull << (REG_FR0 + m);
	if (syms & SYM_F0)    rv |= 1ull << REG_FR0;
	if (syms & SYM_DN)    rv |= 3ull << (REG_FR0 + (n & 0xe));
	if (syms & SYM_FVN)   rv |= 0xfull << (REG_FR0 + ((n & 0xc)));
	if (syms & SYM_FVM)   rv |= 0xfull << (REG_FR0 + (((op >> 8) & 0x3) * 4));
	if (syms & SYM_RBANK) rv |= 1ull << (REG_RBANK0 + (m & 7));
	if (syms & SYM_GBR)   rv |= 1ull << REG_GBR;
	if (syms & SYM_SR)    rv |= 1ull << REG_SR;
	if (syms & SYM_SSR)   rv |= 1ull << REG_SSR;
	if (syms & SYM_SPC)   rv |= 1ull << REG_SPC;
	if (syms & SYM_VBR)   rv |= 1ull << REG_VBR;
	if (syms & SYM_DBR)   rv |= 1ull << REG_DBR;
	if (syms & SYM_SGR)   rv |= 1ull << REG_SGR;
	if (syms & SYM_PR)    rv |= 1ull << REG_PR;
	if (syms & SYM_MACH)  rv |= 1ull << REG_MACH;
	if (syms & SYM_MACL)  rv |= 1ull << REG_MACL;
	if (syms & SYM_FPUL)  rv |= 1ull << REG_FPUL;
	if (syms & SYM_FPSCR) rv |= 1ull << REG_FPSCR;
	if (syms & SYM_T)     rv |= 1ull << REG_T;
	if (syms & SYM_XMTRX) rv |= 1ull << REG_XMTRX;
	return rv;
}

// Which register file a dependency is on. fr0-fr15 plus the FPU's own control
// and transfer registers; xmtrx is the bank the matrix instructions read, so a
// wait on it is an FPU wait too. Everything else - r0-r15, the banked set, the
// control registers, macl/mach, T - is the CPU side.
// Every FP-side register as a mask, for table 8.3 note 8.
static u64 fpuRegMask()
{
	static u64 m = 0;
	if (m == 0)
		for (int r = 0; r < 64; r++)
			if ((r >= REG_FR0 && r < REG_FR0 + 16)
					|| r == REG_FPUL || r == REG_FPSCR || r == REG_XMTRX)
				m |= 1ull << r;
	return m;
}

static bool isFpuReg(int r)
{
	return (r >= REG_FR0 && r < REG_FR0 + 16)
			|| r == REG_FPUL || r == REG_FPSCR || r == REG_XMTRX;
}

// ---------------------------------------------------------------------------
// Opcode lookup. Linear over 212 entries, memoised into a 64K table on first
// use - the same shape as flycast's own OpDesc.
// ---------------------------------------------------------------------------
// Corrections to flycast's opcode table.
//
// Several SH4 floating-point instructions share one opcode between single and
// double precision, selected by FPSCR.PR at runtime, and flycast's table has a
// single row for each. Almost every row carries the single-precision numbers,
// which is the right default. Two do not:
//
//   fsqrt  latency 23, pattern 41  - both the DOUBLE-precision values.
//                                    Single precision is 11/12, pattern 37.
//   ftrc   pattern 38              - pattern 38 is "double-precision
//                                    computation 1". Single precision is 36.
//                                    Its latency of 4 is already correct.
//
// Checked against the SH4 program manual table 8.3 and against shared-ptr's
// instruction summary, which lists the FRn and DRn forms separately: fsqrt FRn
// is 11/12 and fsqrt DRn is 23/25.
//
// This is corrected here rather than in sh4_opcode_list.cpp deliberately.
// Nothing in flycast reads LatencyCycles or ex_type today - Sh4Cycles uses
// only IssueCycles and the functional unit - so the errors are dormant there,
// and changing that table would change emulator timing for a reason unrelated
// to this model. The right fix upstream is a second row per opcode selected by
// FPSCR.PR, which is a larger change than this model needs.
//
// Consequence worth stating: this model assumes PR=0. A guest running in
// double-precision mode is mis-modelled for these instructions, and the
// analyser has no way to know from the opcode alone.
struct OpCorrection
{
	u16 mask;
	u16 rez;
	u8 latency;
	u8 exType;
};

static const OpCorrection opCorrections[] = {
	{ 0xF0FF, 0xF06D, 11, 37 },	// fsqrt FRn
	{ 0xF0FF, 0xF03D,  4, 36 },	// ftrc FRm,FPUL
	// Single-precision arithmetic is latency 3 in table 8.3; flycast's table
	// carries 4. Measured, not just read: `validation/fpustall` case C is four
	// chained fadds at distance 1, and hardware charges 6 stall cycles per
	// group of four - two per dependency, which is latency minus distance with
	// latency 3. At 4 the model charged three per dependency.
	{ 0xF00F, 0xF000,  3, 36 },	// fadd FRm,FRn
	{ 0xF00F, 0xF001,  3, 36 },	// fsub FRm,FRn
	{ 0xF00F, 0xF002,  3, 36 },	// fmul FRm,FRn
	{ 0xF00F, 0xF00E,  3, 36 },	// fmac FR0,FRm,FRn
	// FSRRA and FSCA are Sega-private single-precision instructions. They are
	// absent from table 8.3 of the program manual entirely - the strings do not
	// appear in it - and the only Renesas documents that describe them are
	// SH-4A, which reimplemented the FPU and whose numbers do not transfer.
	// Hitachi's own SH7091 training material gives FSRRA pitch 3 / latency 6
	// and FSCA pitch 4 / latency 7; hardware here measures FSRRA at 3.00/6.16.
	// flycast's table had FSRRA as a plain one-cycle FE op.
	{ 0xF0FF, 0xF07D,  6, 45 },	// fsrra FRn
	{ 0xF1FF, 0xF0FD,  7, 43 },	// fsca FPUL,DRn - same shape as FTRV
};

static OpInfo correctedTable[opInfoTableSize];
static const OpInfo *opInfoCache[0x10000];
static bool opInfoCacheBuilt;

static void buildOpInfoCache()
{
	if (opInfoCacheBuilt)
		return;

	memcpy(correctedTable, opInfoTable, sizeof(correctedTable));
	for (const OpCorrection &c : opCorrections)
		for (OpInfo &e : correctedTable)
			if (e.mask == c.mask && e.rez == c.rez)
			{
				e.latency = c.latency;
				e.exType = c.exType;
			}

	memset(opInfoCache, 0, sizeof(opInfoCache));
	for (u32 op = 0; op < 0x10000; op++)
	{
		for (size_t i = 0; i < opInfoTableSize; i++)
		{
			const OpInfo &e = correctedTable[i];
			if ((op & e.mask) == e.rez)
			{
				opInfoCache[op] = &e;
				break;
			}
		}
	}
	opInfoCacheBuilt = true;
}

const char *stallReasonName(StallReason r)
{
	switch (r)
	{
	case StallReason::None:           return "none";
	case StallReason::StageFull:      return "stage-full";
	case StallReason::StageLocked:    return "stage-locked";
	case StallReason::ResourceHazard: return "resource";
	case StallReason::FlowDep:        return "flow-dep";
	case StallReason::FpuDep:         return "fpu-dep";
	case StallReason::OutputDep:      return "output-dep";
	case StallReason::PrevStalled:    return "prev-stalled";
	default:                          return "?";
	}
}

bool fullyModelled(const u16 *ops, u32 count, u32 *firstUnknownIndex)
{
	buildOpInfoCache();
	for (u32 i = 0; i < count; i++)
		if (opInfoCache[ops[i]] == nullptr)
		{
			if (firstUnknownIndex != nullptr)
				*firstUnknownIndex = i;
			return false;
		}
	return true;
}

}

namespace pipesim
{

// ---------------------------------------------------------------------------
// The engine. A direct port of the stepping loop in skmp's SH-4 pipeline
// simulator, driven from flycast's opcode tables instead of from assembly
// text. Ported rather than rewritten on purpose: the loop is the part that is
// genuinely fiddly, it is already debugged, and keeping the same structure is
// what makes a cycle-for-cycle differential test against it meaningful.
// ---------------------------------------------------------------------------

static const int MAX_CYCLES = 1000000;	// a backstop, not a model limit: the
											// analyser is also run on blocks unrolled
											// 64 times for the hardware comparison

struct Seq
{
	const u16 *steps;
	int len;
	int step;
	int programOrder;
	int insnIdx;
	bool stall;
	bool active;
	bool background;
	sh4_eu group;
	u64 reads;
	u64 writes;

	u16 cur() const     { return steps[step]; }
	int stage() const   { return steps[step] & STAGE_MASK; }
	int nextStage() const
	{
		return step + 1 < len ? (steps[step + 1] & STAGE_MASK) : -1;
	}
	bool locks() const  { return (steps[step] & (F_LOCK | F_LOCKP)) != 0; }
	bool result() const { return (steps[step] & F_RESULT) != 0; }
	int kick() const    { return steps[step] >> KICK_SHIFT; }
	int nextKick() const
	{
		return step + 1 < len ? (steps[step + 1] >> KICK_SHIFT) : 0;
	}
};

struct Insn
{
	const OpInfo *info;
	int firstSeq;
	int nseq;
	u8 bgFrom;
	int programOrder;
	u64 reads, writes;
	// Cycle this instruction entered the decode stage, and how long it sat
	// there. Diagnostic: the latency clock starts at D-EXIT, so any error in
	// how long the issue model holds an instruction in D propagates into every
	// dependent instruction's stall. Splitting stalls by the producer's D
	// residency says whether that coupling is where the over-stall comes from.
	int dEnter;
	// The cycle from which this instruction's result may be consumed.
	//
	// SHC_PM 8.3 defines latency as the interval between issue and GENERATION
	// of the result, not its write-back, and figure 8.3 (e) shows a consumer
	// reading a register two cycles before the producer reaches S. The SH4
	// forwards. So availability is set when the producer issues, from its
	// latency, and has nothing to do with where its execution pattern ends.
	int availCycle;
};

Result analyze(const u16 *ops, u32 count, InsnDetail *detail, u32 pairFrom,
		u32 pcParity)
{
	buildOpInfoCache();

	Result res;
	res.instructions = count;
	if (count == 0)
		return res;

	std::vector<Seq> pool;
	std::vector<Insn> insns(count);
	std::vector<u16> stepArena;
	// Sequences are addressed by index into pool, and pool must not move while
	// the loop holds indices, so reserve up front.
	pool.reserve(count * 8);
	stepArena.reserve(count * 48);

	// Per-instruction stall accounting.
	std::vector<u16> stallOf(count, 0);
	std::vector<StallReason> reasonOf(count, StallReason::None);
	std::vector<int> blameOf(count, -1);

	// Pass 1: resolve each instruction's pattern and register sets. Patterns
	// with a single sequence carry no explicit result step - the manual gives
	// the latency instead - so the result flag is placed at 1 + latency, which
	// is why those need a private copy of the step list.
	std::vector<int> seqOffsets(count * 8, -1);
	for (u32 i = 0; i < count; i++)
	{
		const OpInfo *info = opInfoCache[ops[i]];
		Insn &in = insns[i];
		in.info = info;
		in.reads = info != nullptr ? resolveSyms(info->reads, ops[i]) : 0;
		in.writes = info != nullptr ? resolveSyms(info->writes, ops[i]) : 0;
		in.firstSeq = (int)pool.size();
		in.nseq = 0;
		in.bgFrom = 0xff;

		// No pipeline data: charge the issue rate and no stalls. Modelled as a
		// single-step EX so it still occupies a slot.
		static const u16 unknownSteps[] = { S(I), S(D), S(EX), S(NA), S(S), END };
		const Pattern *pat = nullptr;
		// exType 45 is not one of the manual's patterns: it is this model's own
		// entry for FSRRA, which the manual does not list at all.
		if (info != nullptr && info->exType == 45)
			pat = &patternFsrra;
		else if (info != nullptr && info->exType < 45)
		{
			if (info->exType == 37)
				pat = isFsqrt(ops[i]) ? &pattern37Fsqrt : &pattern37Fdiv;
			else if (info->exType == 41)
				pat = isFsqrt(ops[i]) ? &pattern41Fsqrt : &pattern41Fdiv;
			else
				pat = &patterns[info->exType];
		}

		if (pat == nullptr || pat->nseq == 0)
		{
			size_t off = stepArena.size();
			for (const u16 *p = unknownSteps; *p != END; p++)
				stepArena.push_back(*p);
			stepArena.push_back(END);
			seqOffsets[i * 8] = (int)off;
			in.nseq = 1;
			pool.push_back(Seq{});
			continue;
		}

		for (int s = 0; s < pat->nseq; s++)
		{
			size_t off = stepArena.size();
			for (const u16 *p = pat->seq[s]; *p != END; p++)
				stepArena.push_back(*p);
			stepArena.push_back(END);
			seqOffsets[i * 8 + s] = (int)off;
			pool.push_back(Seq{});
		}
		in.nseq = pat->nseq;
		in.bgFrom = pat->bgFrom;

		// Single-sequence pattern: place the result at 1 + latency.
		if (pat->nseq == 1 && info != nullptr)
		{
			int off = seqOffsets[i * 8];
			int len = 0;
			while (stepArena[off + len] != END)
				len++;
			int at = 1 + info->latency;
			if (at >= len)
				at = len - 1;	// latency past the pattern: charge it at the end
			stepArena[off + at] |= F_RESULT;
		}
	}

	// Pass 2: bind step pointers now that the arena has stopped growing.
	auto seqSteps = [&](u32 i, int s) { return &stepArena[seqOffsets[i * 8 + s]]; };
	auto seqLen = [&](u32 i, int s) {
		int len = 0;
		const u16 *p = seqSteps(i, s);
		while (p[len] != END)
			len++;
		return len;
	};

	int programOrder = 0;
	int stuckCycles = 0;

	// Section 8.3: a flow dependency stalls for (latency) cycles, but an
	// output dependency - write-after-write - stalls for (latency - 2).
	//
	// Modelling both as "wait until the result exists" is what made every
	// multiply cost 6 cycles instead of 2: consecutive multiplies all write
	// MACL, so each one waited the full latency of the one before it. On
	// hardware a repeated mul.l costs exactly its issue rate.
	//
	// Expressed in this engine: an output dependency is already satisfied once
	// the producer is within two steps of generating its result.
	auto stepsToResult = [&](const Seq &s) -> int {
		for (int k = s.step; k < s.len; k++)
			if (s.steps[k] & F_RESULT)
				return k - s.step;
		return 0;
	};
	std::vector<int> provides[64];
	int stageLock[ST_COUNT];
	for (int i = 0; i < ST_COUNT; i++)
		stageLock[i] = -1;

	std::vector<int> inFlight;
	std::vector<int> toRemove, toResult;
	u32 pc = 0;
	u32 cycle = 0;

	auto nextKickBlocked = [&](const Seq &s) -> bool {
		const int k = s.nextKick();
		if (k == 0)
			return false;
		const Insn &in = insns[s.insnIdx];
		if (k >= in.nseq)
			return false;
		const Seq &ks = pool[in.firstSeq + k];
		if (ks.active || !ks.locks())
			return false;
		const int held = stageLock[ks.stage()];
		if (held < 0 || held == in.firstSeq + k)
			return false;
		// A lock held by another sequence of the same instruction is a
		// hand-off, not contention: patterns like STC (20) lock D in sequence
		// 0 and immediately kick sequence 1, which locks D again. Treating
		// that as contention deadlocked the instruction against itself.
		return pool[held].insnIdx != s.insnIdx;
	};

	// True if advancing this sequence would start another that needs a stage
	// somebody else still holds. FDIV and FSQRT lock F3 for ten cycles and are
	// not pipelined, so a second one has to wait for the divider.
	//
	// Dropping the kick instead loses the instruction's result sequence
	// entirely, and anything waiting on that register then waits forever - the
	// model deadlocks on any loop containing two divides. The reference
	// simulator leaves this case as an explicit TODO and deadlocks too.

	auto providedByOtherSlow = [&](u64 regs, const Seq &s, int slack) -> int {
		if (regs == 0)
			return -1;
		for (int r = 0; r < 64; r++)
		{
			if ((regs & (1ull << r)) == 0)
				continue;
			for (int id : provides[r])
			{
				const Seq &pr = pool[id];
				if (pr.insnIdx == s.insnIdx || pr.programOrder >= s.programOrder)
					continue;
				if (stepsToResult(pr) <= slack)
					continue;
				return pr.insnIdx;
			}
		}
		return -1;
	};

	// `reg`, if given, receives the register that was actually waited on, so
	// the caller can tell a CPU-register stall from an FPU one the way the
	// hardware counters do.
	auto providedByOther = [&](u64 regs, const Seq &s, int *reg = nullptr) -> int {
		if (regs == 0)
			return -1;
		// The operand that arrives LAST is the one holding this instruction up,
		// so that is the one the stall is blamed on. Returning the first match
		// in register-number order instead put the blame on whichever register
		// happened to be numbered lowest, and the numbering is not neutral:
		// r0-r15 are bits 0-15 and fr0-fr15 are 16-31, so every stall waiting
		// on both a GPR and an FPU register was charged to the GPR. Hardware
		// splits its two freeze counters 40/60 CPU-to-FPU on texture2d; this
		// model split them 76/24 the other way, which is the same instructions
		// being misfiled rather than a different number of them.
		int worstSteps = -1;
		int worst = -1;
		for (int r = 0; r < 64; r++)
		{
			if ((regs & (1ull << r)) == 0)
				continue;
			for (int id : provides[r])
			{
				const Seq &p = pool[id];
				if (p.insnIdx == s.insnIdx || p.programOrder >= s.programOrder)
					continue;
				// Forwarded already: the result exists even though the producer
				// has not reached write-back. Holding on until it did was
				// charging the whole tail of the producer's execution pattern -
				// both common patterns put S at D+3, so that is 2 extra cycles
				// on every EX-group dependency and 1 on every load-use, against
				// correct stalls that are usually 0 or 1.
				if ((int)cycle >= insns[p.insnIdx].availCycle)
					continue;
				// Rank by when the value ARRIVES, not by how far the producer
				// still is from write-back. Those stopped agreeing when
				// availability moved onto availCycle above: ranking by
				// steps-to-write-back could name a register that has already
				// been forwarded while a different operand was the real
				// blocker, and when the misnamed one was a GPR the stall was
				// filed as a CPU-register freeze instead of an FPU one.
				const int avail = insns[p.insnIdx].availCycle;
				if (avail > worstSteps)
				{
					worstSteps = avail;
					worst = p.insnIdx;
					if (reg != nullptr)
						*reg = r;
				}
			}
		}
		return worst;
	};

	// Per-cycle trace. PIPESIM_TRACE=<count> prints the first 40 cycles of the
	// first sequence analysed with that many instructions: which stage each
	// in-flight sequence is in, and which of them stalled and why. The
	// per-instruction dump says how much; this says when.
	const char *traceEnv = getenv("PIPESIM_TRACE");
	static int traced = 0;
	// PIPESIM_SKIP skips that many matching blocks first - several unrelated
	// blocks can share an instruction count, and the first one is rarely the
	// one being investigated.
	// PIPESIM_TRACE_OP selects by the block's FIRST OPCODE instead, in hex.
	// Instruction count is a poor selector - unrelated blocks share one, and
	// which of them the analyser reaches first depends on what the guest did.
	const char *opEnv = getenv("PIPESIM_TRACE_OP");
	bool want = false;
	if (opEnv != nullptr)
		want = count > 0 && ops[0] == (u16)strtoul(opEnv, nullptr, 16);
	else if (traceEnv != nullptr)
		want = count == (u32)atoi(traceEnv);
	const char *skipEnv = getenv("PIPESIM_SKIP");
	const bool trace = want
			&& traced++ == (skipEnv != nullptr ? atoi(skipEnv) : 0);

	if (trace)
	{
		fprintf(stderr, "PIPESIM trace count=%u ops:", count);
		for (u32 i = 0; i < count && i < 6; i++)
			fprintf(stderr, " %04x", ops[i]);
		fprintf(stderr, "\n");
	}

	while (cycle < MAX_CYCLES)
	{
		if (inFlight.empty() && pc == count)
			break;

		int prevStall = -1;
		bool anyAdvanced = false;
		// Instructions that left I for D this cycle. At most two: the fetch
		// stage never holds more than that.
		int issuedThisCycle = 0;
		int issuedIdx[2] = { -1, -1 };
		// Per-CYCLE attribution. Several pipeline sequences can be blocked in
		// the same cycle, so counting each one gives events, not time - and a
		// breakdown in events cannot be added to anything.
		//
		// One cycle is charged once, to the OLDEST instruction that could not
		// advance in it. That is the head of the machine: everything behind it
		// is waiting on it rather than on its own hazard, which is what
		// PrevStalled means. Picking the oldest rather than the first one
		// scanned also makes the answer independent of the order the in-flight
		// list happens to be in.
		StallReason cycleReason = StallReason::None;
		int cycleReasonInsn = INT_MAX;
		int cycleReasonBlame = -1;
		toRemove.clear();
		toResult.clear();

		for (size_t idx = 0; idx < inFlight.size(); idx++)
		{
			int id = inFlight[idx];
			Seq &seq = pool[id];
			const int curStage = seq.stage();
			const int nextStage = seq.nextStage();

			int nInNext = 0;
			int hazard = -1;
			bool allParallel = true;
			for (int other : inFlight)
			{
				if (other == id)
					continue;
				const Seq &o = pool[other];
				if (o.background)
					continue;
				if (o.stage() != nextStage)
					continue;
				if (o.programOrder < seq.programOrder)
					nInNext++;
				if (!(o.programOrder > seq.programOrder || isParallel(o.group, seq.group)))
				{
					allParallel = false;
					if (hazard < 0)
						hazard = o.insnIdx;
				}
			}

			StallReason reason = StallReason::None;
			int blame = -1;
			int waitedOn = -1;
			// True if something already issued in this cycle produces a value
			// this instruction reads, with a latency of at least one cycle.
			// Table 8.3 note 8 adds one case with no register in common: a
			// bank switch followed by an LS instruction touching the FP file.
			// The T bit reaching a branch is not a flow dependency the issue
			// stage can see. `validation/fpustall` case A settles it: its loop
			// is 24 fmovs, which are all LS x LS and cannot pair, plus `dt r1`
			// and `bf` - so dt/bf is the ONLY legal pair in the iteration.
			// Hardware reports 6226 parallel issues over 6000 iterations and a
			// reg_stall of TWO. It pairs them, and charges nothing.
			//
			// So the branch condition forwards to the branch unit with an
			// effective latency of zero, and whatever the transfer costs is
			// filed under PMCR 0x27 (branch) rather than 0x28 (CPU register).
			// This replaces the old special case here, which broke the pair -
			// that got the stall count right by accident and the issue slots
			// wrong by 6000 an iteration.
			const u64 depMask = seq.group == BR ? ~(1ull << REG_T) : ~0ull;
			auto dependentOnIssuedThisCycle = [&]() {
				for (int k = 0; k < issuedThisCycle && k < 2; k++)
				{
					const int p = issuedIdx[k];
					if (p < 0)
						continue;
					const u16 pop = ops[p];
					if ((pop == 0xF3FD || pop == 0xFBFD) && seq.group == LS
							&& ((seq.reads | seq.writes) & fpuRegMask()) != 0)
						return true;
					const OpInfo *pi = insns[p].info;
					if (pi == nullptr || pi->latency == 0)
						continue;
					if ((insns[p].writes & seq.reads & depMask) != 0)
						return true;
				}
				return false;
			};

			if (seq.background)
			{
				// The (FPU) half of a multiply runs to completion on its own.
				// It carries the result's latency; it does not compete for
				// issue slots and cannot be held up by the CPU side.
				nInNext = 0;
				allParallel = true;
			}

			if (seq.background)
			{
				seq.stall = false;
				if (seq.locks())
					stageLock[seq.stage()] = -1;
				seq.step++;
				if (seq.result())
					toResult.push_back(id);
				if (seq.nextStage() < 0)
					toRemove.push_back(id);
				anyAdvanced = true;
			}
			else if (nInNext == 2)
			{
				reason = StallReason::StageFull;
				seq.stall = true;
			}
			else if (nextStage >= 0 && stageLock[nextStage] >= 0 && stageLock[nextStage] != id)
			{
				// Note: the reference simulator does not set its stall flag
				// here, so this does not propagate to following instructions.
				// Replicated deliberately - see docs/cachesim/pipesim_notes.md.
				//
				// A lock on one of the FPU stages is an FPU freeze, not a
				// generic stage conflict. `validation/fpustall` case B settles
				// this: an fdiv followed by fadds on completely disjoint
				// registers - no dependency of any kind - puts 563983 cycles
				// into PMCR 0x29. So 0x29 is not "waiting on an FP register",
				// it is that OR waiting on an FPU resource, and filing these
				// under StageLocked was the other half of why the FPU bucket
				// read low and the CPU-register one read high.
				reason = (nextStage >= ST_F0 && nextStage <= ST_F3)
						? StallReason::FpuDep : StallReason::StageLocked;
				blame = pool[stageLock[nextStage]].insnIdx;
				// An FPU stage lock DOES propagate, unlike the integer case
				// above. The SH4 issues in order with no scoreboarding past D,
				// so when an FE instruction cannot enter its stage everything
				// behind it waits too. `validation/fpustall` case C is four
				// chained fadds: hardware charges exactly 9 stall cycles per
				// group, which is three dependent pairs at three cycles each
				// and therefore ZERO overlap between one group and the next.
				// Without this the next group started issuing during the
				// stall and the model charged 7.5.
				if (nextStage >= ST_F0 && nextStage <= ST_F3)
					seq.stall = true;
			}
			else if (!allParallel)
			{
				// Two FE-group instructions contending for the FPU pipeline is
				// an FPU freeze, which is what hardware calls it - 0x29 is a
				// superset of FP-register waits and FPU resource waits, per
				// `validation/fpustall` case B. Leaving it as a generic
				// resource hazard lost the cycles entirely, because nothing
				// maps ResourceHazard to a hardware counter.
				//
				// This branch fires BEFORE the flow-dependency check below, so
				// it also swallows genuine FE-to-FE data dependencies: table
				// 8.2 bars FE x FE from dual issue, so a dependent pair trips
				// the parallelism test before anything looks at the registers.
				// A schedule dump of `fpustall` case C - four chained fadds -
				// showed 47 of its 48 stalls filed as `resource` and one as
				// `fpu-dep`.
				const bool fpuPair = seq.group == FE && hazard >= 0
						&& insns[hazard].info != nullptr
						&& insns[hazard].info->unit == FE;
				reason = fpuPair ? StallReason::FpuDep
						: StallReason::ResourceHazard;
				blame = hazard;
				seq.stall = true;
			}
			else if (curStage == ST_I && nextStage == ST_D && issuedThisCycle == 1
					&& issuedIdx[0] >= 0
					&& (((u32)issuedIdx[0] + pcParity) & 1) != 0)
			{
				// The instruction already issued this cycle sits at a 4n+2
				// address, so it is the SECOND half of an aligned fetch pair
				// and there is nothing left in that pair to go with it. This
				// instruction waits for the next slot.
				//
				// pipesim used to pair any two adjacent legally-pairable
				// instructions regardless of where they sat, which pairs more
				// often than the fetch unit can supply.
				reason = StallReason::ResourceHazard;
				seq.stall = true;
			}
			else if (curStage == ST_I && nextStage == ST_D && issuedThisCycle > 0
					&& dependentOnIssuedThisCycle())
			{
				// A parallel-executable pair is BROKEN when the second
				// instruction reads something the first writes with a latency
				// of one cycle or more. The two are fetched together and issue
				// in successive cycles instead.
				//
				// Figure 8.3(e)-2 states it: "ADD and MOV.L are not executed
				// in parallel, since MOV.L references the result of ADD as its
				// destination address", and the stage diagram shows MOV.L's I
				// stage extended by one so its D lands a cycle late. Figure
				// 8.3(b) is titled "parallel-executable and no dependency" -
				// the qualifier is load-bearing. Table 8.3 note 8 is the same
				// principle applied to a bank switch.
				//
				// Latency ZERO still pairs, which is why the test is on
				// latency and not on the dependency alone. Figure 8.3(e)-1
				// shows MOV R0,R1 / ADD R2,R1 co-issued at identical stage
				// cycles with a true dependency on R1: "ADD is not stalled
				// when executed after an instruction with zero-cycle latency,
				// even if there is dependency." So MOV Rm,Rn, FMOV FRm,FRn,
				// FLDI0/1, FABS, FNEG, FLDS and FSTS keep pairing with their
				// consumers and everything else does not.
				//
				// This subsumes the conditional-branch case that used to be
				// special-cased here: CMP/EQ is MT with latency 1, BT/BF is
				// BR, MT x BR is a legal pairing in table 8.2, so the pair
				// breaks, the distance becomes 1 and the stall is
				// max(0, 1 - 1) = 0.
				//
				// The total cycle count is the same either way, which is why
				// this hid behind four other hypotheses. Consumer EX lands at
				// producer D + latency + 1 whether you co-issue and charge
				// `latency`, or break the pair and charge `latency - 1`. Only
				// the ATTRIBUTION differs - and the attribution is what the
				// hardware counters measure.
				//
				// This is not a stall, and it costs no cycle: something else
				// issued this cycle, so the accounting below does not charge a
				// freeze. It moves the branch from this issue slot to the next
				// one, which is exactly what hardware does.
				//
				// `validation/blockentry` measures it. Its loop scaffolding is
				// `dt r1` then `bf`, and hardware spends one extra issue slot
				// per iteration and ZERO register stalls, where pipesim paired
				// the two and charged dt's latency of 1 as a flow dependency.
				// The cycle count is the same either way, which is why this
				// hid for so long - but the split is not, and the same pattern
				// is every loop and every `if` in compiled code. All three
				// broken counters were this one thing: on texture2d the issue
				// slot deficit (30.2M), the parallel-issue excess (29.2M) and
				// the register stall excess (27.9M) all sit on top of a
				// branch_issued of 25.8M.
				reason = StallReason::ResourceHazard;
				seq.stall = true;
			}
			else if ((curStage != ST_I || (seq.reads != 0 && insns[seq.insnIdx].info != nullptr
						&& insns[seq.insnIdx].info->latency == 0))
					&& (blame = providedByOther(seq.reads & depMask, seq, &waitedOn)) >= 0)
			{
				reason = isFpuReg(waitedOn) ? StallReason::FpuDep
						: StallReason::FlowDep;
				seq.stall = true;
			}
			else if (curStage != ST_I
					&& (blame = providedByOtherSlow(seq.writes, seq, 2)) >= 0)
			{
				reason = StallReason::OutputDep;
				seq.stall = true;
			}
			else if (nextKickBlocked(seq))
			{
				reason = StallReason::StageLocked;
				seq.stall = true;
			}
			else if (prevStall >= 0)
			{
				reason = StallReason::PrevStalled;
				blame = pool[prevStall].insnIdx;
				seq.stall = true;
			}
			else
			{
				anyAdvanced = true;
				seq.stall = false;
				if (curStage == ST_D)
				{
					// The latency clock starts when an instruction LEAVES the
					// decode stage, not when it enters. That is the cycle the
					// functional unit actually begins work; an instruction
					// held in D by a hazard has not started computing anything.
					//
					// The two are the same for an instruction that never
					// stalls, which is why every isolated probe agreed and
					// only dependent CHAINS diverged. In `fpustall` case C -
					// four chained fadds - starting the clock at D-entry made
					// the second dependency in each group free: the producer
					// entered D at c2, stalled there until c6, and the model
					// had its result ready at c7 while the consumer was only
					// able to issue at c6. Hardware charges three cycles for
					// that dependency like the others, 9 per group against the
					// model's 6.
					const OpInfo *oi = insns[seq.insnIdx].info;
					insns[seq.insnIdx].availCycle =
							(int)cycle + (oi != nullptr ? oi->latency : 0);
				}
				if (curStage == ST_I && nextStage == ST_D)
				{
					insns[seq.insnIdx].dEnter = (int)cycle;
					if (issuedThisCycle < 2)
						issuedIdx[issuedThisCycle] = seq.insnIdx;
					issuedThisCycle++;
				}
				if (seq.locks())
					stageLock[seq.stage()] = -1;
				seq.step++;
				if (seq.result())
					toResult.push_back(id);
				if (seq.locks())
					stageLock[seq.stage()] = id;
				if (seq.nextStage() < 0)
					toRemove.push_back(id);

				// Start any sequences this step kicks off, recursively.
				int kicker = id;
				while (pool[kicker].kick() != 0)
				{
					const Insn &in = insns[pool[kicker].insnIdx];
					int k = in.firstSeq + pool[kicker].kick();
					if (pool[kicker].kick() >= in.nseq)
						break;
					Seq &ks = pool[k];
					if (ks.active)
						break;
					// Do not steal a stage lock somebody else still holds.
					// FDIV and FSQRT lock F3 for ten cycles and are not
					// pipelined, so a second one issued while the first is
					// still running has to wait. Overwriting the lock here
					// left the original holder unable to advance and unable
					// to release, and the two deadlocked - which is what the
					// reference simulator does too, where this is an
					// unimplemented case marked with a TODO.
					if (ks.locks() && stageLock[ks.stage()] >= 0
							&& stageLock[ks.stage()] != k)
						break;
					ks.active = true;
					if (ks.locks())
						stageLock[ks.stage()] = k;
					if (ks.nextStage() < 0)
						toRemove.push_back(k);
					inFlight.insert(inFlight.begin() + idx + 1, k);
					idx++;
					kicker = k;
				}
			}

			if (reason != StallReason::None)
			{
				const bool real = reason != StallReason::PrevStalled;
				const bool better = cycleReason == StallReason::None
						|| (real && cycleReason == StallReason::PrevStalled)
						|| (real == (cycleReason != StallReason::PrevStalled)
								&& seq.insnIdx < cycleReasonInsn);
				if (better)
				{
					cycleReason = reason;
					cycleReasonInsn = seq.insnIdx;
					cycleReasonBlame = blame;
				}
				const int i = seq.insnIdx;
				stallOf[i]++;
				if (reasonOf[i] == StallReason::None)
				{
					reasonOf[i] = reason;
					blameOf[i] = blame;
				}
			}

			if (prevStall < 0 && seq.stall)
				prevStall = id;
		}

		// A producer stops being a hazard when its RESULT EXISTS, which SHC_PM
		// 8.3 puts at (issue + latency) - not when its sequence happens to
		// reach the step flagged F_RESULT in the execution pattern.
		//
		// Those two are the same for integer ops and wildly different for FPU
		// ones, whose patterns flag the result early while the latency runs on
		// for several more cycles. Removing on F_RESULT dropped a producer from
		// `provides` before anything could see it, so a chain of dependent
		// fadds never stalled at all: a trace of `fpustall` case C showed
		// seventeen dependent FE instructions issuing back to back, one per
		// cycle, with the flow-dependency check finding nothing to wait for
		// because the thing to wait for had already been deleted.
		//
		// So removal is keyed on availCycle and nothing else. providedByOther
		// skips producers that have already delivered, which makes the two
		// mechanisms one mechanism instead of two that disagree.
		// A pattern that explicitly marks F_RESULT is stating where that
		// opcode's result is generated, and for some opcodes that is the only
		// place it can be stated: MAC.L has four different latencies, one per
		// destination, and the single latency field cannot carry them, so the
		// early one - the address write-back a chain of MACs actually depends
		// on - lives in the pattern. mul.l is the same story, its result at F2
		// rather than FS.
		//
		// So a producer stops being a hazard at whichever comes first: the
		// step its pattern flags, or availCycle. Patterns only flag it where
		// it has been deliberately established; the ones that do not - fadd
		// and friends - fall through to availCycle, which is what fixed the
		// dependent FE chains.
		for (int id : toResult)
		{
			const Seq &sq = pool[id];
			for (int r = 0; r < 64; r++)
				if (sq.writes & (1ull << r))
				{
					auto &v = provides[r];
					for (size_t k = 0; k < v.size(); k++)
						if (v[k] == id)
						{
							v.erase(v.begin() + k);
							break;
						}
				}
		}
		for (int r = 0; r < 64; r++)
		{
			auto &v = provides[r];
			for (size_t k = 0; k < v.size(); )
			{
				const int idx = pool[v[k]].insnIdx;
				if ((int)cycle >= insns[idx].availCycle)
					v.erase(v.begin() + k);
				else
					k++;
			}
		}

		for (int id : toRemove)
		{
			for (size_t k = 0; k < inFlight.size(); k++)
				if (inFlight[k] == id)
				{
					inFlight.erase(inFlight.begin() + k);
					break;
				}
			// Release every stage this sequence holds, not just the one its
			// final step happens to name. Keying the release off the last
			// step leaked the lock whenever a sequence ended on a
			// non-locking step, and the next instruction that needed that
			// stage waited on a holder that no longer existed.
			for (int st = 0; st < ST_COUNT; st++)
				if (stageLock[st] == id)
					stageLock[st] = -1;
			pool[id].active = false;
		}

		// Fetch: up to two instructions per cycle, and only into a free I stage.
		int inIStage = 0;
		for (int id : inFlight)
			if (pool[id].stage() == ST_I)
				inIStage++;

		for (int pipe = 0; pipe < 2 - inIStage && pc != count; pipe++)
		{
			const u32 i = pc++;
			Insn &in = insns[i];
			in.programOrder = programOrder;
			in.availCycle = INT_MAX;	// until it issues
			in.dEnter = -1;

			for (int s = 0; s < in.nseq; s++)
			{
				Seq &sq = pool[in.firstSeq + s];
				sq.steps = seqSteps(i, s);
				sq.len = seqLen(i, s);
				sq.step = 0;
				sq.programOrder = programOrder + s;
				sq.insnIdx = (int)i;
				sq.stall = false;
				sq.active = false;
				sq.background = in.bgFrom != 0xff && s >= in.bgFrom;
				sq.group = in.info != nullptr ? in.info->unit : CO;
				sq.reads = in.reads;
				sq.writes = in.writes;
			}
			programOrder += in.nseq;

			int first = in.firstSeq;
			pool[first].active = true;
			inFlight.push_back(first);

			// Register the sequence that produces this instruction's result,
			// so later instructions can see they have to wait for it.
			int resultSeq = -1;
			for (int s = 0; s < in.nseq && resultSeq < 0; s++)
			{
				const Seq &sq = pool[in.firstSeq + s];
				for (int k = 0; k < sq.len; k++)
					if (sq.steps[k] & F_RESULT)
					{
						resultSeq = in.firstSeq + s;
						break;
					}
			}
			if (resultSeq >= 0 && in.writes != 0)
				for (int r = 0; r < 64; r++)
					if (in.writes & (1ull << r))
						provides[r].push_back(resultSeq);
		}

		if (trace && cycle < 40)
		{
			fprintf(stderr, "c%-3u issued=%d ", cycle, issuedThisCycle);
			for (int id : inFlight)
			{
				const Seq &q = pool[id];
				static const char *SN[] = {"I","D","EX","SX","F0","F1","F2","F3",
						"?","?","?","?"};
				fprintf(stderr, "[i%d:%s%s]", q.insnIdx,
						q.stage() >= 0 && q.stage() < 8 ? SN[q.stage()] : "?",
						q.stall ? "*" : "");
			}
			fprintf(stderr, "  reason=%s insn=%d\n",
					stallReasonName(cycleReason),
					cycleReasonInsn == INT_MAX ? -1 : cycleReasonInsn);
		}

		if (issuedThisCycle > 0)
			res.issueSlots++;
		// Credit the pair to the YOUNGER of the two, so pairFrom selects on the
		// instruction that did the pairing-up rather than on the one it caught.
		if (issuedThisCycle == 2
				&& (u32)std::max(issuedIdx[0], issuedIdx[1]) >= pairFrom)
			res.parallelIssues++;

		// One cycle, one entry, and only when the cycle issued NOTHING.
		//
		// A cycle in which the oldest sequence is blocked but something behind
		// it issues anyway is not a freeze - the machine made progress in it.
		// Charging those was costing 2.9x: hardware's freeze counters plus its
		// issue-slot counter add up to its cycle count (1.024 on texture2d,
		// 1.015 on pvr_dma, the excess being cycles where two freeze reasons
		// overlap and both counters tick), which they could not do if a freeze
		// cycle could also be an issue cycle.
		//
		// This also makes stallCycles + issueSlots == cycles here, which is a
		// stronger identity than the old stallCycles + issueCycles() == cycles
		// and is the one hardware satisfies.
		if (cycleReason != StallReason::None && issuedThisCycle == 0)
		{
			res.stallCycles++;
			res.byReason[(int)cycleReason]++;
			if (cycleReasonBlame >= 0
					&& (cycleReason == StallReason::FlowDep
						|| cycleReason == StallReason::FpuDep))
			{
				const Insn &pr = insns[cycleReasonBlame];
				const int resid = pr.availCycle == INT_MAX || pr.dEnter < 0
						? 0
						: (pr.availCycle - (pr.info != nullptr ? pr.info->latency : 0))
							- pr.dEnter;
				if (resid <= 1)
					res.stallsProducerFast++;
				else
					res.stallsProducerHeld++;
			}
			if ((u32)cycleReasonInsn >= pairFrom && cycleReasonBlame >= 0
					&& (u32)cycleReasonBlame < pairFrom)
				res.wrapStalls++;
		}

		if (!anyAdvanced && pc == count)
		{
			if (++stuckCycles > 64)
			{
				res.stuck = true;
				break;
			}
		}
		else
			stuckCycles = 0;

		cycle++;
	}

	if (cycle >= MAX_CYCLES)
		res.stuck = true;
	res.cycles = cycle;

	if (detail != nullptr)
		for (u32 i = 0; i < count; i++)
		{
			detail[i].offset = i * 2;
			detail[i].op = ops[i];
			detail[i].stallCycles = stallOf[i];
			detail[i].reason = reasonOf[i];
			detail[i].blamedBy = blameOf[i] < 0 ? 0xff : (u8)blameOf[i];
		}

	return res;
}

}
