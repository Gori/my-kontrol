#!/usr/bin/env python3
"""Package S MK1 firmware as xflash upgrade images (the file NI's updater and our DFU write).

usage:
  s49pack.py proof NI.bin [--out DIR]
      NI's upgrade image apart into its two segments, an .xe from them (xe.py,
      firmware/boards/s-mk1/s-mk1.xn), xflash over every --factory-version, each output
      compared with NI's file byte for byte
  s49pack.py image [--dev | --bringup] [--version N] [--factory-version V]
      our firmware, both tiles (make BOARD=s49 [DEV=1] [BRINGUP=1]) -> firmware/build/s49[...]/
      (--bringup: the USB bring-up build, platform/xs1/bringup.h; implies --dev)
  s49pack.py hybrid NI.bin [--version N] [--factory-version V]
      the bring-up image: our tile 0 (make BOARD=s49 HYBRID=1 DEV=1, the probe) and NI's
      tile 1 (its segment 1: USB, DFU, 5-pin) -> fw/kks/pack/ (it holds NI's code)

Every image is checked with ximage.py. Nothing here talks to a device: xflash only ever
gets -o (write a file). Files holding NI's bytes go to --out (default fw/kks/pack, outside
git), never into the repository. XTC: --xtc (default /Applications/XMOS_XTC_15.3.1); its XS1
part files are in configs/.deprecated, which xflash finds through XCC_DEVICE_PATH.

The factory version: NI's 1.2.0 file is what xflash writes for --factory-version 10, 11 or
12 (the tools-12 format, magic 0x01A551E5); 13 and later write another header. So the
factory image on the keyboard is a tools-12-era build, and 12 is the default here.
"""
import argparse, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FW = os.path.join(ROOT, 'firmware')
XN = os.path.join(FW, 'boards', 's-mk1', 's-mk1.xn')
sys.path.insert(0, HERE)
import xe, ximage

FACTORY_VERSIONS = ['10', '11', '12', '13.0', '14.4', '15.3']


def segments(raw):
    """[(flags, payload without its CRC word)] of an upgrade image, and its length L."""
    d = raw.translate(ximage.REV8)
    length, n = struct.unpack_from('<I', d, 0x0C)[0], struct.unpack_from('<I', d, 0x18)[0]
    out = []
    for k in range(n):
        off, flags, words = struct.unpack_from('<3I', d, 0x1C + 12 * k)
        out.append((flags, d[off:off + 4 * words - 4]))
    return out, length


def node_of(flags):
    """The JTAG chain index (s-mk1.xn) of a segment's node: the flags' top half is the node's
    routing id; 0x8000 (our tile 0) is first in the chain, 0 (the boot node) second."""
    return 0 if flags >> 16 == 0x8000 else 1


