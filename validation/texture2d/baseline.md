# texture2d - hardware baseline

Dreamcast, KallistiOS v2.3.0 (4fdde8b2), sh-elf-gcc 17.0.0.
Captured with `dc-tool-ip -x rayimages.elf`, built with `-DDCBENCH_BUILD`.
Raw output is in `hardware.txt`; this file is the readable version of it.

The workload is 18 textures blitted from a 400-entry circular buffer, 640x480 VGA, no frame limiter. Ten passes of 200 measured frames each, 30 warm-up
frames before every pass, RNG re-seeded per pass so all ten measure the same
frames. The SH4 has two performance counters, hence the passes.

**This capture is tied to this exact ELF.** Most of the cache misses here are
conflict misses, which move with code layout: rebuild the example and these
numbers need recapturing. `hardware.txt` and the binary go together.

## Counters, 200 frames

| event | count | per frame |
|---|---:|---:|
| cycles              |  466192668 |   2330963 |
| instructions        |  210278945 |   1051395 |
| parallel_issued     |   75741253 |    378706 |
| icache_miss         |    3870057 |     19350 |
| ocache_miss         |    3613915 |     18070 |
| ocache_read_miss    |    2044192 |     10221 |
| ocache_write_miss   |    1569838 |      7849 |
| icache_stall_cycles |  105620553 |    528103 |
| dcache_stall_cycles |   98339973 |    491700 |
| icache_fill_cycles  |   71755586 |    358778 |
| ocache_fill_cycles  |   72461102 |    362306 |
| reg_stall_cycles    |   21754222 |    108771 |
| fpu_stall_cycles    |   32834218 |    164171 |
| branch_stall_cycles |    8346245 |     41731 |
| ifetch              |  169863545 |    849318 |
| operand_access      |  112631124 |    563156 |
| operand_read        |   67695273 |    338476 |
| branch_taken        |   15640905 |     78205 |
| branch_issued       |   25784503 |    128923 |
| io_access           |      17223 |        86 |

`instructions` is the raw `INSTRUCTION_ISSUED` counter, which counts issue
SLOTS, not instructions. A cycle that issued two increments it once and
`parallel_issued` once, so the instruction count is the sum of the two. Compare
a simulator against the sum, never against either half.

`cycles` is `ELAPSED_TIME`, which stops while the CPU is halted. It is not wall
clock: `wall_ns` in `hardware.txt` is.

## Ratios

These are what a simulator has to reproduce. Raw counts move with any change to
the example; these do not.

| ratio | hardware |
|---|---:|
| total instructions (issue slots + parallel) | 286020198 |
| true IPC (instructions / cycles) | 0.6135 |
| issue slots per cycle | 0.4511 |
| paired share (parallel / instructions) | 26.5% |
| icache miss rate (/ ifetch) | 2.28% |
| ocache miss rate (/ operand_access) | 3.21% |
| write share of ocache misses | 43.4% |
| read share of operand accesses | 60.1% |
| freeze per icache miss | 27.29 cyc |
| freeze per ocache miss | 27.21 cyc |
| **fill** per icache miss | 18.54 cyc |
| **fill** per ocache miss | 20.05 cyc |
| extra freeze per ocache WRITE miss | 16.49 cyc |
| cache stall share of frame | 43.8% |
| register stall share of frame | 4.7% |
| fpu stall share of frame | 7.0% |
| branch stall share of frame | 1.79% |
| cycles per taken branch (freeze) | 0.53 cyc |
| instructions per branch issued | 11.09 |
| instructions per branch taken | 18.29 |
| branch taken rate | 60.7% |
| instructions per operand access | 2.539 |

## Fill against freeze

The two are different counters and they do not agree, which is the useful part.
A **fill** occupies the bus. A **freeze** is what the CPU lost waiting. The gap
between them is everything the write-back buffer and the fill-around managed to
hide - or, on the instruction side, everything that is not the fill at all.

Operand side, across this example and the other one:

- fill per miss is flat at ~20.4 cycles even though the two examples' write
  share of misses differs by 2.7x. So the fill applies to every miss, reads and
  writes alike.
- the remaining freeze lands entirely on write misses, at ~15.2 cycles each.

That gives the two-term law the cache model now uses: every operand miss costs
20.6 cycles, a write miss costs 15.2 more. Fitted on two workloads, and the
flat fill column confirms the first term independently.

Instruction side, freeze per miss is 27.2 in both examples - agreeing to 0.3%
across workloads with a 32x difference in miss count. Fill is only ~18, so
about 9 cycles per miss is fetch restart rather than the fill.

## What this workload is

- **Nearly half of it is memory.** Cache freeze is 43.8% of the frame, split
  almost evenly between instruction and operand side. Whatever else this
  example tests, it is mostly a cache benchmark.
- **It is write-heavy for a blitter**: 43.4% of operand misses are writes,
  against 16.3% in `pvr_dma`. That difference is what made the two examples
  disagree about the cost of an operand miss (27.21 against 23.08) until the
  cost was split into a fill term and a write-back term. Two workloads this far
  apart in mix are why the split could be fitted at all - keep it that way.
- **It does not wait on the graphics hardware.** 86 on-chip IO accesses per
  frame is nothing. There is no PVR poll loop, so every cycle above is the CPU
  actually running code, which is what makes this a usable CPU measurement.
- **The FPU stalls more than the register file does** - 7.0% against 4.7%.
- **Branches are nearly free**: 1.79% of the frame, 0.53 cycles per taken
  branch. Worth knowing because it rules out branch refill as an explanation
  for anything large.
- 466M cycles over 200 frames at 200MHz is 2.33s of CPU against 3.34s of wall
  clock, so about 30% of the time the CPU is halted waiting for vblank.
