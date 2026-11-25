"""The method table of a class (from runs/classmap.txt): slot and function. usage: python vtable.py CLASSID_HEX"""
import os, struct, sys
proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
data = open(os.path.join(proj, 'assets', 'default.xbe'), 'rb').read()
base, = struct.unpack_from('<I', data, 0x104)
nsec, sec_va = struct.unpack_from('<II', data, 0x11C)
secs = [struct.unpack_from('<IIIIII', data, sec_va - base + i * 0x38) for i in range(nsec)]
def rd32(va):
    for flags, sva, vsize, raw, rsize, name in secs:
        if sva <= va < sva + rsize - 3: return struct.unpack_from('<I', data, raw + va - sva)[0]
want = int(sys.argv[1], 16)
for line in open(os.path.join(proj, 'runs', 'classmap.txt')):
    f = line.split()
    if len(f) == 2 and int(f[0], 16) == want:
        t = int(f[1], 16); out = []
        for slot in range(0, 0x1A0, 4):
            fn = rd32(t + slot)
            if not fn or not (0x11000 <= fn < 0x340000): break
            out.append('+%02X %06X' % (slot, fn))
        print('table %06X: ' % t + '  '.join(out)); break
