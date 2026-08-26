# pvrtime - hardware baseline

Dreamcast, KallistiOS v2.3.0 (4fdde8b2). Render time measured between the PVR's
own RNDSTART and RNDDONE interrupts, so it is the render itself and not
anything the CPU did around it. Untextured flat-shaded quads: this measures the
renderer, and a texture fetch would add a term the sweeps do not separate.

## The model

    rnd_ns = 3123187 + 10.735 * ta_bytes + 10.13 * translucent_pixels

Three terms, each measured independently, and the third does not exist in
flycast at all.

### 1. A fixed floor of about 3.12 ms

Every render costs this before anything is drawn. It is the full framebuffer
being processed tile by tile, and it does not move with scene content.

### 2. Linear in TA data volume, 10.735 ns per byte

Fitted over a 128x range, +/- 1.2%:

| quads | vtx bytes | measured ns | fitted | err |
|---:|---:|---:|---:|---:|
| 16   |   1216 | 3165840 | 3136240 | -0.9% |
| 32   |   2432 | 3174160 | 3149294 | -0.8% |
| 64   |   4864 | 3191040 | 3175400 | -0.5% |
| 128  |   9728 | 3227840 | 3227613 | -0.0% |
| 256  |  19456 | 3294320 | 3332039 | +1.1% |
| 512  |  38912 | 3499440 | 3540892 | +1.2% |
| 1024 |  77824 | 3946640 | 3958596 | +0.3% |
| 2048 | 155648 | 4816640 | 4794005 | -0.5% |

### 3. Translucent fill: one pixel per clock. Opaque fill: free.

This is the important one, and the two lists could not behave more differently.

**Opaque** - one quad swept 8x8 to 640x480, a 4800x range in pixels:

| size | pixels | rnd_ns |
|---|---:|---:|
| 8x8 | 64 | 3154880 |
| 128x128 | 16384 | 3155920 |
| 640x480 | 307200 | 3155840 |

Flat to 0.03%. Hidden-surface removal does its job: opaque pixels are free.

**Translucent** - N full-screen quads stacked:

| quads | pixels | rnd_ns | delta per quad |
|---:|---:|---:|---:|
| 1  |  307200 |   6262160 | - |
| 2  |  614400 |   9373680 | 3111520 |
| 4  | 1228800 |  15597440 | 3111880 |
| 8  | 2457600 |  28045200 | 3111940 |
| 16 | 4915200 |  52940400 | 3111900 |
| 32 | 9830400 | 102730880 | 3111905 |

Linear to four significant figures. 3111894 ns per 307200 pixels is
**10.13 ns/pixel = 98.7 Mpixel/s**, which is one pixel per clock at the
documented 100MHz draw clock. Even the first translucent quad costs a full
pass, because a translucent polygon is always blended and never rejected.

### The model predicts a case it was not fitted on

`mix` is 400 32x32 quads - deliberately shaped like the texture2d workload and
excluded from every fit above:

    opaque   measured 3260720
    translucent predicted 3260720 + 409600 px * 10.13 = 7409912
    translucent measured  7316640              -> 1.3% error

## Opaque overdraw has a capacity cliff

| quads | rnd_ns | delta |
|---:|---:|---:|
| 1  | 3156320 | - |
| 2  | 3160000 | +3680 |
| 4  | 3171200 | +11200 |
| 8  | 3190160 | +18960 |
| 16 | 3321680 | +131520 |
| 32 | 6360160 | **+3038480** |

Doubling 16 to 32 costs 23x what the previous doubling did, on 2432 bytes of
data. Not fill, not vertex volume - a capacity cliff, almost certainly object
pointer buffer overflow forcing extra passes. Nothing derived from the byte
count can predict it, and the model above does not try.

## Frame pacing

Every configuration reports `loops=15 renders=15 vblanks=15`: one loop, one
render, one vblank, in lockstep at 60.0/sec. Flycast reports exactly the same.
So the raw KOS PVR path is vblank-locked on both machines and they agree.

That matters because texture2d runs its loop at 81.9/sec on hardware and 59.94
in flycast. Whatever causes that divergence is in GLdc's swap path, not in
KOS's `pvr_wait_ready` and not in render duration - this probe rules both out.

Configurations whose render exceeds a vblank period report more vblanks than
renders (`tr quads=8` shows 28 vblanks for 15 renders), which is the expected
behaviour and confirms the counters mean what they should.
