# Per stick push in a movement log: Kat's (look-at) displacement against the camera's forward.
import math, re, sys
rows = []
# "#event" lines (drive.py): a textbox, void-out or fall; pushes containing one are left out.
events = []
for line in open(sys.argv[1], errors='replace'):
    e = re.match(r'\s*([\d.]+) #event (\w+)', line)
    if e:
        events.append((float(e.group(1)), e.group(2)))
        continue
    m = re.match(r'\s*([\d.]+) Move: sticks L (\d+),(\d+) .*camera ([-\d.]+) ([-\d.]+) ([-\d.]+) look-at ([-\d.]+) ([-\d.]+) ([-\d.]+)', line)
    if m:
        v = [float(x) for x in m.groups()]
        rows.append((v[0], int(v[1]), int(v[2]), v[3:6], v[6:9]))
names = {(128, 0): 'up', (128, 255): 'down', (255, 128): 'right', (0, 128): 'left'}
segs, cur = [], None
for r in rows:
    key = (r[1], r[2])
    if cur and cur[0] == key and r[0] - cur[-1][0] < 0.5: cur.append(r)
    else:
        if cur: segs.append(cur)
        cur = [key, r]
if cur: segs.append(cur)
def interrupted(s):
    return [k for t, k in events if s[1][0] - 0.5 <= t <= s[-1][0] + 0.5]
segs = [s for s in segs if not interrupted(s) or print(f'{s[1][0]:7.1f} {names.get(s[0], "?"):5} left out ({", ".join(interrupted(s))})')]
for s in segs:
    key, first, last = s[0], s[1], s[-1]
    if key not in names or len(s) < 6: continue
    cam, look = first[3], first[4]
    fx, fz = look[0] - cam[0], look[2] - cam[2]
    dx, dz = last[4][0] - look[0], last[4][2] - look[2]
    dist = math.hypot(dx, dz)
    if dist < 0.05:
        print(f'{first[0]:7.1f} {names[key]:5} no movement'); continue
    rel = math.degrees(math.atan2(fx * dz - fz * dx, fx * dx + fz * dz))  # displacement angle from forward
    print(f'{first[0]:7.1f} {names[key]:5} moved {dist:5.2f} at {rel:7.1f} deg from camera forward '
          f'(heading {math.degrees(math.atan2(fx, fz)):7.1f}, {len(s)-1} samples)')
# The angle between the world directions of the up and the right pushes (90 when correct).
ups, rights = [], []
for s in segs:
    key, last = s[0], s[-1]
    first = s[1 + (len(s) - 1) // 2]  # the second half: running straight, past the turn
    d = (last[4][0] - first[4][0], last[4][2] - first[4][2])
    if math.hypot(*d) < 0.2: continue
    (ups if key == (128, 0) else rights if key == (255, 128) else []).append(d)
if ups and rights:
    a = [abs(math.degrees(math.atan2(u[0]*r[1]-u[1]*r[0], u[0]*r[0]+u[1]*r[1]))) for u in ups for r in rights]
    print('UP-vs-RIGHT angle: %.0f deg (mean of %d pairs; 90 is correct, 0 or 180 is collapsed)' % (sum(a)/len(a), len(a)))
else:
    print('UP-vs-RIGHT angle: not measured (no movement)')
