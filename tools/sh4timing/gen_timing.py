#!/usr/bin/env python3
# Generate core/hw/sh4/sh4_timing_data.h from the SH7091 pipeline timing sweep report (mode M0 tables).
import html, re, sys

src, out = sys.argv[1], sys.argv[2]
s = open(src, encoding='utf-8').read()
s = re.sub(r'<(script|style)[^>]*>.*?</\1>', '', s, flags=re.S)
s = re.sub(r'<br\s*/?>|</(p|div|tr|h\d|li|table)>', '\n', s)
s = re.sub(r'</t[dh]>', '\t', s)
s = html.unescape(re.sub(r'<[^>]+>', '', s))
lines = [l for l in s.split('\n') if l.strip()]

def section(start_pred, end_pred, frm=0):
    i = next(k for k in range(frm, len(lines)) if start_pred(lines[k]))
    j = next(k for k in range(i + 1, len(lines)) if end_pred(lines[k]))
    return i, lines[i + 1:j]

# ---- per-form table (M0)
pf, _ = section(lambda l: l.startswith('Per-form tables'), lambda l: True)
_, rows = section(lambda l: l.startswith('Mode M0: FPSCR'), lambda l: l.startswith('Mode M1'), pf)
forms = []  # dicts
for r in rows:
    f = r.split('\t')
    if f[0] == 'form':
        continue
    name, enc, kind, cls, cost = f[0], f[1], f[2], f[3], f[4]
    cls = cls.replace('?', '').strip()
    forms.append(dict(name=name, enc=enc, kind=kind, cls=int(cls[1]) if cls else 0,
                      cost=int(round(float(cost)))))
names = {f['name']: i for i, f in enumerate(forms)}
N = len(forms)

def split_items(txt):
    txt = re.sub(r'\s*\[(WAW|WAR)[^\]]*\]', '', txt)
    return [x.strip() for x in txt.split(', ') if x.strip()]

def form_of(item):
    item = re.sub(r'\s*\[(WAW|WAR)[^\]]*\]', '', item)
    item = item.replace(' not a ramp', '').strip()
    if item in names:
        return names[item], None
    # "<form> <label>"
    k = item.rfind(' ')
    return names[item[:k]], item[k + 1:]

# ---- latency table (M0)
lt, _ = section(lambda l: l.startswith('Latency tables'), lambda l: True)
_, rows = section(lambda l: l.startswith('Mode M0'), lambda l: l.startswith('Mode M1'), lt)
VECTOR_OUT = {'FVn', 'DRn'}
lat = {}          # (A, B, label, elem) -> D
reads = {i: set() for i in range(N)}     # labels read (explicit)
imp_reads = {i: set() for i in range(N)}  # implicit resources read
imp_writes = {i: set() for i in range(N)}
for r in rows:
    f = r.split('\t')
    if f[0].startswith('A (producer)'):
        continue
    a, outp, d, cons = names[f[0]], f[1], int(f[2]), f[4]
    if outp == 'memory':
        continue	# same-line memory ordering: not modelled
    if f[0].startswith('FIPR') and outp == 'FVn':
        outp = 'FIPRn'	# FIPR only writes FR(n+3); these rows are its WAW/RAW through a consumer's FRn
    res = None
    if outp not in ('Rn', 'R0', 'FRn', 'FVn', 'DRn', 'FIPRn'):
        res = outp
        for rr in outp.split('+'):
            imp_writes[a].add(rr)
    for item in split_items(cons):
        b, label = form_of(item)
        elem = -1
        m = re.match(r'(\w+)\[(\d)\]$', label)
        if m:
            label = m.group(1)
            if outp in VECTOR_OUT:
                elem = int(m.group(2))
        if label == 'implicit' and res is None:
            label = 'FR0' if outp.startswith('F') or outp == 'DRn' else 'R0'
        if label == 'implicit':
            for rr in res.split('+'):
                imp_reads[b].add(rr)
                key = (a, rr, b, rr, -1)
                lat[key] = max(lat.get(key, 0), d)
            continue
        reads[b].add(label)
        key = (a, outp, b, label, elem)
        lat[key] = max(lat.get(key, 0), d)

# ---- structural blocking (M0)
block = {}
st, _ = section(lambda l: l.startswith('Structural blocking'), lambda l: True)
_, rows = section(lambda l: l.startswith('mode\tA'), lambda l: l.startswith('Consumer-side holds'), st)
for r in rows:
    f = r.split('\t')
    if f[0] != 'M0':
        continue
    a, d = names[f[1]], int(f[2])
    for item in split_items(f[4]):
        b, _ = form_of(item)
        block[(a, b)] = max(block.get((a, b), 0), d)

# "Pairs with Q outside {1, 2}" are not used: Q is read through stc dbr,r11, which itself waits on DBR/S/control
# register writes (RAW or structural, already in the tables above), so those pairs carry no extra adjacency cost.

# ---- operands from syntax + encoding
LABELS = ['Rn', 'Rm', 'R0', 'FRn', 'FRm', 'FR0', 'FVn', 'FVm', 'DRn', 'FIPRn',
          'T', 'MACH', 'MACL', 'FPUL', 'PR', 'fpscr.mode', 'fpscr.cause', 'S', 'QM', 'DBR', 'SPC', 'SSR', 'VBR', 'memory']
LID = {l: i for i, l in enumerate(LABELS)}
NO_GPR_WRITE = ('CMP/', 'TST', 'DIV0S', 'MUL.L', 'MULS.W', 'MULU.W', 'DMULS.L', 'DMULU.L', 'MAC.', 'FCMP')

