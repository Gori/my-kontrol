"""Read My Kontrol's diagnostics (vendor request 0xB5)."""
import usb.backend.libusb1 as L
import usb.core

be = L.get_backend(find_library=lambda x: '/opt/homebrew/lib/libusb-1.0.dylib')
d = usb.core.find(idVendor=0x17cc, backend=be)
if not d:
    raise SystemExit('no keyboard found')
print(f'USB {d.idVendor:04x}:{d.idProduct:04x}  {usb.util.get_string(d, d.iManufacturer)} / '
      f'{usb.util.get_string(d, d.iProduct)}  serial {usb.util.get_string(d, d.iSerialNumber)}')
if d.idProduct == 0x1862:
    raise SystemExit('in bootloader (update) mode')
r = bytes(d.ctrl_transfer(0xC0, 0xB5, 0, 0, 64))
if r[0] != 2:
    raise SystemExit(f'unknown diag layout {r[0]}: {r.hex(" ")}')
csr = int.from_bytes(r[2:6], 'little')
flags = [k for k, b in [('PIN', 26), ('POR', 27), ('SFT', 28), ('IWDG', 29), ('WWDG', 30), ('LPWR', 31)] if csr >> b & 1]
links = ['none', 'Mackie', 'host script']
print(f'reset flags {flags}  previous stage {int.from_bytes(r[6:8], "little"):#x}  boots {int.from_bytes(r[8:10], "little")}'
      f'  crash count {r[10]}  boot flag armed {r[1]}')
print(f'MIDI out dropped {int.from_bytes(r[11:13], "little")}  DAW link {links[r[13]] if r[13] < 3 else r[13]}')
b = r[14:]
if b:
    hw = b[0]
    print('sensors:', ', '.join(f'{n} {"ok" if hw & m else "NO ANSWER"}' for n, m in
                                [('lights', 1), ('knob touch', 2), ('pitch strip', 4), ('mod strip', 8)]))
    knobs = [(int.from_bytes(b[7 + 4 * i:9 + 4 * i], 'little'), int.from_bytes(b[9 + 4 * i:11 + 4 * i], 'little')) for i in range(8)]
    print('knob wipers:', ' '.join(f'{a}/{c}' for a, c in knobs))
    p = b[39:45]
    print('pedal raw:', [int.from_bytes(p[i:i + 2], 'little') for i in (0, 2, 4)],
          ' keybed dropped', int.from_bytes(b[45:47], 'little'))
