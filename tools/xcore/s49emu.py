#!/usr/bin/env python3
"""Komplete Kontrol S49 MK1 board emulator: both XMOS tiles in xsim plus the devices on their
ports, per docs/hardware/s-mk1.md and fw/kks/notes/xmos.md (§8, §11). No hardware access.

usage:
  s49emu.py [IMAGE] [--ms N] [--script FILE|-e CMD ...] [--trace-lcd] [--leds]
    IMAGE   an upgrade image (default fw/kks/s49_xmos_120.bin, local only) or
            --tiles UI.bin USB.bin (tile RAM images from xld.py)
  Script commands (one per line or -e, times in ms from start):
    at T key NOTE [VEL]       keybed note on (VEL 0..1023, default 600)
    at T keyoff NOTE          keybed note off
    at T button BIT 0|1       input shift-register bit (0..31) pressed (1) / released (0)
    at T midi HEX...          bytes into DIN MIDI IN
    at T knob N VALUE         sensor 0x12 value N (0..8)
    at T show                 print the displays
  Output: LCD command log summary, the 9 displays as text, lit LEDs, DIN MIDI OUT bytes.

Tile naming as in the notes: "UI tile" = image segment 0 (chanend node 0x8000), "USB tile" =
segment 1 (node 0x0000). Port names are per tile.

Devices:
- 3 ST7565-family LCD controllers on UI 4A (CS0..2 bits 0..2, A0 bit 3) / 4B (RST bit 0, SDA
  bit 1, SCK bit 2); RAM decoded per xmos.md §11.3 into 9 displays x (dots/icons + 2 text lines),
  characters by the 16-segment font table (§11).
- LED chains (74HC595 type, outputs active low): A data 1G latch 1E (72 bits), B 1J/1L (80),
  C 1M/1N (112); per-bit brightness 0..127 = on-time fraction between latches.
- 4 x 74HC165 input registers: load 1C, data 1A (buffered, 8 bits per input), 32 bits active low.
- I2C (bit-banged, open drain) SCL 1H, SDA 1F: slaves 0x15 (touch frame, 102 bytes) and 0x12
  (9 x u16, 22 bytes), protocol per samd.md §7.
- Keybed UART into UI 1K, 31 250 Bd 8N1: [0x90|v_hi, note, v_lo] / [0x80|v_hi, note, v_lo].
- DIN MIDI: USB tile 1J (OUT) decoded as a UART, 1L (IN) driven from the script.
- SPI flash on the USB tile (MISO 1C, SS 1A, CLK 1B, MOSI 1D): JEDEC 0x1F4501, erased (0xFF).
"""
import argparse, functools, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import xsim
import ximage

REF = 100_000_000                   # reference clock, Hz
BIT = 3200                          # 31 250 Bd in reference ticks

# 16-segment font (xmos.md §11, cell bits); used only to turn segment patterns back into text
FONT = {
    ' ': 0x0000, '!': 0x0004, '"': 0x0204, '#': 0x5AC0, '$': 0x5ABB, '%': 0x7E99, '&': 0xA133,
    "'": 0x0200, '(': 0x00E1, ')': 0x001E, '*': 0xFF00, '+': 0x5A00, ',': 0x2000, '-': 0x1800,
    '/': 0x2400, '0': 0x24FF, '1': 0x4231, '2': 0x1877, '3': 0x183F, '4': 0x188C,
    '5': 0x18BB, '6': 0x18FB, '7': 0x2403, '8': 0x18FF, '9': 0x18BF, ':': 0x00C0, ';': 0x2200,
    '<': 0x8400, '=': 0x1830, '>': 0x2100, '?': 0x5087, '@': 0x12F7, 'A': 0x18CF, 'B': 0x523F,
    'C': 0x00F3, 'D': 0x423F, 'E': 0x18F3, 'F': 0x18C3, 'G': 0x10FB, 'H': 0x18CC, 'I': 0x4233,
    'J': 0x007C, 'K': 0x8CC0, 'L': 0x00F0, 'M': 0x05CC, 'N': 0x81CC, 'O': 0x00FF, 'P': 0x18C7,
    'Q': 0x80FF, 'R': 0x98C7, 'S': 0x8133, 'T': 0x4203, 'U': 0x00FC, 'V': 0x24C0, 'W': 0xA0CC,
    'X': 0xA500, 'Y': 0x4500, 'Z': 0x2433, '\\': 0x8100, '^': 0xA000, '_': 0x0030, '`': 0x0100,
    '|': 0x4200,
}
GLYPH = {}
for ch, bits in FONT.items():
    GLYPH.setdefault(bits, ch)

# §11.3: cell bit -> (byte offset from base, bit) for rows 1 and 2
ROW1 = [(1, 7), (2, 7), (4, 7), (4, 4), (3, 4), (2, 4), (1, 4), (1, 6), (2, 6), (3, 6), (3, 7),
        (1, 5), (4, 6), (2, 5), (3, 5), (4, 5)]
ROW2 = [(2, 3), (3, 3), (4, 2), (4, 0), (3, 0), (2, 0), (0, 0), (1, 3), (1, 2), (2, 2), (3, 2),
        (1, 1), (4, 1), (1, 0), (2, 1), (3, 1)]
ROW2_LAST = [(0, 3), (1, 3), (3, 3), (3, 0), (2, 0), (1, 0), (0, 0), (0, 2), (1, 2), (2, 2),
             (2, 3), (0, 1), (3, 2), (1, 1), (2, 1), (3, 1)]
# row 0 (docs/hardware/s-mk1.md, tested): the meter's 9 blocks, 9 thin lines and box, as
# page-B byte offsets within the 40-byte block of a display slot (bit 0)
METER_BLOCK = [7, 11, 4, 1, 24, 26, 29, 36, 32]
METER_LINE = [6, 9, 12, 2, 39, 27, 37, 34, 31]
METER_BOX = 14


