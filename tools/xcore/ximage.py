#!/usr/bin/env python3
"""XMOS xflash upgrade image (as NI's KK MK1 updater sends it over DFU): check and rebuild.

Our firmware images are packaged with XMOS's xflash; this tool validates images and rebuilds
NI's image locally from its segments (e.g. after ximage --extract). The S49 boot loader
checks header word 0x04 (0, 0x7FFFFFFF and 0xFFFFFFFF are rejected), so an image built here
with the default --id 0 does not boot; pass the word of the image being rebuilt.

usage:
  ximage.py -o OUT.bin TILE.bin [TILE.bin ...]   build an image, one segment per tile binary
       [--flags F,F,...]  segment flags (default 0x80000002 for the first, 2 for the others)
       [--id N]           header word 0x04 (default 0: rejected by the boot loader)
  ximage.py --check IMAGE [--extract DIR]        parse and validate any image (all CRCs);
                                                 --extract writes seg<N>.bin per segment

Format (fw/kks/notes/xmos.md section 3). The stored file is bit-reversed per byte (SPI flash
order). After reversal, little-endian words:
  0x00 magic 0x01A551E5            0x04 per model, checked by the boot loader (not libflash)
  0x08 CRC over 0x10..L            0x0C L = length incl. header, without the padding
  0x10 version (0)                 0x14 L again (the size libflash reads)
  0x18 segment count n             0x1C n x {file offset, flags, size in words incl. CRC}
Each segment is a tile RAM image (loaded at the tile's RAM base) followed by one CRC word;
after the last segment, zero padding to a 256-byte flash page.
CRC step = XS1 `crc32 r, x, 0xEDB88320`: four bit-reflected CRC rounds with zero data, then
xor the data word. Segment: r = step(~0, words - 1), then every payload word; the stored word
is ~step(r, 0), so stepping it in gives 0xFFFFFFFF. Header CRC: the same from r = ~0 over the
words 0x10..L, without a count.
Segment flags: NI's images use 0x80000002 for segment 0 and 2 for segment 1. Segment 1 holds
the USB code (physical tile 0, which boots from SPI) so bit 31 probably marks a tile loaded
over the xlink; not proven.
"""
import argparse, os, struct, sys

MAGIC = 0x01A551E5
POLY = 0xEDB88320
REV8 = bytes(int('{:08b}'.format(i)[::-1], 2) for i in range(256))
PAGE = 256


def step(r, x):
    for _ in range(32):
        r = r >> 1 ^ (POLY if r & 1 else 0)
    return r ^ x


def crc_words(words, r=0xFFFFFFFF):
    for w in words:
        r = step(r, w)
    return r


def seg_crc(payload):
    """CRC word appended to a segment payload (bytes, a multiple of 4)."""
    words = struct.unpack('<%dI' % (len(payload) // 4), payload)
    return ~step(crc_words(words, step(0xFFFFFFFF, len(words))), 0) & 0xFFFFFFFF


def build(tiles, flags=None, ident=0):
    """Image bytes (bit-reversed, as stored) from tile RAM images."""
    n = len(tiles)
    flags = flags or [0x80000002] + [2] * (n - 1)
    off = 0x1C + 12 * n
    table, body = [], b''
    for t, f in zip(tiles, flags):
        t = t + bytes(-len(t) % 4)
        seg = t + struct.pack('<I', seg_crc(t))
        table.append((off + len(body), f, len(seg) // 4))
        body += seg
    length = off + len(body)
    tail = struct.pack('<3I', 0, length, n) + b''.join(struct.pack('<3I', *e) for e in table) + body
    words = struct.unpack('<%dI' % (len(tail) // 4), tail)
    crc = ~step(crc_words(words), 0) & 0xFFFFFFFF
    img = struct.pack('<4I', MAGIC, ident, crc, length) + tail
    img += bytes(-len(img) % PAGE)
    return img.translate(REV8)


def check(raw, extract=None):
    """Validate an image; prints a report, returns True when everything checks out."""
    d = raw.translate(REV8)
    ok = True

    def bad(msg):
        nonlocal ok
        ok = False
        print('  ERROR: ' + msg)
    if len(d) < 0x1C:
        print('too short')
        return False
    magic, ident, crc, length, version, length2, n = struct.unpack_from('<7I', d)
    print('image: %d bytes, magic 0x%08x, word4 0x%08x, length %d, version %d, %d segments'
          % (len(d), magic, ident, length, version, n))
    if magic != MAGIC:
        bad('magic is not 0x%08x' % MAGIC)
    if length != length2:
        bad('length words differ (0x0c: %d, 0x14: %d)' % (length, length2))
    if length > len(d) or length % 4 or not 0 < n < 16:
        bad('length or segment count out of range')
        return False
    words = struct.unpack_from('<%dI' % ((length - 0x10) // 4), d, 0x10)
    c = ~step(crc_words(words), 0) & 0xFFFFFFFF
    print('  header CRC 0x%08x: %s' % (crc, 'ok' if c == crc else 'BAD (computed 0x%08x)' % c))
    if c != crc:
        ok = False
    end = 0x1C + 12 * n
    for k in range(n):
        off, flags, size = struct.unpack_from('<3I', d, 0x1C + 12 * k)
        seg = d[off:off + 4 * size]
        if off != end or len(seg) != 4 * size or size < 1:
            bad('segment %d at 0x%x (%d words) is not contiguous or runs past the end' % (k, off, size))
            continue
        stored = struct.unpack_from('<I', seg, len(seg) - 4)[0]
        good = seg_crc(seg[:-4]) == stored
        print('  segment %d: offset 0x%05x, flags 0x%08x, %d bytes + CRC 0x%08x: %s'
              % (k, off, flags, len(seg) - 4, stored, 'ok' if good else 'BAD'))
        ok &= good
        end = off + 4 * size
        if extract:
            os.makedirs(extract, exist_ok=True)
            open(os.path.join(extract, 'seg%d.bin' % k), 'wb').write(seg[:-4])
    if end != length:
        bad('segments end at 0x%x, length says 0x%x' % (end, length))
    pad = d[length:]
    if any(pad):
        bad('padding after the image is not zero')
    if len(d) % PAGE:
        print('  note: file is not a multiple of %d bytes' % PAGE)
    print('valid' if ok else 'INVALID')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('tiles', nargs='*')
    ap.add_argument('-o', '--output')
    ap.add_argument('--flags', help='comma separated segment flags')
    ap.add_argument('--id', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--check')
    ap.add_argument('--extract')
    a = ap.parse_args()
    if a.check:
        sys.exit(0 if check(open(a.check, 'rb').read(), a.extract) else 1)
    if not a.tiles or not a.output:
        ap.error('need -o OUT and at least one tile binary')
    flags = [int(f, 0) for f in a.flags.split(',')] if a.flags else None
    if flags and len(flags) != len(a.tiles):
        ap.error('one flag per tile')
    img = build([open(t, 'rb').read() for t in a.tiles], flags, a.id)
    open(a.output, 'wb').write(img)
    print('%s: %d bytes, %d segments' % (a.output, len(img), len(a.tiles)))


if __name__ == '__main__':
    main()
