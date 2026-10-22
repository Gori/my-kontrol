"""Drive the custom firmware's test hooks: UI events and key presses."""
import sys, time
import usb.core, usb.backend.libusb1 as L

be = L.get_backend(find_library=lambda x: '/opt/homebrew/lib/libusb-1.0.dylib')
EV = dict(turn=0, push=1, left=2, right=3, up=4, down=5, back=6, menu=9)


def dev():
    d = usb.core.find(idVendor=0x17cc, idProduct=0x1860, backend=be)
    if not d:
        raise SystemExit('custom firmware not found')
    return d


def ui(d, name, value=0):
    d.ctrl_transfer(0x40, 0xB8, EV[name], value & 0xFFFF, None)


def key(d, note, vel=100, on=1):
    d.ctrl_transfer(0x40, 0xB9, note | (on << 8), vel, None)


if __name__ == '__main__':
    d = dev()
    for tok in sys.argv[1:]:          # e.g. menu turn:1 push key:60 sleep:0.5 keyoff:60
        name, _, arg = tok.partition(':')
        if name == 'key':
            key(d, int(arg))
        elif name == 'keyoff':
            key(d, int(arg), 0, 0)
        elif name == 'sleep':
            time.sleep(float(arg))
        else:
            ui(d, name, int(arg or 0))
        time.sleep(0.15)
