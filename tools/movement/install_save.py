# A decrypted PS4 save (data files + sce_sys/param.sfo) as the runtime's save directory:
# <out>/savedata/1/CUSA01113/<dir>/ and <dir>.sce_sys/{param.bin,icon0.png} (src/runtime_savedata.c).
import os, shutil, struct, sys, time
src, out = sys.argv[1], sys.argv[2]
d = open(os.path.join(src, 'sce_sys/param.sfo'), 'rb').read()
_, _, kt, dt, n = struct.unpack_from('<4sIIII', d, 0)
sfo = {}
for i in range(n):
    ko, fmt, ln, mx, do = struct.unpack_from('<HHIII', d, 20 + 16 * i)
    k = d[kt + ko:d.index(b'\0', kt + ko)].decode()
    v = d[dt + do:dt + do + ln]
    sfo[k] = struct.unpack('<I', v)[0] if fmt == 0x404 else v
name = sfo['SAVEDATA_DIRECTORY'].rstrip(b'\0').decode()
base = os.path.join(out, 'savedata/1', sfo['TITLE_ID'].rstrip(b'\0').decode())
data, meta = os.path.join(base, name), os.path.join(base, name + '.sce_sys')
os.makedirs(data, exist_ok=True); os.makedirs(meta, exist_ok=True)
for f in os.listdir(src):
    if f != 'sce_sys' and os.path.isfile(os.path.join(src, f)):
        shutil.copy2(os.path.join(src, f), os.path.join(data, f))
text = lambda k, size: sfo.get(k, b'').rstrip(b'\0')[:size - 1].ljust(size, b'\0')
param = text('MAINTITLE', 128) + text('SUBTITLE', 128) + text('DETAIL', 1024) + \
        struct.pack('<Iiq', sfo.get('SAVEDATA_LIST_PARAM', 0), 0, int(time.time())) + bytes(32)
assert len(param) == 1328
open(os.path.join(meta, 'param.bin'), 'wb').write(param)
icon = os.path.join(src, 'sce_sys/icon0.png')
if os.path.exists(icon): shutil.copy2(icon, os.path.join(meta, 'icon0.png'))
print('installed', name, 'in', base)
