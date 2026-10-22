"""Read the custom firmware's 0xB5 diagnostics."""
import usb.core, usb.backend.libusb1 as L
be = L.get_backend(find_library=lambda x: '/opt/homebrew/lib/libusb-1.0.dylib')
d = usb.core.find(idVendor=0x17cc, backend=be)
print('pid', hex(d.idProduct))
if d.idProduct == 0x1862:
    raise SystemExit('in bootloader DFU mode; flash the firmware first')
r = bytes(d.ctrl_transfer(0xC0, 0xB5, 0, 0, 49))
csr = int.from_bytes(r[2:6], 'little')
flags = [k for k, b in [('PIN', 26), ('POR', 27), ('SFT', 28), ('IWDG', 29), ('WWDG', 30), ('LPWR', 31)] if csr >> b & 1]
print(f'armed={r[0]} configured={r[1]} reset_flags={flags}')
print(f'previous boot: last stage={int.from_bytes(r[6:8], "little"):#x} boot counter={int.from_bytes(r[8:10], "little")}')
if len(r) >= 42:
    print('flag page at reset (0x0801FC00):', r[10:26].hex(' '), '|', r[26:42].hex(' '))
if len(r) >= 43:
    print('peripherals: LED driver', 'ok' if r[42] & 1 else 'NO ANSWER', '| knob touch', 'ok' if r[42] & 2 else 'NO ANSWER',
          '| pitch strip', 'ok' if r[42] & 4 else 'NO ANSWER', '| mod strip', 'ok' if r[42] & 8 else 'NO ANSWER')
if len(r) >= 49:
    print('strip probe bytes: pitch', r[43:46].hex(' '), '| mod', r[46:49].hex(' '))
