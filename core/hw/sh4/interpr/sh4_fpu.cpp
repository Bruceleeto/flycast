#include "types.h"
#include <cmath>

#include "sh4_opcodes.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_fpu_approx.h"
#include <cstring>

static u32 GetN(u32 op) {
	return (op >> 8) & 0xf;
}
static u32 GetM(u32 op) {
	return (op >> 4) & 0xf;
}

static double getDRn(Sh4Context *ctx, u32 op) {
	return ctx->getDR((op >> 9) & 7);
}
static double getDRm(Sh4Context *ctx, u32 op) {
	return ctx->getDR((op >> 5) & 7);
}
static void setDRn(Sh4Context *ctx, u32 op, double d) {
	ctx->setDR((op >> 9) & 7, d);
}

static void iNimp(const char *str);

// FIPR, FTRV, FSCA and FSRRA use the bit-exact models in sh4_fpu_approx.h (verified against a real
// Dreamcast): result bits, FPSCR cause/flag update and FPU exception trap (the destination is then unchanged).
static u32 floatBits(float f) {
	u32 u;
	memcpy(&u, &f, sizeof(u));
	return u;
}
static float bitsFloat(u32 u) {
	float f;
	memcpy(&f, &u, sizeof(f));
	return f;
}
static void fpuTrap(Sh4Context *ctx) {
	throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
}

#define CHECK_FPU_32(v) v = fixNaN(v)

//fadd <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0000)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		ctx->fr[n] += ctx->fr[m];
		CHECK_FPU_32(ctx->fr[n]);
	}
	else
	{
		double d = getDRn(ctx, op) + getDRm(ctx, op);
		d = fixNaN64(d);
		setDRn(ctx, op, d);
	}
}

//fsub <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0001)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		ctx->fr[n] -= ctx->fr[m];
		CHECK_FPU_32(ctx->fr[n]);
	}
	else
	{
		double d = getDRn(ctx, op) - getDRm(ctx, op);
		d = fixNaN64(d);
		setDRn(ctx, op, d);
	}
}
//fmul <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0010)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		ctx->fr[n] *= ctx->fr[m];
		CHECK_FPU_32(ctx->fr[n]);
	}
	else
	{
		double d = getDRn(ctx, op) * getDRm(ctx, op);
		d = fixNaN64(d);
		setDRn(ctx, op, d);
	}
}
//fdiv <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0011)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		ctx->fr[n] /= ctx->fr[m];

		CHECK_FPU_32(ctx->fr[n]);
	}
	else
	{
		double d = getDRn(ctx, op) / getDRm(ctx, op);
		d = fixNaN64(d);
		setDRn(ctx, op, d);
	}
}
//fcmp/eq <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0100)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		ctx->sr.T = ctx->fr[m] == ctx->fr[n];
	}
	else
	{
		ctx->sr.T = getDRn(ctx, op) == getDRm(ctx, op);
	}
}
//fcmp/gt <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0101)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		if (ctx->fr[n] > ctx->fr[m])
			ctx->sr.T = 1;
		else
			ctx->sr.T = 0;
	}
	else
	{
		ctx->sr.T = getDRn(ctx, op) > getDRm(ctx, op);
	}
}
//All memory opcodes are here
//fmov.s @(R0,<REG_M>),<FREG_N>
sh4op(i1111_nnnn_mmmm_0110)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		ctx->fr_hex(n) = ReadMem32(ctx->r[m] + ctx->r[0]);
	}
	else
	{
		u32 n = GetN(op)>>1;
		u32 m = GetM(op);
		if (((op >> 8) & 1) == 0)
			ctx->dr_hex(n) = ReadMem64(ctx->r[m] + ctx->r[0]);
		else
			ctx->xd_hex(n) = ReadMem64(ctx->r[m] + ctx->r[0]);
	}
}


//fmov.s <FREG_M>,@(R0,<REG_N>)
sh4op(i1111_nnnn_mmmm_0111)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		WriteMem32(ctx->r[0] + ctx->r[n], ctx->fr_hex(m));
	}
	else
	{
		u32 n = GetN(op);
		u32 m = GetM(op)>>1;
		if (((op >> 4) & 0x1) == 0)
			WriteMem64(ctx->r[n] + ctx->r[0], ctx->dr_hex(m));
		else
			WriteMem64(ctx->r[n] + ctx->r[0], ctx->xd_hex(m));
	}
}


//fmov.s @<REG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_1000)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		ctx->fr_hex(n) = ReadMem32(ctx->r[m]);
	}
	else
	{
		u32 n = GetN(op)>>1;
		u32 m = GetM(op);
		if (((op >> 8) & 1) == 0)
			ctx->dr_hex(n) = ReadMem64(ctx->r[m]);
		else
			ctx->xd_hex(n) = ReadMem64(ctx->r[m]);
	}
}


