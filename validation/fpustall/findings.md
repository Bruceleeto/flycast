# What PMCR 0x29 counts, and what it exposed

`0x29` is named "pipeline freeze by FPU" and is defined nowhere. The Dreamcast
and SH4 document set has the counter tables - mode number, name, unit - and no
semantics. The name is the only clue and it is an odd one: `0x28` beside it is
"freeze by CPU register", naming a register file, while `0x29` names a
functional unit. Whoever wrote that table had the words "FP register" available
and did not use them.

That matters because pipesim has to file every flow-dependency stall in one
bucket or the other, and the two readings disagree about a large class of
instructions. Every FMOV variant is LS group, not FE (SHC_PM table 8.3, entries
173-179, 194-200, 222-230), yet they write FP registers.

## The measurement

Four unrolled loops, `0x28` and `0x29` read over identical work.

| case | | reg_stall (0x28) | fpu_stall (0x29) |
|---|---|---:|---:|
| A | `fmov.s @rm,frn` -> `fmov.s frn,@rk` | 2 | **72000** |
| B | `fdiv`, then fadds on disjoint registers | 2 | **563983** |
| C | `fadd` -> dependent `fadd` (control) | 68 | 432009 |
| D | `mov.l @rm,r1` -> `mov.l r1,@rk` | **72002** | 0 |

A and D are the experiment. Same LS group, same latency 2, same distance 1,
same one stall per pair - the only difference is whether the awaited register
is `fr0` or `r1`. They land in different counters, so the split IS on the
register file and not on the functional unit.

B is the other half. An `fdiv` followed by fadds on completely disjoint
registers has no dependency of any kind, and it puts 564K cycles into `0x29`.
That is the F3 resource lock (table 8.3: FDIV locks F3 for ten cycles from
cycle 2).

**So `0x29` is a superset: stalls on FP registers OR on FPU resources.**
pipesim was filing resource locks under `StageLocked`, which is why its FPU
bucket read low and its CPU-register bucket read high.

## What the probe found by accident

The structural counters were only there to prove A and D were fair
comparisons. They turned out to be the more valuable half.

| case A | hardware | flycast (then) |
|---|---:|---:|
| issue slots | 150016 | 78009 |
| parallel_issued | 6226 | 78011 |
| paired share | 4% | 50% |

Both instructions in case A are LS group, and SHC_PM table 8.2 says two LS
instructions cannot be issued in parallel. Hardware agrees - it pairs 4% of
them. flycast paired half.

The cause was one clause:

    else if (nextStage != ST_D && !allParallel)

The parallel-executability check was skipped at exactly the stage it governs.
Table 8.2 is about which pairs may ISSUE together, and issue is the D stage.
Removing the guard took case A's issue slots from 78009 to 144015 against
hardware's 150016, and dropped the whole-workload paired share from 1.93x to
1.39x on texture2d and 1.62x to 1.32x on pvr_dma.

This is the general lesson from the probe, not the specific one: a
microbenchmark small enough to reason about exactly will tell you things you
did not think to ask, and the examples cannot, because in an example every
error is mixed with every other error.

## Reproducing

    cd validation/fpustall && make
    dc-tool-ip -t <ip> -x fpustall.elf

No vblank waits and no sleep anywhere: `0x23` stops while the CPU is halted, so
a probe that waits on anything is timing a clock that is not running.
