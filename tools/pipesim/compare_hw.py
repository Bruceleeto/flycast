#!/usr/bin/env python3
"""Join hardware measurements against model predictions for the same corpus.

Blocks where the hardware reports meaningful icache or dcache freeze cycles are
excluded and counted: the model does not claim to cover memory, so scoring it
against a block that was waiting on memory would be measuring the wrong thing.
Excluding them silently would be worse - the count is printed.
"""
import sys, argparse

def read_tsv(path):
    rows = {}
    hdr = None
    for line in open(path):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        if hdr is None:
            hdr = f
            continue
        rows[int(f[0])] = dict(zip(hdr[1:], (float(x) for x in f[1:])))
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hardware"); ap.add_argument("model")
    ap.add_argument("--freeze-tol", type=float, default=0.5,
                    help="max icache+dcache freeze cycles per block to still count it")
    a = ap.parse_args()

    hw, md = read_tsv(a.hardware), read_tsv(a.model)
    common = sorted(set(hw) & set(md))
    if not common:
        sys.exit("no blocks in common")

    rows, skipped = [], 0
    for b in common:
        h, m = hw[b], md[b]
        frz = h.get("frz_ic", 0) + h.get("frz_dc", 0)
        if frz > a.freeze_tol:
            skipped += 1
            continue
        rows.append((b, h["cycles"], m["model_cycles"], h, m))

    if not rows:
        sys.exit("every block was memory-bound; nothing to compare")

    diffs = [(hc - mc) for _, hc, mc, _, _ in rows]
    rel = [(hc - mc) / hc for _, hc, mc, _, _ in rows if hc > 0]
    exact = sum(1 for d in diffs if abs(d) < 0.05)
    within1 = sum(1 for d in diffs if abs(d) < 1.0)

    print("blocks compared      %d  (%d excluded: cache freeze > %.1f cyc)"
          % (len(rows), skipped, a.freeze_tol))
    print("model == hardware    %d  (%.1f%%)" % (exact, 100.0 * exact / len(rows)))
    print("within 1 cycle       %d  (%.1f%%)" % (within1, 100.0 * within1 / len(rows)))
    print("mean signed error    %+.3f cycles  (hardware minus model)" % (sum(diffs) / len(diffs)))
    print("mean absolute error  %.3f cycles" % (sum(abs(d) for d in diffs) / len(diffs)))
    if rel:
        print("mean relative error  %+.2f%%" % (100.0 * sum(rel) / len(rel)))

    # A consistent positive bias means something real is missing from the model
    # rather than the model being noisy, which is a different problem.
    if len(diffs) > 4:
        pos = sum(1 for d in diffs if d > 0.05)
        print("blocks hardware slower than model: %d / %d" % (pos, len(rows)))

    worst = sorted(rows, key=lambda r: -abs(r[1] - r[2]))[:15]
    print("\nworst blocks:")
    print("  block   hw     model   diff   issued  parallel  frz_reg  frz_fpu")
    for b, hc, mc, h, m in worst:
        print("  %5d  %6.2f %6.2f  %+6.2f  %6.2f  %7.2f  %7.2f  %7.2f"
              % (b, hc, mc, hc - mc, h.get("issued", 0), h.get("parallel", 0),
                 h.get("frz_reg", 0), h.get("frz_fpu", 0)))

main()
