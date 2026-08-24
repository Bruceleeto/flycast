#!/bin/bash
# Model prediction using the same slope method as the hardware test.
#
# The hardware measures (cycles(2M) - cycles(M)) / M, which cancels the pipeline
# fill and drain and reports the steady-state cost of one more copy of the
# block. Analysing a block once instead measures it in isolation, including the
# ~5 cycles it takes to fill an empty pipeline - which is why the first
# comparison had the model uniformly about five cycles slow on every block.
set -e
ASM="$1"; M="${2:-32}"
PIPETEST="${PIPETEST:-$(dirname "$0")/../../build/pipetest}"
AS=/opt/toolchains/dc/sh-elf/bin/sh-elf-as
OC=/opt/toolchains/dc/sh-elf/bin/sh-elf-objcopy
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT

python3 - "$ASM" "$OUT" "$M" <<'PY'
import sys,os,re
txt=open(sys.argv[1]).read(); out=sys.argv[2]; M=int(sys.argv[3])
parts=re.split(r'(?m)^(#.*)$',txt); n=0
for i in range(1,len(parts),2):
    ls=[l.strip() for l in parts[i+1].split('\n') if l.strip()]
    if not ls: continue
    for mult,tag in ((1,'a'),(2,'b')):
        open(os.path.join(out,"b%05d%s.s"%(n,tag)),"w").write(
            "\t.little\n"+"\n".join("\t"+x for x in ls*(M*mult))+"\n")
    n+=1
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
"$PIPETEST" "$OUT/ops.txt" | awk -v M="$M" '
  /^cycles=/ {
    split($1,c,"="); split($3,s,"=");
    if (n%2==0) { ca=c[2]; sa=s[2] }
    else { printf "%d\t%.3f\t%.3f\t0\n", int(n/2), (c[2]-ca)/M, (s[2]-sa)/M }
    n++
  }'
