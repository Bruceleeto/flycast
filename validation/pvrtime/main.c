/*
	pvrtime - how long does the PVR take to render a scene?

	flycast answers this with one line (core/hw/pvr/spg.cpp, scheduleRenderDone):

	    cycles = min(450000 + size * 100, 1500000)

	where `size` is the byte length of the TA display list. A 450000-cycle
	floor, linear in vertex volume, capped - and no term for how many pixels
	were actually drawn. On texture2d that model spends 2.06M cycles per frame
	where a Dreamcast spends 401K.

	This probe measures the real function. KOS timestamps the PVR's own
	RNDSTART and RNDDONE interrupts, so `rnd_last_time` is the render itself
	rather than anything the CPU did around it - and the same binary run under
	flycast reports flycast's model instead, which makes the two directly
	comparable.

	Three sweeps, chosen so the terms separate:

	  geom   many tiny quads. Vertex volume grows, pixels drawn stay near zero,
	         so this isolates the per-vertex cost - the only thing flycast
	         currently models.
	  fill   one quad, growing from small to full screen. Vertex volume is
	         constant, so anything that moves is fill rate.
	  over   N full-screen quads stacked, in the opaque list and again in the
	         translucent list. The PVR defers and hidden-surface-removes, so
	         opaque overdraw should stay cheap while translucent should not.
	         If that holds, a fill term has to know which list it is costing.

	Output is one DCPVR line per configuration, all numbers per frame.
*/
#include <kos.h>
#include <dc/pvr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Frames per configuration. The first few after a change are not
// representative - the buffers are still being cycled - so the reported figure
// is the median of the last MEASURE of them.
#define WARM    6
#define MEASURE 9
#define FRAMES  (WARM + MEASURE)

#define MAX_QUADS 2048

static uint64_t samples[MEASURE];

