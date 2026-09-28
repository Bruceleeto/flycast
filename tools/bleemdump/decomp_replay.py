#!/usr/bin/env python3
# Replay bleem's LZ decompressor (8c0da880, called from 8c0daa9c) on a RAM snapshot taken at its entry,
# with a minimal SH4 interpreter covering only the opcodes it uses. Dirty O-cache lines are overlaid first.
# usage: decomp_replay.py <snapshot dir> [out.bin]
import struct, sys

snap = sys.argv[1]
ram = bytearray(open(snap + '/ram.bin', 'rb').read())
oc = open(snap + '/ocache.bin', 'rb').read()
for i in range(len(oc) // 36):
    aa = struct.unpack_from('<I', oc, i * 36)[0]
    a = (aa & 0x1ffffc00) | ((i << 5) & 0x3e0)
    if aa & 3 == 3 and a >> 24 == 0x0c:
        ram[a & 0xffffff:(a & 0xffffff) + 32] = oc[i * 36 + 4:i * 36 + 36]
# optional word overrides after the O-cache overlay: PATCH="8c037a00=12345678,..." (P1/U0 addresses)
import os
for kv in filter(None, os.environ.get('PATCH', '').split(',')):
    k, v = kv.split('=')
    struct.pack_into('<I', ram, int(k, 16) & 0xffffff, int(v, 16))
regs = {}
for line in open(snap + '/regs.txt'):
    k, *v = line.split()
    if v:
        try: regs[k] = int(v[0], 16)
        except ValueError: pass

M = 0xffffffff
def off(a):
    # every address used here is main RAM (P1 8cxxxxxx, U0 0e8xxxxx mapped 1:1 by the UTLB, 7c O-cache RAM excluded)
    assert (a >> 24) & 0x1f in (0x0c, 0x0d, 0x0e, 0x0f), hex(a)
    return a & 0xffffff
def rl(a): return struct.unpack_from('<I', ram, off(a))[0]
def rw(a): return struct.unpack_from('<H', ram, off(a))[0]
def rb(a): return ram[off(a)]
def wb(a, v): ram[off(a)] = v & 0xff
def sx8(v): return v - 0x100 if v & 0x80 else v
def sx16(v): return v - 0x10000 if v & 0x8000 else v

r = [regs['r%d' % i] for i in range(16)]
T = regs['sr'] & 1
pc, pr = 0x8c0da880, 0xdeadbee0
stack = {}
n = 0
pending = None  # branch target after the delay slot
while True:
    op = rw(pc)
    nx, npc = op >> 12, pc + 2
    rn, rm, d8, d4 = (op >> 8) & 15, (op >> 4) & 15, op & 0xff, op & 15
    br = None
    if op == 0x0009: pass
    elif op == 0x000b: br = pr
    elif nx == 0x6 and d4 == 2: r[rn] = rl(r[rm])
    elif nx == 0x6 and d4 == 3: r[rn] = r[rm]
    elif nx == 0x6 and d4 == 0xc: r[rn] = r[rm] & 0xff
    elif nx == 0x6 and d4 == 7: r[rn] = ~r[rm] & M
    elif nx == 0x5: r[rn] = rl(r[rm] + d4 * 4)
    elif nx == 0x3 and d4 == 0xc: r[rn] = (r[rn] + r[rm]) & M
    elif nx == 0x3 and d4 == 8: r[rn] = (r[rn] - r[rm]) & M
    elif nx == 0x3 and d4 == 0: T = int(r[rn] == r[rm])
    elif nx == 0x3 and d4 == 2: T = int(r[rn] >= r[rm])
    elif nx == 0x3 and d4 == 6: T = int(r[rn] > r[rm])
    elif nx == 0x2 and d4 == 8: T = int(r[rn] & r[rm] == 0)
    elif nx == 0x2 and d4 == 9: r[rn] &= r[rm]
    elif nx == 0x2 and d4 == 0: wb(r[rn], r[rm])
    elif nx == 0x2 and d4 == 4: r[rn] = (r[rn] - 1) & M; wb(r[rn], r[rm])
    elif nx == 0x0 and d4 == 0xc: r[rn] = sx8(rb(r[0] + r[rm])) & M
    elif nx == 0x0 and d8 == 0x29: r[rn] = T
    elif nx == 0x4 and d8 == 0x22: r[rn] = (r[rn] - 4) & M; stack[r[rn]] = pr
    elif nx == 0x4 and d8 == 0x26: pr = stack[r[rn]]; r[rn] = (r[rn] + 4) & M
    elif nx == 0x4 and d8 == 0x01: T = r[rn] & 1; r[rn] >>= 1
    elif nx == 0x4 and d8 == 0x25: T, r[rn] = r[rn] & 1, (r[rn] >> 1) | (T << 31)
    elif nx == 0x4 and d8 == 0x24: T, r[rn] = r[rn] >> 31, ((r[rn] << 1) | T) & M
    elif nx == 0x4 and d8 == 0x19: r[rn] >>= 8
    elif nx == 0x4 and d8 == 0x09: r[rn] >>= 2
    elif nx == 0x4 and d8 == 0x10: r[rn] = (r[rn] - 1) & M; T = int(r[rn] == 0)
    elif nx == 0x4 and d4 == 0xd:
        s = r[rm] & 0x1f if r[rm] < 0x80000000 else -(((~r[rm]) & 0x1f) + 1)
        r[rn] = (r[rn] << s) & M if s >= 0 else (r[rn] >> -s if (r[rm] & 0x1f) else 0)
    elif nx == 0x7: r[rn] = (r[rn] + sx8(d8)) & M
    elif nx == 0xe: r[rn] = sx8(d8) & M
    elif nx == 0x9: r[rn] = sx16(rw(pc + 4 + d8 * 2)) & M
    elif op >> 8 == 0x84: r[0] = sx8(rb(r[rm] + d4)) & M
    elif op >> 8 == 0x80: wb(r[rm] + d4, r[0])
    elif op >> 8 == 0x85: r[0] = sx16(rw(r[rm] + d4 * 2)) & M
    elif op >> 8 == 0x88: T = int(r[0] == sx8(d8) & M)
    elif op >> 8 == 0x89:
        if T: npc = pc + 4 + sx8(d8) * 2
    elif op >> 8 == 0x8b:
        if not T: npc = pc + 4 + sx8(d8) * 2
    elif op >> 8 == 0x8d:
        if T: br = pc + 4 + sx8(d8) * 2
    elif op >> 8 == 0x8f:
        if not T: br = pc + 4 + sx8(d8) * 2
    elif nx == 0xa: br = pc + 4 + ((op & 0xfff) - (0x1000 if op & 0x800 else 0)) * 2
    elif nx == 0xb: br = pc + 4 + ((op & 0xfff) - (0x1000 if op & 0x800 else 0)) * 2; pr = pc + 4
    else:
        sys.exit('unhandled op %04x at %08x after %d' % (op, pc, n))
    n += 1
    if pending is not None:
        npc, pending = pending, None
    elif br is not None:
        pending = br
    pc = npc
    if pc == 0xdeadbee0 or n > 20000000:
        break
print('done after %d instructions; r10 %08x r12 %08x r11 %08x' % (n, r[10], r[12], r[11]))
h = r[11]
print('header at %08x:' % h, bytes(ram[off(h):off(h) + 16]).hex(' '))
print('halfword @+0xa = %04x -> copy source %08x' % (rw(h + 10), (h + sx16(rw(h + 10))) & M))
if len(sys.argv) > 2:
    size = struct.unpack_from('<I', ram, 0x37800)[0]
    open(sys.argv[2], 'wb').write(ram[off(h):off(h) + size])
