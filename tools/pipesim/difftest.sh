#!/bin/bash
# Assemble an .asm file, run it through both the flycast pipeline model and
# skmp's reference simulator, and diff the per-block cycle counts.
set -e
ASM="$1"
HARNESS=/home/bruce/Downloads/bloop/tools/SH-4-pipeline-simulator-harness-main
AS=/opt/toolchains/dc/sh-elf/bin/sh-elf-as
OBJCOPY=/opt/toolchains/dc/sh-elf/bin/sh-elf-objcopy
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# Blocks are separated by blank lines in the reference harness, so assemble
# each block on its own and keep the separation.
python3 - "$ASM" "$OUT" <<'PY'
import sys,os
src=open(sys.argv[1]).read().split('\n')
out=sys.argv[2]
blocks=[];cur=[]
for l in src:
    s=l.split('!')[0].split(';')[0].strip()
    if s.startswith('#'): s=''
    if s.startswith('#'):
        if cur: blocks.append(cur); cur=[]
        continue
    if not s:
        if cur: blocks.append(cur); cur=[]
    else: cur.append(s)
if cur: blocks.append(cur)
for i,b in enumerate(blocks):
    open(os.path.join(out,"b%03d.s"%i),"w").write("\t.little\n"+"\n".join("\t"+x for x in b)+"\n")
print(len(blocks))
PY
N=$(ls "$OUT"/b*.s | wc -l)
: > "$OUT/ops.txt"
for f in "$OUT"/b*.s; do
	$AS -little -o "$f.o" "$f"
	$OBJCOPY -O binary --only-section=.text "$f.o" "$f.bin"
	python3 -c "
import sys,struct
d=open('$f.bin','rb').read()
for i in range(0,len(d),2): print('%04x'%struct.unpack_from('<H',d,i))
print()
" >> "$OUT/ops.txt"
done

echo "# insns= is dropped: the reference counts pipeline sequences, this counts instructions"
echo "=== flycast pipesim ==="
./pipetest "$OUT/ops.txt" | tee /dev/stderr | sed -E 's/  insns=[0-9]+//' > "$OUT/mine.txt"
echo "=== reference simulator ==="
# The reference builds a DOM table per block and runs out of heap on large
# inputs, so feed it in chunks.
python3 - "$ASM" "$OUT/chunks" <<'PY2'
import sys,os
import re
txt=open(sys.argv[1]).read()
parts=re.split(r'(?m)^#',txt)
blocks=['#'+b for b in parts if b.strip()]
os.makedirs(sys.argv[2],exist_ok=True)
CH=40
for i in range(0,len(blocks),CH):
    open(os.path.join(sys.argv[2],"c%04d.asm"%(i//CH)),"w").write('\n\n'.join(blocks[i:i+CH])+'\n')
PY2
: > "$OUT/ref_raw.txt"
for c in "$OUT"/chunks/*.asm; do
	( cd "$HARNESS" && node --max-old-space-size=4096 analyze.js "$c" ) | grep '^cycles=' >> "$OUT/ref_raw.txt"
done
sed -E 's/  insns=[0-9]+//' "$OUT/ref_raw.txt" > "$OUT/ref.txt"
cat "$OUT/ref_raw.txt" >&2
echo "=== diff ==="
if diff -u "$OUT/ref.txt" "$OUT/mine.txt"; then
	echo "IDENTICAL on $N blocks"
else
	echo "MISMATCH"
	exit 1
fi