def xflash(xtc, xe_path, out, version, factory):
    env = dict(os.environ, XMOS_TOOL_PATH=xtc,
               XCC_DEVICE_PATH='%s/configs:%s/configs/.deprecated' % (xtc, xtc),
               PATH='%s/bin:%s' % (xtc, os.environ.get('PATH', '')))
    cmd = [os.path.join(xtc, 'bin', 'xflash'), '--upgrade', str(version), xe_path,
           '--factory-version', factory, '-o', out]
    assert '-o' in cmd                          # a file, never a device
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode or not os.path.exists(out):
        raise SystemExit('xflash failed: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return cmd


def make_xe(path, images):
    """images: [(JTAG index, RAM image)]"""
    open(path, 'wb').write(xe.build(images, open(XN, 'rb').read()))


def check(path):
    ok = ximage.check(open(path, 'rb').read())
    if not ok:
        raise SystemExit('%s: ximage.py --check failed' % path)


def proof(a):
    ni = open(a.ni, 'rb').read()
    segs, length = segments(ni)
    version = struct.unpack_from('<I', ni.translate(ximage.REV8), 0x10)[0]
    os.makedirs(a.out, exist_ok=True)
    xe_path = os.path.join(a.out, 'ni-rebuilt.xe')
    make_xe(xe_path, sorted((node_of(f), p) for f, p in segs))
    print('NI: %d bytes, L = %d, version %d, segments %s' % (
        len(ni), length, version, ', '.join('0x%08x %d B' % (f, len(p)) for f, p in segs)))
    best, matches = None, []
    for fv in FACTORY_VERSIONS:
        out = os.path.join(a.out, 'ni-rebuilt-fv%s.bin' % fv)
        cmd = xflash(a.xtc, xe_path, out, version, fv)
        mine = open(out, 'rb').read()
        diff = [i for i in range(min(len(mine), len(ni))) if mine[i] != ni[i]]
        inside = [i for i in diff if i < length]
        pad = sorted(set(mine[i] for i in diff if i >= length))
        same_size = len(mine) == len(ni)
        print('--factory-version %-5s %6d bytes: %s' % (fv, len(mine),
              'IDENTICAL' if same_size and not diff else
              '%d bytes differ, %d of them inside L%s%s' % (
                  len(diff), len(inside),
                  ', all after L (page padding, ours 0x%s, NI 0x00)' % ''.join('%02x' % x for x in pad)
                  if diff and not inside else '', '' if same_size else ', other size')))
        if same_size and not inside:
            matches.append(fv)
            if best is None or fv == a.factory_version:
                best = (fv, cmd, len(diff))
    if best:
        fv, cmd, n = best
        print('match: %s' % ' '.join(os.path.relpath(x, ROOT) if os.path.isabs(x) and x.startswith(ROOT) else x
                                     for x in cmd))
        print('--factory-version %s: header (word 0x04 included) and both segments byte-identical '
              'to NI\'s; the %d differing bytes are the page padding after L, which no CRC covers'
              % (', '.join(matches), n))
    return 0 if best else 1


def build_fw(args):
    r = subprocess.run(['make', '-C', FW, 'BOARD=s49', '-j8'] + args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('make failed:\n' + r.stdout[-3000:] + r.stderr[-3000:])


def image(a):
    dev = a.dev or a.bringup
    build_fw((['DEV=1'] if dev else []) + (['BRINGUP=1'] if a.bringup else []))
    tag = ('-bringup' if a.bringup else '') + ('-dev' if dev else '')
    b = os.path.join(FW, 'build', 's49' + tag)
    t0, t1 = (open(os.path.join(b, n), 'rb').read() for n in ('tile0.bin', 'tile1.bin'))
    name = 'my-kontrol-s49' + tag
    xe_path = os.path.join(b, name + '.xe')
    out = os.path.join(b, name + '-upgrade.bin')
    make_xe(xe_path, [(0, t0), (1, t1)])
    cmd = xflash(a.xtc, xe_path, out, a.version, a.factory_version)
    check(out)
    print(' '.join(cmd))
    print(os.path.relpath(out, ROOT))


def hybrid(a):
    build_fw(['HYBRID=1', 'DEV=1'])
    b = os.path.join(FW, 'build', 's49-hybrid-dev')
    t0 = open(os.path.join(b, 'tile0.bin'), 'rb').read()
    segs, _ = segments(open(a.ni, 'rb').read())
    ni_tile1 = [p for f, p in segs if node_of(f) == 1][0]
    os.makedirs(a.out, exist_ok=True)
    xe_path = os.path.join(a.out, 'hybrid-dev.xe')
    out = os.path.join(a.out, 'hybrid-dev-upgrade.bin')
    make_xe(xe_path, [(0, t0), (1, ni_tile1)])
    cmd = xflash(a.xtc, xe_path, out, a.version, a.factory_version)
    check(out)
    print(' '.join(cmd))
    print(os.path.relpath(out, ROOT))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('what', choices=['proof', 'image', 'hybrid'])
    ap.add_argument('ni', nargs='?', help="NI's upgrade image (proof, hybrid)")
    ap.add_argument('--out', default=os.path.join(ROOT, 'fw', 'kks', 'pack'))
    ap.add_argument('--xtc', default='/Applications/XMOS_XTC_15.3.1')
    ap.add_argument('--dev', action='store_true')
    ap.add_argument('--bringup', action='store_true')
    ap.add_argument('--version', type=int, default=0, help='xflash --upgrade version (NI: 0)')
    ap.add_argument('--factory-version', default='12')
    a = ap.parse_args()
    if a.what in ('proof', 'hybrid') and not a.ni:
        ap.error('%s needs NI\'s image' % a.what)
    sys.exit({'proof': proof, 'image': image, 'hybrid': hybrid}[a.what](a) or 0)


if __name__ == '__main__':
    main()
