# pvr_dma - hardware baseline

Dreamcast, KallistiOS v2.3.0 (4fdde8b2), sh-elf-gcc 17.0.0.
Captured with `dc-tool-ip -x pvr_dma.elf`, built with `-DDCBENCH_BUILD`.
Raw output is in `hardware.txt`; this file is the readable version of it.

The workload is a transformed 3D scene submitted by DMA, scripted orbit camera, 640x480 VGA. Ten passes of 200 measured frames each, 30 warm-up
frames before every pass, RNG re-seeded per pass so all ten measure the same
frames. The SH4 has two performance counters, hence the passes.

**This capture is tied to this exact ELF.** Most of the cache misses here are
conflict misses, which move with code layout: rebuild the example and these
numbers need recapturing. `hardware.txt` and the binary go together.

## Counters, 200 frames

| event | count | per frame |
|---|---:|---:|
| cycles              |  512709514 |   2563548 |
| instructions        |  299731792 |   1498659 |
| parallel_issued     |  135706083 |    678530 |
| icache_miss         |     120022 |       600 |
| ocache_miss         |    5496134 |     27481 |
| ocache_read_miss    |    4600547 |     23003 |
| ocache_write_miss   |     895572 |      4478 |
| icache_stall_cycles |    3264052 |     16320 |
| dcache_stall_cycles |  126861397 |    634307 |
| icache_fill_cycles  |    2043674 |     10218 |
| ocache_fill_cycles  |  114057434 |    570287 |
| reg_stall_cycles    |   26751814 |    133759 |
| fpu_stall_cycles    |   50181301 |    250907 |
| branch_stall_cycles |   13679189 |     68396 |
| ifetch              |  259092807 |   1295464 |
| operand_access      |  171338627 |    856693 |
| operand_read        |  113029562 |    565148 |
| branch_taken        |   18186899 |     90934 |
| branch_issued       |   30048912 |    150245 |
| io_access           |      15022 |        75 |

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
| total instructions (issue slots + parallel) | 435437875 |
| true IPC (instructions / cycles) | 0.8493 |
| issue slots per cycle | 0.5846 |
| paired share (parallel / instructions) | 31.2% |
| icache miss rate (/ ifetch) | 0.05% |
| ocache miss rate (/ operand_access) | 3.21% |
| write share of ocache misses | 16.3% |
| read share of operand accesses | 66.0% |
| freeze per icache miss | 27.20 cyc |
| freeze per ocache miss | 23.08 cyc |
| **fill** per icache miss | 17.03 cyc |
| **fill** per ocache miss | 20.75 cyc |
| extra freeze per ocache WRITE miss | 14.30 cyc |
| cache stall share of frame | 25.4% |
| register stall share of frame | 5.2% |
| fpu stall share of frame | 9.8% |
| branch stall share of frame | 2.67% |
| cycles per taken branch (freeze) | 0.75 cyc |
| instructions per branch issued | 14.49 |
| instructions per branch taken | 23.94 |
| branch taken rate | 60.5% |
| instructions per operand access | 2.541 |

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

- **It is a float benchmark with a cache problem.** The FPU stalls for 9.8% of
  the frame - more than `texture2d` - and the register file for 5.2%, while the
  instruction cache barely misses at all (120K against `texture2d`'s 3.87M, a
  32x difference). The hot loop fits in cache; the data does not.
- **Operand misses are 24.7% of the frame on their own.** This is the example
  that decides whether a data cache model is right.
- **Read-dominated**: 83.7% of operand misses are reads, against `texture2d`'s
  56.6%. The pair of examples spans a 2.7x range in write mix, which is what
  let the operand miss cost be fitted as a fill plus a write-back term rather
  than as one averaged constant.
- **Freeze per operand miss is 23.08 cycles here against 27.21 in
  `texture2d`** - and that is not a contradiction. Both are `20.6 + 15.2 x
  (write share)` to within 1%.
- **The instruction side agrees with `texture2d` to 0.3%** at 27.20 cycles of
  freeze per miss, despite the 32x difference in how many misses there are.
  A per-miss cost that survives that is a per-miss cost.
- 513M cycles over 200 frames at 200MHz is 2.56s of CPU against 3.35s of wall
  clock: about 23% of the time the CPU is halted waiting for vblank.
