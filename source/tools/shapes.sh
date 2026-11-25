#!/bin/bash
# The picture at several window shapes, side by side: one run per shape (each with its own time limit), one frame
# captured at SECONDS, all put on one sheet as they are shown (the 640 x 480 frame stretched to the window's shape).
#   bash tools/shapes.sh NAME SECONDS "LEVEL or empty for the start-up screens" "W1xH1 W2xH2 ..." [capture times, default SECONDS-2]
proj="$(cd "$(dirname "$0")/.." && pwd)"
cd "$proj" || exit 2
name="$1"; secs="$2"; level="$3"; shapes="$4"; caps="${5:-$((secs - 2))}"
for wh in $shapes; do
    w=${wh%x*}; h=${wh#*x}
    args=(-Seconds "$secs" -FpsCap 60 -VSync -Tail 0 -Captures "$caps" -Width "$w" -Height "$h" -Name "${name}_$wh")
    [ -n "$level" ] && args+=(-Level "$level")
    timeout $((secs + 50)) powershell -NoProfile -ExecutionPolicy Bypass -File tools/run.ps1 "${args[@]}" 2>&1 | grep -i "FREEZE\|exited\|not running" | cut -c1-160
done
python - "$name" $shapes <<'EOF'
import glob, sys
from PIL import Image, ImageDraw
name, shapes = sys.argv[1], sys.argv[2:]
H = 360
rows = []
for wh in shapes:
    w, h = (int(v) for v in wh.split('x'))
    files = sorted(glob.glob('runs/%s_%s/*.bmp' % (name, wh)))
    tiles = []
    for f in files:
        im = Image.open(f).convert('RGB').resize((round(H * w / h), H), Image.LANCZOS)
        ImageDraw.Draw(im).text((6, 4), wh, fill=(255, 255, 0))
        tiles.append(im)
    rows.append(tiles)
width = max(sum(t.width + 8 for t in r) for r in rows if r) if any(rows) else 10
sheet = Image.new('RGB', (width, len(rows) * (H + 8)), (70, 0, 70))
for y, r in enumerate(rows):
    x = 0
    for t in r:
        sheet.paste(t, (x, y * (H + 8))); x += t.width + 8
sheet.save('runs/%s_sheet.png' % name)
print('runs/%s_sheet.png' % name, sheet.size)
EOF
