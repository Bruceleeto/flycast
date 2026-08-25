# Hardware probes

Small Dreamcast programs that measure one thing each on real silicon, so the
cache and pipeline models are calibrated against measurement rather than
against assumption. Every constant in `PenaltyConfig` that is not marked
provisional came from one of these.

`taload` is derived from `bruces_balls` rather than written from scratch,
because the thing it is chasing only appears under a real rendering load and
every attempt to synthesise one has measured the synthesis instead. It needs
`-lsh4zam`, so its Makefile differs from the others.

Build one:

    source /opt/toolchains/dc/kos/environ.sh
    cd ocprobe && make

Run it on hardware:

    dc-tool-ip -t <dreamcast-ip> -x ocprobe.elf

Each prints a tab-separated table and exits on its own. They can also be run
under flycast to check they do not crash, but the counter columns come back
zero there - flycast does not emulate PMCR, which is the entire point of
running them on hardware.

## The probes

| probe | measures | status |
|---|---|---|
| `ocprobe` | operand cache MISS COUNT against known geometry | done - model exact |
| `occost` | what a miss COSTS: clean fill vs dirty eviction | done - 14.3 / 21.9 |
| `occlean` | the same walk with no console output, for model-side comparison | done |
| `sqprobe` | one 32-byte store queue flush, RAM and TA | **superseded** - single-queue, inflates 5x |
| `sqsweep` | store queue cost vs SPACING between flushes | done - flat 2.0, no spacing effect |
| `wbsweep` | write miss cost vs SPACING between evictions | done - `max(7.0, 42.2 - gap)` |
| `taload` | store queue freeze vs TA rendering LOAD | **not yet run** |

## The rule these exist to enforce

Three separate wrong conclusions in this project came from the same mistake:
comparing two measurements that were not taken under the same conditions. The
operand cache was reported "2x low" for weeks by comparing hardware on one
binary against the model on another. The store queue TA cost was reported as
254 cycles per flush by a probe that fed the TA garbage. Both survived because
there were only two numbers, and two numbers disagreeing cannot say which is
wrong.

So every probe here is built to produce a THIRD number - what arithmetic says
the answer must be, or the whole-workload total the figure implies. Two
agreeing against one identifies the liar. `ocprobe` is the clearest example:
its `expected` column is derived from cache geometry alone, and it is what
proved the model right and the long-standing bug report wrong.
