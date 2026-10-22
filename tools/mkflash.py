"""DFU flasher for keyboards with NI's bootloader; mirrors NI KKMUpdater 3.17.0 (standard DFU 1.1)."""
import sys, time, struct, argparse
import usb.core, usb.util, usb.backend.libusb1 as L

BE = L.get_backend(find_library=lambda x: '/opt/homebrew/lib/libusb-1.0.dylib')
VID = 0x17cc
DFU_DETACH, DFU_DNLOAD, DFU_GETSTATUS, DFU_CLRSTATUS, DFU_GETSTATE, DFU_ABORT = 0, 1, 3, 4, 5, 6
STATES = {0: 'appIDLE', 1: 'appDETACH', 2: 'dfuIDLE', 3: 'dfuDNLOAD-SYNC', 4: 'dfuDNBUSY', 5: 'dfuDNLOAD-IDLE',
          6: 'dfuMANIFEST-SYNC', 7: 'dfuMANIFEST', 8: 'dfuMANIFEST-WAIT-RESET', 9: 'dfuUPLOAD-IDLE', 10: 'dfuERROR'}


def find():
    return list(usb.core.find(find_all=True, idVendor=VID, backend=BE))


def dfu_info(d):
    raw = bytes(d.ctrl_transfer(0x80, 6, 0x0200, 0, 1024))
    i, cur, info = 0, None, None
    while i < len(raw):
        l, t = raw[i], raw[i + 1]
        if t == 4:
            cur = raw[i + 2:i + 8]  # ifnum, alt, neps, class, subclass, proto
        if t == 0x21 and l >= 7 and cur is not None and cur[3] == 0xfe and cur[4] == 1:
            info = dict(iface=cur[0], proto=cur[5], attrs=raw[i + 2],
                        timeout=raw[i + 3] | raw[i + 4] << 8, xfer=raw[i + 5] | raw[i + 6] << 8)
        i += l
    return info


def status(d, iface):
    s = bytes(d.ctrl_transfer(0xA1, DFU_GETSTATUS, 0, iface, 6, timeout=5000))
    return s[0], s[1] | s[2] << 8 | s[3] << 16, s[4]


def flash_type(d):
    return bytes(d.ctrl_transfer(0xC0, 0xB3, 0, 1, 4, timeout=1000))


def describe(d):
    info = dfu_info(d)
    print(f'  {d.idVendor:04x}:{d.idProduct:04x} bcdDevice={d.bcdDevice:#06x} dfu={info}')
    return info


def cmd_enter(a):
    devs = find()
    assert len(devs) == 1, f'expected 1 NI device, found {len(devs)}'
    d = devs[0]
    info = describe(d)
    if info['proto'] == 2:
        print('already in DFU mode'); return
    print('flash type:', flash_type(d))
    print('sending DFU_DETACH wValue=%d wIndex=%d' % (info['timeout'], info['iface']))
    try:
        d.ctrl_transfer(0x21, DFU_DETACH, info['timeout'], info['iface'], None, timeout=5000)
    except usb.core.USBError as e:
        print('  detach returned', e, '(device may have dropped off the bus, expected)')
    usb.util.dispose_resources(d)
    for _ in range(20):
        time.sleep(0.5)
        devs = find()
        if devs:
            dd = devs[0]
            try:
                inf = dfu_info(dd)
            except usb.core.USBError:
                continue
            if inf and inf['proto'] == 2:
                print('DFU mode device:'); describe(dd)
                try:
                    print('  state:', STATES.get(status(dd, inf['iface'])[2]))
                except usb.core.USBError as e:
                    print('  getstatus error', e)
                try:
                    print('  flash type (DFU mode):', flash_type(dd))
                except usb.core.USBError as e:
                    print('  flash type error', e)
                return
    print('no DFU-mode device found; current devices:'); [describe(x) for x in find()]


def cmd_flash(a):
    img = open(a.image, 'rb').read()
    for _ in range(50):                    # the bootloader may still be enumerating
        devs = find()
        if len(devs) == 1:
            break
        time.sleep(0.1)
    assert len(devs) == 1, f'expected 1 keyboard, found {len(devs)}'
    d = devs[0]; info = describe(d)
    assert info['proto'] == 2, 'device not in DFU mode'
    # the bootloader stalls the 0xB3 flash-type request; it is checked in app mode by `enter`
    st, _, state = status(d, info['iface'])
    if state == 10:
        d.ctrl_transfer(0x21, DFU_CLRSTATUS, 0, info['iface'], None)
    xfer = info['xfer']
    blocks = [img[i:i + xfer] for i in range(0, len(img), xfer)]
    print(f'writing {len(img)} bytes in {len(blocks)} blocks of {xfer}')
    t0 = time.time()
    for n, blk in enumerate(blocks + [b'']):
        d.ctrl_transfer(0x21, DFU_DNLOAD, n, info['iface'], blk if blk else None, timeout=5000)
        while True:
            st, poll, state = status(d, info['iface'])
            if st != 0:
                raise SystemExit(f'block {n}: status {st} state {STATES.get(state)}')
            if state in (5, 2) or (not blk and state in (6, 7, 8)):
                break
            time.sleep(max(poll, 1) / 1000)
        if n % 100 == 0 or not blk:
            print(f'  block {n}/{len(blocks)} state={STATES.get(state)} {time.time() - t0:.1f}s', flush=True)
    print('manifest phase; state', STATES.get(state))
    for _ in range(20):
        try:
            st, poll, state = status(d, info['iface'])
            print('  status', st, STATES.get(state))
            if state in (2, 8, 0):
                break
            time.sleep(max(poll, 50) / 1000)
        except usb.core.USBError as e:
            print('  device left bus (expected after manifest):', e); break
    if state == 2:
        print('sending DFU_DETACH to return to app')
        try:
            d.ctrl_transfer(0x21, DFU_DETACH, info['timeout'], info['iface'], None, timeout=5000)
        except usb.core.USBError as e:
            print('  ', e)
    usb.util.dispose_resources(d)
    time.sleep(5)
    print('devices now:'); [describe(x) for x in find()]


p = argparse.ArgumentParser()
sp = p.add_subparsers(dest='cmd', required=True)
sp.add_parser('info').set_defaults(f=lambda a: [describe(x) for x in find()])
sp.add_parser('enter').set_defaults(f=cmd_enter)
f = sp.add_parser('flash'); f.add_argument('image'); f.add_argument('--expect-type', default='none'); f.set_defaults(f=cmd_flash)
a = p.parse_args(); a.f(a)
