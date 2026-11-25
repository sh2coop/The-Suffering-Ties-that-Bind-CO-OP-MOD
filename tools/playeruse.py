"""For every function that takes "the player" from the object manager's list: what it does with him. Prints, per
function, its size, how many call it directly, whether it walks the whole list (reads the count at +0x12C) or
takes the first entry only, and the instructions around the read.  usage: python playeruse.py [OUT.txt [LINES_AFTER]]"""
import glob, os, re, sys
gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'game')
func = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
ins = re.compile(r'^\t// ([0-9A-F]{8}) (.*)')
funcs, calls = {}, {}
for path in sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp'))):
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = func.match(line)
        if m:
            cur = m.group(1); funcs[cur] = []
            continue
        m = ins.match(line)
        if m and cur:
            funcs[cur].append((m.group(1), m.group(2)))
            c = re.match(r'call 0x([0-9A-F]{8})', m.group(2))
            if c: calls[c.group(1)] = calls.get(c.group(1), 0) + 1
after = int(sys.argv[2]) if len(sys.argv) > 2 else 9
out = open(sys.argv[1], 'w', encoding='utf-8') if len(sys.argv) > 1 else sys.stdout
n = whole = 0
for name in sorted(funcs):
    body = funcs[name]
    hits = []
    for i, (a, t) in enumerate(body):
        l = re.match(r'mov (e..), \[0x00384BA8\]', t)
        if not l: continue
        for j in range(i + 1, min(i + 7, len(body))):
            if ('[%s+0x124]' % l.group(1)) in body[j][1]: hits.append(j)
    if not hits: continue
    n += 1
    # the first entry only (`mov r, [list]`), or an entry by number (`[list+r*4]`: a walk over the players)
    count = False
    for j in hits:
        reg = re.match(r'mov (e..), \[e..\+0x124\]', body[j][1])
        nxt = ' '.join(t for a, t in body[j + 1: j + 4])
        if reg and re.search(r'\[%s\+e..\*4\]' % reg.group(1), nxt): count = True
    whole += count
    print('== sub_%s  %d instructions, %d direct callers, %s' % (name, len(body), calls.get(name, 0),
          'BY NUMBER (walks the players)' if count else 'FIRST PLAYER ONLY'), file=out)
    for j in hits[:2]:
        for a, t in body[max(0, j - 3): j + after]: print('   %s %s' % (a, t), file=out)
        print('   ...', file=out)
print('%d functions, %d take a player by number' % (n, whole), file=out)
