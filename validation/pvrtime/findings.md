# pvrtime - what the comparison says about flycast

## Fixed

`scheduleRenderDone` in `core/hw/pvr/spg.cpp` was:

    cycles = min(450000 + size * 100, 1500000)

Against hardware that is **28% low on the floor and 47x too steep per byte**,
so any real scene pinned the 1.5M cap and every render took 7.5ms. Refitted
against flycast's own `size` - the whole display list, object pointer blocks
included, which runs about 1.7x the vertex bytes a guest sees through
`PVR_TA_VERTBUF_POS`:

    cycles = min(624083 + size * 1.2747, 1500000)

Result, hardware against flycast:

| quads | hardware ns | flycast ns | ratio |
|---:|---:|---:|---:|
| 16   | 3165840 | 3137200 | 0.991 |
| 128  | 3227840 | 3229200 | 1.000 |
| 512  | 3499440 | 3542480 | 1.012 |
| 2048 | 4816640 | 4795920 | 0.996 |
| fill, any size | 3155840 | 3124000 | 0.990 |

Within 1.2% across the whole sweep, where it had been 2.4x out at the floor and
saturated everywhere else.

## Correct as it stands

**No fill term for opaque polygons.** Hardware renders an 8x8 quad and a
640x480 quad in the same 3.156ms. flycast's lack of a fill term is right, not
an omission - modelling opaque fill would be modelling something that does not
exist.

## Not modelled: translucent fill

Hardware charges **10.13 ns per translucent pixel** - one pixel per clock at
100MHz - linear to four significant figures over a 32x range, and it predicts a
held-out scene to 1.3%. flycast charges nothing.

A single full-screen translucent quad is 3.1ms of real render time that flycast
completes instantly. On an alpha-heavy title that is the difference between a
16ms frame and a 60ms one, so this is the largest remaining error in the PVR
model by a wide margin.

**Why it is not fixed here.** `scheduleRenderDone(ctx)` is called from
`rend_start_render` (`Renderer_if.cpp:531`) *before* `ta_parse` runs - at that
point the context holds the raw TA byte stream and nothing else, so there are
no polygon areas to sum. Adding the term needs either a second TA parser or a
reordering so the estimate is made after parsing. That is a refactor rather
than a coefficient change, which is why it is written down here with its
measured coefficient instead of forced in.

## Small gap found on the way

flycast does not implement `PVR_TA_VERTBUF_POS`, so a guest reading
`pvr_stats_t.vtx_buffer_used` gets 0. Harmless for the fit above - the sweeps
know their own vertex counts - but it is the register KOS uses to report how
much vertex data a scene submitted, and any guest profiling itself through KOS
sees a zero.
