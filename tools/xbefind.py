"""Finds values in the Xbox image (assets/default.xbe) and the code that reads them.
  python xbefind.py float 1.7777778 [tolerance]   addresses holding that float, and the instructions that name them
  python xbefind.py dword 3FE38E39               the same for a 32-bit value (hex)
  python xbefind.py refs 2A1234                  instructions in generated/game that name this address
  python xbefind.py read 2A1234 [count]          dwords and floats at an address
Addresses are those of the running image (what the game's code uses)."""
import glob, os, re, struct, sys

proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
data = open(os.path.join(proj, 'assets', 'default.xbe'), 'rb').read()
base, = struct.unpack_from('<I', data, 0x104)
nsec, sec_va = struct.unpack_from('<II', data, 0x11C)
sections = []
for i in range(nsec):
    flags, va, vsize, raw, rsize, name_va = struct.unpack_from('<IIIIII', data, sec_va - base + i * 0x38)
    name = data[name_va - base:data.index(b'\0', name_va - base)].decode(errors='replace')
    sections.append((va, vsize, raw, rsize, name))

def va_of(off):
    for va, vsize, raw, rsize, name in sections:
        if raw <= off < raw + rsize: return va + off - raw, name
    return None, None

def off_of(va_):
    for va, vsize, raw, rsize, name in sections:
        if va <= va_ < va + rsize: return raw + va_ - va
    return None

def refs(addr):
    pat = re.compile(r'0x0*%X\b' % addr, re.I)
    out = []
    for path in sorted(glob.glob(os.path.join(proj, 'generated', 'game', 'x86_recomp.*.cpp'))):
        for line in open(path, encoding='utf-8', errors='replace'):
            if line.startswith('\t// ') and pat.search(line): out.append(line[4:].rstrip())
    return out

def find(match):
    hits = []
    for va, vsize, raw, rsize, name in sections:
        for off in range(raw, raw + rsize - 3, 4):
            if match(data, off): hits.append((va + off - raw, name))
    return hits

mode = sys.argv[1]
if mode == 'float':
    want = float(sys.argv[2]); tol = float(sys.argv[3]) if len(sys.argv) > 3 else abs(want) * 1e-6
    hits = find(lambda d, o: abs(struct.unpack_from('<f', d, o)[0] - want) <= tol)
elif mode == 'dword':
    want = int(sys.argv[2], 16)
    hits = find(lambda d, o: struct.unpack_from('<I', d, o)[0] == want)
elif mode == 'refs':
    for r in refs(int(sys.argv[2], 16)): print(r)
    sys.exit(0)
elif mode == 'read':
    a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    o = off_of(a)
    if o is None: sys.exit('not in the image file (it may be memory the game fills at run time)')
    for i in range(n):
        d, = struct.unpack_from('<I', data, o + i * 4); f, = struct.unpack_from('<f', data, o + i * 4)
        print('%08X  %08X  %g' % (a + i * 4, d, f))
    sys.exit(0)
else:
    sys.exit(__doc__)
for va, name in hits:
    r = refs(va) if name not in ('.text',) else []
    print('%08X %-8s %d readers' % (va, name, len(r)))
    for line in r[:12]: print('    ' + line)
