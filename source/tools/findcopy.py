"""Functions that copy 16 dwords into [reg+0x14] (an engine object's matrix): lea edi,[x+0x14] ... mov ecx,0x10 ... rep movsd.
usage: python findcopy.py"""
import glob, os, re
gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'game')
func = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
for path in sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp'))):
    cur, lines = None, []
    def check():
        if not cur: return
        for i, t in enumerate(lines):
            if re.search(r'lea edi, \[e..\+0x14\]$', t):
                window = lines[i:i + 6]
                if any('mov ecx, 0x10' in w for w in lines[max(0, i - 4):i + 6]) and any('rep movsd' in w for w in window):
                    print('sub_%s  %d instructions  at %s' % (cur, len(lines), t[:8]))
                    return
    for line in open(path, encoding='utf-8', errors='replace'):
        m = func.match(line)
        if m:
            check()
            cur, lines = m.group(1), []
            continue
        if cur and line.startswith('\t// '): lines.append(line[4:].rstrip())
    check()
