"""Compares runs made with S2_MEASURE=1 and scripts/measure.txt: how far Torque walked, how far he turned, how
high and how long he jumped, in each run. The same numbers at 60 and at 120 frames a second mean the game's speed
does not follow the frame rate.   usage: python measure.py runs/m60/log.txt runs/m120/log.txt [more logs]"""
import math, os, re, sys

LINE = re.compile(r'measure: t ([\d.]+) frame (\d+) dt ([\d.]+) pos ([-\d.]+) ([-\d.]+) ([-\d.]+) yaw ([-\d.]+)(?: carried (\d+))?')

def load(path):
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if m: rows.append(tuple(float(x) if x is not None else float('nan') for x in m.groups()))
    return rows

def window(rows, a, b):
    return [r for r in rows if a <= r[0] <= b]

def dist(rows):
    if len(rows) < 2: return float('nan')
    return math.hypot(rows[-1][3] - rows[0][3], rows[-1][5] - rows[0][5])

def turn(rows):
    total = 0.0
    for p, q in zip(rows, rows[1:]):
        d = q[6] - p[6]
        while d > 180: d -= 360
        while d < -180: d += 360
        total += d
    return total

def jump(rows):
    """height over the ground before, and seconds off it"""
    if len(rows) < 3: return float('nan'), float('nan')
    ground = rows[0][4]
    up = [r for r in rows if r[4] > ground + 0.02]
    if not up: return 0.0, 0.0
    return max(r[4] for r in up) - ground, up[-1][0] - up[0][0] + 0.05

def report(path):
    rows = load(path)
    if not rows: return None
    play = window(rows, 19, 41)
    fps = (play[-1][1] - play[0][1]) / (play[-1][0] - play[0][0]) if len(play) > 1 else float('nan')
    dts = sorted(r[2] for r in play)
    h1, a1 = jump(window(rows, 27.8, 30.5))
    h2, a2 = jump(window(rows, 35.4, 38.5))
    # firing: when what he carries steps down (a reload), seconds after the trigger was pulled at 43
    fire = window(rows, 42.5, 62)
    steps = [q[0] - 43.0 for p_, q in zip(fire, fire[1:]) if q[7] < p_[7]]
    spread = (max(dts) - min(dts)) * 1000 if dts else float('nan')
    return {
        'frames a second': fps,
        "the game's frame time (ms, median)": dts[len(dts) // 2] * 1000 if dts else float('nan'),
        "the game's frame time: longest less shortest (ms)": spread,
        'firing: first reload after (s)': steps[0] if steps else float('nan'),
        'firing: second reload after (s)': steps[1] if len(steps) > 1 else float('nan'),
        'firing: third reload after (s)': steps[2] if len(steps) > 2 else float('nan'),
        'walk forward 3 s (m)': dist(window(rows, 19.9, 24.0)),
        'turn right 1 s (degrees)': turn(window(rows, 24.9, 27.0)),
        'jump on the spot: height (m)': h1,
        'jump on the spot: time off the ground (s)': a1,
        'walk left 2 s (m)': dist(window(rows, 30.9, 34.0)),
        'running jump: distance in 2 s (m)': dist(window(rows, 34.9, 38.5)),
        'running jump: height (m)': h2,
        'running jump: time off the ground (s)': a2,
        'turn left 1 s (degrees)': turn(window(rows, 38.9, 41.0)),
    }

paths = sys.argv[1:]
reports = [report(p) for p in paths]
names = [os.path.basename(os.path.dirname(os.path.abspath(p))) for p in paths]
print('%-44s' % '' + ''.join('%12s' % n[:11] for n in names))
keys = next(r for r in reports if r).keys()
for k in keys:
    print('%-44s' % k + ''.join('%12.3f' % (r[k] if r else float('nan')) for r in reports))
