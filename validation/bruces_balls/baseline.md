# bruces_balls - hardware baseline

Dreamcast, KallistiOS v2.3.0 (4fdde8b2), SH4ZAM, sh-elf-gcc 17.0.0.
Captured with `dc-tool-ip -x bruces_balls.elf`, built with `-DDCBENCH_BUILD`.
Raw output is in `hardware.txt`; this file is the readable version of it.

80 balls, 64000 triangles, submitted to the TA through the store queues with
KOS's direct-rendering API. Ten passes of 200 measured frames, 30 warm-up
frames before each. Fully deterministic - there is no RNG anywhere and each
ball advances by a fixed per-ball velocity - so re-seeding a pass is just
rebuilding the initial state, and all ten passes measure the same 200 frames.

**This capture is tied to this exact ELF.** Rebuild the example and it needs
recapturing.

## Why this example is worth having

It is the opposite of the other two in almost every dimension, which is what
makes the three of them together able to separate terms that any one of them
would leave tangled:

| | texture2d | pvr_dma | bruces_balls |
|---|---:|---:|---:|
| instruction cache misses | 3.87M | 120K | **55K** |
| operand cache misses | 3.61M | 5.50M | **24.8K** |
| FPU stall share of frame | 7.0% | 9.8% | **31.7%** |
| store queue flushes | few | few | **13.4M** |

Essentially no cache traffic at all - 24.8K operand misses against texture2d's
3.6M. So this measures the pipeline with the cache model switched off, and it
is the only example that drives the store queues hard.

## Counters, 200 frames

| event | count | per frame |
|---|---:|---:|
| cycles              |  596307411 |   2981537 |
| instructions        |  338319552 |   1691598 |
| parallel_issued     |  136825333 |    684127 |
| icache_miss         |      55249 |       276 |
| ocache_miss         |      24786 |       124 |
| ocache_read_miss    |      22714 |       114 |
| ocache_write_miss   |       2111 |        11 |
| icache_stall_cycles |    1453327 |      7267 |
| dcache_stall_cycles |   47464532 |    237323 |
| icache_fill_cycles  |    1000282 |      5001 |
| ocache_fill_cycles  |     506770 |      2534 |
| reg_stall_cycles    |   13602752 |     68014 |
| fpu_stall_cycles    |  189287637 |    946438 |
| branch_stall_cycles |    6546826 |     32734 |
| ifetch              |  275192679 |   1375963 |
| operand_access      |   74706885 |    373534 |
| operand_read        |   47885892 |    239429 |
| branch_taken        |   12717146 |     63586 |
| branch_issued       |   13467289 |     67336 |
| io_access           |      13448 |        67 |

`instructions` is the raw `INSTRUCTION_ISSUED` counter, which counts issue
SLOTS. The instruction count is that plus `parallel_issued`.

## Ratios

| ratio | hardware |
|---|---:|
| total instructions (issue slots + parallel) | 475144885 |
| true IPC | 0.7968 |
| paired share | 28.8% |
| icache miss rate (/ ifetch) | 0.0201% |
| ocache miss rate (/ operand_access) | 0.0332% |
| freeze per icache miss | 26.31 cyc |
| **fill** per icache miss | 18.10 cyc |
| data-side freeze per ocache miss | **1915 cyc** - see below |
| **fill** per ocache miss | 20.45 cyc |
| store queue flushes to TA (whole run) | 13440600 |
| data-side freeze per SQ flush | **3.49 cyc** |
| fpu stall share of frame | **31.7%** |
| register stall share of frame | 2.3% |
| cache stall share of frame | 0.24% (instruction side only) |
| branch stall share of frame | 1.10% |
| cycles per taken branch (freeze) | 0.51 cyc |
| instructions per operand access | 6.360 |

## The store queue, which is what this example is really for

The data-side freeze counter reads 47,464,532 cycles against 24,786 operand
cache misses. That is 1,915 cycles per miss, which is not a cache miss - it is
the store queue, and PMCR 0x25 counts it.

Two things confirm it rather than merely suggesting it:

- **The fill counter.** 0x22 reads 506,770 cycles. So 99% of the data-side
  freeze in this run has no bus fill behind it at all.
- **The flush count.** cachesim counts 13,440,600 flushes to the TA over the
  same 200 frames. Freeze minus fill over flushes is **3.49 cycles per flush**.

That 3.49 does NOT reconcile with `tools/hwprobe/sqsweep`, which swept the gap
between flushes from 16 to 205 cycles and measured a flat 2.0 to both RAM and
the TA. The shortfall is 1.49 cycles per flush, it is a pipeline freeze, and it
does not appear on the bus. Write misses were the previous guess and are ruled
out: there are 2,111 of them in the whole run, worth about 32K cycles of 47.5M.

Still open. The model uses the swept 2.0 rather than the inferred 3.49, so the
gap stays visible as `dcache_stall_cycles` reading 0.57 here instead of being
absorbed into a constant.

## What else this workload says

- **It is a float benchmark.** The FPU stalls for 31.7% of the frame, against
  7-10% in the other two. Nothing else in the suite constrains the FPU model
  this hard.
- **The register file barely stalls at all** - 2.3%, where the FPU is 31.7%.
  The two flow-dependency counters split 7/93 here and 40/60 on texture2d, so
  the pair of examples pins the split from both ends.
- **Hand-scheduled code pairs better than compiled code**: 28.8% of
  instructions are the second of a pair, against texture2d's 26.5% - but less
  than might be expected, and far less than pipesim's 45%.
- **Branches cost 1.10% of the frame**, 0.51 cycles per taken branch,
  consistent with the 1.00 measured directly in `validation/blockentry`.
- 596M cycles over 200 frames at 200MHz is 2.98s of CPU against 3.34s of wall
  clock, so about 11% of the time the CPU is halted - the least idle of the
  three examples.
