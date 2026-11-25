"""Lists the D3D library functions that game code calls and that config/game.toml does not name, with what helps
to identify each: its length, the bytes its return pops, how many places call it, the graphics-chip commands it
writes (immediates of the form count<<18 | method), device offsets it touches and the functions it calls.
usage: python d3dscan.py [--all]   (--all also lists the named ones)"""
import glob, os, re, sys

proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
gen = os.path.join(proj, 'generated', 'game')
show_all = '--all' in sys.argv
D3D_LO, D3D_HI = 0x2761C0, 0x289880

names = {}
sect = None
for line in open(os.path.join(proj, 'config', 'game.toml'), encoding='utf-8'):
    line = line.split('#')[0].strip()
    if line.startswith('['):
        sect = line
        continue
    if sect == '[symbols]' and '=' in line:
        k, v = [x.strip() for x in line.split('=', 1)]
        try:
            a = int(v, 16)
        except ValueError:
            continue
        if a: names[a] = k

func_re = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
ins_re = re.compile(r'^\s*// ([0-9A-F]{8}) (.*)$')
funcs = {}       # start -> list of (addr, text)
callers = {}     # target -> set of caller instruction addresses outside the D3D section
cur = None
for path in sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp'))):
    for line in open(path, encoding='utf-8', errors='replace'):
        m = func_re.match(line)
        if m:
            cur = int(m.group(1), 16)
            funcs[cur] = []
            continue
        m = ins_re.match(line)
        if m and cur is not None:
            a, text = int(m.group(1), 16), m.group(2)
            funcs[cur].append((a, text))
            c = re.match(r'(call|jmp) 0x([0-9A-F]{8})$', text)
            if c:
                t = int(c.group(2), 16)
                if D3D_LO <= t < D3D_HI and not (D3D_LO <= a < D3D_HI):
                    callers.setdefault(t, set()).add(a)

starts = sorted(funcs)
for t in sorted(callers):
    if t in names and not show_all: continue
    body = funcs.get(t)
    if body is None:
        print('%06X  (not a function start)  callers %d' % (t, len(callers[t])))
        continue
    end = body[-1][0]
    ret = next((x[1] for x in reversed(body) if x[1].startswith('ret')), '?')
    cmds = sorted({int(m.group(1), 16) for a, x in body for m in [re.search(r'\], 0x([0-9A-F]{5,6})$', x)] if m and int(m.group(1), 16) & 0x3 == 0 and 0x40000 <= int(m.group(1), 16) < 0x2000000})
    calls = [names.get(int(m.group(1), 16), m.group(1)[2:]) for a, x in body for m in [re.match(r'call 0x([0-9A-F]{8})$', x)] if m]
    offs = sorted({int(m.group(1), 16) for a, x in body for m in [re.search(r'\[e[a-z]{2}\+0x([0-9A-F]{3,4})\]', x)] if m})
    print('%06X %-34s len %4X  %-8s callers %2d  cmds %s  dev+%s  calls %s' % (
        t, names.get(t, '?'), end - t, ret, len(callers[t]), ','.join('%X' % c for c in cmds[:6]),
        ','.join('%X' % o for o in offs[:6]), ','.join(calls[:6])))
