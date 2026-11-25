"""A sheet of a run's frame captures, side by side. usage: python sheet.py RUN [WIDTH_EACH] [FIRST [COUNT]]"""
import glob, os, sys
from PIL import Image
run = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runs', sys.argv[1])
w = int(sys.argv[2]) if len(sys.argv) > 2 else 480
fs = sorted(glob.glob(os.path.join(run, '*.bmp')))
a = int(sys.argv[3]) if len(sys.argv) > 3 else 0
fs = fs[a:a + (int(sys.argv[4]) if len(sys.argv) > 4 else 4)]
ims = [Image.open(f) for f in fs]
for im in ims: im.thumbnail((w, w))
sheet = Image.new('RGB', (sum(i.width for i in ims), max(i.height for i in ims)))
x = 0
for i in ims: sheet.paste(i, (x, 0)); x += i.width
sheet.save(os.path.join(run, 'sheet.png')); print(len(ims), 'pictures')
