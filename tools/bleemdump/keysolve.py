#!/usr/bin/env python3
"""Enumerate every (R3, R4) key for bleem's low-vector decode that yields the observed stub.

Decode (ac000000..ac000010, runs over 0c000020..0c0007ff):
    x = (c ^ R3) + R4;  [p] = x;  R3 -= x;  R4 += x
Constraint: decoded words 0x20, 0x24, 0x28 exactly and the low half of 0x2c (dfc0) equal the
stub Flycast executed. Solved bit-serially from the LSB (xor/add only carry upward).
Usage: keysolve.py <0x800-byte ciphertext of 0c000000..0c0007ff>
"""
import struct, sys
r = struct.unpack('<512I', open(sys.argv[1], 'rb').read())
tgt = [(8, 0xcb2b4018, 0xffffffff), (9, 0x203e120b, 0xffffffff), (10, 0x402bd005, 0xffffffff), (11, 0x0000dfc0, 0x0000ffff)]
sols = []
def ok(R3, R4, bits):
    m = (1 << bits) - 1
    for i, t, tm in tgt:
        x = ((r[i] ^ R3) + R4) & 0xffffffff
        if (x ^ t) & tm & m:
            return False
        R3 = (R3 - x) & 0xffffffff; R4 = (R4 + x) & 0xffffffff
    return True
def dfs(R3, R4, b):
    if b == 32:
        sols.append((R3, R4)); return
    for a in (0, 1):
        for c in (0, 1):
            n3, n4 = R3 | (a << b), R4 | (c << b)
            if ok(n3, n4, b + 1):
                dfs(n3, n4, b + 1)
dfs(0, 0, 0)
def dec(R3, R4):
    d = list(r)
    for i in range(8, 512):
        x = ((r[i] ^ R3) + R4) & 0xffffffff; d[i] = x; R3 = (R3 - x) & 0xffffffff; R4 = (R4 + x) & 0xffffffff
    return d
print('%d keys satisfy the stub constraint' % len(sols))
for s in sols:
    d = dec(*s)
    print('R3 %08x R4 %08x -> [30] %08x [40] %08x (jmp target) [330] %08x (r15)' % (s[0], s[1], d[12], d[16], d[0x330 // 4]))
