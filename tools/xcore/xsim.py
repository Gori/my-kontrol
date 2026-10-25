#!/usr/bin/env python3
"""XMOS XS1 simulator: tiles, hardware threads, timers, ports, clock blocks, channels, locks,
synchronisers and events. No dependencies.

usage:
  xsim.py TILE.bin [-m MAP] [--max N] [--trace]      one tile, test programs (xtest.py)

Runs a tile RAM image (as xld.py writes it) from the RAM base. With the map file: a call to
`_sim_putc` prints r0 as a character and returns, reaching `_exit` stops with r0 as the exit
status. Exit status of xsim: the program's r0 at _exit, 125 on any other stop.
Board models (s49emu.py) use System/Tile directly: several tiles, devices on ports.

Model (semantics follow the XS1 architecture; tool_axe was read as a reference):
- Time is counted in core cycles (default 500 MHz, CPT = 5 cycles per 100 MHz reference tick).
  A tile issues one instruction per cycle; a thread at most one per 4 cycles, so with n
  runnable threads each gets one instruction every max(4, n) cycles. Threads run in bursts of
  BURST instructions (cross-thread ordering is approximate within a burst).
- Blocking instructions (timer/port/channel input, full channel buffers, locks, waiteu, syncs)
  deschedule the thread; it is woken when the resource changes and the instruction re-executes.
  When no thread can run, time jumps to the next scheduled event.
- Ports: 1/4/8/16/32-bit, clocked by a clock block (ref/2d, or an external clock port, which
  never ticks here), counter of falling edges, setpt/getts, conditions EQ/NEQ, buffered with
  transfer width and setpsc, inverted, clock ports. Pins are modelled per transfer: devices get
  port_out(port, value, n, time, period) for n chunks shifted out one per edge, port_release()
  when the port turns into an input, and drive inputs with Port.set_ext(); a device can also
  supply sampled values for buffered inputs (Port.in_hook). Ready/strobe signals are ignored.
- Channels: 8-token input buffers, routes claimed until END/PAUSE, between tiles by node id;
  a destination that does not exist swallows the packet.
- Events: setv/setev/eeu/edu/eet/eef/clre/waiteu/waitet/waitef, ED. Interrupts and kernel
  calls are not modelled (they stop the simulation).
Not covered: xCONNECT switch registers, crc8 timing, dual issue (XS2), exact pipeline timing.
"""
import argparse, collections, functools, heapq, os, pickle, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xdis

M = 0xFFFFFFFF
BITP = xdis.BITP
CP, DP, SP, LR = 12, 13, 14, 15
EEBLE, IEBLE, INENB = 1, 2, 4
CT_END, CT_PAUSE, CT_ACK = 1, 2, 3
INF = float('inf')
BURST = 16

# setc values (XS1)
INUSE_OFF, INUSE_ON = 0x0, 0x8
COND_FULL, COND_AFTER, COND_EQ, COND_NEQ = 0x1, 0x9, 0x11, 0x19
IE_EVENT, IE_INTERRUPT = 0x2, 0xa
RUN_STOPR, RUN_STARTR, RUN_CLRBUF = 0x7, 0xf, 0x17
BUF_NOBUFFERS, BUF_BUFFERS = 0x2007, 0x200f
PORT_DATA, PORT_CLOCK, PORT_READY = 0x5007, 0x500f, 0x5017
INV_NO, INV_YES = 0x6007, 0x600f


class SimError(Exception):
    pass


class Stop(Exception):
    pass


class Block(Exception):
    """Raised by a blocking instruction; resume = pc to continue at when woken (None: re-execute
    the instruction)."""
    def __init__(self, resume=None):
        self.resume = resume


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def sext(v, n):
    if 1 <= n < 32:
        v &= (1 << n) - 1
        if v >> (n - 1) & 1:
            v -= 1 << n
    return v & M


def zext(v, n):
    return v & ((1 << n) - 1) if n < 32 else v


def mask(n):
    return (1 << n) - 1 if n < 32 else M


def crc(d, x, p, bits):
    for _ in range(bits):
        d = d >> 1 ^ (p if d & 1 else 0)
    return d ^ (x << (32 - bits) & M)


def port_name(pid):
    w, n = pid >> 16, pid >> 8 & 255
    return '%d%s' % (w, chr(65 + n))


# ---------------------------------------------------------------------------------------------
# Resources

class Res:
    """Base: id, owning tile, threads waiting for a change (woken all at once)."""
    eventable = False

    def __init__(self, tile, rid):
        self.tile, self.id = tile, rid
        self.in_use = False
        self.waiters = set()

    def wait(self, t):
        self.waiters.add(t)

    def notify(self, time=None):
        if self.waiters:
            w, self.waiters = self.waiters, set()
            for t in w:
                t.wake(time)

    def setc(self, t, v):
        if v == INUSE_ON:
            self.in_use = True
            return self.reset(t)
        if v == INUSE_OFF:
            self.in_use = False
            return
        raise SimError('setc 0x%x not valid for resource 0x%x' % (v, self.id))

    def reset(self, t):
        pass


class EvRes(Res):
    """A resource that can raise events: vector, environment, enable, owner."""
    eventable = True

    def __init__(self, tile, rid):
        super().__init__(tile, rid)
        self.vector, self.ev, self.enabled, self.owner, self.interrupt = 0, rid, False, None, False

    def ev_reset(self, t):
        self.vector, self.ev, self.interrupt = 0, self.id, False
        self.disable()
        self.owner = t

    def enable(self, t):
        self.owner = t
        if not self.enabled:
            self.enabled = True
            t.events.append(self)

    def disable(self):
        if self.enabled:
            self.enabled = False
            if self.owner and self in self.owner.events:
                self.owner.events.remove(self)

    def ready(self, now):
        return False

    def arm(self, t, now):
        """The owner waits for an event: make sure it is woken when ready() may change."""
        self.wait(t)

    def ed(self):
        return self.ev if self.ev == self.id else (self.ev & 0xffff) | self.tile.base

    def take(self, t):
        """Event taken: hook for resources that change state (none here)."""


