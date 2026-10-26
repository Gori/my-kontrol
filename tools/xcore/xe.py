#!/usr/bin/env python3
"""XMOS executable (XE) files: read them, and make one from tile RAM images, so XMOS's own
xflash can package our firmware (our toolchain links raw tile images, not ELF).

usage:
  xe.py --info FILE.xe                              list the sectors (and check their CRCs)
  xe.py -o OUT.xe --xn NET.xn NODE:IMAGE.bin ...    one binary sector + goto per image

Format (XMOS XTC Tools guide, "XMOS executable (XE) file format"): the header "XMOS", 2, 0,
then sectors: u16 type, u16 0, u64 size of the contents block; the block is u8 padding, 3
reserved bytes, the data, the padding, and a CRC. The CRC is zlib's CRC-32 over the sector
header and block up to the CRC, with the register started as if four zero bytes had gone in
first: the guide says only "initialised with 0xFFFFFFFF", and this is what matches every
sector XTC 15.3.1 writes (`--info` checks them).

Sector types used: 1 binary (u16 node, u16 tile, u64 load address, the image), 5 goto
(node, tile, u64 address), 8 XN, 0x5555 last. A node is its index in the XN's JTAG chain.
What xflash needs for an S MK1 upgrade image, found by trying (tools/xcore/s49pack.py):
- binary sectors, each followed by its goto (with ELF sectors xflash's tools-12 format
  refuses with "overlays are not supported"; with all gotos last it takes only one image);
- an XN naming XTC's own XS1 parts, which XTC 15 keeps in configs/.deprecated (package
  XS1-L2A-QF124, nodes XS1-L1A): firmware/boards/s-mk1/s-mk1.xn;
- the segments come out in JTAG chain order, each tagged with its node's routing id.
"""
import argparse, struct, zlib

T_BINARY, T_ELF, T_SYSCONFIG, T_NODEDESC, T_GOTO, T_CALL, T_XN, T_LAST = 1, 2, 3, 4, 5, 6, 8, 0x5555
NAMES = {1: 'BINARY', 2: 'ELF', 3: 'SYSCONFIG', 4: 'NODEDESC', 5: 'GOTO', 6: 'CALL', 8: 'XN',
         9: 'PROGINFO', 0xC: 'XSCOPE', 0x5555: 'LASTSEC', 0xFFFF: 'SKIP'}


def sector_crc(blob):
    return zlib.crc32(b'\0\0\0\0' + blob) & 0xFFFFFFFF


def sector(stype, data=None):
    if data is None:
        return struct.pack('<HHQ', stype, 0, 0)
    pad = -len(data) % 4
    size = 4 + len(data) + pad + 4
    blob = struct.pack('<HHQ', stype, 0, size) + struct.pack('<B3x', pad) + data + bytes(pad)
    return blob + struct.pack('<I', sector_crc(blob))


def read(path):
    """[(type, data)]; raises on a bad header or CRC."""
    d = open(path, 'rb').read()
    if d[:4] != b'XMOS':
        raise ValueError('%s: not an XE file' % path)
    out, o = [], 8
    while o + 12 <= len(d):
        stype, _, size = struct.unpack_from('<HHQ', d, o)
        if size == 0:
            out.append((stype, None))
            o += 12
            if stype == T_LAST:
                break
            continue
        blob = d[o:o + 12 + size - 4]
        if sector_crc(blob) != struct.unpack_from('<I', d, o + 12 + size - 4)[0]:
            raise ValueError('%s: CRC error in the sector at 0x%x' % (path, o))
        pad = d[o + 12]
        out.append((stype, d[o + 16:o + 12 + size - 4 - pad]))
        o += 12 + size
    return out


def build(images, xn, base=0x10000):
    """images: [(node, ram image bytes)], tile 0 of each node, loaded at base and started."""
    out = b'XMOS\x02\x00\x00\x00'
    for node, img in images:
        out += sector(T_BINARY, struct.pack('<HHQ', node, 0, base) + img)
        out += sector(T_GOTO, struct.pack('<HHQ', node, 0, base))
    return out + sector(T_XN, xn) + sector(T_LAST)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--info')
    ap.add_argument('-o')
    ap.add_argument('--xn')
    ap.add_argument('images', nargs='*', help='NODE:IMAGE.bin, in load order')
    a = ap.parse_args()
    if a.info:
        for stype, data in read(a.info):
            extra = ''
            if stype in (T_ELF, T_BINARY, T_GOTO, T_CALL) and data:
                node, tile, addr = struct.unpack_from('<HHQ', data)
                extra = 'node %d tile %d address 0x%x, %d bytes' % (node, tile, addr, len(data) - 12)
            elif stype == T_NODEDESC and data:
                extra = 'node %d JTAG id 0x%x' % struct.unpack_from('<H2xI', data)
            elif data:
                extra = '%d bytes' % len(data)
            print('%-10s %s' % (NAMES.get(stype, hex(stype)), extra))
        print('CRCs ok')
        return
    if not (a.o and a.xn and a.images):
        ap.error('-o, --xn and images are needed')
    imgs = []
    for spec in a.images:
        node, path = spec.split(':', 1)
        imgs.append((int(node, 0), open(path, 'rb').read()))
    open(a.o, 'wb').write(build(imgs, open(a.xn, 'rb').read()))


if __name__ == '__main__':
    main()
