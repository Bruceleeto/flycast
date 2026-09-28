#!/bin/sh
# Disassemble a range of a RAM snapshot: dis.sh <snapshot dir> <start VMA hex, e.g. 8c000000> <length hex>
# ram.bin is physical RAM from 0c000000; the VMA's low 24 bits select the offset.
snap=$1; vma=$2; len=$3
tmp=$(mktemp)
python3 -c "import sys;d=open('$snap/ram.bin','rb').read();o=0x$vma&0xffffff;sys.stdout.buffer.write(d[o:o+0x$len])" > "$tmp"
sh4-linux-gnu-objdump -D -b binary -m sh4 -EL --adjust-vma=0x$vma "$tmp" | tail -n +8
rm -f "$tmp"