class LCD:
    """Three ST7565-family controllers sharing SDA/SCK/A0/RST, one chip select each."""

    def __init__(self, tile):
        self.a, self.b = tile.port('4A'), tile.port('4B')
        self.a.listeners.append(self)
        self.b.listeners.append(self)
        self.va, self.vb = 0xf, 0
        self.ram = [bytearray(16 * 256) for _ in range(3)]
        self.page, self.col = [0] * 3, [0] * 3
        self.shift, self.nbits = [0] * 3, [0] * 3
        self.arg = [None] * 3            # pending 2-byte command (0x81 contrast)
        self.log = [[] for _ in range(3)]  # (time ms, 'cmd'|'data', byte), commands only
        self.resets = []
        self.data_bytes = [0] * 3
        self.refreshes = [0] * 3
        self.refresh_t = [[] for _ in range(3)]

    def port_out(self, port, v, n, t, period):
        v = v >> (port.width * (n - 1)) & port.wmask
        if port is self.a:
            self.va = v
            return
        old, self.vb = self.vb, v
        if not v & 1:
            for c in range(3):
                self.nbits[c], self.arg[c] = 0, None
        elif not old & 1:
            self.resets.append(t)                     # end of a reset pulse
        if not old & 4 and v & 4 and v & 1:          # SCK rising, not in reset
            bit = v >> 1 & 1
            for c in range(3):
                if not self.va >> c & 1:
                    self.shift[c] = (self.shift[c] << 1 | bit) & 255
                    self.nbits[c] += 1
                    if self.nbits[c] == 8:
                        self.nbits[c] = 0
                        self.byte(c, self.shift[c], self.va >> 3 & 1, t)

    def port_release(self, port, t):
        pass

    def byte(self, c, b, data, t):
        if data:
            self.ram[c][self.page[c] * 256 + self.col[c]] = b
            self.col[c] = (self.col[c] + 1) & 255
            self.data_bytes[c] += 1
            return
        self.log[c].append((t, b))
        if self.arg[c] is not None:
            self.arg[c] = None
            return
        if b == 0x81:
            self.arg[c] = b
        elif b & 0xf0 == 0xb0:
            self.page[c] = b & 15
            if b == 0xb0:
                self.refreshes[c] += 1
                self.refresh_t[c].append(t)
        elif b & 0xf0 == 0x10:
            self.col[c] = (b & 15) << 4 | self.col[c] & 15
        elif b & 0xf0 == 0x00:
            self.col[c] = self.col[c] & 0xf0 | b & 15

    def buffer(self, c):
        """The firmware's 240-byte buffer of controller c: page 0 and page 15 from column 0x24."""
        r = self.ram[c]
        return r[0x24:0x24 + 120] + r[15 * 256 + 0x24:15 * 256 + 0x24 + 120]

    def cells(self, d):
        """Display d (0..8): (row-1 cells, row-2 cells, row-1 dots, row-2 dots, meter bits:
        box, 9 blocks, 9 lines)."""
        c, s = d // 3, d % 3
        buf = self.buffer(c)
        bit = lambda off, b: buf[off] >> b & 1
        r1, r2, d1, d2 = [], [], [], []
        for p in range(8):
            q = 8 * s + p
            base = 5 * q
            r1.append(sum(bit(base + o, b) << i for i, (o, b) in enumerate(ROW1)))
            if p < 7:
                r2.append(sum(bit(base + o, b) << i for i, (o, b) in enumerate(ROW2)))
            else:
                r2.append(sum(bit(base + 1 + o, b) << i for i, (o, b) in enumerate(ROW2_LAST)))
            d1.append(bit(5 * q + 4, 3) if p <= 6 else 0)
            if p <= 5:
                d2.append(bit(5 * q + 5, 1))
            elif p == 6:
                d2.append(bit(5 * q + 5, 0))
            else:
                d2.append(0)
        O = 120 + 40 * s
        icons = [bit(O + METER_BOX, 0)] + [bit(O + x, 0) for x in METER_BLOCK] + \
                [bit(O + x, 0) for x in METER_LINE]
        return r1, r2, d1, d2, icons

    def text(self, d):
        r1, r2, d1, d2, icons = self.cells(d)
        glyph = getattr(self, 'glyph', None) or GLYPH

        def line(cells, dots):
            s = ''
            for v, dot in zip(cells, dots):
                s += glyph.get(v, '?') + ('.' if dot else '')
            return s
        box, blocks, lines = icons[0], icons[1:10], icons[10:]
        if not (box or any(blocks) or any(lines)):
            meter = ' ' * 11
        else:
            meter = ('[' if box else ' ') + ''.join('#' if b_ else '_' if l_ else ' ' for b_, l_ in zip(blocks, lines)) + (']' if box else ' ')
        return line(r1, d1), line(r2, d2), meter

    def unknown(self, d):
        r1, r2 = self.cells(d)[:2]
        glyph = getattr(self, 'glyph', None) or GLYPH
        return [v for v in r1 + r2 if v not in glyph]


