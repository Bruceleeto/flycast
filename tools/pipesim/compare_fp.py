#!/usr/bin/env python3
"""Normalise the FP probe to per-instruction issue rate and latency.

Blocks contain several instructions; the harness reports per block. Dividing by
the instruction count gives the per-instruction figure, which is what the
opcode table holds and what the manual quotes.
"""
import re, sys, json, os

def blocks(path):
    out = []
    for b in re.split(r'(?m)^#.*$', open(path).read()):
        ls = [l.strip() for l in b.split('\n') if l.strip()]
        if ls:
            out.append(ls)
    return out

def read_tsv(path, col):
    r = {}
    for line in open(path):
        f = line.rstrip('\n').split('\t')
        if f and f[0].isdigit():
            try:
                r[int(f[0])] = float(f[col])
            except (ValueError, IndexError):
                pass
    return r

def main():
    corpus, names_f, model_f = sys.argv[1], sys.argv[2], sys.argv[3]
    hw_f = sys.argv[4] if len(sys.argv) > 4 else None

    bl = blocks(corpus)
    names = {}
    for line in open(names_f):
        i, n = line.rstrip('\n').split('\t')
        names[int(i)] = n

    model = read_tsv(model_f, 1)
    hw = read_tsv(hw_f, 1) if hw_f else {}

    # Group issue_/lat_ pairs back together.
    per = {}
    for i, body in enumerate(bl):
        n = names.get(i, str(i))
        kind, _, insn = n.partition('_')
        d = per.setdefault(insn, {})
        count = len(body)
        if i in model:
            d[kind + '_model'] = model[i] / count
        if i in hw:
            d[kind + '_hw'] = hw[i] / count

    have_hw = bool(hw)
    if have_hw:
        print("  %-10s %-19s %-19s" % ("", "issue rate", "latency"))
        print("  %-10s %8s %8s   %8s %8s" % ("insn", "hw", "model", "hw", "model"))
        print("  " + "-" * 50)
    else:
        print("  %-10s %10s %10s" % ("insn", "issue", "latency"))
        print("  " + "-" * 32)

    for insn, d in per.items():
        if have_hw:
            print("  %-10s %8.2f %8.2f   %8.2f %8.2f%s" % (
                insn, d.get('issue_hw', 0), d.get('issue_model', 0),
                d.get('lat_hw', 0), d.get('lat_model', 0),
                "   <-- differs" if (abs(d.get('lat_hw', 0) - d.get('lat_model', 0)) >= 0.75
                                     or abs(d.get('issue_hw', 0) - d.get('issue_model', 0)) >= 0.75)
                else ""))
        else:
            print("  %-10s %10.2f %10.2f" % (insn, d.get('issue_model', 0), d.get('lat_model', 0)))

main()
