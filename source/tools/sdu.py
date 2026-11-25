"""The waves of one of the game's sound files (.sdu, read out of the disc image with xiso.py): the list, or one
wave's header and data.   python sdu.py FILE list      python sdu.py FILE head ID(hex)      python sdu.py FILE raw ID OUT"""
import struct, sys

def directory(d):
    # entries of 14 bytes at the end: u16 kind (0x302 header, 0x304 data), u32 id, u32 offset, u32 size
    at = len(d) - 14
    while at > 0 and struct.unpack_from('<H', d, at)[0] in (0x302, 0x304):
        at -= 14
    at += 14
    out = {}
    while at + 14 <= len(d):
        kind, ident, off, size = struct.unpack_from('<HIII', d, at)
        out.setdefault(ident, {})[kind] = (off, size)
        at += 14
    return out

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    waves = directory(d)
    if sys.argv[2] == 'list':
        for ident in sorted(waves):
            w = waves[ident]
            h = w.get(0x302, (0, 0)); a = w.get(0x304, (0, 0))
            hd = struct.unpack_from('<15I', d, h[0]) if h[1] >= 60 else ()
            print('%08X data %7d at %07X  head %s' % (ident, a[1], a[0], ' '.join('%X' % x for x in hd)))
    else:
        w = waves[int(sys.argv[3], 16)]
        if sys.argv[2] == 'head':
            off, size = w[0x302]
            print(d[off:off + size].hex(' ', 4))
        else:
            off, size = w[0x304]
            open(sys.argv[4], 'wb').write(d[off:off + size])
            print('wrote', size)
