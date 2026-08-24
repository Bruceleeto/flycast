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
};

Result analyze(const u16 *ops, u32 count, InsnDetail *detail)
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

	auto providedByOther = [&](u64 regs, const Seq &s) -> int {
		if (regs == 0)
			return -1;
		for (int r = 0; r < 64; r++)
		{
			if ((regs & (1ull << r)) == 0)
				continue;
			for (int id : provides[r])
			{
				const Seq &p = pool[id];
				if (p.insnIdx != s.insnIdx && p.programOrder < s.programOrder)
					return p.insnIdx;
			}
		}
		return -1;
	};

	while (cycle < MAX_CYCLES)
	{
		if (inFlight.empty() && pc == count)
			break;

		int prevStall = -1;
		bool anyAdvanced = false;
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
				reason = StallReason::StageLocked;
				blame = pool[stageLock[nextStage]].insnIdx;
			}
			else if (nextStage != ST_D && !allParallel)
			{
				reason = StallReason::ResourceHazard;
				blame = hazard;
				seq.stall = true;
			}
			else if ((curStage != ST_I || (seq.reads != 0 && insns[seq.insnIdx].info != nullptr
						&& insns[seq.insnIdx].info->latency == 0))
					&& (blame = providedByOther(seq.reads, seq)) >= 0)
			{
				reason = StallReason::FlowDep;
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
				res.stallCycles++;
				res.byReason[(int)reason]++;
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

		for (int id : toResult)
		{
			const Seq &s = pool[id];
			for (int r = 0; r < 64; r++)
				if (s.writes & (1ull << r))
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
