#!/usr/bin/env python3
"""Corpus that measures FP instruction issue rate and latency on hardware.

The whole-program comparison on bruces_balls left 9.8% of the frame
unexplained, and FPU interlock is 32.4% of that frame, so the suspicion is that
the per-instruction FP latencies are wrong. This measures them directly instead
of arguing about which manual column applies.

Two blocks per instruction:

  ISSUE   the same instruction on independent registers. The steady-state cost
          is its issue rate, because nothing has to wait for anything.
  LAT     a dependent chain: each instruction reads what the last one wrote.
          The steady-state cost is its latency.

Both are measured by the same unroll-and-difference method as the rest of the
harness, so pipeline fill cancels.
"""
import sys

# (name, issue-form template, latency-form template)
# {a}/{b} are substituted with rotating independent registers for the issue
# form; the latency form is written out explicitly as a self-dependency.
PROBES = [
    ("fadd",     ["fadd fr{m},fr{n}"],          ["fadd fr1,fr0"]),
    ("fsub",     ["fsub fr{m},fr{n}"],          ["fsub fr1,fr0"]),
    ("fmul",     ["fmul fr{m},fr{n}"],          ["fmul fr1,fr0"]),
    ("fdiv",     ["fdiv fr{m},fr{n}"],          ["fdiv fr1,fr0"]),
    ("fsqrt",    ["fsqrt fr{n}"],               ["fsqrt fr0"]),
    ("fsrra",    ["fsrra fr{n}"],               ["fsrra fr0"]),
    ("fabs",     ["fabs fr{n}"],                ["fabs fr0"]),
    ("fneg",     ["fneg fr{n}"],                ["fneg fr0"]),
    ("fmov",     ["fmov fr{m},fr{n}"],          ["fmov fr0,fr1", "fmov fr1,fr0"]),
    ("fldi1",    ["fldi1 fr{n}"],               ["fldi1 fr0"]),
    ("fmac",     ["fmac fr0,fr{m},fr{n}"],      ["fmac fr0,fr1,fr2", "fmac fr0,fr2,fr1"]),
    ("fipr",     ["fipr fv0,fv{v}"],            ["fipr fv4,fv0", "fipr fv0,fv4"]),
    ("ftrv",     ["ftrv xmtrx,fv{v}"],          ["ftrv xmtrx,fv0"]),
    ("fcmpeq",   ["fcmp/eq fr{m},fr{n}"],       ["fcmp/eq fr1,fr0"]),
    ("float",    ["float fpul,fr{n}"],          ["float fpul,fr0"]),
    ("ftrc",     ["ftrc fr{n},fpul"],           ["ftrc fr0,fpul", "fsts fpul,fr0"]),
    ("flds",     ["flds fr{n},fpul"],           ["flds fr0,fpul", "fsts fpul,fr0"]),
    ("fsts",     ["fsts fpul,fr{n}"],           ["fsts fpul,fr0"]),
    # Integer comparisons for reference, so the FP numbers can be read against
    # something whose cost is not in doubt.
    ("add",      ["add r{rm},r{rn}"],           ["add r1,r0"]),
    ("mul",      ["mul.l r{rm},r{rn}"],         ["mul.l r0,r1", "sts macl,r0"]),
    ("mac",      ["mac.l @r3+,@r3+"],           ["mac.l @r3+,@r3+"]),
]

def issue_block(tmpl, count=8):
    """Independent registers, so nothing waits: measures issue rate."""
    out = []
    for i in range(count):
        # Eight distinct destinations, so nothing in the block depends on
        # anything else in it and what is measured is the issue rate alone.
        line = tmpl[0]
        out.append(line.format(n=(i % 8) * 2, m=(i % 8) * 2 + 1,
                               v=((i % 2) + 1) * 4, rn=i % 3, rm=(i + 1) % 3))
    return out

def main():
    blocks = []
    for name, issue, lat in PROBES:
        blocks.append(("issue_" + name, issue_block(issue)))
        # Repeat the dependent form enough that the chain dominates.
        blocks.append(("lat_" + name, lat * 4))

    out = []
    for i, (name, body) in enumerate(blocks):
        out.append("# %s\n" % name + "\n".join(body))
    print("\n\n".join(out))

    with open(sys.argv[1] if len(sys.argv) > 1 else "fp_probe_names.txt", "w") as f:
        for i, (name, _) in enumerate(blocks):
            f.write("%d\t%s\n" % (i, name))

main()
