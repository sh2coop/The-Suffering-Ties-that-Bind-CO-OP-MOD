"""Where did the game crash? Reads the newest minidump in CrashDumps (or the one given), finds the module of the
exception address, and for Suffering2.dll names the nearest exported recompiled function below it.
usage: python crashsite.py [dump] [--rel <release dir>] [--addr 0x7ff...]"""
import glob, os, struct, sys

args = sys.argv[1:]
rel = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release')
addr_arg = None
dump = None
i = 0
while i < len(args):
    if args[i] == '--rel': rel = args[i + 1]; i += 2
    elif args[i] == '--addr': addr_arg = int(args[i + 1], 16); i += 2
    else: dump = args[i]; i += 1
if dump is None:
    dumps = sorted(glob.glob(os.path.join(rel, 'CrashDumps', '*.dmp')), key=os.path.getmtime)
    if not dumps: sys.exit('no dumps')
    dump = dumps[-1]
print('dump:', dump)
d = open(dump, 'rb').read()
sig, ver, nstreams, dir_rva = struct.unpack_from('<IIII', d, 0)
assert sig == 0x504D444D
streams = {}
for n in range(nstreams):
    t, size, rva = struct.unpack_from('<III', d, dir_rva + n * 12)
    streams[t] = (rva, size)

def mdstring(rva):
    n, = struct.unpack_from('<I', d, rva)
    return d[rva + 4: rva + 4 + n].decode('utf-16le')

modules = []
rva, size = streams[4]
count, = struct.unpack_from('<I', d, rva)
for n in range(count):
    base, msize, chk, ts, name_rva = struct.unpack_from('<QIIII', d, rva + 4 + n * 108)
    modules.append((base, msize, mdstring(name_rva)))

exc_addr = addr_arg
tid = None
if 6 in streams:
    rva, size = streams[6]
    tid, _ = struct.unpack_from('<II', d, rva)
    code, flags, rec, addr, nparam = struct.unpack_from('<IIQQI', d, rva + 8)
    params = struct.unpack_from('<15Q', d, rva + 8 + 32)
    print('exception %08X at %016X thread %d params %s' % (code, addr, tid, ' '.join('%X' % p for p in params[:nparam])))
    if exc_addr is None: exc_addr = addr

def exports(path):
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    nsec = struct.unpack_from('<H', b, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', b, opt)[0]
    ddir = opt + (112 if magic == 0x20B else 96)
    exp_rva = struct.unpack_from('<I', b, ddir)[0]
    secs = []
    so = opt + struct.unpack_from('<H', b, pe + 20)[0]
    for n in range(nsec):
        vsize, va, rsize, raw = struct.unpack_from('<IIII', b, so + n * 40 + 8)
        secs.append((va, max(vsize, rsize), raw))
    def off(r):
        for va, sz, raw in secs:
            if va <= r < va + sz: return raw + r - va
        raise ValueError(hex(r))
    e = off(exp_rva)
    nfunc, nname = struct.unpack_from('<II', b, e + 20)
    afunc, aname, aord = struct.unpack_from('<III', b, e + 28)
    out = []
    for n in range(nname):
        nr = struct.unpack_from('<I', b, off(aname) + n * 4)[0]
        o = struct.unpack_from('<H', b, off(aord) + n * 2)[0]
        fr = struct.unpack_from('<I', b, off(afunc) + o * 4)[0]
        s = off(nr)
        out.append((fr, b[s: b.index(b'\0', s)].decode()))
    out.sort()
    return out

def where(a):
    for base, msize, name in modules:
        if base <= a < base + msize:
            r = a - base
            s = '%s+0x%X' % (os.path.basename(name), r)
            if os.path.basename(name).lower() == 'suffering2.dll':
                ex = exports(os.path.join(rel, 'Suffering2.dll'))
                best = None
                for fr, nm in ex:
                    if fr <= r: best = (fr, nm)
                    else: break
                if best: s += '  = %s+0x%X' % (best[1], r - best[0])
            return s
    return '?'

if exc_addr is not None:
    print('at', where(exc_addr))
