"""Code that takes "the player" from the object manager's list of players ([0x384BA8] -> +0x124 list, +0x12C count):
every place that reads +0x124 within a few instructions of loading the manager pointer.  usage: python playerrefs.py"""
import glob, os, re
gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'game')
func = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
ins = re.compile(r'^\t// ([0-9A-F]{8}) (.*)')
hits = {}
for path in sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp'))):
    cur, reg, age = None, None, 0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = func.match(line)
        if m:
            cur, reg = m.group(1), None
            continue
        m = ins.match(line)
        if not m: continue
        addr, text = m.groups()
        l = re.match(r'mov (e..), \[0x00384BA8\]', text)
        if l:
            reg, age = l.group(1), 0
            continue
        if reg:
            age += 1
            if age > 6: reg = None
            elif ('[%s+0x124]' % reg) in text:
                hits.setdefault(cur, []).append(addr)
                reg = None
for f in sorted(hits): print('sub_%s  %s' % (f, ' '.join(hits[f])))
print(len(hits), 'functions')
