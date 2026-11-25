"""Whose function is it? From runs/classmap.txt (class id and method table of every class the game made, gathered by
tools/classrun.ps1), the class names (work/re/riot_classes.tsv) and the recompiled code: for each function that
takes "the player" from the object manager's list, the class (and method slot) it belongs to, directly or through
the functions that call it (up to three calls up).
usage: python classmap.py [OUT.tsv]"""
import glob, os, re, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
proj = os.path.join(here, '..')
data = open(os.path.join(proj, 'assets', 'default.xbe'), 'rb').read()
base, = struct.unpack_from('<I', data, 0x104)
nsec, sec_va = struct.unpack_from('<II', data, 0x11C)
secs = []
for i in range(nsec):
    flags, va, vsize, raw, rsize, name_va = struct.unpack_from('<IIIIII', data, sec_va - base + i * 0x38)
    secs.append((va, raw, rsize))
def rd32(va):
    for sva, raw, rsize in secs:
        if sva <= va < sva + rsize - 3: return struct.unpack_from('<I', data, raw + va - sva)[0]
    return None

names = {}
tsv = os.path.join(proj, '..', '..', 'work', 're', 'riot_classes.tsv')
for line in open(tsv, encoding='utf-8', errors='replace'):
    f = line.rstrip('\n').split('\t')
    if len(f) > 3 and f[1].startswith('0x'): names[int(f[1], 16)] = f[3]

# the recompiled code: functions, their direct calls
gen = os.path.join(proj, 'generated', 'game')
func = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
ins = re.compile(r'^\t// ([0-9A-F]{8}) (.*)')
bodies, callers = {}, {}
for path in sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp'))):
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = func.match(line)
        if m:
            cur = int(m.group(1), 16); bodies[cur] = []
            continue
        m = ins.match(line)
        if m and cur is not None:
            bodies[cur].append(m.group(2))
            c = re.match(r'(?:call|jmp) 0x([0-9A-F]{8})', m.group(2))
            if c: callers.setdefault(int(c.group(1), 16), set()).add(cur)

# method tables: function -> [(class, slot)]
owner = {}
tables = {}
for line in open(os.path.join(proj, 'runs', 'classmap.txt')):
    f = line.split()
    if len(f) == 2: tables[int(f[0], 16)] = int(f[1], 16)
for cls, table in tables.items():
    for slot in range(0, 0x200, 4):
        fn = rd32(table + slot)
        if fn is None or fn not in bodies: break
        owner.setdefault(fn, []).append((cls, slot))

def who(fn):
    """classes that have fn in their table, most particular first (a method many classes share is a base's)"""
    return sorted(owner.get(fn, []), key=lambda cs: cs[0])

def label(fn, depth=0, seen=None):
    seen = seen or set()
    if fn in seen: return []
    seen.add(fn)
    own = who(fn)
    if own:
        cls = sorted({c for c, s in own})
        slot = own[0][1]
        shown = ', '.join(names.get(c, '%08X' % c) for c in cls[:4]) + (' and %d more' % (len(cls) - 4) if len(cls) > 4 else '')
        return [('%s%s method +0x%X' % ('<- ' * depth, shown, slot))]
    if depth >= 3: return []
    out = []
    for c in sorted(callers.get(fn, []))[:6]:
        out += label(c, depth + 1, seen)
    return out[:4]

uses = []
for fn, body in sorted(bodies.items()):
    for i, t in enumerate(body):
        l = re.match(r'mov (e..), \[0x00384BA8\]', t)
        if l and any(('[%s+0x124]' % l.group(1)) in u for u in body[i + 1: i + 7]):
            uses.append(fn)
            break
out = open(sys.argv[1], 'w', encoding='utf-8') if len(sys.argv) > 1 else sys.stdout
known = 0
for fn in uses:
    l = label(fn)
    known += bool(l)
    print('%08X\t%d\t%d\t%s' % (fn, len(bodies[fn]), len(callers.get(fn, [])), ' | '.join(l) if l else '?'), file=out)
print('# %d functions, %d placed; %d classes with a method table' % (len(uses), known, len(tables)), file=out)
