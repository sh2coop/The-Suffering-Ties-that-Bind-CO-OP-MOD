"""Lists or reads files of an Xbox disc image (XDVDFS), read-only.
  python xiso.py IMAGE list [substring]          paths and sizes
  python xiso.py IMAGE get PATH OUT              one file written to OUT
The image may start with the DVD-video part (the game part then begins at 0x18300000)."""
import struct, sys

SECTOR = 2048


def open_image(path):
    f = open(path, 'rb')
    for base in (0, 0x18300000, 0xFD90000, 0x2080000):
        f.seek(base + 32 * SECTOR)
        head = f.read(32)
        if head[:20] == b'MICROSOFT*XBOX*MEDIA':
            root, size = struct.unpack_from('<II', head, 20)
            return f, base, root, size
    raise SystemExit('not an Xbox disc image')


def walk(f, base, sector, size, prefix, out):
    if not size:
        return
    f.seek(base + sector * SECTOR)
    table = f.read(size)

    def node(at):
        if at + 14 > len(table):
            return
        left, right, start, length, attr, n = struct.unpack_from('<HHIIBB', table, at)
        if left == 0xFFFF:
            return
        name = table[at + 14:at + 14 + n].decode('latin-1')
        if left:
            node(left * 4)
        if attr & 0x10:
            walk(f, base, start, length, prefix + name + '/', out)
        else:
            out.append((prefix + name, start, length))
        if right:
            node(right * 4)

    node(0)


def files(path):
    f, base, root, size = open_image(path)
    out = []
    walk(f, base, root, size, '', out)
    return f, base, out


if __name__ == '__main__':
    f, base, out = files(sys.argv[1])
    if sys.argv[2] == 'list':
        want = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        for name, start, length in sorted(out):
            if want in name.lower():
                print('%10d  %s' % (length, name))
    else:
        want = sys.argv[3].replace('\\', '/').lower()
        for name, start, length in out:
            if name.lower() == want:
                f.seek(base + start * SECTOR)
                open(sys.argv[4], 'wb').write(f.read(length))
                print('wrote', length)
                break
        else:
            raise SystemExit('no such file')
