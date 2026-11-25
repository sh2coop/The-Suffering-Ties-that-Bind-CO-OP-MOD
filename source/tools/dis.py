"""Prints the original instructions of one recompiled function from generated/game (the comment lines), with labels.
usage: python dis.py 0013E890 [more addresses]      python dis.py --calls 0013E890   (only calls, jumps and labels)"""
import glob, os, re, sys

gen = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'game')
only = '--calls' in sys.argv
want = [a.upper().zfill(8) for a in sys.argv[1:] if not a.startswith('--')]
heads = {'X86_FUNC(__imp__sub_%s) {' % a: a for a in want}
for path in sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp'))):
    inside = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            s = line.rstrip('\n')
            if inside is None:
                if s in heads:
                    inside = heads[s]
                    print('== sub_%s  (%s)' % (inside, os.path.basename(path)))
                continue
            if s.startswith('X86_WEAK_FUNC('):
                inside = None
                continue
            if s.startswith('loc_'):
                print(s)
            elif s.startswith('\t// '):
                t = s[4:]
                if not only or re.search(r' (call|j[a-z]+|ret|loop[a-z]*) ', t + ' '): print(t)
