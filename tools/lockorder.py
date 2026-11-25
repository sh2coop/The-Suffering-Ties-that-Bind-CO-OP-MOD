"""Lock order in the game's code. The game takes a critical section with `mov ecx, <address>; call 0x2220E0` and
leaves it with `push <address>; call [0x337700]`. For each function this finds which fixed locks it holds while it
calls other functions, follows those calls (direct calls only), and lists every pair "A held while B is taken".
A pair that also appears reversed is a lock-order inversion: two threads can stop each other for good.
usage: python lockorder.py [--depth 6] [--lock 3986AC]"""
import glob, os, re, sys
from collections import defaultdict

proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
gen = os.path.join(proj, 'generated', 'game')
depth = int(sys.argv[sys.argv.index('--depth') + 1]) if '--depth' in sys.argv else 6
only = int(sys.argv[sys.argv.index('--lock') + 1], 16) if '--lock' in sys.argv else None
ENTER, LEAVE_THUNK = 0x2220E0, '0x00337700'

func_re = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
ins_re = re.compile(r'^\s*// ([0-9A-F]{8}) (.*)$')
funcs = {}
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
            funcs[cur].append((int(m.group(1), 16), m.group(2)))

# per function, in address order: ('enter', lock), ('leave', lock), ('call', target)
events = {}
for f, body in funcs.items():
    ev = []
    last_ecx = None
    last_push = None
    for a, text in body:
        m = re.match(r'mov ecx, 0x([0-9A-F]+)$', text)
        if m: last_ecx = int(m.group(1), 16)
        m = re.match(r'push 0x([0-9A-F]+)$', text)
        if m: last_push = int(m.group(1), 16)
        m = re.match(r'call 0x([0-9A-F]{8})$', text)
        if m:
            t = int(m.group(1), 16)
            if t == ENTER:
                if last_ecx and 0x36ED20 <= last_ecx < 0x4BE5A0: ev.append(('enter', last_ecx, a))
            else:
                ev.append(('call', t, a))
        if text == 'call [%s]' % LEAVE_THUNK and last_push and 0x36ED20 <= last_push < 0x4BE5A0:
            ev.append(('leave', last_push, a))
    if ev: events[f] = ev

# locks a function may take, itself or through direct calls, to the given depth
memo = {}
def takes(f, d):
    key = (f, d)
    if key in memo: return memo[key]
    memo[key] = set()
    out = set()
    for kind, v, a in events.get(f, ()):
        if kind == 'enter': out.add(v)
        elif kind == 'call' and d > 0: out |= takes(v, d - 1)
    memo[key] = out
    return out

pairs = defaultdict(list)  # (held, taken) -> [(function, address, via)]
for f, ev in events.items():
    held = []
    for kind, v, a in ev:
        if kind == 'enter':
            for h in held:
                if h != v: pairs[(h, v)].append((f, a, None))
            held.append(v)
        elif kind == 'leave':
            if v in held: held.remove(v)
        elif kind == 'call' and held:
            for t in takes(v, depth):
                for h in held:
                    if h != t: pairs[(h, t)].append((f, a, v))

locks = sorted({x for p in pairs for x in p})
print('%d locks in nested use, %d ordered pairs' % (len(locks), len(pairs)))
done = set()
for (a, b), where in sorted(pairs.items()):
    if (b, a) not in pairs or (b, a) in done: continue
    if only and only not in (a, b): continue
    done.add((a, b))
    print('\nINVERSION %06X <-> %06X' % (a, b))
    for x, y in ((a, b), (b, a)):
        print('  %06X held, %06X taken:' % (x, y))
        for f, at, via in pairs[(x, y)][:12]:
            print('    in sub_%08X at %08X%s' % (f, at, ' through sub_%08X' % via if via else ''))
        if len(pairs[(x, y)]) > 12: print('    ... %d more' % (len(pairs[(x, y)]) - 12))
