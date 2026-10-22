"""Hex-dump memory of a developer build (make DEV=1): mkpeek.py ADDRESS [LENGTH]"""
import sys

import usb.backend.libusb1 as L
import usb.core

be = L.get_backend(find_library=lambda x: '/opt/homebrew/lib/libusb-1.0.dylib')
d = usb.core.find(idVendor=0x17cc, idProduct=0x1860, backend=be)
addr, n = int(sys.argv[1], 0), int(sys.argv[2], 0) if len(sys.argv) > 2 else 64
data = b''
while len(data) < n:
    a = addr + len(data)
    data += bytes(d.ctrl_transfer(0xC0, 0xBD, a & 0xFFFF, a >> 16, min(64, n - len(data))))
for i in range(0, n, 16):
    row = data[i:i + 16]
    print(f'{addr + i:08x}  {row.hex(" "):47}  {"".join(chr(c) if 32 <= c < 127 else "." for c in row)}')
