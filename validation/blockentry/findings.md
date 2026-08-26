# Does a block boundary cost pairing? No. Three other things fell out.

Built to test whether pipesim over-pairs because it analyses a block as though
it looped straight into itself. It does not, and the probe was more useful for
what it ruled out than for what it confirmed.

Every case runs the same dependency-free body - `mov`/`add` alternating, MT and
EX groups, no instruction reading what another writes - cut into blocks by a
taken branch at four different frequencies. Nothing here goes through the
flow-dependency rule, so the result is about issue behaviour alone.

## 1. Block entry costs no pairing at all

| | LONG | BLK16 | BLK8 | BLK4 |
|---|---:|---:|---:|---:|
| hardware paired share | 48.6% | 48.6% | 48.8% | 48.9% |

A taken branch every four instructions pairs exactly as well as no branches at
all. Flat to a tenth of a point across a 16x change in block size. The
entry-pairing hypothesis is dead, and so is the block-wrap one it grew out of -
which `pipesim::Result::wrapStalls` had already bounded at 21% of the gap.

## 2. A taken branch costs exactly one cycle

| | branch_taken | branch_stall | per branch |
|---|---:|---:|---:|
| LONG | 3999 | 4025 | 1.006 |
| BLK16 | 19999 | 19999 | 1.000 |
| BLK8 | 35999 | 36236 | 1.007 |
| BLK4 | 68031 | 67999 | 1.000 |

A measured constant, not a hypothesis. pipesim models zero. Worth about 2% of a
frame, so it stays below the open items above it - but it no longer needs
measuring, only implementing.

## 3. A conditional branch does not co-issue with its T producer

This was the find. The loop scaffolding GCC emits is:

    dt r1        EX group, writes r1 and T, latency 1
    bf <top>     BR group, reads T

EX x BR is a legal group pairing, so pipesim co-issued them, which makes the
dependency distance 0 - and SHC_PM 8.3 charges FULL latency at distance 0. So
pipesim charged one cycle of flow dependency per loop iteration.

Hardware spends one extra ISSUE SLOT per iteration and zero register stalls. It
does not pair them. Same cycle count either way, which is why this hid: it is
invisible in `cycles` and shows up only in the split between issue slots,
parallel issues and register stalls - all three of which were wrong in exactly
the way this predicts.

| LONG | hardware | pipesim before | after |
|---|---:|---:|---:|
| issue slots | 152016 | 148009 | 152008 |
| parallel_issued | 144001 | 148004 | 144005 |
| reg_stall | 67 | 4253 | 239 |

Note this is NOT a stall in the model: something else issues in that cycle, so
no freeze is charged. It moves the branch to the next issue slot, which is what
hardware does.

**On real workloads it is worth about 11% of the remaining gap, not all of it.**
The arithmetic looked far better than that beforehand - the reg_stall excess on
texture2d was 27.9M against a branch_issued of 25.8M, which is a 1.08 ratio and
a coincidence. It only bites when the compare and the branch are adjacent, and
compiled code usually schedules something between them. Worth writing down as a
warning: a ratio near 1.0 between two large counters is not evidence of
mechanism.

## Reproducing

    cd validation/blockentry && make
    dc-tool-ip -t <ip> -x blockentry.elf
