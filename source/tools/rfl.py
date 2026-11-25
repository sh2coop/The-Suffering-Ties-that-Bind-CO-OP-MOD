"""The Riot engine's class table in the Xbox image (assets/default.xbe): every reflected class with its id, name,
property count and where its record is.  The record layout was read from the PS2 build (work\\re\\riot_classes.tsv):
{u32 class_id, u32 ?, u32 flags, char* name, u32 nprops, u32 ngroups, u32* groups}.
  python rfl.py [name filter]        list classes (case-insensitive substring)
  python rfl.py --props "GUI Audio"  also dump the dwords around the record and its group table
"""
import os, re, struct, sys

proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
data = open(os.path.join(proj, 'assets', 'default.xbe'), 'rb').read()
base, = struct.unpack_from('<I', data, 0x104)
nsec, sec_va = struct.unpack_from('<II', data, 0x11C)
sections = []
for i in range(nsec):
    flags, va, vsize, raw, rsize, name_va = struct.unpack_from('<IIIIII', data, sec_va - base + i * 0x38)
    sections.append((va, vsize, raw, rsize))

def off_of(va):
    for sva, vsize, raw, rsize in sections:
        if sva <= va < sva + rsize: return raw + va - sva
    return None

def va_of(off):
    for sva, vsize, raw, rsize in sections:
        if raw <= off < raw + rsize: return sva + off - raw
    return None

def cstr(va, limit=80):
    o = off_of(va)
    if o is None: return None
    e = data.find(b'\0', o, o + limit)
    if e < 0: return None
    s = data[o:e]
    return s.decode() if s and all(32 <= c < 127 for c in s) else None

def u32(va):
    o = off_of(va)
    return struct.unpack_from('<I', data, o)[0] if o is not None and o + 4 <= len(data) else None

# every pointer to a printable string, then keep those that sit in a record of the expected layout
classes = []
for sva, vsize, raw, rsize in sections:
    for off in range(raw, raw + rsize - 28, 4):
        cid, unk, flags, name_va, nprops, ngroups, groups = struct.unpack_from('<IIIIIII', data, off)
        if not (0x10000 <= name_va < 0x800000) or nprops > 400 or ngroups > 64 or not cid: continue
        if ngroups and off_of(groups) is None: continue
        name = cstr(name_va)
        if not name or len(name) < 3: continue
        classes.append((sva + off - raw, cid, flags, name, nprops, ngroups, groups))

# the table is one run of such records: keep the longest run of records 0x1C apart
classes.sort()
best, run = [], []
for c in classes:
    if run and c[0] - run[-1][0] == 0x1C: run.append(c)
    else:
        if len(run) > len(best): best = run
        run = [c]
if len(run) > len(best): best = run

args = [a for a in sys.argv[1:] if not a.startswith('--')]
flt = args[0].lower() if args else ''
dump = '--props' in sys.argv
print('# %d classes, table at %08X..%08X' % (len(best), best[0][0], best[-1][0] + 0x1C) if best else '# no table found')
for va, cid, flags, name, nprops, ngroups, groups in best:
    if flt and flt not in name.lower(): continue
    print('%08X  id %08X  flags %04X  props %3d  groups %2d at %08X  %s' % (va, cid, flags, nprops, ngroups, groups, name))
    if dump:
        for g in range(ngroups):
            row = [u32(groups + g * 4)]
            print('    group %d: %s' % (g, ' '.join('%08X' % v if v is not None else '?' for v in row)))