class Chain:
    """74HC595-style chain: data port shifts buffered bytes, latch port rising edge latches.
    Bits are numbered like the firmware buffer (byte i/8 bit i%8, sent last byte first)."""

    def __init__(self, name, data, latch, nbits):
        self.name, self.nbits = name, nbits
        self.data, self.latch = data, latch
        data.listeners.append(self)
        latch.listeners.append(self)
        self.sent = []                   # bits since the last latch, in shift order
        self.state = [1] * nbits         # latched outputs (1 = off, active low)
        self.on_time = [0.0] * nbits
        self.t_last = 0.0
        self.t0 = 0.0
        self.latches = 0
        self.lv = 0

    def port_out(self, port, v, n, t, period):
        if port is self.data:
            for i in range(n * port.width):
                self.sent.append(v >> i & 1)
            return
        lv = v >> (port.width * (n - 1)) & 1
        if lv and not self.lv:
            self.latch_now(t)
        self.lv = lv

    def port_release(self, port, t):
        pass

    def latch_now(self, t):
        self.account(t)
        bits, self.sent = self.sent, []
        total = len(bits)
        nbytes = total // 8
        for k, b in enumerate(bits):
            idx = 8 * (nbytes - 1 - k // 8) + k % 8
            if 0 <= idx < self.nbits:
                self.state[idx] = b
        self.latches += 1

    def account(self, t):
        dt = t - self.t_last
        if dt > 0:
            for i, s in enumerate(self.state):
                if not s:
                    self.on_time[i] += dt
        self.t_last = t

    def reset_window(self, t):
        self.account(t)
        self.on_time = [0.0] * self.nbits
        self.t0 = t

    def brightness(self, t):
        self.account(t)
        span = t - self.t0
        return [round(127 * x / span) if span else 0 for x in self.on_time]


class Duty:
    """The fraction of time a 1-bit output is high (PWM), since reset_window()."""

    def __init__(self, port):
        port.listeners.append(self)
        self.level, self.t_last, self.t0, self.high = 0, 0.0, 0.0, 0.0

    def port_out(self, port, v, n, t, period):
        self.account(t)
        self.level = v >> (port.width * (n - 1)) & 1

    def port_release(self, port, t):
        pass

    def account(self, t):
        if t > self.t_last:
            if self.level:
                self.high += t - self.t_last
            self.t_last = t

    def reset_window(self, t):
        self.account(t)
        self.high, self.t0 = 0.0, t

    def brightness(self, t):
        self.account(t)
        return round(127 * self.high / (t - self.t0)) if t > self.t0 else 0


class Inputs:
    """4 x 74HC165: 1C low/high = parallel load, then each 8-bit input of 1A returns the next
    byte (LSB first). bits[i] = 1 when input i is pressed (pins are active low)."""

    def __init__(self, tile):
        self.load, self.data = tile.port('1C'), tile.port('1A')
        self.load.listeners.append(self)
        self.data.in_hook = self.sample
        self.pressed = 0
        self.latched, self.pos = 0xffffffff, 0
        self.lv = 1
        self.loads = 0

    def port_out(self, port, v, n, t, period):
        lv = v >> (port.width * (n - 1)) & 1
        if not lv:
            self.latched = ~self.pressed & 0xffffffff
            self.pos = 0
            self.loads += 1
        self.lv = lv

    def port_release(self, port, t):
        pass

    def sample(self, port, times):
        out = []
        for _ in times:
            out.append(self.latched >> self.pos & 1 if self.pos < 32 else 1)
            self.pos += 1
        return out


class I2CBus:
    """Open-drain bus on two 1-bit ports with slaves {address: object with write(bytes) and
    read() -> bytes}."""

    def __init__(self, sys_, scl, sda, slaves):
        self.sys, self.scl_p, self.sda_p = sys_, scl, sda
        scl.listeners.append(self)
        sda.listeners.append(self)
        self.slaves = slaves
        self.m = {scl: None, sda: None}  # master drive: None = released
        self.slave_sda = 1
        self.scl, self.sda = 1, 1
        self.state = 'idle'
        self.bits, self.nbit = 0, 0
        self.addr, self.rd, self.wbuf, self.rbuf = None, False, [], b''
        self.ack_phase = False
        self.log = []
        self.reads = 0
        self.writes = {}                 # (address, bytes) -> count
        self.update(0)

    def port_out(self, port, v, n, t, period):
        self.m[port] = v >> (port.width * (n - 1)) & 1
        self.update(t)

    def port_release(self, port, t):
        self.m[port] = None
        self.update(t)

    def level(self, port):
        d = self.m[port]
        lv = 1 if d is None else d
        if port is self.sda_p:
            lv &= self.slave_sda
        return lv

    def update(self, t):
        scl, sda = self.level(self.scl_p), self.level(self.sda_p)
        old_scl, old_sda = self.scl, self.sda
        self.scl, self.sda = scl, sda
        if scl and old_scl and old_sda and not sda:
            self.start()
        elif scl and old_scl and not old_sda and sda:
            self.stop()
        elif scl and not old_scl:
            self.rise(sda)
        elif not scl and old_scl:
            self.fall()
        self.scl_p.set_ext(self.level(self.scl_p), t)
        self.sda_p.set_ext(self.level(self.sda_p), t)

    def start(self):
        if self.state == 'write' and self.addr is not None:
            self.finish_write()
        self.state, self.bits, self.nbit, self.addr = 'addr', 0, 0, None
        self.slave_sda, self.ack_phase = 1, False

    def stop(self):
        if self.state == 'write' and self.addr is not None:
            self.finish_write()
        self.state, self.slave_sda, self.ack_phase = 'idle', 1, False

    def finish_write(self):
        s = self.slaves.get(self.addr)
        if s and self.wbuf:
            s.write(bytes(self.wbuf))
            self.log.append(('W', self.addr, bytes(self.wbuf)))
            del self.log[:-64]
            key = (self.addr, bytes(self.wbuf))
            self.writes[key] = self.writes.get(key, 0) + 1
        self.wbuf = []

    def rise(self, sda):
        if self.state in ('idle', 'nack'):
            return
        if self.ack_phase:
            if self.state == 'read' and sda:          # master NACK: end of read
                self.state = 'nack'
            return
        if self.state in ('addr', 'write'):
            self.bits = self.bits << 1 | sda
            self.nbit += 1

    def fall(self):
        if self.state in ('idle', 'nack'):
            self.slave_sda = 1
            return
        if self.ack_phase:                             # end of the ACK clock
            self.ack_phase = False
            self.slave_sda = 1
            if self.state == 'read':
                self.drive_read_bit()
            return
        if self.state in ('addr', 'write') and self.nbit == 8:
            b, self.bits, self.nbit = self.bits, 0, 0
            if self.state == 'addr':
                self.addr, self.rd = b >> 1, b & 1
                if self.addr not in self.slaves:
                    self.state = 'nack'
                    return
                self.slave_sda, self.ack_phase = 0, True
                if self.rd:
                    s = self.slaves[self.addr]
                    self.rbuf = s.read()
                    self.log.append(('R', self.addr, self.rbuf))
                    self.reads += 1
                    self.state, self.rpos, self.rbit = 'read', 0, 0
                else:
                    self.state, self.wbuf = 'write', []
            else:
                self.wbuf.append(b)
                self.slave_sda, self.ack_phase = 0, True
            return
        if self.state == 'read':
            self.rbit += 1
            if self.rbit == 8:
                self.rbit, self.rpos = 0, self.rpos + 1
                self.slave_sda, self.ack_phase = 1, True   # master acks
                return
            self.drive_read_bit()

    def drive_read_bit(self):
        b = self.rbuf[self.rpos] if self.rpos < len(self.rbuf) else 0xff
        self.slave_sda = b >> (7 - self.rbit) & 1


class Sensor:
    """I2C sensor MCU: '03 nn' selects the streamed reply, reads return [cmd, len, payload]."""

    def __init__(self, length, payload):
        self.length, self.payload = length, payload
        self.reply = 5
        self.writes = []

    def write(self, b):
        self.writes.append(b)
        if b[0] in (2, 3) and len(b) > 1:
            self.reply = b[1]

    def read(self):
        if self.reply == 5:
            return bytes([5, self.length]) + self.payload()
        return bytes([0x60, 3, 0])


class UartTx:
    """Drives a 1-bit input port with 8N1 frames (idle high)."""

    def __init__(self, sys_, port):
        self.sys, self.port = sys_, port
        port.set_ext(1, 0)
        self.free_at = 0

    def send(self, data, t):
        cpt = self.sys.cpt
        t = max(t, self.free_at)
        for b in data:
            frame = [0] + [b >> i & 1 for i in range(8)] + [1]
            for k, lv in enumerate(frame):
                when = t + k * BIT * cpt
                self.sys.at(when, functools.partial(self.port.set_ext, lv, when))
            t += 10 * BIT * cpt
        self.free_at = t


class UartRx:
    """Decodes 8N1 frames from the output of a 1-bit port. Decodes the logical level: an
    inverted port (setc INV_INVERT) drives an inverting line driver on the board."""

    def __init__(self, sys_, port, name):
        self.sys, self.port, self.name = sys_, port, name
        port.listeners.append(self)
        self.edges = []                  # (time, logical level) changes not yet decoded
        self.level = 1
        self.pos = 0.0                   # everything before this time is decoded
        self.bytes = []                  # (time ms, byte, stop bit)

    def port_out(self, port, v, n, t, period):
        for i in range(n):
            lv = (v >> i & 1) ^ port.inverted
            if lv != self.level:
                self.edges.append((t + i * period, lv))
                self.level = lv
        self.flush(t)

    def port_release(self, port, t):
        pass

    def value_at(self, t):
        lv = 1 - self.edges[0][1] if self.edges else self.level
        for te, x in self.edges:
            if te > t:
                break
            lv = x
        return lv

    def flush(self, now):
        bt = BIT * self.sys.cpt
        while True:
            starts = [e for e in self.edges if e[1] == 0 and e[0] >= self.pos]
            if not starts or now < starts[0][0] + 9.6 * bt:
                return
            t0 = starts[0][0]
            b = sum(self.value_at(t0 + (1.5 + i) * bt) << i for i in range(8))
            stop = self.value_at(t0 + 9.5 * bt)
            self.bytes.append((t0 / self.sys.cpt / 1e5, b, stop))
            self.pos = t0 + 9.5 * bt
            keep = [e for e in self.edges if e[0] <= self.pos]
            self.edges = keep[-1:] + [e for e in self.edges if e[0] > self.pos]


class SpiFlash:
    """SPI NOR flash, AT25DF081A-like (JEDEC 1F 45 01), mode 0, MSB first. Edge driven: on a
    rising CLK edge MISO takes the next output bit, then MOSI is sampled.
    Like the real chip: the status register comes up with the sector protection set (as NI's
    firmware leaves it, 0x0C), erase/program/status writes need a write enable first, and
    erase/program are refused while protected (counted in .refused). Below 0x55000 the
    contents stand for NI's data (a fixed test pattern, not NI's bytes); any change there is
    counted in .violations."""

    PROTECTED_END = 0x55000

    def __init__(self, tile, size=1 << 20):
        self.miso, self.ss, self.clk, self.mosi = (tile.port(n) for n in ('1C', '1A', '1B', '1D'))
        for p in (self.ss, self.mosi, self.clk):
            p.listeners.append(self)
        self.mem = bytearray(b'\xff') * size
        for a in range(0, self.PROTECTED_END, 251):
            self.mem[a] = a * 7 & 0xFF
        self.sr, self.wel = 0x0C, False
        self.refused = self.violations = 0
        self.window = (0, 0)                 # the upgrade slot (xmos_layout): changes allowed
        self.sel, self.clk_v, self.mosi_v = False, 0, 0
        self.cmd, self.nbit, self.byte, self.outq = [], 0, 0, []
        self.log = []

    def port_out(self, port, v, n, t, period):
        lv = v >> (port.width * (n - 1)) & 1
        if port is self.ss:
            if lv and self.sel and self.cmd:
                self.log.append(bytes(self.cmd[:6]))
                del self.log[:-200]
                self.finish(self.cmd)
            if not lv and not self.sel:
                self.cmd, self.nbit, self.byte, self.outq = [], 0, 0, []
            self.sel = not lv
        elif port is self.mosi:
            self.mosi_v = lv
        elif port is self.clk:
            if lv and not self.clk_v and self.sel:
                self.miso.set_ext(self.outq.pop(0) if self.outq else 1, t)
                self.byte = (self.byte << 1 | self.mosi_v) & 255
                self.nbit += 1
                if self.nbit == 8:
                    self.nbit = 0
                    self.cmd.append(self.byte)
                    self.respond()
            self.clk_v = lv

    def finish(self, c):
        """At chip-select release: the command takes effect."""
        if c[0] == 0x06:
            self.wel = True
            return
        if c[0] == 0x01 and len(c) >= 2:
            if self.wel:
                self.sr = 0x0C if c[1] == 0x7F else 0 if c[1] == 0 else self.sr
            self.wel = False
            return
        if c[0] in (0x20, 0x02) and len(c) >= 4:
            a = c[1] << 16 | c[2] << 8 | c[3]
            if not self.wel or self.sr & 0x0C:
                self.refused += 1
            elif c[0] == 0x20:
                a &= ~4095
                if a < self.PROTECTED_END and not self.window[0] <= a < self.window[1]:
                    self.violations += 1
                self.mem[a:a + 4096] = b'\xff' * 4096
            else:
                for i, x in enumerate(c[4:]):
                    k = a & ~255 | (a + i) & 255
                    if k < self.PROTECTED_END and not self.window[0] <= k < self.window[1]:
                        self.violations += 1
                    self.mem[k] &= x
            self.wel = False

    def respond(self):
        c = self.cmd
        if len(c) == 1:
            if c[0] == 0x9F:
                self.queue(bytes([0x1F, 0x45, 0x01]))
            elif c[0] == 0x05:
                self.queue(bytes([self.sr | (2 if self.wel else 0)]) * 64)
        if c[0] in (0x0B, 0x03) and len(c) == (5 if c[0] == 0x0B else 4):
            a = c[1] << 16 | c[2] << 8 | c[3]
            self.queue(bytes(self.mem[a:a + 256]))
        if c[0] in (0x0B, 0x03) and len(c) > 8 and not self.outq:
            a = (c[1] << 16 | c[2] << 8 | c[3]) + len(c) - (5 if c[0] == 0x0B else 4)
            self.queue(bytes(self.mem[a:a + 256]))

    def xmos_layout(self, upgrade=None, factory=None):
        """Give the flash an XMOS boot layout like the S49's (fw notes xmos.md section 13), from
        our own test bytes: a boot loader whose word 0 is its size n (factory image at 4n + 8 =
        0x1000), a factory image built by ximage (two segments, about 80 KB), the upgrade image
        bytes `upgrade` (if given) in the sector after it. Sets .window to the upgrade slot the
        firmware must use (platform/xs1/upgrade.c): changes there are not violations."""
        n = 0x3FE
        self.mem[0:4] = struct.pack('<I', n).translate(ximage.REV8)
        for i in range(4, 4 * n + 8):
            self.mem[i] = i * 3 & 0xFF
        seg = lambda size, k: bytes((0x11 + i * 13 + k) & 0xFF for i in range(size))
        fac = factory or ximage.build([seg(0xB000, 0), seg(0x9000, 1)], ident=0x12345678)
        self.mem[0x1000:0x1000 + len(fac)] = fac
        lo = 0x1000 + len(fac) + 4095 & ~4095
        if upgrade:
            self.mem[lo:lo + len(upgrade)] = upgrade
            self.window = (lo, min(lo + 0x20000, 0x4C000))
        else:
            lo = lo + 0xFFFF & ~0xFFFF
            self.window = (lo, min(lo + 0x20000, 0x4C000))
        return self.window

    def queue(self, data):
        for b in data:
            self.outq += [b >> (7 - i) & 1 for i in range(8)]

    def port_release(self, port, t):
        pass


class HostLink:
    """Stand-in for tile 1 (USB, 5-pin MIDI, flash) so a tile-0 image runs without it.

    Link contract (what our tile 0 and tile 1 firmware exchange; the real tile 1 must match):
    - Each tile allocates its link chanend first in main, so it is chanend 0 of its node:
      LINK_TILE0 = 0x80000002, LINK_TILE1 = 0x00000002 with NI's node numbering (tile 0 is node
      0x8000, tile 1 node 0x0000); link-time constants on both sides. Each setd's its link
      chanend to the other's.
    - Tile 1 speaks first: USB_STATE once it is up (at least 1 ms after boot, so tile 0 has
      allocated its end). Tile 0 sends nothing on the link before it has received a packet;
      tokens sent to an unallocated chanend are lost.
    - Every message is one packet of byte tokens: [type, n, n payload bytes], then the END
      control token (1), which closes the route. n <= 255. Multi-byte numbers little endian.
      Use outt/int for the bytes and outct/chkct for END. Either side may send at any time
      after that, so each side needs a thread (or event) receiving the link.
    tile 0 -> tile 1:
      0x81 USB_MIDI_TX  4-byte USB-MIDI event packets (byte 0: cable << 4 | CIN), any number
      0x82 DIN_TX       raw bytes for the 5-pin MIDI OUT
      0x83 FLASH_READ   u32 address, u8 length          -> FLASH_DATA
      0x84 FLASH_ERASE  u32 address (4 KB sector)       -> FLASH_DONE
      0x85 FLASH_WRITE  u32 address, data (in one page) -> FLASH_DONE
      0x86 KBD_TAP      u8 modifier, u8 key: one press and release on the USB keyboard
      0x87 SYSTEM       u8 1 = reboot into update mode, 2 = remove the upgrade image (FLASH_DONE)
                        and reboot into the factory image
    tile 1 -> tile 0:
      0x01 USB_STATE    u8 bit 0 = configured by a host, bit 1 = update mode (sent once, then
                        nothing); sent first and on changes
      0x02 USB_MIDI_RX  4-byte USB-MIDI event packets from the host
      0x03 DIN_RX       raw bytes from the 5-pin MIDI IN
      0x04 FLASH_DATA   bytes read
      0x05 FLASH_DONE   u8 0 = ok
      0x06 DEV_EVENT    developer builds: u8 type, u8 id, s16 value, given to app_event()
    The stub: no USB host (USB_STATE 0 unless set), a 1 MB flash in RAM (refusing changes
    below 0x55000 like the firmware's driver; attempts counted in .violations), DIN and USB
    traffic recorded in .din_tx and .usb_tx, input injected with din_rx() / usb_rx().
    """
    USB_MIDI_TX, DIN_TX, FLASH_READ, FLASH_ERASE, FLASH_WRITE = 0x81, 0x82, 0x83, 0x84, 0x85
    LINK_TILE0 = 0x80000002
    PROTECTED_END = 0x55000                  # below: NI's data, never erased or written
    USB_STATE, USB_MIDI_RX, DIN_RX, FLASH_DATA, FLASH_DONE, DEV_EVENT = 1, 2, 3, 4, 5, 6

    def __init__(self, sys_, node=0x0000):
        self.sys = sys_
        self.tile = sys_.tile('usb-stub', node, b'', start=False)
        self.ce = self.tile.chanends[0]
        self.ce.in_use = True
        self.ce.sink = self.receive
        self.peer = self.LINK_TILE0
        self.up = False                  # USB_STATE sent: tile 0's end is allocated
        self.rx = []
        self.out = []                    # tokens waiting for room at the peer
        self.usb = 0
        self.flash = bytearray(b'\xff') * (1 << 20)
        for a in range(0, self.PROTECTED_END, 251):   # stands for NI's data (a test pattern)
            self.flash[a] = a * 7 & 0xFF
        self.violations = 0
        self.din_tx, self.usb_tx, self.log = [], [], []
        sys_.at(sys_.cpt * 100_000, self.start)

    def start(self):
        """Like tile 1 coming up: send USB_STATE once tile 0's link chanend exists."""
        if self.sys.chanend(self.peer) is None:
            self.sys.at(self.sys.now + self.sys.cpt * 100_000, self.start)
            return
        self.up = True
        self.send(self.USB_STATE, bytes([self.usb]))

    def receive(self, ce, now):
        while ce.buf:
            v, ct = ce.buf.popleft()
            if not ct:
                self.rx.append(v)
            elif v == xsim.CT_END:
                self.packet(bytes(self.rx), now)
                self.rx = []
        ce.wake_space(now)

    def packet(self, p, now):
        if len(p) < 2 or len(p) != 2 + p[1]:
            self.log.append(('bad packet', p.hex()))
            return
        typ, d = p[0], p[2:]
        t = now / self.sys.cpt / 1e5
        self.log.append((t, typ, d))
        if typ == self.DIN_TX:
            self.din_tx += [(t, x) for x in d]
        elif typ == self.USB_MIDI_TX:
            self.usb_tx += [(t, d[i:i + 4]) for i in range(0, len(d) - 3, 4)]
        elif typ == self.FLASH_READ:
            a, n = struct.unpack('<IB', d[:5])
            self.send(self.FLASH_DATA, bytes(self.flash[a:a + n]), now)
        elif typ == self.FLASH_ERASE:
            a = struct.unpack('<I', d[:4])[0] & ~4095
            if a < self.PROTECTED_END:          # the same guard as drivers/spi_flash.c
                self.violations += 1
                self.send(self.FLASH_DONE, b'\1', now)
                return
            self.flash[a:a + 4096] = b'\xff' * 4096
            self.send(self.FLASH_DONE, b'\0', now)
        elif typ == self.FLASH_WRITE:
            a = struct.unpack('<I', d[:4])[0]
            if a < self.PROTECTED_END:
                self.violations += 1
                self.send(self.FLASH_DONE, b'\1', now)
                return
            for i, x in enumerate(d[4:]):
                self.flash[a + i] &= x
            self.send(self.FLASH_DONE, b'\0', now)

    def send(self, typ, payload, now=None):
        """Queue a packet to tile 0 (dropped before the link is up)."""
        if not self.up:
            return False
        self.out += [(typ, False), (len(payload), False)] + [(x, False) for x in payload]
        self.out.append((xsim.CT_END, True))
        self.pump(self.sys.now if now is None else now)
        return True

    def pump(self, now=None):
        d = self.sys.chanend(self.peer)
        if d is None:
            return
        n = min(len(self.out), d.CAP - len(d.buf))
        if n > 0:
            d.buf.extend(self.out[:n])
            del self.out[:n]
            d.notify(now)
        if self.out:
            d.space.add(self)            # woken like a sending thread when tile 0 reads

    def wake(self, time=None):
        self.pump(time)

    def dev_event(self, typ, id_, value):
        """An event for the core (DEV builds only), like tools/mktest.py over USB."""
        return self.send(self.DEV_EVENT, struct.pack('<BBh', typ, id_, value))

    def din_rx(self, data):
        return self.send(self.DIN_RX, bytes(data))

    def usb_sysex(self, cable, msg):
        """A SysEx message from the USB host, in link packets of at most 60 event packets."""
        p = sysex_packets(cable, msg)
        for i in range(0, len(p), 240):
            self.send(self.USB_MIDI_RX, p[i:i + 240])

    def usb_rx(self, cable, msg):
        """A MIDI message from the USB host on a cable, as USB-MIDI event packets."""
        cin = {0x8: 8, 0x9: 9, 0xA: 10, 0xB: 11, 0xC: 12, 0xD: 13, 0xE: 14}.get(msg[0] >> 4, 15)
        pkt = bytes([cable << 4 | cin]) + bytes(msg[:3]).ljust(3, b'\0')
        return self.send(self.USB_MIDI_RX, pkt)


def sysex_packets(cable, msg):
    """USB-MIDI event packets (4 bytes each) carrying one SysEx message on a cable."""
    out = b''
    for i in range(0, len(msg), 3):
        part = msg[i:i + 3]
        cin = 4 if len(msg) - i > 3 else 4 + len(part)
        out += bytes([cable << 4 | cin]) + part.ljust(3, b'\0')
    return out


class UsbHost:
    """A USB host at the level where our SIE thread (firmware usb_ulpi.c) hands transactions
    to TinyUSB: the thread is replaced by this model when it starts (a hook on
    usb_ulpi_sie_thread), which takes the DCD's commands from ulpi_cmdq and answers in
    ulpi_evq with one-token bells, exactly as the SIE does (usb_ulpi.h). So everything above
    the wire runs as built for tile 1: the DCD, TinyUSB, our descriptors, MIDI, HID, DFU and
    the flash writes. The wire itself (ULPI, the PHY, timing) is not modelled.

    run(ms) advances the board; control() / bulk_out() / bulk_in() run it until the device
    has answered (or a time limit)."""
    QSIZE, Q_HEAD, Q_TAIL, Q_BELL = 16, 256, 260, 264
    CMD_OPEN, CMD_XFER, CMD_STALL, CMD_CLR_STALL, CMD_SET_ADDR, CMD_CLOSE_ALL = 1, 2, 3, 4, 5, 6
    EVT_SETUP, EVT_COMPLETE, EVT_RESET = 0x40, 0x41, 0x42

    def __init__(self, board, tile, syms):
        self.b, self.tile = board, tile
        self.cmdq, self.evq = syms['ulpi_cmdq'], syms['ulpi_evq']
        self.park = syms['_exit']
        tile.hooks[syms['usb_ulpi_sie_thread']] = self.sie_start
        self.sie = None
        self.xfer = {}                      # ep -> (address, length) queued by the device
        self.stalled = set()
        self.address = 0
        self.log = []

    # -- tile RAM
    def rd32(self, a):
        return struct.unpack_from('<I', self.tile.ram, a - self.tile.base)[0]

    def wr32(self, a, v):
        struct.pack_into('<I', self.tile.ram, a - self.tile.base, v & 0xFFFFFFFF)

    def rd(self, a, n):
        return bytes(self.tile.ram[a - self.tile.base:a - self.tile.base + n])

    def wr(self, a, data):
        self.tile.ram[a - self.tile.base:a - self.tile.base + len(data)] = data

    # -- the SIE side
    def sie_start(self, t):
        self.sie = self.tile.sys.chanend(t.R[0])
        self.dcd = self.sie.dest_id
        self.sie.sink = self.bell
        return self.park                    # the thread waits for good

    def bell(self, d, now):
        d.pop(len(d.buf), now)
        q = self.cmdq
        self.wr32(q + self.Q_BELL, 0)
        while self.rd32(q + self.Q_TAIL) != self.rd32(q + self.Q_HEAD):
            tail = self.rd32(q + self.Q_TAIL)
            tag, a, b, c = struct.unpack_from('<4I', self.tile.ram, q + 16 * (tail % self.QSIZE) - self.tile.base)
            self.wr32(q + self.Q_TAIL, tail + 1)
            self.command(tag, a, b, c)

    def command(self, tag, a, b, c):
        if tag == self.CMD_XFER:
            self.xfer[a] = (b, c)
            self.stalled.discard(a)
        elif tag == self.CMD_STALL:
            self.stalled.add(a)
            self.xfer.pop(a, None)
        elif tag == self.CMD_CLR_STALL:
            self.stalled.discard(a)
        elif tag == self.CMD_SET_ADDR:
            self.address = a
            self.xfer[0x80] = (0, 0)        # the SIE sends the status ZLP itself
        elif tag == self.CMD_CLOSE_ALL:
            for ep in [e for e in self.xfer if e & 0x7F]:
                del self.xfer[ep]

    def event(self, tag, a=0, b=0, c=0):
        q = self.evq
        head = self.rd32(q + self.Q_HEAD)
        if head - self.rd32(q + self.Q_TAIL) >= self.QSIZE:
            raise RuntimeError('USB event queue full')
        struct.pack_into('<4I', self.tile.ram, q + 16 * (head % self.QSIZE) - self.tile.base, tag, a, b, c)
        self.wr32(q + self.Q_HEAD, head + 1)
        if not self.rd32(q + self.Q_BELL):
            self.wr32(q + self.Q_BELL, 1)
            d = self.tile.sys.chanend(self.dcd)
            d.buf.append((0, False))
            d.notify(self.tile.sys.now)

    # -- the host side
    def up(self):
        return self.sie is not None

    def wait(self, cond, ms=50.0):
        """Run the board until cond() or ms pass; False on a time limit or a stop."""
        sys_ = self.b.sys
        tick = 1e5 * sys_.cpt                # cycles per ms
        end = sys_.now + ms * tick
        while not cond():
            if sys_.now >= end - 1:
                return False
            if sys_.run(until=min(end, sys_.now + 0.05 * tick)) != 'time limit':
                return False
        return True

    def reset(self):
        self.xfer.clear()
        self.stalled.clear()
        self.address = 0
        self.event(self.EVT_RESET, 0)       # TUSB_SPEED_FULL
        self.wait(lambda: False, 1.0)

    def _complete(self, ep, n):
        self.xfer.pop(ep, None)
        self.event(self.EVT_COMPLETE, ep, n)

    def control(self, rtype, req, value=0, index=0, length=0, data=b'', ms=200.0):
        """One control transfer: (bytes for IN, b'' for OUT) or None if stalled / no answer."""
        setup = struct.pack('<BBHHH', rtype, req, value, index, length)
        self.stalled.discard(0x00), self.stalled.discard(0x80)
        self.xfer.pop(0x00, None), self.xfer.pop(0x80, None)
        a, b = struct.unpack('<II', setup)
        self.event(self.EVT_SETUP, a, b)
        stall = lambda: 0x00 in self.stalled or 0x80 in self.stalled
        got = b''
        if length and rtype & 0x80:
            while True:
                if not self.wait(lambda: 0x80 in self.xfer or stall(), ms) or stall():
                    return None
                addr, n = self.xfer[0x80]
                got += self.rd(addr, n) if n else b''
                self._complete(0x80, n)
                if n < 64 or len(got) >= length:
                    break
            if not self.wait(lambda: 0x00 in self.xfer or stall(), ms) or stall():
                return None
            self._complete(0x00, 0)
        else:
            sent = 0
            while sent < length:
                if not self.wait(lambda: 0x00 in self.xfer or stall(), ms) or stall():
                    return None
                addr, n = self.xfer[0x00]
                k = min(n, length - sent)
                self.wr(addr, data[sent:sent + k])
                sent += k
                self._complete(0x00, k)
            if not self.wait(lambda: 0x80 in self.xfer or stall(), ms) or stall():
                return None
            self._complete(0x80, 0)
        self.wait(lambda: False, 0.05)       # the stack takes the status stage
        return got

    def bulk_out(self, ep, data, ms=50.0):
        if not self.wait(lambda: ep in self.xfer, ms):
            return False
        addr, n = self.xfer[ep]
        self.wr(addr, data[:n])
        self._complete(ep, min(n, len(data)))
        return True

    def bulk_in(self, ep):
        """What the device has queued on IN endpoint ep, b'' if nothing."""
        if ep not in self.xfer:
            return b''
        addr, n = self.xfer[ep]
        data = self.rd(addr, n)
        self._complete(ep, n)
        return data

    def enumerate(self):
        """Reset, device descriptor, address, configuration (all of it), SET_CONFIGURATION;
        returns (device descriptor, configuration) or None."""
        self.reset()
        dev = self.control(0x80, 6, 0x0100, 0, 64)
        if not dev or self.control(0x00, 5, 7) is None:
            return None
        head = self.control(0x80, 6, 0x0200, 0, 9)
        if not head:
            return None
        cfg = self.control(0x80, 6, 0x0200, 0, head[2] | head[3] << 8)
        if not cfg or self.control(0x00, 9, 1) is None:
            return None
        return dev, cfg

    def string(self, index):
        s = self.control(0x80, 6, 0x0300 | index, 0x0409, 255)
        return s[2:s[0]].decode('utf-16-le') if s else None


def image_at(mem, a):
    """The tile RAM images (tile 0, tile 1) of a valid xflash image at flash address a, or None:
    magic, lengths, header CRC and segment CRCs checked."""
    hdr = bytes(mem[a:a + 0x1C]).translate(ximage.REV8)
    magic, _, crc, length, _, length2, n = struct.unpack('<7I', hdr)
    if magic != ximage.MAGIC or length != length2 or not 0 < n < 8 or length % 4 or length > 0x30000:
        return None
    d = bytes(mem[a:a + length]).translate(ximage.REV8)
    words = struct.unpack_from('<%dI' % ((length - 0x10) // 4), d, 0x10)
    if ~ximage.step(ximage.crc_words(words), 0) & 0xFFFFFFFF != crc:
        return None
    tiles = {}
    for k in range(n):
        off, flags, size = struct.unpack_from('<3I', d, 0x1C + 12 * k)
        seg = d[off:off + 4 * size]
        if ximage.seg_crc(seg[:-4]) != struct.unpack_from('<I', seg, len(seg) - 4)[0]:
            return None
        tiles[0 if flags >> 16 == 0x8000 else 1] = seg[:-4]
    return tiles.get(0), tiles.get(1)


def boot_images(mem):
    """What the boot loader starts from this flash, as far as fw notes xmos.md sections 3, 4
    and 13 describe it (not a model of the loader's code): an upgrade image in a sector after
    the factory image when it checks out, else the factory image. ((tile 0, tile 1), 'upgrade'
    or 'factory'), or (None, None)."""
    n = struct.unpack('<I', bytes(mem[0:4]).translate(ximage.REV8))[0]
    f = 4 * n + 8
    fac = image_at(mem, f) if 8 <= f < 0x10000 else None
    if not fac:
        return None, None
    flen = struct.unpack('<I', bytes(mem[f + 12:f + 16]).translate(ximage.REV8))[0]
    for a in range(f + flen + 4095 & ~4095, 0x4C000, 4096):
        up = image_at(mem, a)
        if up:
            return up, 'upgrade'
    return fac, 'factory'


def stack_blocks(map_path):
    """[(address, size, 'object:symbol')] of the bss blocks in a map (thread stacks among them)."""
    blocks, sec = [], None
    for line in open(map_path):
        if not line.startswith(' '):
            sec = line.split(':')[0].strip()
            continue
        p = line.split()
        if sec.startswith('blocks') and len(p) >= 6 and p[2] == 'bss':
            blocks.append((int(p[0], 16), int(p[1]), '%s:%s' % (p[3], p[5].replace('.data', ''))))
    return blocks


def stack_use(tile, map_path):
    """[(name, bytes available, bytes used)] for every started thread of a tile: the main
    stack (above the image, from the map's stack area) or the static array a thread's stack
    is in (its block in the map), from the lowest sp seen (xsim tracks it per thread)."""
    blocks, main = [], None
    sec = None
    for line in open(map_path):
        if not line.startswith(' '):
            sec = line.split(':')[0].strip()
            continue
        p = line.split()
        if sec == 'areas' and p[0] == 'stack':
            main = int(p[1], 16)
        elif sec.startswith('blocks') and len(p) >= 6 and p[2] == 'bss':
            blocks.append((int(p[0], 16), int(p[1]), '%s:%s' % (p[3], p[5].replace('.data', ''))))
    out = []
    for t in tile.threads:
        if t.sp_top is None:
            continue
        name, start = 'main', main
        for a, size, n in blocks:
            if a <= t.sp_top - 4 < a + size:
                name, start = n, a
                break
        if start is None:
            continue
        out.append((name, t.sp_top - start, t.sp_top - t.min_sp))
    return out


class Board:
    """Both tiles and their devices. usb_img None: tile 1 is the HostLink stub."""

    def __init__(self, ui_img, usb_img=None, mhz=500, ui_syms=None, usb_syms=None):
        self.sys = s = xsim.System(mhz)
        self.ui = s.tile('ui', 0x8000, ui_img, ui_syms)
        ui = self.ui
        self.lcd = LCD(ui)
        self.chains = [Chain('A light guide 1', ui.port('1G'), ui.port('1E'), 72),
                       Chain('B light guide 2', ui.port('1J'), ui.port('1L'), 80),
                       Chain('C buttons/strips', ui.port('1M'), ui.port('1N'), 112)]
        self.inputs = Inputs(ui)
        self.backlight = Duty(ui.port('1O'))
        self.knobs = [0] * 9
        self.touch = [0] * 48
        self.pedal, self.switches = 0, 0
        self.s15 = Sensor(102, self.touch_frame)
        self.s12 = Sensor(22, self.knob_frame)
        self.i2c = I2CBus(s, ui.port('1H'), ui.port('1F'), {0x15: self.s15, 0x12: self.s12})
        self.keybed = UartTx(s, ui.port('1K'))
        if usb_img is None:
            self.link, self.usb = HostLink(s), None
            self.din_in = self.din_out = self.flash = self.host = None
            return
        self.link = None
        self.usb = usb = s.tile('usb', 0x0000, usb_img, usb_syms)
        self.din_in = UartTx(s, usb.port('1L'))
        self.din_out = UartRx(s, usb.port('1J'), 'DIN OUT')
        self.flash = SpiFlash(usb)
        self.host = UsbHost(self, usb, usb_syms) if usb_syms and 'usb_ulpi_sie_thread' in usb_syms else None

    def touch_frame(self):
        return struct.pack('<H48hBB', self.pedal, *self.touch, self.switches, 0)

    def knob_frame(self):
        return struct.pack('<9H2B', *self.knobs, 0, 0)

    def ms(self):
        return self.sys.now / self.sys.cpt / 1e5

    def run_ms(self, ms):
        return self.sys.run(until=ms * 1e5 * self.sys.cpt)

    def key(self, note, vel, on=True):
        v = max(0, min(1023, vel))
        self.keybed.send(bytes([(0x90 if on else 0x80) | v >> 7, note, v & 127]), self.sys.now)

    def displays(self):
        out = []
        for d in range(9):
            l1, l2, icons = self.lcd.text(d)
            out.append((d, l1, l2, icons))
        return out


def load_image(path):
    raw = open(path, 'rb').read()
    d = raw.translate(ximage.REV8)
    n = struct.unpack_from('<I', d, 0x18)[0]
    segs = []
    for k in range(n):
        off, flags, words = struct.unpack_from('<3I', d, 0x1C + 12 * k)
        segs.append(d[off:off + 4 * words - 4])
    return segs


def ms(b, t):
    return t / b.sys.cpt / 1e5


def report(b, show_log=True):
    lcd = b.lcd
    print('t = %.1f ms, %d instructions' % (b.ms(), b.sys.steps))
    if show_log:
        for c in range(3):
            cmds = [x for _, x in lcd.log[c]]
            init = ' '.join('%02X' % x for x in cmds[:11])
            print('LCD %d: reset at %s ms, init %s, %d refreshes (at %s ms), %d data bytes'
                  % (c, ', '.join('%.2f' % ms(b, t) for t in lcd.resets) or '-', init,
                     lcd.refreshes[c], ', '.join('%.0f' % ms(b, t) for t in lcd.refresh_t[c][:6]),
                     lcd.data_bytes[c]))
    print('displays (line 1 | line 2 | meter):')
    for d, l1, l2, icons in b.displays():
        print('  %d  [%-8s] [%-8s]  %s' % (d + 1, l1, l2, icons))
        unk = lcd.unknown(d)
        if unk:
            print('     unknown cells: %s' % ' '.join('%04x' % v for v in unk))


def threads(b):
    for tl in (b.ui, b.usb):
        if tl is None:
            print('USB tile: HostLink stub')
            continue
        st = []
        for t in tl.threads:
            if t.state != 'free':
                st.append('%d:%s@%x' % (t.num, t.state, t.pc))
        print('%s tile threads: %s' % (tl.name_.upper(), ' '.join(st)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('image', nargs='?', default=os.path.join(ROOT, 'fw', 'kks', 's49_xmos_120.bin'))
    ap.add_argument('--tiles', nargs=2)
    ap.add_argument('--ms', type=float, default=3500)
    ap.add_argument('--script')
    ap.add_argument('-e', action='append', default=[])
    ap.add_argument('--leds', action='store_true')
    ap.add_argument('--mhz', type=int, default=500)
    a = ap.parse_args()
    if a.tiles:
        ui_img, usb_img = (open(p, 'rb').read() for p in a.tiles)
    else:
        ui_img, usb_img = load_image(a.image)[:2]
    b = Board(ui_img, usb_img, a.mhz)
    cmds = []
    for line in (open(a.script).read().split('\n') if a.script else []) + a.e:
        line = line.split('#')[0].split()
        if line:
            if line[0] != 'at':
                sys.exit('script: expected "at T ...": %s' % ' '.join(line))
            cmds.append((float(line[1]), line[2], line[3:]))
    cmds.sort(key=lambda c: c[0])
    cmds.append((a.ms - 20, 'window', []))
    cmds.sort(key=lambda c: c[0])
    for t, cmd, args in cmds + [(a.ms, 'end', [])]:
        why = b.run_ms(t)
        if why != 'time limit':
            print('stopped at %.3f ms: %s' % (b.ms(), why))
            break
        if cmd == 'key':
            b.key(int(args[0]), int(args[1]) if len(args) > 1 else 600)
        elif cmd == 'keyoff':
            b.key(int(args[0]), 0, False)
        elif cmd == 'button':
            bit = int(args[0])
            b.inputs.pressed = b.inputs.pressed & ~(1 << bit) | (int(args[1]) & 1) << bit
        elif cmd == 'midi':
            b.din_in.send(bytes.fromhex(''.join(args)), b.sys.now)
        elif cmd == 'knob':
            b.knobs[int(args[0])] = int(args[1])
        elif cmd == 'show':
            report(b, False)
        elif cmd == 'window':
            for ch in b.chains:
                ch.reset_window(b.sys.now)
    report(b)
    if b.din_out is None:
        print('DIN MIDI OUT (HostLink): ' + ' '.join('%02X' % x for _, x in b.link.din_tx))
    elif b.din_out.flush(b.sys.now) or b.din_out.bytes:
        print('DIN MIDI OUT: ' + ' '.join('%02X@%.2fms' % (x, t) for t, x, s in b.din_out.bytes))
    else:
        print('DIN MIDI OUT: nothing')
    if a.leds:
        print('LEDs, brightness 0..127 over the last 20 ms (chain bit:value):')
        for ch in b.chains:
            br = ch.brightness(b.sys.now)
            lit = ['%d:%d' % (i, v) for i, v in enumerate(br) if v]
            print('  %s: %d latches, lit %s' % (ch.name, ch.latches, ' '.join(lit) or 'none'))
    print('I2C: %d reads; writes: %s' % (b.i2c.reads, ', '.join(
        '0x%02x [%s%s] x%d' % (ad, d[:4].hex(' '), ' ...%dB' % len(d) if len(d) > 4 else '', n)
        for (ad, d), n in sorted(b.i2c.writes.items()))))
    threads(b)


if __name__ == '__main__':
    main()
