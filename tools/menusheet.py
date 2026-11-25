"""The menu side of a run's frame captures (640 x 480 game frames), enlarged, side by side: for reading the
Display and Co-op page's texts.  usage: python menusheet.py RUNFOLDER OUT.png [columns=4] [first=0] [count=all]"""
import glob, os, sys
from PIL import Image

run, out = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
first = int(sys.argv[4]) if len(sys.argv) > 4 else 0
count = int(sys.argv[5]) if len(sys.argv) > 5 else 0
files = sorted(glob.glob(os.path.join(run, '*.bmp')))[first:]
if count: files = files[:count]
box = (285, 60, 640, 440)   # the page's texts
# shown 16:9, a 640 x 480 frame is stretched across: the same here, so the letters have their shape
cw, ch = int((box[2] - box[0]) * 2 * 4 / 3), (box[3] - box[1]) * 2
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (cols * cw + (cols - 1) * 6, rows * ch + (rows - 1) * 6), (60, 60, 60))
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').crop(box).resize((cw, ch), Image.LANCZOS)
    sheet.paste(im, ((i % cols) * (cw + 6), (i // cols) * (ch + 6)))
sheet.save(out)
print(out, sheet.size, len(files), 'frames')
