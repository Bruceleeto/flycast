# texture2d - hardware baseline

Dreamcast, KallistiOS v2.3.0 (4fdde8b2), GLdc 1.1-698-ga1cd, raylib 5.5.
Captured with `dc-tool-ip -x rayimages.elf`, built with `-DDCBENCH_BUILD`.
Raw output is in `hardware.txt`; this file is the readable version of it.

The workload is 18 textures blitted from a 400-entry circular buffer, 640x480
VGA, no frame limiter. Seven passes of 200 measured frames each, 30 warm-up
frames before every pass, RNG re-seeded per pass so all seven measure the same
frames.

## Repeatability

Two full captures of the same binary agreed to better than 0.001% on
instructions, operand accesses, branches, register stalls, dual issue and
icache misses. Only two counters moved:

| counter | spread between runs |
|---|---:|
| cycles      | 0.9% |
| ocache_miss | 7.0% |

So the workload is deterministic and any divergence larger than this is real.
Those two are the reason `cycles` and `ocache_miss` carry looser tolerances in
`validate.py` than the structural counters do.

## Counters

| event | count | per frame |
|---|---:|---:|
| cycles              | 489282132 | 2446411 |
| instructions        | 210379415 | 1051897 |
| icache_miss         |   4964654 |   24823 |
| ocache_miss         |   3384476 |   16922 |
| icache_stall_cycles | 129840930 |  649205 |
| dcache_stall_cycles |  95707422 |  478537 |
| ifetch              | 167825334 |  839127 |
| operand_access      | 112696753 |  563484 |
| reg_stall_cycles    |  21598698 |  107993 |
| fpu_stall_cycles    |  33618748 |  168094 |
| parallel_issued     |  75698113 |  378491 |
| branch_taken        |  15644053 |   78220 |
| branch_issued       |  25785383 |  128927 |
| io_access           |     14822 |      74 |

## Ratios

These are what a simulator has to reproduce. Raw counts move with any change to
the example; these do not.

| ratio | hardware |
|---|---:|
| IPC (instructions / cycles)          | 0.4300 |
| icache miss rate (/ ifetch)          | 2.96% |
| ocache miss rate (/ operand_access)  | 3.00% |
| cycles per icache miss               | 26.2 |
| cycles per ocache miss               | 28.3 |
| cache stall share of frame           | 46.1% |
| register stall share of frame        | 4.4% |
| fpu stall share of frame             | 6.9% |
| dual-issue rate (parallel / instr)   | 36.0% |
| instructions per branch issued       | 8.16 |
| instructions per branch taken        | 13.45 |
| branch taken rate                    | 60.7% |
| instructions per operand access      | 1.867 |

## What this workload is

- **Half of it is memory.** Cache stalls are 46% of the frame. Whatever else
  this example tests, it is mostly a cache benchmark.
- **A miss costs about 27 cycles.** 26.2 for instruction, 28.3 for operand, and
  the two being so close suggests both are dominated by the same SDRAM latency
  rather than by anything cache-specific.
- **It does not wait on the graphics hardware.** 74 uncached accesses per frame
  is nothing. There is no PVR poll loop here, so the CPU is never idle and
  every cycle above is the CPU actually running code. That is what makes this a
  usable CPU measurement.
- **The FPU stalls more than the register file does** - 6.9% against 4.4%.
- **Dual issue works 36% of the time**, and 61% of branches are taken.
- 489M cycles for 200 frames at 200MHz is 2.45s, about 82fps. There is no frame
  limiter and GLdc does not block on vsync here, so the example is not pinned
  to the refresh rate.

## Note on instructions per operand access

1.867 instructions per operand access is a property of the compiled code and
cannot change between two runs of the same binary. It is therefore the most
useful single consistency check on any simulator's instruction counting: a
model that disagrees with it is miscounting one of the two, whatever its other
numbers look like.
