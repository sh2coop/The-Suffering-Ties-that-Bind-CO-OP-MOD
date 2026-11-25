"""For the functions classmap.py could not place: the nearest functions below and above that belong to a class
(object files are laid out one after another, so neighbours are the same source file as a rule).
usage: python classnear.py OWNER.tsv"""
import bisect, os, struct, sys
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'classmap.py')).read().split('uses = []')[0])
spec = sorted(fn for fn, o in owner.items() if len({c for c, s in o}) <= 3)
def nm(fn): return '/'.join(sorted({names.get(c, '%08X' % c) for c, s in owner[fn]}))
for line in open(sys.argv[1]):
    f = line.rstrip('\n').split('\t')
    if len(f) < 4 or line.startswith('#'): continue
    fn = int(f[0], 16)
    if f[3] != '?': print(line.rstrip('\n')); continue
    i = bisect.bisect(spec, fn)
    lo = spec[i - 1] if i else None; hi = spec[i] if i < len(spec) else None
    print('%s\t%s\t%s\tnear: %s (-0x%X) .. %s (+0x%X)' % (f[0], f[1], f[2], nm(lo), fn - lo, nm(hi), hi - fn))
