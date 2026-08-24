#!/usr/bin/env python3
"""Generate random SH4 blocks for the pipeline model differential test.

Instructions are sampled from tools/pipesim/isa_vocabulary.txt, which is every
instruction form that

  - the reference simulator has a literal table entry for, so that none of its
    two silent fallbacks can fire (it matches branches on mnemonic alone, and
    rewrites unrecognised mov.l/mov.w operands to @(0,pc)) - a fallback would
    make it mis-assemble rather than fail, and the comparison would be against
    a different program than the one being tested;
  - sh-elf-as assembles to exactly one 16-bit instruction, so the opcodes fed
    to the flycast model are the same instructions the reference was given.

Branches and PC-relative forms are excluded: their behaviour depends on
targets neither tool is given.

--dense restricts to low register numbers so that flow and output dependencies
actually occur. A uniform sample over 16 registers mostly exercises issue rules
and would report agreement the dependency logic never earned.
"""
import random, sys, os, re, argparse

VOCAB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "isa_vocabulary.txt")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blocks", type=int, nargs="?", default=500)
    ap.add_argument("seed", type=int, nargs="?", default=1)
    ap.add_argument("--dense", action="store_true",
                    help="only r0-r3/fr0-fr3, so dependencies are common")
    ap.add_argument("--hwsafe", action="store_true",
                    help="use the hardware-safe vocabulary: r3 is the only memory "
                         "base and is never written, so no block can compute an "
                         "address from data it just loaded")
    ap.add_argument("--vocab", default=VOCAB)
    ap.add_argument("--min", type=int, default=2)
    ap.add_argument("--max", type=int, default=12)
    args = ap.parse_args()

    vocab = args.vocab
    if args.hwsafe and vocab == VOCAB:
        vocab = os.path.join(os.path.dirname(VOCAB), "isa_vocabulary_hwsafe.txt")
    forms = [l.rstrip("\n") for l in open(vocab) if l.strip()]
    if args.dense:
        keep = re.compile(r"\b(?:x?[fd]?r|fv)(\d+)\b")
        forms = [f for f in forms if all(int(n) <= 3 for n in keep.findall(f))]
        if not forms:
            sys.exit("no forms left after --dense filter")

    # Sample a mnemonic first, then a form of it. Sampling forms uniformly
    # would be dominated by the instructions that have 256 immediate variants,
    # and produce blocks that barely touch memory or the FPU.
    by_mnemonic = {}
    for f in forms:
        by_mnemonic.setdefault(f.split()[0], []).append(f)
    mnemonics = sorted(by_mnemonic)

    random.seed(args.seed)
    pick = lambda: random.choice(by_mnemonic[random.choice(mnemonics)])
    out = []
    for b in range(args.blocks):
        n = random.randint(args.min, args.max)
        # The reference starts a new fragment at a comment line, not a blank
        # one, so every block needs a titled separator.
        out.append("# block %d\n" % b + "\n".join(pick() for _ in range(n)))
    print("\n\n".join(out))

main()
