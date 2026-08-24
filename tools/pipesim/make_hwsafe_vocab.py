#!/usr/bin/env python3
"""Derive the hardware-safe instruction vocabulary from the full one.

Two independent filters, for two different failure modes.

MEMORY SAFETY. The first version of the hardware test crashed with a data
address error because a block did `mov.l @r2,r3` and then stored through r3 -
it turned data it had just loaded into an address. So exactly one register (r3)
is allowed to address memory, and no instruction that writes r3 may appear.
Post-increment and pre-decrement forms are excluded too: they write their own
base register, and over 64 unrolled copies they walk out of the scratch buffer.

MACHINE SAFETY. Some instructions are perfectly valid and would wreck the
measurement or the machine:

  sleep          halts the CPU until an interrupt
  ldc Rn,SR      changes the interrupt mask, register bank and privilege level
  ldc Rn,VBR     moves the exception vector base, so any later fault jumps wild
  lds Rn,FPSCR   swaps the FP register bank and the precision mode, which also
                 silently invalidates the single/double assumption the model is
                 being tested under

`lds Rn,PR` is allowed: it clobbers the return address, but the generated
prologue saves PR and the epilogue restores it.

Anything writing GBR would also be unsafe, since GBR is the base for the
GBR-relative forms; none survive the earlier filters, but the check is here so
that stays true if the vocabulary is regenerated.
"""
import re, sys, os

AT = re.compile(r'@\([^)]*\)|@-?r\d+\+?|@r\d+')
REG = re.compile(r'\b(?:x?[fd]?r|fv)\d+\b|\bgbr\b|\bpc\b')

UNSAFE = [
    (re.compile(r'^sleep\b'),            "halts the CPU"),
    (re.compile(r'^ldc(\.l)?\s+.*,sr$'), "writes SR"),
    (re.compile(r'^ldc(\.l)?\s+.*,vbr$'),"writes VBR"),
    (re.compile(r'^ldc(\.l)?\s+.*,gbr$'),"writes GBR, which is a memory base"),
    (re.compile(r'^lds(\.l)?\s+.*,fpscr$'), "writes FPSCR: FP bank and precision mode"),
    (re.compile(r'^(rte|trapa|ldtlb|brk)\b'), "privileged or control transfer"),
    (re.compile(r'^f(rchg|schg)\b'),     "toggles an FPSCR mode bit"),
]

def main():
    src = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "isa_vocabulary.txt")
    dst = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "isa_vocabulary_hwsafe.txt")

    kept, dropped = [], {}
    for line in open(src):
        k = line.rstrip("\n")
        if not k.strip():
            continue

        why = None
        for pat, reason in UNSAFE:
            if pat.match(k):
                why = reason
                break
        if why is None:
            ats = AT.findall(k)
            inside = set()
            for a in ats:
                inside |= set(REG.findall(a))
            outside = set(REG.findall(AT.sub(' ', k)))
            others = (inside | outside) - {'r3', 'gbr'}
            if any('+' in a or a.startswith('@-') for a in ats):
                why = "post-increment or pre-decrement writes its own base"
            elif not inside <= {'r3', 'gbr'}:
                why = "addresses memory through something other than r3/GBR"
            elif 'r3' in outside:
                why = "writes r3, the memory base"
            elif not all(re.fullmatch(r'(?:fr|fv)?[0-2]|r[0-2]', o) for o in others):
                why = "uses a register outside r0-r2 / fr0-fr2"

        if why is None:
            kept.append(k)
        else:
            dropped.setdefault(why, []).append(k)

    open(dst, "w").write("\n".join(kept) + "\n")
    total = len(kept) + sum(len(v) for v in dropped.values())
    print("kept %d of %d forms -> %s" % (len(kept), total, dst))
    for why in sorted(dropped, key=lambda w: -len(dropped[w])):
        ex = dropped[why][0]
        print("  dropped %6d  %-52s e.g. %s" % (len(dropped[why]), why, ex))

main()
