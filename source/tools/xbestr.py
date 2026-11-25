"""Strings in the Xbox image (assets/default.xbe) that hold a word, with their running-image addresses.
  python xbestr.py shadow"""
import os, re, struct, sys

proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
data = open(os.path.join(proj, 'assets', 'default.xbe'), 'rb').read()
base, = struct.unpack_from('<I', data, 0x104)
nsec, sec_va = struct.unpack_from('<II', data, 0x11C)
sections = []
for i in range(nsec):
    flags, va, vsize, raw, rsize, name_va = struct.unpack_from('<IIIIII', data, sec_va - base + i * 0x38)
    sections.append((va, vsize, raw, rsize))

def va_of(off):
    for va, vsize, raw, rsize in sections:
        if raw <= off < raw + rsize:
            return va + off - raw
    return 0

word = sys.argv[1].lower().encode()
for m in re.finditer(rb'[\x20-\x7e]{4,}', data):
    if word in m.group().lower():
        print('%08X  %s' % (va_of(m.start()), m.group().decode()))
