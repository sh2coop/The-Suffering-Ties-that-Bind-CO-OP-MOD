"""Calls of the view object's methods ([0x3934DC]->vtbl[n]) in the recompiled code, by method offset.
usage: python viewcalls.py [offset hex ...]   (no offsets: a count per offset)"""
import glob, os, re, sys
gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'game')
want = {int(a, 16) for a in sys.argv[1:]}
load = re.compile(r'^\t// ([0-9A-F]{8}) mov (e..), \[0x003934DC\]')
ins = re.compile(r'^\t// ([0-9A-F]{8}) (.*)')
func = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
counts = {}
for path in sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp'))):
    cur = None
    reg = None      # register holding the view
    vt = None       # register holding its method table
    age = 0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = func.match(line)
        if m:
            cur, reg, vt = m.group(1), None, None
            continue
        m = ins.match(line)
        if not m: continue
        addr, text = m.groups()
        l = load.match(line)
        if l:
            reg, vt, age = l.group(2), None, 0
            continue
        if reg is None: continue
        age += 1
        if age > 14:
            reg = vt = None
            continue
        m2 = re.match(r'mov (e..), \[(e..)\]$', text)
        if m2 and m2.group(2) == reg:
            vt = m2.group(1)
            continue
        m3 = re.match(r'(call|jmp) \[(e..)\+0x([0-9A-F]+)\]', text)
        if m3 and vt and m3.group(2) == vt:
            off = int(m3.group(3), 16)
            counts.setdefault(off, []).append((addr, cur))
            reg = vt = None
        m4 = re.match(r'(call|jmp) \[(e..)\]$', text)
        if m4 and vt and m4.group(2) == vt:
            counts.setdefault(0, []).append((addr, cur))
            reg = vt = None
for off in sorted(counts):
    if want and off not in want: continue
    if want:
        for addr, cur in counts[off]: print('+%02X  at %s in sub_%s' % (off, addr, cur))
    else:
        print('+%02X  %d calls' % (off, len(counts[off])))
