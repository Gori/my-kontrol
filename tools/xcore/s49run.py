#!/usr/bin/env python3
"""Bring-up harness for My Kontrol on the Komplete Kontrol S49 MK1: runs our tile images on the
s49emu board model, shows the 9 displays and the lights, injects input, captures MIDI.

usage:
  s49run.py TILE0.bin [TILE1.bin] [--map0 MAP] [--map1 MAP] [--stdio] [--watch] [--ms N] [-e CMD]
    TILE0.bin   tile 0 (core + panel) RAM image from xld.py
    TILE1.bin   tile 1 (USB, 5-pin MIDI, flash); without it tile 1 is the HostLink stub
                (tools/xcore/s49emu.py documents the link contract our tiles must follow)
    --map0/1    xld map files: symbol names in error reports
    --stdio     read commands from stdin (below), answer on stdout; emulated time only moves on
                `tick`, so a session is deterministic
    --watch     run --ms milliseconds, redrawing the displays and lights whenever they change
    -e CMD      run a command first (repeatable; with --watch: `at T CMD`, T in ms)

Commands (--stdio, one per line; blank lines and # comments are skipped):
  tick MS               run MS ms; then prints `din-out <hex>` and `out <cable> <hex>` lines
                        for MIDI sent meanwhile, `stopped <reason>` if the emulation stopped
  key NOTE [RAW]        keybed note on; RAW = the scanner's 10-bit velocity value (default 100;
                        NI's firmware maps small values to loud notes)
  keyoff NOTE           keybed note off
  knob N DELTA          sensor 0x12 value N (0..8) += DELTA (16-bit, wraps)
  knob-set N VALUE      sensor 0x12 value N = VALUE
  btn BIT [MS]          input-register bit 0..31 pressed for MS ms (default 100)
  btn-down BIT / btn-up BIT
  turn N                the Control encoder N detents (+ = clockwise), on input bits 8/9
  pedal VALUE           sensor 0x15 ADC value (expression pedal, guess)
  sw N 0|1              sensor 0x15 switch input N (0, 1: sustain tip/ring, guess)
  touch NODE VALUE      sensor 0x15 touch delta NODE 0..47 (knobs, encoder, strips)
  din-in HEX            bytes into the 5-pin MIDI IN
  in CABLE HEX          a MIDI message from the USB host (HostLink stub only)
  usb 0|1               USB host connected (HostLink stub only)
  event TYPE ID VALUE   an event for the core, as core/hal.h's mk_event_t (0 key, 1 button,
                        2 encoder, 3 knob, 4 knob touch, 5 strip, 6 pedal); DEV=1 builds only
                        (the link's DEV_EVENT, like tools/mktest.py over USB)
  screen                `screen`, 9 lines `<display> <bar> |<line 1>|<line 2>|`, `end`
  leds                  `leds`, then `guide <key> <r> <g> <b>` and `led <bit> <level>` for every
                        lit light and `backlight <level>` (1O; levels 0..127, averaged over
                        the last tick), `end`
  threads               thread states of both tiles
  quit
Answers `error ...` for an unknown command.

Display lines: a '.' after a character is that character's dot. The meter is shown as
[#####____]: the box, 9 blocks ('#') and the thin line under unlit ones ('_').
Light guide: key k is chain A (keys 0..23) or B (24..48), 3 bits per key; the bit order inside
each 24-bit group is the one NI's firmware uses (xmos.md §8.3).
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import s49emu, xsim

# light guide: chain bit of R, G, B for local key j of a 24-bit group (xmos.md §8.3)
GROUP = [6, 7, 5, 3, 4, 2, 0, 1, 15, 13, 14, 12, 10, 11, 9, 23, 8, 22, 20, 21, 19, 17, 18, 16]
RAMP = ' .:-=+*#%@'
CIN_LEN = {0x8: 3, 0x9: 3, 0xA: 3, 0xB: 3, 0xC: 2, 0xD: 2, 0xE: 3, 0xF: 1, 0x2: 2, 0x3: 3,
           0x4: 3, 0x5: 1, 0x6: 2, 0x7: 3}


FONT16 = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'firmware', 'drivers', 'font16.c')


def load_font16(path=FONT16):
    """{segment bits: character} from our firmware's font table (drivers/font16.c), so the
    displays read back as the characters our firmware meant; None if the file is missing."""
    if not os.path.exists(path):
        return None
    import re
    names = {'SEG_A1': 1, 'SEG_A2': 2, 'SEG_B': 4, 'SEG_C': 8, 'SEG_D2': 16, 'SEG_D1': 32,
             'SEG_E': 64, 'SEG_F': 128, 'SEG_H': 256, 'SEG_I': 512, 'SEG_J': 1024,
             'SEG_G1': 2048, 'SEG_G2': 4096, 'SEG_K': 8192, 'SEG_L': 16384, 'SEG_M': 32768}
    names.update(A=3, D=48, G=6144)
    glyph = {}
    for m in re.finditer(r"\['(\\?.)' - 32\] = ([^,]+),", open(path).read()):
        ch = m.group(1)[-1]
        bits = eval(m.group(2), {}, dict(names))
        glyph.setdefault(bits, ch)
    glyph.setdefault(0, ' ')
    return glyph


def guide_bits(key):
    """(chain index, [R, G, B chain bits]) of light-guide key 0..48."""
    if key == 48:
        return 1, [78, 79, 77]
    chain, local = (0, key) if key < 24 else (1, key - 24)
    g, j = divmod(local, 8)
    return chain, [24 * g + GROUP[3 * j + c] for c in range(3)]


class Harness:
    def __init__(self, tile0, tile1=None, map0=None, map1=None):
        syms0 = xsim.read_map(map0) if map0 else None
        syms1 = xsim.read_map(map1) if map1 else None
        self.b = s49emu.Board(tile0, tile1, ui_syms=syms0, usb_syms=syms1)
        font = load_font16()
        if font:                        # our glyphs first, NI's for the rest
            self.b.lcd.glyph = {**s49emu.GLYPH, **font}
        self.din_seen = self.usb_seen = 0
        self.stopped = None
        self.window()

    # -- time
    def window(self):
        for ch in self.b.chains + [self.b.backlight]:
            ch.reset_window(self.b.sys.now)

    def tick(self, ms):
        self.window()
        if self.stopped:
            return
        why = self.b.run_ms(self.b.ms() + ms)
        if why != 'time limit':
            self.stopped = why

    def at(self, ms, fn):
        self.b.sys.at(self.b.sys.now + ms * 1e5 * self.b.sys.cpt, fn)

    # -- input
    def key(self, note, raw=100, on=True):
        self.b.key(note, raw, on)

    def button(self, bit, down):
        inp = self.b.inputs
        inp.pressed = inp.pressed | 1 << bit if down else inp.pressed & ~(1 << bit)

    def dev_event(self, typ, id_, value):
        """DEV_EVENT to tile 0: from the stub, or with our tile 1 image put straight into tile
        0's link chanend when no route into it is open (so it can't split a packet)."""
        if self.b.link:
            self.b.link.dev_event(typ, id_, value)
            return
        payload = struct.pack('<BBh', typ, id_, value)
        toks = [(s49emu.HostLink.DEV_EVENT, False), (len(payload), False)] + \
               [(x, False) for x in payload] + [(xsim.CT_END, True)]

        def inject():
            d = self.b.sys.chanend(s49emu.HostLink.LINK_TILE0)
            if d is None or d.source is not None or d.buf:
                self.at(0.05, inject)
                return
            d.buf.extend(toks)
            d.notify(self.b.sys.now)
        inject()

    def turn(self, detents, bits=(8, 9), ms=2.0):
        """Turn the Control encoder: its quadrature on input bits 8/9, one state every ms
        (4 states a detent, as our board decodes it)."""
        seq = [(0, 0), (1, 0), (1, 1), (0, 1)]
        step = 1 if detents > 0 else -1
        st = getattr(self, 'enc_state', 0)
        for i in range(abs(detents) * 4):
            st = (st + step) % 4
            a, b = seq[st]

            def apply(a=a, b=b):
                inp = self.b.inputs
                inp.pressed = inp.pressed & ~(3 << bits[0]) | a << bits[0] | b << bits[1]
            self.at(ms * (i + 1), apply)
        self.enc_state = st

    def din_in(self, data):
        if self.b.link:
            self.b.link.din_rx(data)
        else:
            self.b.din_in.send(data, self.b.sys.now)

    # -- output
    def midi_out(self):
        """New MIDI since the last call: ['din-out ..', 'out <cable> ..']"""
        b, lines = self.b, []
        if b.link:
            din = [x for _, x in b.link.din_tx[self.din_seen:]]
            self.din_seen = len(b.link.din_tx)
            for _, p in b.link.usb_tx[self.usb_seen:]:
                n = CIN_LEN.get(p[0] & 15, 3)
                lines.append('out %d %s' % (p[0] >> 4, p[1:1 + n].hex(' ').upper()))
            self.usb_seen = len(b.link.usb_tx)
        else:
            b.din_out.flush(b.sys.now)
            din = [x for _, x, _ in b.din_out.bytes[self.din_seen:]]
            self.din_seen = len(b.din_out.bytes)
        if din:
            lines.insert(0, 'din-out ' + ' '.join('%02X' % x for x in din))
        return lines

    def displays(self):
        """[(bar, line 1, line 2)] for displays 1..9."""
        out = []
        for d in range(9):
            l1, l2, meter = self.b.lcd.text(d)
            out.append((meter, l1, l2))
        return out

    def guide(self):
        """[(r, g, b)] brightness 0..127 of the 49 light-guide keys."""
        br = [ch.brightness(self.b.sys.now) for ch in self.b.chains]
        out = []
        for k in range(49):
            c, bits = guide_bits(k)
            out.append(tuple(br[c][x] if x < len(br[c]) else 0 for x in bits))
        return out, br[2]

    # -- views
    def screen_lines(self):
        return ['%d %s |%s|%s|' % (d + 1, bar, l1, l2)
                for d, (bar, l1, l2) in enumerate(self.displays())]

    def led_lines(self):
        guide, chain_c = self.guide()
        lines = ['guide %d %d %d %d' % ((k,) + rgb) for k, rgb in enumerate(guide) if any(rgb)]
        lines += ['led %d %d' % (i, v) for i, v in enumerate(chain_c) if v]
        lines.append('backlight %d' % self.b.backlight.brightness(self.b.sys.now))
        return lines

    def grid(self):
        """Human view: the 9 displays side by side, the light guide and the button lights."""
        disp = self.displays()
        w = max(13, max(len(x) for d in disp for x in d[1:]) + 2)
        rows = ['   ' + ''.join(('%d' % (i + 1)).ljust(w) for i in range(9)),
                '   ' + ''.join(d[0].ljust(w) for d in disp),
                '   ' + ''.join(d[1].ljust(w) for d in disp),
                '   ' + ''.join(d[2].ljust(w) for d in disp)]
        guide, chain_c = self.guide()
        keys = ''
        for r, g, b in guide:
            m = max(r, g, b)
            if not m:
                keys += '.'
                continue
            name = {(1, 0, 0): 'r', (0, 1, 0): 'g', (0, 0, 1): 'b', (1, 1, 0): 'y',
                    (1, 0, 1): 'm', (0, 1, 1): 'c', (1, 1, 1): 'w'}[(r * 2 >= m, g * 2 >= m, b * 2 >= m)]
            keys += name.upper() if m >= 64 else name
        rows.append('   light guide (keys 1..49, R G B Y M C W, capital = bright): ' + keys)
        rows.append('   button/strip lights (chain bits 0..111): |%s|  backlight %d'
                    % (''.join(RAMP[min(9, v * 10 // 128)] for v in chain_c),
                       self.b.backlight.brightness(self.b.sys.now)))
        rows.append('   t = %.1f ms%s' % (self.b.ms(), ('  STOPPED: ' + self.stopped) if self.stopped else ''))
        return rows

    def threads(self):
        out = []
        for tl in (self.b.ui, self.b.usb):
            if tl is None:
                out.append('tile 1: HostLink stub, link %s' % ('up' if self.b.link.up else 'down'))
                continue
            st = ['%d:%s@%s' % (t.num, t.state, tl.name(t.pc)) for t in tl.threads if t.state != 'free']
            out.append('tile %d: %s' % (0 if tl is self.b.ui else 1, ' '.join(st)))
        return out

    # -- commands
    def command(self, line):
        """Run one command line; returns the answer lines."""
        a = line.split('#')[0].split()
        if not a:
            return []
        cmd, args = a[0], a[1:]
        try:
            if cmd == 'tick':
                self.tick(float(args[0]))
                out = self.midi_out()
                return out + (['stopped ' + self.stopped] if self.stopped else [])
            if cmd == 'key':
                self.key(int(args[0]), int(args[1]) if len(args) > 1 else 100)
            elif cmd == 'keyoff':
                self.key(int(args[0]), 0, False)
            elif cmd == 'knob':
                n = int(args[0])
                self.b.knobs[n] = (self.b.knobs[n] + int(args[1])) & 0xffff
            elif cmd == 'knob-set':
                self.b.knobs[int(args[0])] = int(args[1]) & 0xffff
            elif cmd == 'btn':
                bit, ms = int(args[0]), float(args[1]) if len(args) > 1 else 100
                self.button(bit, True)
                self.at(ms, lambda: self.button(bit, False))
            elif cmd == 'turn':
                self.turn(int(args[0]))
            elif cmd in ('btn-down', 'btn-up'):
                self.button(int(args[0]), cmd == 'btn-down')
            elif cmd == 'pedal':
                self.b.pedal = int(args[0]) & 0xffff
            elif cmd == 'sw':
                n, v = int(args[0]), int(args[1])
                self.b.switches = self.b.switches & ~(1 << n) | (v & 1) << n
            elif cmd == 'touch':
                self.b.touch[int(args[0])] = int(args[1])
            elif cmd == 'din-in':
                self.din_in(bytes.fromhex(''.join(args)))
            elif cmd == 'in':
                if not self.b.link:
                    return ['error: `in` needs the HostLink stub (no tile 1 image)']
                self.b.link.usb_rx(int(args[0]), bytes.fromhex(''.join(args[1:])))
            elif cmd == 'usb':
                if not self.b.link:
                    return ['error: `usb` needs the HostLink stub (no tile 1 image)']
                self.b.link.usb = int(args[0]) & 1
                self.b.link.send(self.b.link.USB_STATE, bytes([self.b.link.usb]))
            elif cmd == 'event':
                self.dev_event(int(args[0]), int(args[1]), int(args[2]))
            elif cmd == 'screen':
                return ['screen'] + self.screen_lines() + ['end']
            elif cmd == 'leds':
                return ['leds'] + self.led_lines() + ['end']
            elif cmd == 'threads':
                return self.threads()
            elif cmd == 'quit':
                raise SystemExit(0)
            else:
                return ['error: unknown command %r' % cmd]
        except (IndexError, ValueError) as e:
            return ['error: %s: %s' % (line.strip(), e)]
        return []


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('tiles', nargs='+')
    ap.add_argument('--map0')
    ap.add_argument('--map1')
    ap.add_argument('--stdio', action='store_true')
    ap.add_argument('--watch', action='store_true')
    ap.add_argument('--ms', type=float, default=100)
    ap.add_argument('-e', action='append', default=[])
    a = ap.parse_args()
    if len(a.tiles) > 2:
        ap.error('at most two tile images')
    imgs = [open(p, 'rb').read() for p in a.tiles]
    h = Harness(imgs[0], imgs[1] if len(imgs) > 1 else None, a.map0, a.map1)
    if a.stdio:
        for line in a.e + list(sys.stdin):
            for x in h.command(line):
                print(x)
            sys.stdout.flush()
        return
    timed = []
    for c in a.e:
        p = c.split()
        if p and p[0] == 'at':
            timed.append((float(p[1]), ' '.join(p[2:])))
        else:
            h.command(c)
    timed.sort()
    last = None
    t = 0.0
    while t < a.ms:
        while timed and timed[0][0] <= t:
            for x in h.command(timed.pop(0)[1]):
                print(x)
        step = min(10.0, a.ms - t)
        h.tick(step)
        t += step
        for x in h.midi_out():
            print('%8.1f ms  %s' % (h.b.ms(), x))
        view = h.grid()
        if a.watch and view[:-1] != last:
            sys.stdout.write('\x1b[H\x1b[2J' + '\n'.join(view) + '\n')
            last = view[:-1]
        if h.stopped:
            break
    if not a.watch:
        print('\n'.join(h.grid()))
    print('\n'.join(h.threads()))


if __name__ == '__main__':
    main()
