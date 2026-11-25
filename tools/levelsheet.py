"""One picture of the level smoke test: a row per level (runs/levels_<name>), its captures left to right.
usage: python levelsheet.py [out.png]"""
import glob, os, sys
from PIL import Image, ImageDraw

proj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
runs = os.path.join(proj, 'runs')
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(runs, 'levels_sheet.png')
rows = []
for d in sorted(glob.glob(os.path.join(runs, 'levels_*'))):
    if not os.path.isdir(d): continue
    pics = sorted(glob.glob(os.path.join(d, '*.bmp')))
    rows.append((os.path.basename(d)[7:], pics))
if not rows: sys.exit('no level runs')
tw, th, cols = 320, 240, max(len(r[1]) for r in rows) or 1
sheet = Image.new('RGB', (cols * (tw + 4) + 4, len(rows) * (th + 22) + 4), (30, 30, 30))
draw = ImageDraw.Draw(sheet)
for y, (name, pics) in enumerate(rows):
    top = y * (th + 22) + 4
    draw.text((6, top), name + ('' if pics else '   (no captures)'), fill=(255, 255, 255))
    for x, p in enumerate(pics):
        sheet.paste(Image.open(p).convert('RGB').resize((tw, th)), (x * (tw + 4) + 4, top + 16))
sheet.save(out)
print(out, sheet.size, len(rows), 'levels')
