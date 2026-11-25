# Share of running samples moving exactly along the x = z diagonal (the bug: z speed = x speed).
import math, re, sys
prev = None; diag = total = 0
# Samples within 2 s of an "#event" line (drive.py: textbox, void-out, fall) are left out.
events = [float(m.group(1)) for m in (re.match(r'\s*([\d.]+) #event', l) for l in open(sys.argv[1], errors='replace')) if m]
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\s*([\d.]+) Move: sticks L (\d+),(\d+) .*look-at ([-\d.]+) ([-\d.]+) ([-\d.]+)', line)
    if not m: continue
    r = [float(x) for x in m.groups()]
    if prev and (r[1], r[2]) == (prev[1], prev[2]) and (r[1], r[2]) != (128, 128) and 0 < r[0] - prev[0] < 0.5:
        vx, vz = r[3] - prev[3], r[5] - prev[5]
        sp = math.hypot(vx, vz)
        if sp / (r[0] - prev[0]) > 1.0 and not any(abs(t - r[0]) < 2 for t in events):
            total += 1
            diag += abs(vx - vz) < 0.03 * sp
    prev = r
print(f'diagonal {diag}/{total} = {100*diag/max(total,1):.0f}% (bug: most; correct: few)' if total >= 10 else f'not enough movement ({total} samples)')
