"""Inject input into a developer build (make DEV=1) over USB.

usage: mktest.py TOKEN...
  key:N[:VEL]   press key N (0 = lowest)     keyoff:N   release it
  btn:NAME      press and release a button   down:NAME / up:NAME
  turn:D        turn the 4D encoder D detents
  knob:K:D      turn knob K by D steps       touch:K / untouch:K
  strip:S:V     strip 0 pitch / 1 mod to V (0..16383, -1 = release)
  sleep:SEC
Buttons: shift scale arp undo quantize ideas loop metro tempo play rec stop
  preset_up preset_down mute solo browser plugin track oct_down oct_up
  up left right down push
"""
import sys
import time

import usb.backend.libusb1 as L
import usb.core

be = L.get_backend(find_library=lambda x: '/opt/homebrew/lib/libusb-1.0.dylib')
EV_KEY, EV_BUTTON, EV_ENCODER, EV_KNOB, EV_KNOB_TOUCH, EV_STRIP, EV_PEDAL = range(7)
BUTTONS = ['shift', 'scale', 'arp', 'undo', 'quantize', 'ideas', 'loop', 'metro', 'tempo', 'play', 'rec',
           'stop', 'preset_up', 'preset_down', 'mute', 'solo', 'browser', 'plugin', 'track', 'oct_down',
           'oct_up', 'up', 'left', 'right', 'down', 'push']


def dev():
    d = usb.core.find(idVendor=0x17cc, idProduct=0x1860, backend=be)
    if not d:
        raise SystemExit('keyboard not found in app mode')
    return d


def event(d, typ, ident, value):
    d.ctrl_transfer(0x40, 0xB8, typ | (ident << 8), value & 0xFFFF, None)


def main(tokens):
    d = dev()
    for tok in tokens:
        name, *args = tok.split(':')
        if name == 'key':
            event(d, EV_KEY, int(args[0]), int(args[1]) if len(args) > 1 else 100)
        elif name == 'keyoff':
            event(d, EV_KEY, int(args[0]), 0)
        elif name in ('btn', 'down', 'up'):
            b = BUTTONS.index(args[0])
            if name != 'up':
                event(d, EV_BUTTON, b, 1)
            if name == 'btn':
                time.sleep(0.05)
            if name != 'down':
                event(d, EV_BUTTON, b, 0)
        elif name == 'turn':
            event(d, EV_ENCODER, 0, int(args[0]))
        elif name == 'knob':
            event(d, EV_KNOB, int(args[0]), int(args[1]))
        elif name in ('touch', 'untouch'):
            event(d, EV_KNOB_TOUCH, int(args[0]), name == 'touch')
        elif name == 'strip':
            event(d, EV_STRIP, int(args[0]), int(args[1]))
        elif name == 'sleep':
            time.sleep(float(args[0]))
            continue
        else:
            raise SystemExit(f'unknown token {tok}\n\n{__doc__}')
        time.sleep(0.12)


if __name__ == '__main__':
    main(sys.argv[1:])