//fmov.s @<REG_M>+,<FREG_N>
sh4op(i1111_nnnn_mmmm_1001)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		ctx->fr_hex(n) = ReadMem32(ctx->r[m]);
		ctx->r[m] += 4;
	}
	else
	{
		u32 n = GetN(op)>>1;
		u32 m = GetM(op);
		if (((op >> 8) & 1) == 0)
			ctx->dr_hex(n) = ReadMem64(ctx->r[m]);
		else
			ctx->xd_hex(n) = ReadMem64(ctx->r[m]);
		ctx->r[m] += 8;
	}
}


//fmov.s <FREG_M>,@<REG_N>
sh4op(i1111_nnnn_mmmm_1010)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		WriteMem32(ctx->r[n], ctx->fr_hex(m));
	}
	else
	{
		u32 n = GetN(op);
		u32 m = GetM(op)>>1;

		if (((op >> 4) & 0x1) == 0)
			WriteMem64(ctx->r[n], ctx->dr_hex(m));
		else
			WriteMem64(ctx->r[n], ctx->xd_hex(m));
	}
}

//fmov.s <FREG_M>,@-<REG_N>
sh4op(i1111_nnnn_mmmm_1011)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		u32 addr = ctx->r[n] - 4;

		WriteMem32(addr, ctx->fr_hex(m));

		ctx->r[n] = addr;
	}
	else
	{
		u32 n = GetN(op);
		u32 m = GetM(op)>>1;

		u32 addr = ctx->r[n] - 8;
		if (((op >> 4) & 0x1) == 0)
			WriteMem64(addr, ctx->dr_hex(m));
		else
			WriteMem64(addr, ctx->xd_hex(m));

		ctx->r[n] = addr;
	}
}

//end of memory opcodes

//fmov <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_1100)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		ctx->fr[n] = ctx->fr[m];
	}
	else
	{
		u32 n = GetN(op)>>1;
		u32 m = GetM(op)>>1;
		switch ((op >> 4) & 0x11)
		{
			case 0x00:
				//dr[n] = dr[m];
				ctx->dr_hex(n) = ctx->dr_hex(m);
				break;

			case 0x01:
				//dr[n] = xd[m];
				ctx->dr_hex(n) = ctx->xd_hex(m);
				break;

			case 0x10:
				//xd[n] = dr[m];
				ctx->xd_hex(n) = ctx->dr_hex(m);
				break;

			case 0x11:
				//xd[n] = xd[m];
				ctx->xd_hex(n) = ctx->xd_hex(m);
				break;
		}
	}
}


//fabs <FREG_N>
sh4op(i1111_nnnn_0101_1101)
{
	int n=GetN(op);

	if (ctx->fpscr.PR == 0)
		ctx->fr_hex(n) &= 0x7FFFFFFF;
	else
		ctx->fr_hex(n & 0xE) &= 0x7FFFFFFF;

}

//FSCA FPUL, DRn//F0FD//1111_nnn0_1111_1101
sh4op(i1111_nnn0_1111_1101)
{
	int n = GetN(op) & 0xE;
	u32 s = floatBits(ctx->fr[n]), c = floatBits(ctx->fr[n + 1]);
	if (sh4_fsca_ex(ctx->fpul, &ctx->fpscr.full, &s, &c))
		fpuTrap(ctx);
	ctx->fr[n] = bitsFloat(s);
	ctx->fr[n + 1] = bitsFloat(c);
}

//FSRRA //1111_nnnn_0111_1101
sh4op(i1111_nnnn_0111_1101)
{
	u32 n = GetN(op);
	u32 r;
	int trap = sh4_fsrra_ex(floatBits(ctx->fr[n]), &ctx->fpscr.full, &r);
	ctx->fr[n] = bitsFloat(r);	// unchanged on a trap or with PR=1
	if (trap)
		fpuTrap(ctx);
}

//fcnvds <DR_N>,FPUL
sh4op(i1111_nnnn_1011_1101)
{

	if (ctx->fpscr.PR == 1)
	{
		u32 *p = &ctx->fpul;
		*((float *)p) = (float)getDRn(ctx, op);
	}
	else
	{
		iNimp("FCNVDS: Single precision mode");
	}
}


//fcnvsd FPUL,<DR_N>
sh4op(i1111_nnnn_1010_1101)
{
	if (ctx->fpscr.PR == 1)
	{
		u32 *p = &ctx->fpul;
		setDRn(ctx, op, (double)*((float *)p));
	}
	else
	{
		iNimp("FCNVSD: Single precision mode");
	}
}

//fipr <FV_M>,<FV_N>
sh4op(i1111_nnmm_1110_1101)
{
	int n = GetN(op) & 0xC;
	int m = (GetN(op) & 0x3) << 2;
	u32 fvm[4], fvn[4], r;
	for (int i = 0; i < 4; i++)
	{
		fvm[i] = floatBits(ctx->fr[m + i]);
		fvn[i] = floatBits(ctx->fr[n + i]);
	}
	r = fvn[3];
	if (sh4_fipr_ex(fvm, fvn, &ctx->fpscr.full, &r))
		fpuTrap(ctx);
	ctx->fr[n + 3] = bitsFloat(r);
}

