"""What is done with "the player" once it is taken: per function in work/re/player_uses.txt, the calls and the
fields read in the lines that follow. Prints a line per function and a count of each call target.
usage: python playerwhat.py USES.txt"""
import re, sys, collections
cur = None; rows = {}; order = []
for line in open(sys.argv[1], encoding='utf-8'):
    m = re.match(r'== sub_([0-9A-F]{8})', line)
    if m: cur = m.group(1); rows[cur] = []; order.append(cur); continue
    m = re.match(r'\s+([0-9A-F]{8}) (.*)', line)
    if m and cur: rows[cur].append(m.group(2))
count = collections.Counter()
for fn in order:
    calls = []; fields = []
    for t in rows[fn]:
        c = re.match(r'call (0x[0-9A-F]{8}|\[.*\])', t)
        if c: calls.append(c.group(1).replace('0x00', ''))
        for f in re.findall(r'\[(?:esi|edi|ebx|ecx|eax|edx|ebp)\+0x([0-9A-F]+)\]', t):
            if f not in ('124', '12C'): fields.append(f)
    for c in set(calls): count[c] += 1
    print(fn, 'calls', ' '.join(dict.fromkeys(calls)) or '-', '| fields', ' '.join(dict.fromkeys(fields)) or '-')
print()
for c, n in count.most_common(40): print(n, c)
