"""Which recompiled function holds each address (the start address at or before it), with the function's size and
the number of direct callers.  usage: python whose.py 1F4685 1F487B ..."""
import bisect, glob, os, re, sys

proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
starts = []
sizes = {}
callers = {}
func_re = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
call_re = re.compile(r'^\t// [0-9A-F]{8} call 0x([0-9A-F]{8})')
last_re = re.compile(r'^\t// ([0-9A-F]{8}) ')
for path in sorted(glob.glob(os.path.join(proj, 'generated', 'game', 'x86_recomp.*.cpp'))):
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = func_re.match(line)
        if m:
            cur = int(m.group(1), 16)
            starts.append(cur)
            sizes[cur] = cur
            continue
        if cur is None or not line.startswith('\t// '): continue
        m = last_re.match(line)
        if m: sizes[cur] = max(sizes[cur], int(m.group(1), 16))
        m = call_re.match(line)
        if m:
            t = int(m.group(1), 16)
            callers[t] = callers.get(t, 0) + 1
starts.sort()
for a in sys.argv[1:]:
    v = int(a, 16)
    i = bisect.bisect_right(starts, v) - 1
    if i < 0:
        print('%06X  before the first function' % v)
        continue
    f = starts[i]
    inside = v <= sizes[f] + 16
    print('%06X  in sub_%08X +0x%X  (to %06X, %d direct callers)%s' % (v, f, v - f, sizes[f], callers.get(f, 0), '' if inside else '  PAST ITS END'))