static int cmp_u64(const void *a, const void *b)
{
	const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

// One quad, as a four-vertex strip. Flat colour and no texture: this is
// measuring the renderer, and a texture fetch would add a term we are not
// trying to separate here.
static void quad(float x, float y, float w, float h, float z, uint32_t argb)
{
	pvr_vertex_t v;
	memset(&v, 0, sizeof(v));
	v.argb = argb;
	v.oargb = 0;
	v.z = z;

	v.flags = PVR_CMD_VERTEX;
	v.x = x;       v.y = y + h;  pvr_prim(&v, sizeof(v));
	v.x = x;       v.y = y;      pvr_prim(&v, sizeof(v));
	v.x = x + w;   v.y = y + h;  pvr_prim(&v, sizeof(v));
	v.flags = PVR_CMD_VERTEX_EOL;
	v.x = x + w;   v.y = y;      pvr_prim(&v, sizeof(v));
}

// Runs one configuration and prints its line.
//   list   PVR_LIST_OP_POLY or PVR_LIST_TR_POLY
//   n      number of quads
//   w,h    size of each in pixels
//   stack  true to draw them all on top of each other (overdraw) rather than
//          tiled across the screen
static void sweep(const char *name, int list, int n, float w, float h, int stack)
{
	pvr_poly_cxt_t cxt;
	pvr_poly_hdr_t hdr;
	pvr_stats_t st;

	if (n > MAX_QUADS)
		n = MAX_QUADS;

	pvr_poly_cxt_col(&cxt, list);
	pvr_poly_compile(&hdr, &cxt);

	// Translucent quads have to actually blend or the comparison with the
	// opaque list means nothing.
	const uint32_t argb = list == PVR_LIST_TR_POLY ? 0x804080ff : 0xff4080ff;

	size_t vtx = 0;
	uint64_t reg = 0;
	// Renders actually started, against vblanks that went by, across the whole
	// configuration. KOS only clears render_completed in the vblank handler
	// and refuses to start a render while it is set, which reads like a hard
	// one-render-per-vblank gate - but texture2d runs at 82 loop iterations a
	// second on hardware against 60 in flycast, so something about that is not
	// what it looks like. These two counters say which.
	pvr_stats_t st0;
	pvr_get_stats(&st0);
	const size_t frame0 = st0.frame_count, vbl0 = st0.vbl_count;
	const uint64_t wall0 = timer_ns_gettime64();

	for (int f = 0; f < FRAMES; f++)
	{
		pvr_wait_ready();
		pvr_scene_begin();
		pvr_list_begin(list);
		pvr_prim(&hdr, sizeof(hdr));

		for (int i = 0; i < n; i++)
		{
			float x, y;
			if (stack)
			{
				// All on the same pixels. Depth is staggered so nothing is
				// rejected as coincident.
				x = 0.0f;
				y = 0.0f;
			}
			else
			{
				// Tiled across the screen, wrapping. Keeps the quads on
				// screen so they are actually rasterised.
				const int perRow = (int)(640.0f / (w > 0.0f ? w : 1.0f));
				const int cols = perRow > 0 ? perRow : 1;
				x = (float)((i % cols) * (int)w);
				y = (float)(((i / cols) * (int)h) % 480);
			}
			quad(x, y, w, h, 1.0f + (float)i * 0.01f, argb);
		}

		pvr_list_finish();
		pvr_scene_finish();

		if (f >= WARM)
		{
			pvr_get_stats(&st);
			samples[f - WARM] = st.rnd_last_time;
			vtx = st.vtx_buffer_used;
			reg = st.reg_last_time;
		}
	}

	qsort(samples, MEASURE, sizeof(samples[0]), cmp_u64);

	pvr_get_stats(&st);
	const uint64_t wall = timer_ns_gettime64() - wall0;

	printf("DCPVR %s list=%s quads=%d w=%d h=%d px=%d vtxbytes=%u "
			"rnd_ns=%llu rnd_lo=%llu rnd_hi=%llu reg_ns=%llu "
			"loops=%d renders=%u vblanks=%u wall_ns=%llu\n",
			name, list == PVR_LIST_TR_POLY ? "tr" : "op",
			n, (int)w, (int)h, (int)(w * h) * n, (unsigned)vtx,
			(unsigned long long)samples[MEASURE / 2],
			(unsigned long long)samples[0],
			(unsigned long long)samples[MEASURE - 1],
			(unsigned long long)reg,
			FRAMES,
			(unsigned)(st.frame_count - frame0),
			(unsigned)(st.vbl_count - vbl0),
			(unsigned long long)wall);
}

int main(int argc, char **argv)
{
	// Both lists enabled so the translucent sweep has somewhere to go. A
	// generous vertex buffer: the geometry sweep needs 2048 quads of four
	// 32-byte vertices, and running out would silently truncate the scene.
	pvr_init_params_t params = {
		// opaque polys, opaque modifiers, TRANSLUCENT POLYS, translucent
		// modifiers, punch-thru. Index 2 is the translucent list: enabling
		// index 3 instead left it disabled and every tr sweep silently
		// reported the previous configuration's numbers.
		{ PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
		1024 * 1024,
		0,	// no DMA: it would put a second variable in the registration time
		0,	// no FSAA
		0,	// autosort
		2	// extra OPBs
	};
	pvr_init(&params);
	pvr_set_bg_color(0.0f, 0.0f, 0.0f);

	printf("DCPVR begin warm=%d measure=%d\n", WARM, MEASURE);

	// 1. Vertex volume, with the pixel count held near zero.
	for (int n = 16; n <= MAX_QUADS; n *= 2)
		sweep("geom", PVR_LIST_OP_POLY, n, 2.0f, 2.0f, 0);

	// 2. Fill, with the vertex volume held at one quad.
	{
		static const int side[] = { 8, 32, 64, 128, 256, 480 };
		for (unsigned i = 0; i < sizeof(side) / sizeof(side[0]); i++)
			sweep("fill", PVR_LIST_OP_POLY, 1, (float)side[i], (float)side[i], 0);
		// Full screen, the largest single-quad case there is.
		sweep("fill", PVR_LIST_OP_POLY, 1, 640.0f, 480.0f, 0);
	}

	// 3. Overdraw, opaque then translucent. The difference between the two is
	// the question: a deferred renderer should reject the opaque ones.
	for (int n = 1; n <= 32; n *= 2)
		sweep("over", PVR_LIST_OP_POLY, n, 640.0f, 480.0f, 1);
	for (int n = 1; n <= 32; n *= 2)
		sweep("over", PVR_LIST_TR_POLY, n, 640.0f, 480.0f, 1);

	// 4. A shape resembling the texture2d workload, as a cross-check that the
	// fitted model predicts a scene it was not fitted on.
	sweep("mix", PVR_LIST_OP_POLY, 400, 32.0f, 32.0f, 0);
	sweep("mix", PVR_LIST_TR_POLY, 400, 32.0f, 32.0f, 0);

	printf("DCPVR end\n");
	return 0;
}
