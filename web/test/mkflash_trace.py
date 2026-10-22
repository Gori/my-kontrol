"""Run the real tools/mkflash.py against a simulated keyboard and print every control
transfer and sleep as JSON. No USB access: pyusb is replaced by stubs before import.

usage: python3 -I mkflash_trace.py <mkflash.py> <scenario.json>
scenario: {"cmd": "enter"|"flash", "image": path, "mode": "app"|"dfu", plus Sim options}
The simulator below must stay identical to Sim in mockusb.js.
"""
import hashlib, io, json, os, runpy, sys, time, types

MKFLASH, SCEN = sys.argv[1], json.load(open(sys.argv[2]))
DESC = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fixtures', 'descriptors.json')))
DESC = {k: bytes(b for d in DESC[k] for b in d) for k in ('app', 'dfu')}
trace = []


class USBError(Exception):
    pass


def st(status, poll, state):
    return bytes([status, poll & 0xff, (poll >> 8) & 0xff, (poll >> 16) & 0xff, state, 0])


class Sim:
    def __init__(s, o):
        s.mode = o.get('mode', 'dfu')
        s.state = o.get('initial_state', 2)
        s.busy_every = o.get('busy_every', 0)
        s.busy_polls = o.get('busy_polls', 1)
        s.busy_poll_ms = o.get('busy_poll_ms', 3)
        s.manifest = [list(x) for x in o.get('manifest', [[8, 0]])]
        s.error_at_block = o.get('error_at_block', -1)
        s.detach_raises = o.get('detach_raises', False)
        s.pending = 0
        s.block = -1
        s.gone = False

    def ctrl(s, bm, req, value, index, data_or_len, timeout):
        if bm & 0x80:
            trace.append(['ctrl', bm, req, value, index, data_or_len, None, timeout or 1000])
        else:
            data = bytes(data_or_len or b'')
            trace.append(['ctrl', bm, req, value, index, len(data),
                          hashlib.sha256(data).hexdigest() if data else None, timeout or 1000])
        if s.gone:
            raise USBError('No such device')
        if bm == 0x80 and req == 6:
            return DESC[s.mode][:data_or_len]
        if bm == 0xC0 and req == 0xB3:
            if s.mode == 'app':
                return b'none'
            raise USBError('Pipe error')
        if bm == 0x21 and req == 0:
            if s.mode == 'app':
                s.mode, s.state = 'dfu', 2
                if s.detach_raises:
                    raise USBError('No such device (it may have been disconnected)')
            return 0
        if bm == 0x21 and req == 4:
            s.state = 2
            return 0
        if bm == 0x21 and req == 1:
            if data:
                s.block, s.state = value, 3
                s.pending = s.busy_polls if s.busy_every and value % s.busy_every == 0 else 0
            else:
                s.state = 6
            return len(data)
        if bm == 0xA1 and req == 3:
            if s.state == 3:
                if s.error_at_block == s.block:
                    s.state = 10
                    return st(3, 0, 10)
                if s.pending > 0:
                    s.pending -= 1
                    return st(0, s.busy_poll_ms, 4)
                s.state = 5
                return st(0, 0, 5)
            if s.state == 6:
                s.state = 'M'
                return st(0, 0, 6)
            if s.state == 'M':
                item = s.manifest.pop(0) if len(s.manifest) > 1 else s.manifest[0]
                if item[0] == 'leave':
                    s.gone = True
                    raise USBError('No such device')
                return st(0, item[1], item[0])
            if s.state == 10:
                return st(0x0E, 0, 10)
            return st(0, 0, s.state)
        raise USBError('Pipe error')


sim = Sim(SCEN)


class Dev:
    idVendor = 0x17cc
    bcdDevice = 0x0200

    @property
    def idProduct(self):
        return 0x1860 if sim.mode == 'app' else 0x1862

    def ctrl_transfer(self, bm, req, value=0, index=0, data_or_len=None, timeout=None):
        return sim.ctrl(bm, req, value, index, data_or_len, timeout)


usb = types.ModuleType('usb')
core = types.ModuleType('usb.core')
util = types.ModuleType('usb.util')
backend = types.ModuleType('usb.backend')
libusb1 = types.ModuleType('usb.backend.libusb1')
core.USBError = USBError
core.find = lambda find_all=False, idVendor=None, backend=None: [] if sim.gone else [Dev()]
util.dispose_resources = lambda d: trace.append(['dispose'])
libusb1.get_backend = lambda find_library=None: object()
usb.core, usb.util, usb.backend, backend.libusb1 = core, util, backend, libusb1
sys.modules.update({'usb': usb, 'usb.core': core, 'usb.util': util,
                    'usb.backend': backend, 'usb.backend.libusb1': libusb1})
time.sleep = lambda sec: trace.append(['sleep', round(sec * 1000)])

sys.argv = [MKFLASH, SCEN['cmd']] + ([SCEN['image']] if SCEN['cmd'] == 'flash' else [])
real_stdout, sys.stdout = sys.stdout, io.StringIO()
try:
    runpy.run_path(MKFLASH, run_name='__main__')
except SystemExit as e:
    trace.append(['exit', str(e.code)])
finally:
    sys.stdout = real_stdout
json.dump(trace, sys.stdout)
