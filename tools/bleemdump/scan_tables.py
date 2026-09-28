#!/usr/bin/env python3
"""Scan 16 MB RAM snapshots (physical 0c000000, shown as 8cxxxxxx) for dispatch-table shapes.

The beta PSX dynarec (/home/bruce/Downloads/asm/dispatch_tables.md) used 8-byte
(handler, emitter) entries in 64/64/32-entry tables, the primary table starting with
two 0x80808080 sentinel pairs, and one shared illegal handler in ~30 slots of each.
This reports, without assuming the beta addresses:
  pairs: runs of >=32 8-byte entries (even RAM pointer, 0-or-pointer) with a handler
         repeated >=16 times;
  flat:  runs of >=32 4-byte even RAM pointers with one value repeated >=12 times;
  sentinel: 0x80808080 0x80808080 pairs that are not inside a long 0x80 fill.
"""
import struct, sys, collections

def isptr(v):
    return (v & 0x1f000000) == 0x0c000000 and (v >> 29) in (4, 5) and not v & 1

def scan(path):
    ram = open(path, 'rb').read()
    n = len(ram) // 4
    w = struct.unpack('<%dI' % n, ram)
    va = lambda i: 0x8c000000 + i * 4
    i = 0
    while i < n:
        if not isptr(w[i]):
            i += 1
            continue
        j = i
        while j < n and isptr(w[j]):
            j += 1
        run = w[i:j]
        if len(run) >= 32:
            top, topn = collections.Counter(run).most_common(1)[0]
            if topn >= 12:
                print('flat  %08x: %d pointers, %08x x%d, %d distinct' % (va(i), len(run), top, topn, len(set(run))))
        i = j
    for par in (0, 1):
        i = par
        while i + 1 < n:
            j = i
            while j + 1 < n and ((isptr(w[j]) and (w[j + 1] == 0 or isptr(w[j + 1])))
                                 or (w[j] == 0x80808080 and w[j + 1] == 0x80808080)):
                j += 2
            if (j - i) // 2 >= 32:
                top, topn = collections.Counter(w[i:j:2]).most_common(1)[0]
                if topn >= 16 and len(set(w[i + 1:j:2])) > 2:
                    print('pairs %08x: %d entries, handler %08x x%d' % (va(i), (j - i) // 2, top, topn))
            i = j + 2 if j > i else i + 2
    for i in range(2, n - 3):
        if w[i] == w[i + 1] == 0x80808080 and w[i - 1] != 0x80808080 and w[i + 2] != 0x80808080:
            print('sentinel %08x' % va(i))

for p in sys.argv[1:]:
    print('==', p)
    scan(p)
