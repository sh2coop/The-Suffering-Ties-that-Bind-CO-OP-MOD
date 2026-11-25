"""Summarise the frame captures of a run: for each capture folder's final.bmp, the mean colour of the top bar, the
bottom bar and the middle of the picture, and how much of the frame is pure white. Flags frames with white bars.
usage: python capstat.py [cap dir] [--sheet out.png]"""
import os, sys
from PIL import Image

argv = sys.argv[1:]
sheet_out = None
if '--sheet' in argv:
    i = argv.index('--sheet'); sheet_out = argv[i + 1]; del argv[i:i + 2]
args = argv
cap = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'build', 'release', 'cap')

def mean(im, box):
    c = im.crop(box).resize((1, 1), Image.BOX).getpixel((0, 0))
    return '%3d %3d %3d' % c

rows = []
thumbs = []
for d in sorted(os.listdir(cap)):
    p = os.path.join(cap, d, 'final.bmp')
    if not os.path.exists(p):
        continue
    im = Image.open(p).convert('RGB')
    w, h = im.size
    hist = im.histogram()
    white = sum(1 for px in im.resize((64, 48)).getdata() if min(px) >= 250) / (64 * 48.0)
    draws = 0
    ft = os.path.join(cap, d, 'frame.txt')
    if os.path.exists(ft):
        with open(ft, errors='replace') as f:
            draws = sum(1 for line in f if line[:4].isdigit() and ' draw ' in line[:12])
    top, mid, bot = mean(im, (0, 0, w, 8)), mean(im, (w // 4, h // 4, 3 * w // 4, 3 * h // 4)), mean(im, (0, h - 8, w, h))
    flag = ''
    if white > 0.9: flag = 'WHITE FRAME'
    elif top.split() == ['255'] * 3 or bot.split() == ['255'] * 3: flag = 'WHITE BAR'
    rows.append((d, w, h, draws, top, mid, bot, white, flag))
    thumbs.append((d, flag, im.resize((160, 120))))

for r in rows:
    print('%s %dx%d draws %3d  top [%s] mid [%s] bottom [%s] white %3.0f%% %s' % (r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7] * 100, r[8]))
bad = [r for r in rows if r[8]]
print('%d captures, %d with white bars or white frames' % (len(rows), len(bad)))
if sheet_out and thumbs:
    cols = 8
    sheet = Image.new('RGB', (cols * 164, ((len(thumbs) + cols - 1) // cols) * 124), (60, 0, 60))
    for i, (d, flag, t) in enumerate(thumbs):
        sheet.paste(t, ((i % cols) * 164 + 2, (i // cols) * 124 + 2))
    sheet.save(sheet_out)
    print('sheet', sheet_out, sheet.size)
