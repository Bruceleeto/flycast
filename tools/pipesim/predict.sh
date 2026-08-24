#!/bin/bash
# Emit the model's per-block cycle prediction for a corpus, in the same
# tab-separated shape the hardware test prints, so the two can be joined.
set -e
ASM="$1"
PIPETEST="${PIPETEST:-$(dirname "$0")/../../build/pipetest}"
AS=/opt/toolchains/dc/sh-elf/bin/sh-elf-as
OC=/opt/toolchains/dc/sh-elf/bin/sh-elf-objcopy
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
python3 - "$ASM" "$OUT" <<'PY'
import sys,os,re
txt=open(sys.argv[1]).read(); out=sys.argv[2]
parts=re.split(r'(?m)^(#.*)$',txt); n=0
for i in range(1,len(parts),2):
    ls=[l.strip() for l in parts[i+1].split('\n') if l.strip()]
    if not ls: continue
    open(os.path.join(out,"b%05d.s"%n),"w").write("\t.little\n"+"\n".join("\t"+x for x in ls)+"\n"); n+=1
PY
: > "$OUT/ops.txt"
for f in "$OUT"/b*.s; do
  $AS -little -o "$f.o" "$f"
  $OC -O binary --only-section=.text "$f.o" "$f.bin"
  python3 -c "
import struct
d=open('$f.bin','rb').read()
print('\n'.join('%04x'%struct.unpack_from('<H',d,i) for i in range(0,len(d),2)))
print()" >> "$OUT/ops.txt"
done
echo -e "block\tmodel_cycles\tmodel_stalls\tinsns"
"$PIPETEST" "$OUT/ops.txt" | awk -F'[= ]+' '{printf "%d\t%s\t%s\t%s\n", NR-1, $2, $6, $4}'
