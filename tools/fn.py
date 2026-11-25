"""Print the instructions of one recompiled function. usage: python fn.py ADDRESS [FROM [TO]] (hex)"""
import glob, os, re, sys
gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'game')
want = 'X86_FUNC(__imp__sub_%08X)' % int(sys.argv[1], 16)
lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 1 << 32
for path in glob.glob(os.path.join(gen, 'x86_recomp.*.cpp')):
    s = open(path, encoding='utf-8', errors='replace').read()
    i = s.find(want)
    if i < 0: continue
    j = s.find('\nX86_FUNC(', i + 10)
    for m in re.finditer(r'^\t// ([0-9A-F]{8}) (.*)', s[i:j if j > 0 else None], re.M):
        if lo <= int(m.group(1), 16) <= hi: print(m.group(1)[2:], m.group(2))
    break
