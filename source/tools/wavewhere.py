"""Which sound files of the disc image hold a wave.   python wavewhere.py IMAGE ID(hex) [OUT to save that file]"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xiso, sdu
f, base, out = xiso.files(sys.argv[1])
want = int(sys.argv[2], 16)
for name, start, length in sorted(out):
    if not name.lower().endswith('.sdu') or name.lower().startswith(('french', 'english/pal')): continue
    f.seek(base + start * 2048)
    d = f.read(length)
    waves = sdu.directory(d)
    if want in waves:
        print(name, len(waves), 'waves')
        if len(sys.argv) > 3:
            open(sys.argv[3], 'wb').write(d); break
