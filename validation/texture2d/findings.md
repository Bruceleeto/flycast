# texture2d - status

Run `../validate.py texture2d` to reproduce. `hardware.txt` is the Dreamcast
capture, `flycast.txt` what flycast last produced.

## Verdict

flycast runs this program correctly and at the right speed. Where it is wrong
is in explaining where the time went inside a frame.

## Validated

| | hardware | flycast | ratio |
|---|---:|---:|---:|
| total instructions | 286020327 | 284970044 | 0.996 |
| icache misses | 3869238 | 3862854 | 0.998 |
| ocache misses | 3488884 | 3220996 | 0.923 |
| icache miss rate | 2.28% | 2.47% | 1.083 |
| ocache miss rate | 3.10% | 3.16% | 1.020 |
| wall time, 200 frames | 3.3442s | 3.3439s | 1.000 |
| PVR render time | see ../pvrtime | | 1.012 |

Same instructions, same misses, same frame rate.

## Open

| | hardware | flycast | ratio |
|---|---:|---:|---:|
| cycles per ocache miss | 27.94 | 9.42 | **0.337** |
| cycles per icache miss | 25.22 | 16.80 | 0.666 |
| dual-issue rate | 36.0% | 114.6% | 2.01 |
| reg stall cycles | 21754203 | 83412813 | 3.83 |
| fpu stall cycles | 33171732 | 0 | not modelled |
| total cycles | 462100902 | 404483304 | 0.875 |

The cycle total is 12% low because of the two penalty rows above: flycast counts
the misses correctly and then under-charges for them. The dual-issue and reg
stall rows are one bug - a model that issues too densely has to stall more to
compensate - and both feed the profile report rather than the emulation.

Not retuned here because the existing constants were themselves measured on
hardware, for an *isolated* miss; these are the effective cost under load. One
workload cannot tell whether that multiplier is a property of the chip or of
this program. Revisit with a second example.

## Fixed during this work

- **PVR render time** was `450000 + size * 100` capped at 1.5M: 28% low on the
  floor and 47x too steep per byte, so every scene pinned the cap at 7.5ms. Now
  `624083 + size * 1.2747`, fitted in `../pvrtime`, within 1.2%.
- **PMCR elapsed-time** counted wall cycles. Hardware stops the counter while
  the CPU is halted (`../pmcrclock`: 200.00MHz busy, 0.78MHz asleep). flycast
  now subtracts halted cycles, which moved `cycles` from 1.366 to 0.875.

## Counters that are not what their names say

Three cost real investigation time; they are documented in `hardware.txt` too.

- **`instructions` (0x13)** counts issue SLOTS. The instruction count is this
  plus `parallel_issued`. Read raw it made flycast look 35% out.
- **`cycles` (0x23)** counts only while the CPU is awake.
- **`io_access` (0x0d)** counts the SH4's own on-chip registers, not uncached
  accesses generally. 206x apart when mapped to the latter.

## Not modelled by flycast, reading zero by design

`branch_taken`, `branch_issued`, `io_access`, `fpu_stall_cycles`. A zero is
obviously missing; a guess costs someone an investigation.

## Baseline fragility

96% of this workload's cache misses are conflict misses, so they move up to 23%
with code layout. `hardware.txt` is only valid for the exact ELF that produced
it - rebuild the example and it needs recapturing.