class Timer(EvRes):
    def __init__(self, tile, rid):
        super().__init__(tile, rid)
        self.after, self.data = False, 0

    def reset(self, t):
        self.after, self.data = False, 0
        self.ev_reset(t)

    def delay(self, now):
        """Cycles until the condition holds (0 = now)."""
        if not self.after:
            return 0
        cpt = self.tile.sys.cpt
        tick = int(now // cpt)
        d = s32((self.data + 1 - tick) & M)
        return 0 if d <= 0 else (tick + d) * cpt - now

    def ready(self, now):
        return self.delay(now) == 0

    def arm(self, t, now):
        d = self.delay(now)
        if d:
            self.tile.sys.wake_at(now + d, t)

    def cond(self, t, v):
        self.owner = t
        if v == COND_AFTER:
            self.after = True
        elif v == COND_FULL:
            self.after = False
        else:
            raise SimError('timer condition 0x%x' % v)

    def setd(self, t, v):
        self.owner = t
        self.data = v

    def getd(self, t):
        return self.data

    def inp(self, t, now):
        self.owner = t
        d = self.delay(now)
        if d:
            tile, sys_ = self.tile, self.tile.sys
            if len(tile.runq) == 1 and d <= sys_.short_wait and \
                    (not sys_.heap or sys_.heap[0][0] > now + d):
                tile.time = now + d          # nothing else can run meanwhile: just let time pass
                return int(tile.time // sys_.cpt) & M
            sys_.wake_at(now + d, t)
            raise Block
        return int(now // self.tile.sys.cpt) & M


class Lock(Res):
    def __init__(self, tile, rid):
        super().__init__(tile, rid)
        self.held = False

    def inp(self, t, now):
        if self.held:
            self.wait(t)
            raise Block
        self.held = True
        return self.id

    def out(self, t, v, now):
        self.held = False
        self.notify(now)


class Sync(Res):
    def __init__(self, tile, rid):
        super().__init__(tile, rid)
        self.threads, self.paused, self.join = [], 0, False

    def alloc(self, master):
        self.threads, self.paused, self.join = [master], 0, False

    def sync(self, t, master, resume, now):
        """Barrier of the master and all slaves (msync/ssync/mjoin)."""
        if self.paused + 1 < len(self.threads):
            self.paused += 1
            t.state = 'sync'
            raise Block(resume)
        self.paused = 0
        if not self.join:
            for x in self.threads:
                if x is not t:
                    x.resume(now)
            return
        self.join = False
        slaves, self.threads = self.threads[1:], self.threads[:1]
        for x in slaves:
            if x is not t:
                x.free()
        if not master:
            self.threads[0].resume(now)
            t.free()
            raise Block(resume)


class ClockBlock(Res):
    def __init__(self, tile, rid):
        super().__init__(tile, rid)
        self.running, self.start, self.div, self.ext = False, 0, 1, False
        self.ports = set()

    def reset(self, t):
        self.running, self.div, self.ext = False, 1, False

    def setc(self, t, v):
        now = t.tile.time
        if v == RUN_STARTR:
            self.running, self.start = True, int(now // self.tile.sys.cpt)
            for p in self.ports:
                p.clock_started(now)
            return
        if v == RUN_STOPR:
            self.running = False
            return
        super().setc(t, v)

    def setd(self, t, v):
        v &= 0xff
        self.div = 2 * v if v else 1

    def setclk(self, t, src):
        self.ext = src != 1          # 1 = reference clock; otherwise a port: no clock here

    def period(self):
        return self.div * self.tile.sys.cpt

    def ticking(self):
        return self.running and not self.ext


class Port(EvRes):
    def __init__(self, tile, rid):
        super().__init__(tile, rid)
        self.width = rid >> 16
        self.wmask = mask(self.width)
        self.name = port_name(rid)
        self.listeners, self.in_hook = [], None
        self.ext = 0                  # value driven onto the pins from outside
        self.reset_state()
        self.clock = tile.clkblks[0]
        tile.clkblks[0].ports.add(self)
        self.k0 = 0

    def reset_state(self):
        self.output, self.drive = False, 0
        self.buffered, self.tw, self.psc = False, self.width, None
        self.cond, self.data = COND_FULL, 0
        self.time_reg, self.ts = None, 0
        self.pending, self.busy_until = None, 0
        self.inverted, self.ptype = False, PORT_DATA
        self.in_deadline, self.wait_start = None, None
        self.last_change = 0
        self.cap = None               # timed input captured at the port time: (value, ts)
        self.cap_seq, self.armed = 0, False

    def reset(self, t):
        self.reset_state()
        self.ev_reset(t)
        self.k0 = self.edges(t.tile.time)

    # -- clock
    def edges(self, now):
        """Falling edges of the port clock since the clock block started."""
        cb = self.clock
        if not cb.ticking():
            return 0
        return (int(now // self.tile.sys.cpt) - cb.start) // cb.div

    def counter(self, now):
        return (self.edges(now) - self.k0) & 0xffff

    def clock_started(self, now):
        self.k0 = 0

    def time_of(self, count, now):
        """Time of the next edge at which the port counter equals count (INF: no clock)."""
        cb = self.clock
        if not cb.ticking():
            return INF
        k = self.edges(now)
        delta = (count - (k - self.k0)) & 0xffff
        if delta == 0:
            return now
        return (cb.start + (k + delta) * cb.div) * self.tile.sys.cpt

    # -- pins
    def pins(self):
        v = self.drive if self.output else self.ext
        return v ^ 1 if self.inverted else v

    def set_ext(self, v, now):
        """Device drives the input pins."""
        v &= self.wmask
        if v != self.ext:
            self.ext = v
            self.last_change = now
            self.notify(now)

    def meets(self):
        p = self.pins()
        return self.cond == COND_FULL or (p == self.data) == (self.cond == COND_EQ)

    def cond_set(self, t, v):
        self.owner = t
        if v not in (COND_FULL, COND_EQ, COND_NEQ):
            raise SimError('port condition 0x%x' % v)
        self.cond = v
        self.notify(t.tile.time)

    def setd(self, t, v):
        self.owner = t
        self.data = v & self.wmask
        self.notify(t.tile.time)

    def getd(self, t):
        return self.data

    def setc(self, t, v):
        if v in (COND_FULL, COND_EQ, COND_NEQ):
            return self.cond_set(t, v)
        if v == BUF_BUFFERS or v == BUF_NOBUFFERS:
            self.buffered = v == BUF_BUFFERS
            if not self.buffered:
                self.tw = self.width
            return
        if v in (PORT_DATA, PORT_CLOCK, PORT_READY):
            self.ptype = v
            return
        if v in (INV_NO, INV_YES):
            self.inverted = v == INV_YES
            return
        if v == RUN_CLRBUF:
            self.pending, self.time_reg, self.in_deadline, self.cap = None, None, None, None
            return
        if v & 7 == 3 or v >> 12 in (1, 3, 4):     # drive modes, master/slave, ready, sdelay
            return
        super().setc(t, v)

    def setclk(self, t, cb):
        self.clock.ports.discard(self)
        self.clock = cb
        cb.ports.add(self)
        self.k0 = self.edges(t.tile.time)

    def settw(self, t, v):
        if v % self.width or not self.width <= v <= 32:
            raise SimError('port %s: transfer width %d' % (self.name, v))
        self.tw = v

    def setpt(self, t, v):
        self.time_reg = v & 0xffff
        self.cap, self.armed = None, False
        if not self.output:
            self.arm_capture(t.tile.time)

    def arm_capture(self, now):
        """An input port samples at the port time whether or not an `in` is waiting."""
        te = self.time_of(self.time_reg, now)
        if te == INF:
            return
        self.cap_seq += 1
        self.armed = True
        if te <= now:
            self.capture(self.cap_seq, now)
        else:
            self.tile.sys.at(te, functools.partial(self.capture, self.cap_seq, te))

    def capture(self, seq, te):
        if seq != self.cap_seq or self.time_reg is None:
            return
        n = (self.psc or self.tw) // self.width if self.buffered else 1
        period = self.clock.period()
        self.psc = None
        self.time_reg, self.armed = None, False
        self.cap = (self.sample([te - (n - 1 - i) * period for i in range(n)]), self.counter(te))
        self.notify(te)

    def clrpt(self, t):
        self.time_reg, self.cap, self.armed = None, None, False

    def setpsc(self, t, v):
        self.psc = v

    def chunks(self):
        n = (self.psc or self.tw) // self.width if self.buffered else 1
        self.psc = None
        return n

    # -- output
    def out(self, t, v, now):
        self.owner = t
        if self.pending is not None:
            self.wait(t)
            raise Block
        n = self.chunks()
        start = max(now, self.busy_until)
        if self.time_reg is not None:
            start = max(self.time_of(self.time_reg, now), self.busy_until)
            self.time_reg = None
        if start == INF:
            self.pending = (v, INF, n)     # never clocked out
            return
        if start > now:
            self.pending = (v, start, n)
            self.tile.sys.at(start, self.apply)
        else:
            self.emit(v, now, n)

    def apply(self):
        if self.pending is None:
            return
        v, start, n = self.pending
        self.pending = None
        self.emit(v, start, n)
        self.notify(start)

    def emit(self, v, start, n):
        period = self.clock.period()
        w = self.width
        if self.inverted:
            v ^= mask(w * n)
        self.output = True
        self.drive = v >> (w * (n - 1)) & self.wmask
        self.ts = self.counter(start)
        self.busy_until = start + n * period
        for d in self.listeners:
            d.port_out(self, v & mask(w * n), n, start, period)

    def syncr(self, t, now):
        if self.pending is not None:
            self.wait(t)
            raise Block
        if self.busy_until > now:
            self.tile.sys.wake_at(self.busy_until, t)
            raise Block

    # -- input
    def release(self, now):
        if self.output:
            self.output = False
            for d in self.listeners:
                d.port_release(self, now)

    def sample(self, times):
        if self.in_hook:
            vals = self.in_hook(self, times)
        else:
            vals = [self.pins()] * len(times)
        v = 0
        for i, x in enumerate(vals):
            v |= (x & self.wmask) << (self.width * i)
        return v

    def inp(self, t, now):
        self.owner = t
        self.release(now)
        period = self.clock.period()
        n = (self.psc or self.tw) // self.width if self.buffered else 1
        if self.cap is not None:
            v, self.ts = self.cap
            self.cap = None
            return v
        if self.time_reg is not None:
            if not self.armed:
                self.arm_capture(now)
            if self.cap is None:
                self.wait(t)
                raise Block
            v, self.ts = self.cap
            self.cap = None
            return v
        elif self.cond != COND_FULL:
            if not self.meets():
                if self.wait_start is None:
                    self.wait_start = now
                self.wait(t)
                raise Block
            te = now
            if self.wait_start is not None:
                te = min(now, max(self.wait_start, self.last_change))
            self.wait_start = None
            if self.buffered:
                self.cond = COND_FULL
        elif n > 1:
            if self.in_deadline is None:
                self.in_deadline = now + (n - 1) * period
            if self.in_deadline > now:
                self.tile.sys.wake_at(self.in_deadline, t)
                raise Block
            te, self.in_deadline = self.in_deadline, None
        else:
            te = now
        if not self.clock.ticking():
            self.wait(t)                # no clock: never completes
            raise Block
        self.psc = None
        self.ts = self.counter(te)
        return self.sample([te - (n - 1 - i) * period for i in range(n)])

    def peek(self, t, now):
        return self.pins()

    def take(self, t):
        """Event taken: an input port latches the timestamp of the condition becoming true."""
        if not self.output and self.cond != COND_FULL:
            now = self.tile.time
            self.ts = self.counter(min(now, max(self.last_change, now - self.tile.sys.cpt * 64)))

    def ready(self, now):
        if self.output:
            return self.pending is None
        if self.cap is not None:
            return True
        if self.time_reg is not None:
            return False
        return self.meets()

    def arm(self, t, now):
        self.wait(t)


class Chanend(EvRes):
    CAP = 8

    def __init__(self, tile, rid):
        super().__init__(tile, rid)
        self.buf = collections.deque()
        self.dest_id, self.route, self.junk = 0, None, False
        self.source, self.claims = None, collections.deque()
        self.space = set()            # senders waiting for room in this buffer
        self.sink = None              # device consuming tokens sent here: sink(chanend, now)

    def reset(self, t):
        self.ev_reset(t)

    def setd(self, t, v):
        if self.route is not None or self.junk:
            raise SimError('setd on chanend 0x%x in a packet' % self.id)
        self.dest_id = v

    def getd(self, t):
        return self.dest_id

    # -- sending
    def open(self, t):
        if self.route is not None or self.junk:
            return
        d = self.tile.sys.chanend(self.dest_id)
        if d is None:
            self.junk = True
            return
        if d.source is None or d.source is self:
            d.source = self
            self.route = d
            return
        if self not in d.claims:
            d.claims.append(self)
        d.space.add(t)
        raise Block

    def send(self, t, toks, now):
        self.open(t)
        if self.junk:
            return
        d = self.route
        if len(d.buf) + len(toks) > self.CAP:
            d.space.add(t)
            raise Block
        d.buf.extend(toks)
        d.notify(now)
        if d.sink:
            d.sink(d, now)

    def out(self, t, v, now):
        self.send(t, [(v >> 24 & 255, False), (v >> 16 & 255, False), (v >> 8 & 255, False),
                      (v & 255, False)], now)

    def outt(self, t, v, now):
        self.send(t, [(v & 255, False)], now)

    def outct(self, t, v, now):
        v &= 255
        if v == CT_PAUSE:
            self.open(t)
        else:
            self.send(t, [(v, True)], now)
        if v in (CT_END, CT_PAUSE):
            if self.route is not None:
                self.route.release(now)
            self.route, self.junk = None, False

    def release(self, now):
        self.source = None
        while self.claims:
            c = self.claims.popleft()
            self.source = c
            c.route = self
            break
        self.wake_space(now)

    def wake_space(self, now):
        if self.space:
            w, self.space = self.space, set()
            for t in w:
                t.wake(now)

    # -- receiving
    def need(self, t, n):
        if len(self.buf) < n:
            self.owner = t
            self.wait(t)
            raise Block

    def pop(self, n, now):
        v = [self.buf.popleft() for _ in range(n)]
        self.wake_space(now)
        return v

    def inp(self, t, now):
        for i in range(min(4, len(self.buf))):
            if self.buf[i][1]:
                raise SimError('in: control token in data on chanend 0x%x' % self.id)
        self.need(t, 4)
        a, b, c, d = self.pop(4, now)
        return a[0] << 24 | b[0] << 16 | c[0] << 8 | d[0]

    def intoken(self, t, now, ct):
        self.need(t, 1)
        if self.buf[0][1] != ct:
            raise SimError('%s: %s token at chanend 0x%x (value 0x%x)' % (
                'inct' if ct else 'int', 'data' if ct else 'control', self.id, self.buf[0][0]))
        return self.pop(1, now)[0][0]

    def chkct(self, t, v, now):
        self.need(t, 1)
        tok = self.buf[0]
        if not tok[1] or tok[0] != v & 255:
            raise SimError('chkct %d failed at chanend 0x%x (got %s 0x%x)' % (
                v, self.id, 'ct' if tok[1] else 'data', tok[0]))
        self.pop(1, now)

    def testct(self, t, now):
        self.need(t, 1)
        return int(self.buf[0][1])

    def testwct(self, t, now):
        for i in range(min(4, len(self.buf))):
            if self.buf[i][1]:
                return i + 1
        self.need(t, 4)
        return 0

    def ready(self, now):
        return bool(self.buf)


# ---------------------------------------------------------------------------------------------
# Threads and instruction semantics

class Thread(Res):
    def __init__(self, tile, num):
        super().__init__(tile, num << 8 | 4)
        self.num = num
        self.R = [0] * 16
        self.pc, self.sr = tile.base, 0
        self.ed = self.et = self.kep = self.ksp = self.spc = self.ssr = self.sed = 0
        self.state = 'free'           # free, paused, run, wait, sync
        self.sync = None
        self.cache = {}
        self.events = []
        self.res_ops = 0              # resource instructions executed (spin detection)
        self.snap = None

    def spin(self, pc):
        """Called on a taken backward branch. Registers equal to the previous visit and no
        resource instruction since: the loop only waits for another thread to change memory,
        so sleep for SPIN_SLEEP cycles instead of executing it."""
        snap = (pc, self.res_ops, self.sr, *self.R)
        if snap == self.snap:
            self.snap = None
            self.tile.sys.wake_at(self.tile.time + self.tile.sys.spin_sleep, self)
            raise Block(pc)
        self.snap = snap

    def wake(self, time=None):
        if self.state == 'wait':
            self.state = 'run'
            self.tile.runnable(self, time)

    def resume(self, time):
        """Released from a synchroniser or started."""
        if self.state in ('sync', 'paused'):
            self.state = 'run'
            self.tile.runnable(self, time)

    def free(self):
        self.state = 'free'
        self.sync = None
        self.in_use = False
        for r in list(self.events):
            r.disable()

    def clre(self):
        self.sr &= ~(EEBLE | INENB)
        for r in list(self.events):
            r.disable()

    def try_event(self, now):
        """Take a ready enabled event (EEBLE set): returns the new pc or None."""
        for r in self.events:
            if r.ready(now):
                if r.interrupt:
                    raise SimError('interrupts are not modelled (resource 0x%x)' % r.id)
                self.sr &= ~(EEBLE | INENB)
                self.ed = r.ed()
                r.take(self)
                return r.vector
        return None

    def __getstate__(self):
        d = dict(self.__dict__)
        d['cache'] = {}               # closures: rebuilt on demand
        return d

    def compile(self, pc):
        tile = self.tile
        i = tile.addr(pc, 2)
        insn = xdis.decode(tile.ram, i, pc, tile.tab)
        if not insn.op:
            raise SimError('undecodable halfword 0x%04x at %s' % (insn.word, tile.name(pc)))
        f = semantics(self, insn)
        if f is None:
            raise SimError('unsupported instruction %s at %s' % (insn.text(), tile.name(pc)))
        self.cache[pc] = f
        return f


def semantics(th, insn):
    """Closure executing insn for thread th; returns the next pc."""
    tile = th.tile
    R, n, o, nxt = th.R, insn.op.name, insn.ops, insn.addr + insn.size
    ld8, ld16, ld32, st8, st16, st32 = tile.ld8, tile.ld16, tile.ld32, tile.st8, tile.st16, tile.st32
    res = tile.res
    k = insn.op.kinds
    if len(o) >= 3 and k[:3] in ('rrr', 'rru', 'rrb'):
        a, b, c = o[:3]
        if k[2] == 'b':
            c = BITP[c]
        imm = k[2] != 'r'
    if n in ('ADD', 'ADDI', 'SUB', 'SUBI', 'AND', 'OR', 'XOR', 'EQ', 'EQI', 'LSS', 'LSU', 'MUL',
             'SHL', 'SHLI', 'SHR', 'SHRI', 'ASHR', 'ASHRI', 'LDAWF', 'LDAWFI', 'LDAWB', 'LDAWBI',
             'LDA16F', 'LDA16B'):
        fn = {'ADD': lambda x, y: x + y, 'SUB': lambda x, y: x - y, 'AND': lambda x, y: x & y,
              'OR': lambda x, y: x | y, 'XOR': lambda x, y: x ^ y, 'EQ': lambda x, y: int(x == y),
              'LSS': lambda x, y: int(s32(x) < s32(y)), 'LSU': lambda x, y: int(x < y),
              'MUL': lambda x, y: x * y, 'SHL': lambda x, y: x << y if y < 32 else 0,
              'SHR': lambda x, y: x >> y if y < 32 else 0,
              'ASHR': lambda x, y: s32(x) >> min(y, 32),
              'LDAWF': lambda x, y: x + 4 * y, 'LDAWB': lambda x, y: x - 4 * y,
              'LDA16F': lambda x, y: x + 2 * y, 'LDA16B': lambda x, y: x - 2 * y}[n.rstrip('I')]
        if n == 'ADDI':                 # the most common instruction: no lambda call
            def f():
                R[a] = R[b] + c & M
                return nxt
        elif imm:
            def f():
                R[a] = fn(R[b], c) & M
                return nxt
        else:
            def f():
                R[a] = fn(R[b], R[c]) & M
                return nxt
        return f
    if n in ('DIVS', 'DIVU', 'REMS', 'REMU'):
        def f():
            x, y = R[b], R[c]
            if n[3] == 'S':
                x, y = s32(x), s32(y)
            if y == 0 or (n == 'DIVS' and x == -1 << 31 and y == -1):
                raise SimError('arithmetic exception (%s) at %s' % (n, tile.name(insn.addr)))
            q = abs(x) // abs(y) * (1 if (x < 0) == (y < 0) else -1)
            R[a] = (q if n[:3] == 'DIV' else x - q * y) & M
            return nxt
        return f
    if n in ('LDW', 'LDWI', 'LD16S', 'LD8U'):
        sc = {'LDW': 4, 'LDWI': 4, 'LD16S': 2, 'LD8U': 1}[n]
        ld = {4: ld32, 2: lambda x: sext(ld16(x), 16), 1: ld8}[sc]
        if imm:
            def f():
                R[a] = ld(R[b] + sc * c & M)
                return nxt
        else:
            def f():
                R[a] = ld(R[b] + sc * R[c] & M)
                return nxt
        return f
    if n in ('STW', 'STWI', 'ST16', 'ST8'):
        sc = {'STW': 4, 'STWI': 4, 'ST16': 2, 'ST8': 1}[n]
        st = {4: st32, 2: st16, 1: st8}[sc]
        if imm:
            def f():
                st(R[b] + sc * c & M, R[a])
                return nxt
        else:
            def f():
                st(R[b] + sc * R[c] & M, R[a])
                return nxt
        return f
    if n == 'CRC':
        def f():
            R[a] = crc(R[a], R[b], R[c], 32)
            return nxt
        return f
    if n == 'CRC8':
        r, x, p, d = o[:4]

        def f():
            v = R[x]
            R[d] = crc(R[d], v & 255, R[p], 8)
            R[r] = v >> 8
            return nxt
        return f
    if n in ('STWDP', 'STWSP', 'LDWDP', 'LDWSP', 'LDAWDP', 'LDAWSP', 'LDWCP'):
        r, u = o
        base = DP if 'DP' in n else SP if 'SP' in n else CP
        if n.startswith('ST'):
            def f():
                st32(R[base] + 4 * u & M, R[r])
                return nxt
        elif n.startswith('LDAW'):
            def f():
                R[r] = R[base] + 4 * u & M
                return nxt
        else:
            def f():
                R[r] = ld32(R[base] + 4 * u & M)
                return nxt
        return f
    if n == 'LDC':
        r, u = o

        def f():
            R[r] = u
            return nxt
        return f
    if n in ('BRFT', 'BRBT', 'BRFF', 'BRBF'):
        r, u = o
        t = nxt + 2 * u if n[2] == 'F' else nxt - 2 * u
        if n[2] == 'B':
            spin = th.spin
            if n[3] == 'T':
                def f():
                    if R[r]:
                        spin(t)
                        return t
                    return nxt
            else:
                def f():
                    if R[r]:
                        return nxt
                    spin(t)
                    return t
            return f
        if n[3] == 'T':
            return lambda: t if R[r] else nxt
        return lambda: nxt if R[r] else t
    if n in ('BRFU', 'BRBU'):
        t = nxt + 2 * o[0] if n == 'BRFU' else nxt - 2 * o[0]
        if n == 'BRBU':
            spin = th.spin

            def f():
                spin(t)
                return t
            return f
        return lambda: t
    if n in ('BLRF', 'BLRB'):
        t = nxt + 2 * o[0] if n == 'BLRF' else nxt - 2 * o[0]

        def f():
            R[LR] = nxt
            return t
        return f
    if n in ('LDAPF', 'LDAPB'):
        t = nxt + 2 * o[0] if n == 'LDAPF' else nxt - 2 * o[0]

        def f():
            R[11] = t
            return nxt
        return f
    if n == 'BLACP':
        def f():
            R[LR] = nxt
            return ld32(R[CP] + 4 * o[0] & M)
        return f
    if n == 'BLAT':
        def f():
            R[LR] = nxt
            return ld32(R[11] + 4 * o[0] & M)
        return f
    if n == 'LDWCPL':
        def f():
            R[11] = ld32(R[CP] + 4 * o[0] & M)
            return nxt
        return f
    if n == 'LDAWCP':
        def f():
            R[11] = R[CP] + 4 * o[0] & M
            return nxt
        return f
    if n == 'ENTSP':
        u = o[0]

        def f():
            if u:
                st32(R[SP], R[LR])
                R[SP] = R[SP] - 4 * u & M
                if R[SP] < tile.min_sp:
                    tile.min_sp = R[SP]
            return nxt
        return f
    if n == 'EXTSP':
        def f():
            R[SP] = R[SP] - 4 * o[0] & M
            if R[SP] < tile.min_sp:
                tile.min_sp = R[SP]
            return nxt
        return f
    if n == 'RETSP':
        u = o[0]

        def f():
            if u:
                R[SP] = R[SP] + 4 * u & M
                R[LR] = ld32(R[SP])
            return R[LR]
        return f
    if n == 'EXTDP':
        def f():
            R[DP] = R[DP] - 4 * o[0] & M
            return nxt
        return f
    if n in ('GETSR', 'SETSR', 'CLRSR'):
        def f():
            if n == 'GETSR':
                R[11] = th.sr & o[0]
                return nxt
            th.sr = th.sr | o[0] if n == 'SETSR' else th.sr & ~o[0]
            if th.sr & EEBLE:
                pc = th.try_event(tile.time)
                if pc is not None:
                    return pc
            return nxt
        return f
    if n in ('ANDNOT', 'SEXT', 'SEXTI', 'ZEXT', 'ZEXTI', 'NOT', 'NEG', 'MKMSK', 'MKMSKI',
             'BITREV', 'BYTEREV', 'CLZ'):
        d, s = o
        fn = {'ANDNOT': lambda x, y: x & ~R[y], 'SEXT': lambda x, y: sext(x, R[y]),
              'SEXTI': lambda x, y: sext(x, BITP[y]), 'ZEXT': lambda x, y: zext(x, R[y]),
              'ZEXTI': lambda x, y: zext(x, BITP[y]), 'NOT': lambda x, y: ~R[y],
              'NEG': lambda x, y: -R[y], 'MKMSK': lambda x, y: mask(R[y]),
              'MKMSKI': lambda x, y: mask(BITP[y]),
              'BITREV': lambda x, y: int('{:032b}'.format(R[y])[::-1], 2),
              'BYTEREV': lambda x, y: int.from_bytes(R[y].to_bytes(4, 'little'), 'big'),
              'CLZ': lambda x, y: 32 - R[y].bit_length()}[n]

        def f():
            R[d] = fn(R[d], s) & M
            return nxt
        return f
    if n in ('LADD', 'LSUB'):
        d, x, y, e, c = o

        def f():
            v = R[x] + R[y] + (R[c] & 1) if n == 'LADD' else R[x] - R[y] - (R[c] & 1)
            R[d], R[e] = v & M, (v >> 32 & 1) if n == 'LADD' else int(v < 0)
            return nxt
        return f
    if n == 'LMUL':
        h, x, y, l, a1, a2 = o

        def f():
            v = R[x] * R[y] + R[a1] + R[a2]
            R[h], R[l] = v >> 32 & M, v & M
            return nxt
        return f
    if n in ('MACCU', 'MACCS'):
        h, x, y, l = o

        def f():
            p = R[x] * R[y] if n == 'MACCU' else s32(R[x]) * s32(R[y])
            v = (R[h] << 32 | R[l]) + p
            R[h], R[l] = v >> 32 & M, v & M
            return nxt
        return f
    if n == 'LDIVU':
        q, lo, dv, r, hi = o

        def f():
            if R[hi] >= R[dv]:
                raise SimError('arithmetic exception (ldivu) at %s' % tile.name(insn.addr))
            v = R[hi] << 32 | R[lo]
            R[q], R[r] = v // R[dv] & M, v % R[dv]
            return nxt
        return f
    if n in ('BLA', 'BAU', 'BRU', 'SETSP', 'SETDP', 'SETCP', 'ECALLF', 'ECALLT'):
        r = o[0]

        def f():
            if n == 'BLA':
                R[LR] = nxt
                return R[r]
            if n == 'BAU':
                return R[r]
            if n == 'BRU':
                return nxt + 2 * R[r] & M
            if n in ('ECALLF', 'ECALLT'):
                if bool(R[r]) == (n == 'ECALLT'):
                    raise SimError('%s exception at %s' % (n.lower(), tile.name(insn.addr)))
                return nxt
            R[{'SETSP': SP, 'SETDP': DP, 'SETCP': CP}[n]] = R[r]
            return nxt
        return f
    if n == 'SETKEP':
        def f():
            th.kep = R[11] & ~127
            return nxt
        return f
    if n in ('GETID', 'GETED', 'GETET', 'GETKEP', 'GETKSP'):
        def f():
            R[11] = {'GETID': th.num, 'GETED': th.ed, 'GETET': th.et, 'GETKEP': th.kep,
                     'GETKSP': th.ksp}[n]
            return nxt
        return f
    if n == 'GETPS':
        d, r = o

        def f():
            R[d] = tile.ps.get(R[r], 0)
            return nxt
        return f
    if n == 'SETPS':
        r, v = o

        def f():
            tile.ps[R[r]] = R[v]
            return nxt
        return f
    if n == 'CLRE':
        def f():
            th.clre()
            return nxt
        return f
    f = resource_semantics(th, insn, n, o, nxt, res)
    if f is None:
        return None

    def g():
        th.res_ops += 1
        return f()
    return g


def resource_semantics(th, insn, n, o, nxt, res):
    tile, R = th.tile, th.R
    if n == 'GETR':
        d, typ = o

        def f():
            R[d] = tile.getr(th, typ)
            return nxt
        return f
    if n == 'FREER':
        def f():
            tile.freer(res(R[o[0]]))
            return nxt
        return f
    if n in ('SETCI', 'SETC'):
        r, v = o
        get = (lambda: v) if n == 'SETCI' else (lambda: R[v])

        def f():
            x, val = res(R[r]), get()
            if val in (COND_FULL, COND_AFTER, COND_EQ, COND_NEQ) and hasattr(x, 'cond'):
                x.cond(th, val) if isinstance(x, Timer) else x.cond_set(th, val)
            elif val in (IE_EVENT, IE_INTERRUPT) and x.eventable:
                x.interrupt = val == IE_INTERRUPT
            else:
                x.setc(th, val)
            return nxt
        return f
    if n == 'SETD':
        r, v = o

        def f():
            res(R[r]).setd(th, R[v])
            return nxt
        return f
    if n == 'GETD':
        d, r = o

        def f():
            R[d] = res(R[r]).getd(th)
            return nxt
        return f
    if n == 'SETCLK':
        r, v = o

        def f():
            x = res(R[r])
            if isinstance(x, Port):
                x.setclk(th, res(R[v]))
            else:
                x.setclk(th, R[v])
            return nxt
        return f
    if n in ('SETTW', 'SETPT', 'SETPSC'):
        r, v = o
        m = {'SETTW': 'settw', 'SETPT': 'setpt', 'SETPSC': 'setpsc'}[n]

        def f():
            getattr(res(R[r]), m)(th, R[v])
            return nxt
        return f
    if n == 'SETRDY':
        return lambda: nxt
    if n == 'CLRPT':
        def f():
            res(R[o[0]]).clrpt(th)
            return nxt
        return f
    if n in ('OUT', 'OUTT', 'OUTSHR'):
        r, v = o

        def f():
            x = res(R[r])
            if n == 'OUTT':
                x.outt(th, R[v], tile.time)
                return nxt
            x.out(th, R[v], tile.time)
            if n == 'OUTSHR':
                R[v] = R[v] >> x.tw
            return nxt
        return f
    if n == 'OUTPW':
        v, r, w = o
        w = BITP[w]

        def f():
            x = res(R[r])
            x.psc = w
            x.out(th, R[v], tile.time)
            return nxt
        return f
    if n in ('OUTCT', 'OUTCTI'):
        r, v = o
        get = (lambda: v) if n == 'OUTCTI' else (lambda: R[v])

        def f():
            res(R[r]).outct(th, get(), tile.time)
            return nxt
        return f
    if n in ('CHKCT', 'CHKCTI'):
        r, v = o
        get = (lambda: v) if n == 'CHKCTI' else (lambda: R[v])

        def f():
            res(R[r]).chkct(th, get(), tile.time)
            return nxt
        return f
    if n in ('IN', 'INSHR'):
        d, r = o

        def f():
            x = res(R[r])
            v = x.inp(th, tile.time)
            if n == 'INSHR':
                w = x.tw
                v = (R[d] >> w | v << (32 - w)) & M
            R[d] = v & M
            return nxt
        return f
    if n == 'INPW':
        d, r, w = o
        w = BITP[w]

        def f():
            x = res(R[r])
            x.psc = w
            R[d] = x.inp(th, tile.time) << (32 - w) & M
            return nxt
        return f
    if n in ('INT', 'INCT'):
        d, r = o

        def f():
            R[d] = res(R[r]).intoken(th, tile.time, n == 'INCT')
            return nxt
        return f
    if n in ('TESTCT', 'TESTWCT'):
        d, r = o

        def f():
            x = res(R[r])
            R[d] = x.testct(th, tile.time) if n == 'TESTCT' else x.testwct(th, tile.time)
            return nxt
        return f
    if n == 'PEEK':
        d, r = o

        def f():
            R[d] = res(R[r]).peek(th, tile.time)
            return nxt
        return f
    if n == 'GETTS':
        d, r = o

        def f():
            R[d] = res(R[r]).ts
            return nxt
        return f
    if n == 'ENDIN':
        d, r = o

        def f():
            R[d] = 0
            return nxt
        return f
    if n == 'SYNCR':
        def f():
            res(R[o[0]]).syncr(th, tile.time)
            return nxt
        return f
    # events
    if n in ('SETV', 'SETEV'):
        r = o[0]

        def f():
            x = res(R[r])
            x.owner = th
            if n == 'SETV':
                x.vector = R[11]
            else:
                x.ev = R[11]
            return nxt
        return f
    if n in ('EEU', 'EDU', 'EET', 'EEF'):
        if n in ('EEU', 'EDU'):
            r, c = o[0], None
        else:
            c, r = o

        def f():
            x = res(R[r])
            on = n == 'EEU' or (n == 'EET' and R[c]) or (n == 'EEF' and not R[c])
            if on:
                x.enable(th)
                if th.sr & EEBLE:
                    pc = th.try_event(tile.time)
                    if pc is not None:
                        return pc
            else:
                x.owner = th
                x.disable()
            return nxt
        return f
    if n in ('WAITEU', 'WAITET', 'WAITEF'):
        c = o[0] if o else None

        def f():
            if n == 'WAITET' and not R[c] or n == 'WAITEF' and R[c]:
                return nxt
            th.sr |= EEBLE
            now = tile.time
            pc = th.try_event(now)
            if pc is not None:
                return pc
            if not th.events:
                if tile.sys.stop_on_park:
                    raise Stop('thread %d parked (%s, no events) at %s' % (
                        th.num, n.lower(), tile.name(insn.addr)))
                raise Block                 # parked for good; the other threads go on
            for x in th.events:
                x.arm(th, now)
            raise Block
        return f
    # threads and synchronisers
    if n == 'GETST':
        d, s = o

        def f():
            R[d] = tile.getst(th, res(R[s]))
            return nxt
        return f
    if n in ('TINITPC', 'TINITSP', 'TINITDP', 'TINITCP', 'TINITLR'):
        v, r = o
        reg = {'TINITSP': SP, 'TINITDP': DP, 'TINITCP': CP, 'TINITLR': LR}.get(n)

        def f():
            t = res(R[r])
            if reg is None:
                t.pc = R[v]
            else:
                t.R[reg] = R[v]
            return nxt
        return f
    if n == 'TSETR':
        u, v, r = o

        def f():
            res(R[r]).R[u] = R[v]
            return nxt
        return f
    if n == 'TSETMR':
        u, v = o

        def f():
            (th.sync.threads[0] if th.sync else th).R[u] = R[v]
            return nxt
        return f
    if n == 'TSTART':
        def f():
            t = res(R[o[0]])
            t.resume(tile.time)
            return nxt
        return f
    if n in ('MSYNC', 'MJOIN'):
        def f():
            s = res(R[o[0]])
            if n == 'MJOIN':
                s.join = True
            s.sync(th, True, nxt, tile.time)
            return nxt
        return f
    if n == 'SSYNC':
        def f():
            if th.sync is None:
                th.state = 'sync'
                raise Block(nxt)
            th.sync.sync(th, False, nxt, tile.time)
            return nxt
        return f
    if n == 'FREET':
        def f():
            th.free()
            raise Block(nxt)
        return f
    return None


# ---------------------------------------------------------------------------------------------
# Tiles and the system

class SwitchConfig:
    """A node's switch registers, at chanend ID node << 16 | 0xC30C: WRITEC (0xC0) / READC
    (0xC1), return chanend ID >> 8 (3 bytes), register (2 bytes), [value (4 bytes)], END;
    the answer is ACK [+ value] + END, none for return 0xFFFFFF. A write to the PLL register
    (6) resets the chip: the run stops with 'chip reset'."""
    PLL_CTL = 6

    def __init__(self, tile):
        self.tile = tile
        self.regs = {self.PLL_CTL: 0x0080_2B00}
        self.pkt = []
        self.end = Chanend(tile, tile.node << 16 | 0xC30C)
        self.end.in_use = True
        self.end.sink = self.sink

    def sink(self, d, now):
        self.pkt += d.pop(len(d.buf), now)        # taken as they come: a write is 11 tokens
        if not self.pkt or self.pkt[-1] != (CT_END, True):
            return
        toks, self.pkt = self.pkt, []
        if len(toks) < 7 or not toks[0][1] or toks[0][0] not in (0xC0, 0xC1):
            raise SimError('switch 0x%x: bad config packet %s' % (self.tile.node, toks))
        write = toks[0][0] == 0xC0
        ret = toks[1][0] << 16 | toks[2][0] << 8 | toks[3][0]
        reg = toks[4][0] << 8 | toks[5][0]
        if write:
            self.regs[reg] = toks[6][0] << 24 | toks[7][0] << 16 | toks[8][0] << 8 | toks[9][0]
        reply = [(CT_ACK, True)]
        if not write:
            v = self.regs.get(reg, 0)
            reply += [(v >> 24 & 255, False), (v >> 16 & 255, False), (v >> 8 & 255, False),
                      (v & 255, False)]
        reply.append((CT_END, True))
        if ret != 0xFFFFFF:
            r = self.tile.sys.chanend(ret << 8 | 2)
            if r is not None:
                r.buf.extend(reply)
                r.notify(now)
        if write and reg == self.PLL_CTL:
            self.tile.sys.reset_requested = True
            raise Stop('chip reset')


class Tile:
    def __init__(self, sys_, name, node, image=b'', base=0x10000, size=0x10000, syms=None,
                 start=True):
        self.sys, self.name_, self.node = sys_, name, node
        self.base, self.size = base, size
        self.ram = bytearray(size)
        self.ram[:len(image)] = image
        self.m16 = memoryview(self.ram).cast('H')
        self.m32 = memoryview(self.ram).cast('I')
        self.tab = xdis.table('xs1')
        self.syms = syms or {}
        self.ps = {11: base}
        self.time = 0.0
        self.runq = collections.deque()
        self.min_sp = base + size
        self.hooks = {}
        self.clkblks = [ClockBlock(self, i << 8 | 6) for i in range(6)]
        self.clkblks[0].running = True
        self.threads = [Thread(self, i) for i in range(8)]
        self.timers = [Timer(self, i << 8 | 1) for i in range(10)]
        self.chanends = [Chanend(self, node << 16 | i << 8 | 2) for i in range(32)]
        self.switch = SwitchConfig(self)
        self.syncs = [Sync(self, i << 8 | 3) for i in range(8)]
        self.locks = [Lock(self, i << 8 | 5) for i in range(4)]
        self.ports = {}
        for w, cnt in ((1, 16), (4, 6), (8, 4), (16, 4), (32, 2)):
            for i in range(cnt):
                p = Port(self, w << 16 | i << 8)
                self.ports[p.id] = p
        if start:                     # start=False: a tile that only hosts device chanends
            t0 = self.threads[0]
            t0.state, t0.in_use, t0.pc = 'run', True, base
            self.runq.append(t0)

    def __getstate__(self):
        d = dict(self.__dict__)
        del d['m16'], d['m32']
        return d

    def __setstate__(self, d):
        self.__dict__.update(d)
        self.m16 = memoryview(self.ram).cast('H')
        self.m32 = memoryview(self.ram).cast('I')

    def port(self, name):
        """Port by name ('1K', '4A')."""
        w, letter = int(name[:-1]), name[-1]
        return self.ports[w << 16 | (ord(letter) - 65) << 8]

    # -- memory
    def addr(self, a, align):
        i = a - self.base
        if a & (align - 1) or not 0 <= i < self.size:
            raise SimError('%s: bad %d-byte access at 0x%x' % (self.name_, align, a))
        return i

    def ld8(self, a):
        return self.ram[self.addr(a, 1)]

    def ld16(self, a):
        return self.m16[self.addr(a, 2) >> 1]

    def ld32(self, a):
        return self.m32[self.addr(a, 4) >> 2]

    def st8(self, a, v):
        self.ram[self.addr(a, 1)] = v & 255

    def st16(self, a, v):
        self.m16[self.addr(a, 2) >> 1] = v & 0xffff

    def st32(self, a, v):
        self.m32[self.addr(a, 4) >> 2] = v & M

    def name(self, a):
        best = max(((v, n) for n, v in self.syms.items() if v <= a), default=None)
        s = '%s+0x%x' % (best[1], a - best[0]) if best else '0x%x' % a
        return '%s:%s' % (self.name_, s) if self.sys and len(self.sys.tiles) > 1 else s

    # -- resources
    def res(self, rid):
        typ, num = rid & 255, rid >> 8 & 255
        try:
            if typ == 0:
                return self.ports[rid & 0xffffff00]
            if typ == 1:
                return self.timers[num]
            if typ == 2:
                if rid >> 16 != self.node:
                    raise IndexError
                return self.chanends[num]
            if typ == 3:
                return self.syncs[num]
            if typ == 4:
                return self.threads[num]
            if typ == 5:
                return self.locks[num]
            if typ == 6:
                return self.clkblks[num]
        except (IndexError, KeyError):
            pass
        raise SimError('%s: illegal resource 0x%x' % (self.name_, rid))

    def getr(self, t, typ):
        pool = {1: self.timers, 2: self.chanends, 3: self.syncs, 4: self.threads,
                5: self.locks}.get(typ)
        for x in pool or ():
            if not x.in_use:
                x.in_use = True
                if typ == 4:
                    x.state, x.sync, x.sr = 'paused', None, 0   # registers persist (crt0 sets cp/dp)
                elif typ == 3:
                    x.alloc(t)
                else:
                    x.reset(t)
                    if typ == 5:
                        x.held = False
                    if typ == 2:
                        x.buf.clear()
                        x.dest_id, x.route, x.junk = 0, None, False
                return x.id
        return 0

    def getst(self, t, s):
        tid = self.getr(t, 4)
        if tid:
            x = self.threads[tid >> 8 & 255]
            x.sync = s
            s.threads.append(x)
            s.paused += 1
        return tid

    def freer(self, x):
        x.in_use = False
        if isinstance(x, EvRes):
            x.disable()

    # -- scheduling
    def runnable(self, t, time):
        if not self.runq:
            self.time = max(self.time, time if time is not None else self.sys.now)
        self.runq.append(t)


class System:
    """Tiles, the event queue and the scheduler."""

    def __init__(self, mhz=500):
        self.cpt = mhz // 100           # core cycles per 10 ns reference tick
        self.tiles = []
        self.heap, self.seq = [], 0
        self.now = 0.0
        self.steps = 0
        self.stop_on_park = False           # a thread waiting with no events ends the run
        self.reset_requested = False        # a switch PLL write (SwitchConfig) stopped the run
        self.spin_sleep = 2000 * self.cpt   # 20 us
        self.short_wait = 1000 * self.cpt   # timer waits up to 10 us skip the scheduler when
                                            # no other thread of the tile can run before

    def tile(self, name, node, image, syms=None, start=True):
        t = Tile(self, name, node, image, syms=syms, start=start)
        self.tiles.append(t)
        return t

    def chanend(self, rid):
        if rid & 0xFFFF == 0xC30C:
            for t in self.tiles:
                if t.node == rid >> 16:
                    return t.switch.end
            return None
        if rid & 255 != 2:
            return None
        for t in self.tiles:
            if t.node == rid >> 16:
                num = rid >> 8 & 255
                return t.chanends[num] if num < 32 and t.chanends[num].in_use else None
        return None

    def at(self, time, fn):
        self.seq += 1
        heapq.heappush(self.heap, (time, self.seq, fn))

    def wake_at(self, time, t):
        self.at(time, functools.partial(t.wake, time))

    def ticks(self):
        return int(self.now // self.cpt)

    def save(self, path, extra=None):
        """Snapshot of the whole simulation (with `extra`, e.g. the board) to a file."""
        with open(path, 'wb') as f:
            pickle.dump((self, extra), f, protocol=pickle.HIGHEST_PROTOCOL)

    @staticmethod
    def load(path):
        with open(path, 'rb') as f:
            return pickle.load(f)

    def run(self, until=INF, max_steps=None, trace=False):
        """Run until the time `until` (cycles), max_steps instructions, or a stop. Returns the
        reason as a string."""
        heap, tiles = self.heap, self.tiles
        limit = max_steps if max_steps is not None else INF
        while True:
            best = None
            for tl in tiles:
                if tl.runq and (best is None or tl.time < best.time):
                    best = tl
            tnext = heap[0][0] if heap else INF
            if best is None or tnext <= best.time:
                if tnext == INF:
                    if until < INF:         # everything waits for input: time just passes
                        self.now = until
                        return 'time limit'
                    return 'idle: no thread can run'
                if tnext > until:
                    self.now = until
                    return 'time limit'
                self.now = tnext
                heapq.heappop(heap)[2]()
                continue
            if best.time > until:
                self.now = until
                return 'time limit'
            if self.steps >= limit:
                return 'instruction limit (%d) reached' % limit
            try:
                self.burst(best, trace)
            except Stop as e:
                return str(e)
            except SimError as e:
                t = best.runq[0]
                return 'error: %s (thread %d at %s, lr %s)' % (e, t.num, best.name(t.pc),
                                                              best.name(t.R[LR]))

    def burst(self, tile, trace):
        t = tile.runq[0]
        nrun = len(tile.runq)
        inc = max(4, nrun) / nrun
        pc, cache, hooks = t.pc, t.cache, tile.hooks
        self.now = tile.time
        steps = 0
        try:
            for _ in range(BURST):
                if hooks and pc in hooks:
                    pc = hooks[pc](t)
                    continue
                f = cache.get(pc) or t.compile(pc)
                if trace:
                    i = xdis.decode(tile.ram, pc - tile.base, pc, tile.tab)
                    print('%s t%d %8x  %-32s %s' % (tile.name_, t.num, pc, i.text(),
                                                   ' '.join('%x' % r for r in t.R[:12])))
                pc = f()
                tile.time += inc
                steps += 1
            t.pc = pc
            if tile.runq and tile.runq[0] is t:
                tile.runq.rotate(-1)
        except Block as b:
            t.pc = b.resume if b.resume is not None else pc
            tile.time += inc
            if t.state == 'run':
                t.state = 'wait'
            tile.runq.remove(t)
        except SimError:
            t.pc = pc
            raise
        finally:
            self.steps += steps


# ---------------------------------------------------------------------------------------------
# Single-tile programs (xtest.py)

class Sim:
    """One tile running a test program; _sim_putc prints, _exit stops."""

    def __init__(self, image, base=0x10000, size=0x10000, syms=None):
        self.sys = System()
        self.sys.stop_on_park = True
        self.tile = self.sys.tile('tile', 0, image, syms)
        self.base, self.size = base, size
        self.syms = syms or {}
        self.out = []
        if '_sim_putc' in self.syms:
            self.tile.hooks[self.syms['_sim_putc']] = self.putc
        if '_exit' in self.syms:
            self.tile.hooks[self.syms['_exit']] = self.exit

    def putc(self, t):
        c = chr(t.R[0] & 255)
        self.out.append(c)
        sys.stdout.write(c)
        return t.R[LR]

    def exit(self, t):
        raise Stop('exit %d' % s32(t.R[0]))

    @property
    def steps(self):
        return self.sys.steps

    @property
    def min_sp(self):
        return self.tile.min_sp

    def run(self, max_steps=50_000_000, trace=False):
        return self.sys.run(max_steps=max_steps, trace=trace)


def read_map(path):
    syms = {}
    sec = None
    for line in open(path):
        if not line.startswith(' '):
            sec = line.strip()
            continue
        p = line.split()
        if sec == 'symbols:' and len(p) >= 2:
            syms[p[1]] = int(p[0], 16)
    return syms


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('image')
    ap.add_argument('-m', '--map')
    ap.add_argument('--max', type=int, default=50_000_000)
    ap.add_argument('--trace', action='store_true')
    a = ap.parse_args()
    sim = Sim(open(a.image, 'rb').read(), syms=read_map(a.map) if a.map else {})
    why = sim.run(a.max, a.trace)
    sys.stdout.flush()
    print('\nxsim: %s after %d instructions, stack low water 0x%x' % (why, sim.steps, sim.min_sp),
          file=sys.stderr)
    sys.exit(int(why.split()[1]) & 255 if why.startswith('exit') else 125)


if __name__ == '__main__':
    main()