//fldi0 <FREG_N>
sh4op(i1111_nnnn_1000_1101)
{
	if (ctx->fpscr.PR!=0)
		return;

	u32 n = GetN(op);

	ctx->fr[n] = 0.0f;

}

//fldi1 <FREG_N>
sh4op(i1111_nnnn_1001_1101)
{
	if (ctx->fpscr.PR!=0)
		return;

	u32 n = GetN(op);

	ctx->fr[n] = 1.0f;
}

//flds <FREG_N>,FPUL
sh4op(i1111_nnnn_0001_1101)
{
	u32 n = GetN(op);

	ctx->fpul = ctx->fr_hex(n);
}

//fsts FPUL,<FREG_N>
sh4op(i1111_nnnn_0000_1101)
{
	u32 n = GetN(op);
	ctx->fr_hex(n) = ctx->fpul;
}

//float FPUL,<FREG_N>
sh4op(i1111_nnnn_0010_1101)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		ctx->fr[n] = (float)(int)ctx->fpul;
	}
	else
	{
		setDRn(ctx, op, (double)(int)ctx->fpul);
	}
}


//fneg <FREG_N>
sh4op(i1111_nnnn_0100_1101)
{
	u32 n = GetN(op);
	ctx->fr_hex(n) ^= 0x80000000;
}


//frchg
sh4op(i1111_1011_1111_1101)
{
 	ctx->fpscr.FR = 1 - ctx->fpscr.FR;

	Sh4Context::UpdateFPSCR(ctx);
}

//fschg
sh4op(i1111_0011_1111_1101)
{
	ctx->fpscr.SZ = 1 - ctx->fpscr.SZ;
}

//fsqrt <FREG_N>
sh4op(i1111_nnnn_0110_1101)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);

		ctx->fr[n] = sqrtf(ctx->fr[n]);
		CHECK_FPU_32(ctx->fr[n]);
	}
	else
	{
		setDRn(ctx, op, fixNaN64(sqrt(getDRn(ctx, op))));
	}
}


//ftrc <FREG_N>, FPUL
sh4op(i1111_nnnn_0011_1101)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		if (std::isnan(ctx->fr[n])) {
			ctx->fpul = 0x80000000;
		}
		else
		{
			ctx->fpul = (u32)(s32)ctx->fr[n];
			if ((s32)ctx->fpul > 0x7fffff80)
				ctx->fpul = 0x7fffffff;
#if HOST_CPU == CPU_X86 || HOST_CPU == CPU_X64
			// Intel CPUs convert out of range float numbers to 0x80000000. Manually set the correct sign
			else if (ctx->fpul == 0x80000000 && ctx->fr[n] > 0)
				ctx->fpul--;
#endif
		}
	}
	else
	{
		f64 f = getDRn(ctx, op);
		if (std::isnan(f)) {
			ctx->fpul = 0x80000000;
		}
		else
		{
			ctx->fpul = (u32)(s32)f;
#if HOST_CPU == CPU_X86 || HOST_CPU == CPU_X64
			// Intel CPUs convert out of range float numbers to 0x80000000. Manually set the correct sign
			if (ctx->fpul == 0x80000000 && f > 0)
				ctx->fpul--;
#endif
		}
	}
}


//fmac <FREG_0>,<FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_1110)
{
	if (ctx->fpscr.PR==0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		ctx->fr[n] = std::fma(ctx->fr[0], ctx->fr[m], ctx->fr[n]);
		CHECK_FPU_32(ctx->fr[n]);
	}
	else
	{
		iNimp("fmac <DREG_0>,<DREG_M>,<DREG_N>");
	}
}


//ftrv xmtrx,<FV_N>
sh4op(i1111_nn01_1111_1101)
{
	/*
	XF[0] XF[4] XF[8] XF[12]    FR[n]      FR[n]
	XF[1] XF[5] XF[9] XF[13]  *	FR[n+1] -> FR[n+1]
	XF[2] XF[6] XF[10] XF[14]   FR[n+2]    FR[n+2]
	XF[3] XF[7] XF[11] XF[15]   FR[n+3]    FR[n+3]
	*/
	u32 n = GetN(op) & 0xC;
	u32 xmtrx[16], fv[4];
	for (int i = 0; i < 16; i++)
		xmtrx[i] = floatBits(ctx->xf[i]);
	for (int i = 0; i < 4; i++)
		fv[i] = floatBits(ctx->fr[n + i]);
	if (sh4_ftrv_ex(xmtrx, fv, &ctx->fpscr.full))
		fpuTrap(ctx);
	for (int i = 0; i < 4; i++)
		ctx->fr[n + i] = bitsFloat(fv[i]);
}

static void iNimp(const char *str)
{
	WARN_LOG(INTERPRETER, "Unimplemented SH4 FPU op: %s", str);
}