def field(enc, ch):
    k = enc.find(ch)
    if k < 0:
        return 0, 0
    w = enc.count(ch)
    return 15 - (k + w - 1), w

def operands(fm, i):
    name = fm['name']
    mnem = name.split(' ')[0]
    args = name.split(' ', 1)[1].split(',') if ' ' in name and '(' not in name.split(' ', 1)[1][:1] else []
    if ' ' in name:
        argtxt = name.split(' ', 1)[1]
        args = re.findall(r'@\([^)]*\)|[^,]+', argtxt)
    ops = {}
    def add(label, flag):
        ops[label] = ops.get(label, 0) | flag
    for l in reads[i]:
        add(l, 1)
    for l in imp_reads[i]:
        add(l, 1)
    for l in imp_writes[i]:
        add(l, 2)
    if args and not name.endswith('taken)') and mnem not in ('BRA', 'BSR'):
        dst = args[-1].strip()
        for a in args:
            a = a.strip()
            if a in ('@Rm+', '@-Rn', '@Rn+'):
                add(a.strip('@+-'), 3)
        if dst in ('Rn', 'R0') and not mnem.startswith(NO_GPR_WRITE):
            add(dst, 2)
        if dst in ('FRn', 'DRn', 'FVn') and not mnem.startswith(('FCMP', 'FIPR')):
            add(dst, 2)
        if mnem == 'FIPR':
            add('FIPRn', 2)
        if mnem.startswith('FMOV') and dst.startswith('@'):
            pass
    # read-before-write fallbacks for forms never seen as consumers
    if mnem.startswith(('MOV', 'FMOV')) or mnem in ('OCBI', 'OCBP', 'OCBWB', 'PREF', 'MOVCA.L', 'TAS.B'):
        for a in args:
            for l in re.findall(r'R[mn0]', a):
                if a.startswith('@') or a == args[0].strip():
                    add(l, 1)
    return ops

opers = [operands(f, i) for i, f in enumerate(forms)]

# taken forms
takens = {}
for i, f in enumerate(forms):
    if f['name'].endswith('(not taken)'):
        takens[i] = names[f['name'].replace('(not taken)', '(taken)')]

o = []
o.append('// Generated by gen_timing.py from "SH7091 pipeline timing sweep" (mode M0 measurements). Do not edit.')
o.append('#pragma once')
o.append('#include "types.h"\n')
o.append('namespace sh4timing {\n')
o.append('enum Label : u8 { ' + ', '.join('L_' + re.sub(r'\W', '_', l).upper() for l in LABELS) + ', L_COUNT };\n')
o.append('struct Operand { u8 label; u8 rw; };\t// rw: 1 read, 2 write\n')
o.append('struct Form {\n\tconst char *name;\n\tu16 mask, match;\n\tu8 cls;\t\t// issue class 1..5, 0: issues alone\n'
         '\tu8 cost;\t// issue cycles (0: co-issues with an integer op)\n\ts16 taken;\t// form index when a conditional branch is taken, -1 otherwise\n'
         '\tu8 nshift, nwidth, mshift, mwidth;\n\tu8 nops;\n\tOperand ops[8];\n};\n')
o.append(f'constexpr int FormCount = {N};\n')
o.append('static const Form forms[FormCount] = {')
for i, f in enumerate(forms):
    enc = f['enc']
    mask = int(''.join('1' if c in '01' else '0' for c in enc), 2)
    match = int(''.join(c if c in '01' else '0' for c in enc), 2)
    ns, nw = field(enc, 'n')
    ms, mw = field(enc, 'm')
    ops = opers[i]
    assert len(ops) <= 8, f['name']
    opstr = ', '.join(f'{{ L_{re.sub(r"\W", "_", l).upper()}, {rw} }}' for l, rw in sorted(ops.items(), key=lambda x: LID[x[0]]))
    o.append(f'\t{{ "{f["name"]}", 0x{mask:04x}, 0x{match:04x}, {f["cls"]}, {f["cost"]}, {takens.get(i, -1)}, '
             f'{ns}, {nw}, {ms}, {mw}, {len(ops)}, {{ {opstr} }} }},')
o.append('};\n')

def triples(d, name):
    o.append(f'static const struct {{ u16 a, b; u8 d; }} {name}[] = {{')
    for (a, b), v in sorted(d.items()):
        o.append(f'\t{{ {a}, {b}, {v} }},')
    o.append('};\n')
triples(block, 'structural')
o.append('// producer a writing operand out; consumer b reading (or rewriting) operand label, element elem of a vector result')
o.append('static const struct { u16 a; u8 out; u16 b; u8 label; s8 elem; u8 d; } latency[] = {')
L = lambda l: 'L_' + re.sub(r'\W', '_', l).upper()
for (a, outp, b, l, e), d in sorted(lat.items()):
    o.append(f'\t{{ {a}, {L(outp)}, {b}, {L(l)}, {e}, {d} }},')
o.append('};\n')
o.append('}\t// namespace sh4timing')
open(out, 'w').write('\n'.join(o) + '\n')

# sanity: every 16-bit opcode matches at most one non-taken form
cover = [0] * 65536
for i, f in enumerate(forms):
    if f['name'].endswith(' (taken)'):
        continue
    enc = f['enc']
    mask = int(''.join('1' if c in '01' else '0' for c in enc), 2)
    match = int(''.join(c if c in '01' else '0' for c in enc), 2)
    for op in range(65536):
        if op & mask == match:
            cover[op] += 1
print('forms', N, 'lat', len(lat), 'block', len(block), 'overlaps', sum(1 for c in cover if c > 1),
      'covered', sum(1 for c in cover if c))
