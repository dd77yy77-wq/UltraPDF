#!/usr/bin/env python3
"""Build Shchitok.apk without Android SDK. Usage: build.py <versionCode> <versionName> <out.apk>"""
import sys, os, io, struct, subprocess, shutil, datetime
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tools'))
import apkbuild as A
from apkbuild import N, T_REF, T_STR, T_INT, T_HEX, T_BOOL
from PIL import Image, ImageDraw
import make_icon

HERE = os.path.dirname(os.path.abspath(__file__))
code, name, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
PKG = 'com.dd.shchitok'
DXCP = os.environ.get('DXCP', '/tmp/dalvik/dxout')   # см. tools/setup_dx.sh
KEYDIR = os.environ.get('KEYDIR', '/home/claude/keys')
B = os.path.join(HERE, 'build'); shutil.rmtree(B, ignore_errors=True)
for d in ('stubcls', 'classes', 'dex'): os.makedirs(os.path.join(B, d))

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    err = '\n'.join(l for l in (r.stdout + r.stderr).splitlines() if 'JAVA_TOOL_OPTIONS' not in l)
    if r.returncode: sys.exit('FAILED: %s\n%s' % (' '.join(cmd), err))
    return err

# 1. dex
srcs = lambda root: [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(HERE, root)) for f in fs if f.endswith('.java')]
run(['javac', '-nowarn', '-source', '8', '-target', '8', '-Xlint:-options', '-d', B + '/stubcls'] + srcs('stubs'))
run(['javac', '-encoding', 'UTF-8', '-source', '8', '-target', '8', '-Xlint:-options', '-g:none', '-cp', B + '/stubcls', '-d', B + '/classes'] + srcs('src'))
run(['java', '-cp', DXCP, 'com.android.dx.command.Main', '--dex', '--min-sdk-version=24', '--output=' + B + '/dex/classes.dex', B + '/classes'])
dex = open(B + '/dex/classes.dex', 'rb').read()

# 2. icon resources
bg = make_icon.art(432)
def png(im):
    b = io.BytesIO(); im.save(b, 'PNG', optimize=True); return b.getvalue()
files = {}  # path -> bytes (stored)
files['res/drawable-nodpi-v4/ic_launcher_bg.png'] = png(bg)
core = bg.crop((72, 72, 360, 360))
def legacy(sz):
    im = core.resize((sz, sz), Image.LANCZOS).convert('RGBA')
    m = Image.new('L', (sz*4, sz*4), 0); ImageDraw.Draw(m).rounded_rectangle((0, 0, sz*4-1, sz*4-1), int(sz*4*0.2), fill=255)
    im.putalpha(m.resize((sz, sz), Image.LANCZOS)); return png(im)
dens = [(160, 'mdpi', 48), (240, 'hdpi', 72), (320, 'xhdpi', 96), (480, 'xxhdpi', 144), (640, 'xxxhdpi', 192)]
def cfg(density, sdk=0):
    c = bytearray(64); struct.pack_into('<I', c, 0, 64); struct.pack_into('<H', c, 14, density); struct.pack_into('<H', c, 24, sdk); return bytes(c)
mip_cfgs = []
for dpi, q, sz in dens:
    p = 'res/mipmap-%s-v4/ic_launcher.png' % q; files[p] = legacy(sz); mip_cfgs.append((cfg(dpi), p))
files['res/mipmap-anydpi-v26/ic_launcher.xml'] = A.build_axml(
    N('adaptive-icon', [], [N('background', [(True, 'drawable', T_REF, 0x7f010000)]), N('foreground', [(True, 'drawable', T_REF, 0x0106000d)])]), True)
mip_cfgs.append((cfg(0xfffe, 26), 'res/mipmap-anydpi-v26/ic_launcher.xml'))
arsc = A.build_arsc(PKG, [
    ('drawable', [('ic_launcher_bg', [(cfg(0xffff), 'res/drawable-nodpi-v4/ic_launcher_bg.png')])], 0x100),
    ('mipmap', [('ic_launcher', mip_cfgs)], 0x500),
])

# 3. manifest
a = lambda n, t, v: (True, n, t, v)
A.ATTR_IDS.update({'configChanges': 0x0101001f, 'authorities': 0x01010018, 'grantUriPermissions': 0x0101001b})
manifest = A.build_axml(N('manifest', [a('versionCode', T_INT, code), a('versionName', T_STR, name), (False, 'package', T_STR, PKG)], [
    N('uses-permission', [a('name', T_STR, 'android.permission.INTERNET')]),
    N('uses-permission', [a('name', T_STR, 'android.permission.REQUEST_INSTALL_PACKAGES')]),
    N('uses-sdk', [a('minSdkVersion', T_INT, 24), a('targetSdkVersion', T_INT, 34)]),
    N('application', [a('theme', T_REF, 0x0103022e), a('label', T_STR, 'Щиток'), a('icon', T_REF, 0x7f020000), a('allowBackup', T_BOOL, 0xffffffff)], [
        N('activity', [a('name', T_STR, PKG + '.MainActivity'), a('exported', T_BOOL, 0xffffffff),
                       a('configChanges', T_HEX, 0x40003fb4), a('windowSoftInputMode', T_HEX, 0x10)], [
            N('intent-filter', [], [N('action', [a('name', T_STR, 'android.intent.action.MAIN')]),
                                    N('category', [a('name', T_STR, 'android.intent.category.LAUNCHER')])])]),
        N('provider', [a('name', T_STR, PKG + '.ApkProvider'), a('exported', T_BOOL, 0), a('authorities', T_STR, PKG + '.apk'),
                       a('grantUriPermissions', T_BOOL, 0xffffffff)])])]), False)

# 4. entries
entries = [('AndroidManifest.xml', manifest, 'deflate'), ('resources.arsc', arsc, 'stored')]
for p in sorted(files): entries.append((p, files[p], 'stored'))
entries.append(('classes.dex', dex, 'deflate'))
for dp, _, fs in sorted(os.walk(os.path.join(HERE, 'assets'))):
    for f in sorted(fs):
        full = os.path.join(dp, f); rel = 'assets/' + os.path.relpath(full, os.path.join(HERE, 'assets')).replace(os.sep, '/')
        entries.append((rel, open(full, 'rb').read(), 'stored' if f.endswith(('.jpg', '.png')) else 'deflate'))

# 5. sign
from cryptography.hazmat.primitives.serialization import pkcs12
key, cert, _ = pkcs12.load_key_and_certificates(open(os.path.join(KEYDIR, 'shchitok.p12'), 'rb').read(), b'android')
meta = A.v1_files([(n, d) for n, d, _ in entries], cert, key)
# order: manifest first like apksigner does, then everything else, META-INF last
entries = entries + [(n, d, 'deflate') for n, d in meta]
apk = A.write_zip(entries)
apk = A.v2_sign(apk, cert, key)
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
open(out, 'wb').write(apk)
print('built', out, len(apk), 'bytes')
