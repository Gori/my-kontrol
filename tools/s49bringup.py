#!/usr/bin/env python3
"""Read the S MK1 USB bring-up log (make BOARD=s49 DEV=1 BRINGUP=1; platform/xs1/bringup.h).

A bring-up image that no host configures within 20 s writes a record to the SPI flash log
(0xF5000, 16 records) and goes back to NI's factory image. Two ways to read the records:

  s49bringup.py usb               all 16 from a bring-up image whose USB works (vendor request
                                  0xB9 IN, wValue = record), via libusb
  s49bringup.py midi [--port P]   from the 5-pin MIDI OUT through a MIDI interface: every
                                  bring-up start sends the newest record, and so does a
                                  revert just before it reboots (SysEx F0 7D 4D 4B 40, two
                                  nibbles per byte). Ctrl-C to stop.
  s49bringup.py hex HEX...        decode a record given as hex (e.g. copied from a log)

NI's factory image cannot read 0xF5000, and neither can the hybrid image's tile 0 (the
flash hangs off NI's tile 1, whose tile link offers no general flash read).
"""
import struct, sys

FIELDS = ('wire_events rx_packets bad_pid setup_tokens in_tokens out_tokens other_tokens '
          'evt_reset evt_setup evt_complete dcd_xfers dcd_stalls').split()
STAGES = {0: 'SIE not started', 1: 'ports set up', 2: 'PHY reset written', 3: 'PHY IDs read',
          4: 'PHY at full speed', 5: 'in the SIE loop'}


def decode(r):
    if len(r) < 72 or r[:4] != b"MKBU":
        return None
    version, seq, up = struct.unpack_from('<HHI', r, 4)
    stage, i0, i1, i2, i3, fc, rx0, flags, addr = struct.unpack_from('<9B', r, 12)
    counts = struct.unpack_from('<12I', r, 24)
    seen = [n for b, n in ((1, 'bus reset'), (2, 'SETUP'), (4, 'address set'), (8, 'configured')) if flags & b]
    lines = ['record %d (format %d), %.1f s after start' % (seq, version, up / 1000),
             '  SIE: %s; ULPI vendor %02x%02x product %02x%02x; FUNC_CTRL %02x; last from PHY %02x'
             % (STAGES.get(stage, stage), i1, i0, i3, i2, fc, rx0),
             '  USB: %s; address %d' % (', '.join(seen) or 'nothing seen', addr),
             '  ' + ', '.join('%s %d' % kv for kv in zip(FIELDS, counts))]
    return '\n'.join(lines)


def from_usb():
    import usb.backend.libusb1 as L
    import usb.core
    be = L.get_backend(find_library=lambda x: '/opt/homebrew/lib/libusb-1.0.dylib')
    d = usb.core.find(idVendor=0x17CC, idProduct=0x1350, backend=be)
    if not d:
        sys.exit('no S49 with our firmware on USB')
    for k in range(16):
        txt = decode(bytes(d.ctrl_transfer(0xC0, 0xB9, k, 0, 256)))
        if txt:
            print(txt)


def from_midi(port):
    import mido
    names = [n for n in mido.get_input_names() if port.lower() in n.lower()]
    if not names:
        sys.exit('no MIDI input with "%s" in its name: %s' % (port, mido.get_input_names()))
    print('listening on %s' % names[0])
    for m in mido.open_input(names[0]):
        if m.type == 'sysex' and m.data[:4] == (0x7D, 0x4D, 0x4B, 0x40):
            nib = m.data[4:]
            r = bytes(nib[i] << 4 | nib[i + 1] for i in range(0, len(nib) - 1, 2))
            print(decode(r) or 'not a record: ' + r.hex(' '))


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('usb', 'midi', 'hex'):
        sys.exit(__doc__)
    if sys.argv[1] == 'usb':
        from_usb()
    elif sys.argv[1] == 'midi':
        from_midi(sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == '--port' else '')
    else:
        print(decode(bytes.fromhex(''.join(sys.argv[2:]))) or 'not a record')


if __name__ == '__main__':
    main()
