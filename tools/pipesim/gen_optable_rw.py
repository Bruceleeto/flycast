import json,re
fly=json.load(open('/tmp/claude-1000/-home-bruce-Downloads-flycast/864cef76-76b4-4a72-9dd1-9b6c57b8ea0b/scratchpad/fly_ops.json'))
sim=json.load(open('/tmp/claude-1000/-home-bruce-Downloads-flycast/864cef76-76b4-4a72-9dd1-9b6c57b8ea0b/scratchpad/sim_ops.json'))

TOK={'<REG_N>':'RN','<REG_M>':'RM','R0':'R0','@<REG_N>':'@RN','@<REG_M>':'@RM',
 '@<REG_N>+':'+RN','@<REG_M>+':'+RM','@-<REG_N>':'-RN',
 '<FREG_N>':'FN','<FREG_M>':'FM','<FREG_M_SD_F>':'FM','<FREG_0>':'F0',
 '<DR_N>':'DN','<FV_N>':'FVN','<FV_M>':'FVM','<RM_BANK>':'RBANK','RM_BANK':'RBANK',
 'GBR':'GBR','SR':'SR','SSR':'SSR','SPC':'SPC','VBR':'VBR','DBR':'DBR','SGR':'SGR',
 'PR':'PR','MACH':'MACH','MACL':'MACL','MAC':'MACL','FPUL':'FPUL','FPSCR':'FPSCR','xmtrx':'XMTRX'}
IMMPFX=('#<','<bdisp')

def parse(diss):
    mn,_,rest=diss.strip().partition(' ')
    rest=rest.strip()
    if not rest: return mn,[]
    rest=re.sub(r'@\(<(?:disp\d+[a-z]*)>\s*,\s*([^)]+)\)',r'@D{\1}',rest)
    rest=re.sub(r'@\(R0\s*,\s*([^)]+)\)',r'@Z{\1}',rest)
    rest=re.sub(r'@\(<GBRdisp8[a-z]*>\)',r'@D{GBR}',rest)
    rest=re.sub(r'@\(<PCdisp8[a-z]*>\)',r'@PC',rest)
    return mn,[p.strip() for p in re.split(r',(?![^{}]*\})',rest)]

# T bit
TW=re.compile(r'^(cmp/|tst|tas\.b|div0[su]|div1|addc|addv|subc|subv|shll$|shlr$|shal|shar|rotl|rotr|rotcl|rotcr|dt|clrt|sett|negc)')
TR=re.compile(r'^(addc|subc|rotcl|rotcr|div1|bf|bt|movt|negc)')

def sets_of(mn,parts,flytype):
    mn=mn.lower()
    rd=set(); wr=set()
    def base(tok):
        if tok.startswith('@D{') or tok.startswith('@Z{'):
            inner=tok[3:-1]
            b=TOK.get(inner,inner)
            s={b}
            if tok.startswith('@Z{'): s.add('R0')
            return ('mem',s)
        if tok=='@PC': return ('mem',set())
        if tok.startswith(IMMPFX): return ('imm',set())
        t=TOK.get(tok)
        if t is None: return ('imm',set())
        if t.startswith('+'): return ('post',{t[1:]})
        if t.startswith('-'): return ('pre',{t[1:]})
        if t.startswith('@'): return ('mem',{t[1:]})
        return ('reg',{t})
    kinds=[base(p) for p in parts]
    for i,(k,s) in enumerate(kinds):
        last = (i==len(kinds)-1)
        if k in ('mem',):
            rd|=s                      # address registers are read
        elif k=='post':
            rd|=s; wr|=s               # @Rn+ : read then write back
        elif k=='pre':
            rd|=s; wr|=s               # @-Rn : read then write back
        elif k=='reg':
            if last and len(kinds)>1: wr|=s
            else: rd|=s
    # single-operand ops that modify their operand in place
    if len(kinds)==1 and kinds[0][0]=='reg':
        if re.match(r'^(shll|shlr|shal|shar|rotl|rotr|rotcl|rotcr|dt|neg|not|movt|cmp/p|tas|fabs|fneg|fsqrt|fsrra|ftst|fldi0|fldi1)',mn):
            rd|=kinds[0][1]
            if not mn.startswith('cmp/'): wr|=kinds[0][1]
            if mn in ('fldi0','fldi1'): rd-=kinds[0][1]
        else:
            rd|=kinds[0][1]
    # read-modify-write two-operand ALU forms: dest is also a source
    if len(kinds)>1 and kinds[-1][0]=='reg':
        if re.match(r'^(add|addc|addv|sub|subc|subv|and|or|xor|shad|shld|xtrct|fadd|fsub|fmul|fdiv|fmac|div1|cmp/|tst)',mn):
            wr_last=kinds[-1][1]
            if mn.startswith('cmp/') or mn.startswith('tst'):
                rd|=wr_last; wr-=wr_last
            else:
                rd|=wr_last
    if mn=='fmac': rd.add('F0')
    # FTRV and FIPR both read the destination vector as well as writing it:
    # FVn = XMTRX x FVn, and FIPR takes the dot product of FVm and FVn. Having
    # only the write recorded meant a chain of them looked independent.
    if mn=='ftrv': rd.add('FVN')
    if mn=='fipr': rd.add('FVN')
    # A dependent chain of MAC runs at its issue rate on hardware (pitch 2),
    # so the accumulator is forwarded internally rather than serialising. The
    # write is kept; the read is not a stall source.
    if re.match(r'^mac\.[lw]',mn): rd -= {'MACH','MACL'}
    if re.match(r'^(mul\.l|muls\.w|mulu\.w|dmuls\.l|dmulu\.l)',mn):
        rd|=wr & {'RN','RM'}; wr-= {'RN','RM'}
    if mn=='div0s': rd|=wr & {'RN','RM'}; wr-={'RN','RM'}
    if mn.startswith('fcmp/'):
        rd|=wr & {'FN','FM'}; wr-={'FN','FM'}; wr.add('T')
    if re.match(r'^(mul\.l|muls\.w|mulu\.w)',mn): wr.add('MACL')
    if re.match(r'^(dmuls\.l|dmulu\.l)',mn): wr|={'MACH','MACL'}
    if re.match(r'^mac\.[lw]',mn): wr|={'MACH','MACL'}
    if mn=='clrmac': wr|={'MACH','MACL'}
    if mn=='movt': rd.discard('RN')
    if TW.match(mn): wr.add('T')
    if TR.match(mn): rd.add('T')
    if 'WritesSR' in flytype: wr.add('SR')
    if 'WritesFPSCR' in flytype or 'FWritesFPSCR' in flytype: wr.add('FPSCR')
    rd.discard(None); wr.discard(None)
    return rd,wr

rows=[]
for r in fly:
    mn,parts=parse(r['diss'])
    rd,wr=sets_of(mn,parts,r['type'])
    rows.append(dict(r,mn=mn.lower(),reads=sorted(rd),writes=sorted(wr)))
json.dump(rows,open('/tmp/claude-1000/-home-bruce-Downloads-flycast/864cef76-76b4-4a72-9dd1-9b6c57b8ea0b/scratchpad/rw.json','w'),indent=0)
print("generated",len(rows))
for r in rows:
    if r['mn'] in ('mov.l','mov.b','add','cmp/eq','fmac','mac.l','lds','sts','ldc.l','tas.b','fdiv','ftrv','shad','movt','rotcl','bf'):
        print("%-34s R=%-28s W=%s"%(r['diss'],','.join(r['reads']),','.join(r['writes'])))
