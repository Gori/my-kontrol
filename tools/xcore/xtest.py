#!/usr/bin/env python3
"""Tests for the XS1 toolchain (xas, xld, ximage, xsim). No hardware, nothing is flashed.

usage: [OPT=-O2] xtest.py [roundtrip|core|image|sim ...]     (default: all; OPT default -Os)

roundtrip  disassemble every segment of NI's S49 image with xdis, reassemble with xas, compare
           byte for byte (needs fw/kks/s49_xmos_120.bin; skipped when absent, never committed)
core       compile firmware/core/*.c with clang --target=xcore, assemble, link into one tile
           image with the platform/xs1 runtime (missing board/hal functions stubbed), print sizes
image      build the test program for both tiles, make an upgrade image, validate it with
           ximage --check
sim        run the test program and the core smartplay/host tests in xsim
s49        the S49 bring-up harness (s49run.py) on test/s49_tile0.c + s49_tile1.c, and on
           tile 0 alone with the HostLink stub: LCD text, DIN out, DIN/USB MIDI in echoed
fw         our S49 firmware (make BOARD=s49 DEV=1) on the harness with both tiles: displays,
           a knob's CC on DIN OUT, the menu
standalone (~2 min) the S49 firmware as a standalone instrument on the harness: 5-pin MIDI,
           displays, Smart Play and the light guide, settings in the SPI flash across a power
           cycle, the watchdog, safe mode after 3 unstable starts, the update-mode combo
usb        (~4 min) tile 1's USB above the wire with a host model in place of the SIE thread:
           enumeration, MIDI, update mode, a DFU download and upload, the factory combo
hybrid     (~3 min, needs NI's image locally; skipped without it) our tile 0 next to NI's
           tile 1 on the board model: start-up, the probe in and out through NI's 5-pin MIDI,
           the update-mode start
emu        (not in the default set, ~1 min) run NI's S49 image on the s49emu board model: LCD
           init sequence, standalone display text, a key press giving a note on DIN MIDI OUT
"""
import glob, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import xdis, xas, xcc

NI = os.path.join(ROOT, 'fw', 'kks', 's49_xmos_120.bin')
FW = os.path.join(ROOT, 'firmware')
CLANG = os.environ.get('CLANG', '/opt/homebrew/opt/llvm@22/bin/clang')
CFLAGS = ['--target=xcore', os.environ.get('OPT', '-Os'), '-ffreestanding', '-fno-builtin', '-std=gnu11',
          '-ffunction-sections', '-fdata-sections', '-Wall', '-Wno-unused-function']


def disasm_text(seg, base, isa):
    """xas source for a raw blob: every decoded instruction with labels for pc-relative targets.
    Undecodable halfwords, targets that are not an instruction start, and encodings xas would
    not choose for that text become .short. Returns (text, counts)."""
    insns = xdis.disasm(seg, base)
    starts = {i.addr for i in insns}
    label = {t: 'L_%x' % t for i in insns for t in i.targets() if t in starts}
    n = {'insns': 0, 'raw': 0, 'forced long': 0}
    lines = []
    orig = lambda e, mode: int(e[1][2:], 16)            # label -> its address in the original
    for i in insns:
        if i.addr in label:
            lines.append('%s:' % label[i.addr])
        raw = '\t.short ' + ', '.join('0x%04x' % (i.word >> s & 0xffff) for s in range(0, 8 * i.size, 16))
        if not i.op or any(t not in label for t in i.targets()):
            lines.append(raw)
            n['raw'] += 1
            continue
        vals = []
        for k, v in zip(i.op.kinds, i.ops):
            vals.append(xdis.REGS[v] if k in 'rR' else label[i.targets()[0]] if k in 'FB'
                        else str(xdis.BITP[v] if k == 'b' else v))
        t = i.op.syntax.format(*vals)
        mn, _, ops = t.partition(' ')
        for suffix in ('', '.l'):
            try:
                cands = isa.parse(mn + suffix, ops)
            except xas.AsmError:
                continue
            for op, v in cands:
                f = xas.fit(op, v, i.addr, orig)
                if f is not False:
                    break
            if xas.encode(op, f) == i.word.to_bytes(i.size, 'little'):
                lines.append('\t%s%s %s' % (mn, suffix, ops))
                n['insns'] += 1
                n['forced long'] += suffix == '.l'
                break
        else:
            lines.append(raw)
            n['raw'] += 1
    return '\n'.join(lines) + '\n', n


def test_roundtrip():
    if not os.path.exists(NI):
        print('roundtrip: skipped (%s not present)' % os.path.relpath(NI, ROOT))
        return True
    isa = xas.ISA('xs1')
    ok = True
    for k, (off, flags, seg) in enumerate(xdis.flash_segments(open(NI, 'rb').read())):
        payload = seg[:-4]
        text, n = disasm_text(payload, 0x10000, isa)
        out = xas.link_flat(xas.Assembler(isa).source(text, 'seg%d' % k).obj(), 0x10000, isa)
        same = out == payload
        ok &= same
        diff = next((j for j in range(min(len(out), len(payload))) if out[j] != payload[j]), None)
        print('roundtrip: segment %d: %d bytes, %d instructions (%d forced long), %d raw .short: %s'
              % (k, len(payload), n['insns'], n['forced long'], n['raw'],
                 'byte-identical' if same else 'DIFFERENT (%d bytes, first at 0x%x)'
                 % (len(out), 0x10000 + (diff if diff is not None else len(out)))))
    return ok


BUILD = os.path.join(HERE, 'build')
XS1 = os.path.join(FW, 'platform', 'xs1')
TEST = os.path.join(HERE, 'test')


def cc(src, tag='', inc=()):
    """clang --target=xcore -S; returns the .s path under BUILD."""
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, tag + os.path.splitext(os.path.basename(src))[0] + '.s')
    r = xcc.compile_asm(src, CFLAGS[1:] + ['-I' + i for i in inc], out)   # (xcc adds --target)
    if r:
        raise subprocess.CalledProcessError(r, [CLANG, src])
    return out


def link(name, files, **kw):
    import xld
    r = xld.link(files, **kw)
    out = os.path.join(BUILD, name)
    open(out + '.bin', 'wb').write(r.image)
    with open(out + '.map', 'w') as f:
        r.write_map(f)
    return r, out


def check_link(r, sources):
    """Independent check of a linked image: decode every instruction of every function with
    xdis and compare it with clang's assembly line by line (mnemonic, registers, and the address
    or value each symbolic operand resolves to). Returns the number of instructions checked."""
    import re
    isa = xas.ISA()
    cp, dp = r.lookup(None, '_cp'), r.lookup(None, '_dp')
    tab = xdis.table('xs1')
    img, base = r.image, r.base
    checked = 0
    jt_size = {p.offs[i]: it.size for p in r.placed for i, it in enumerate(p.items) if it.t == 'j'}
    for path in sources:
        ctx = [o.path for o in r.objs].index(path)

        def sym(n):
            try:
                return r.lookup(ctx, n)
            except xas.AsmError:
                return None             # in a block dropped by gc
        funcs, cur = [], None
        for line in open(path):
            s = xas.strip_comment(line).strip()
            m = re.match(r'\.cc_top\s+([\w.$]+)\.function,', s)
            if m:
                cur = [m.group(1), []]
                funcs.append(cur)
            elif s.startswith('.cc_bottom'):
                cur = None
            elif cur is not None and s:
                cur[1].append(s)
        for name, body in funcs:
            addr, local, pairs = sym(name), {}, []
            if addr is None:
                continue
            for s in body:
                if s.endswith(':'):
                    local[s[:-1]] = addr
                    continue
                if s.startswith('.jmptable'):
                    labs = xas.split_args(s.split(None, 1)[1])
                    size = 4 if s.startswith('.jmptable32') else 2
                    ents = [xdis.decode(img, addr - base + size * k, addr + size * k, tab)
                            for k in range(len(labs))]
                    hits = [e.targets()[0] for e in ents]
                    end = addr + jt_size[addr]                     # incl. trampolines
                    for lab, t in zip(labs, hits):
                        if addr + size * len(labs) <= t < end:
                            t = xdis.decode(img, t - base, t, tab).targets()[0]
                        pairs.append(('jt', lab, t))
                    addr = end
                    continue
                if s.startswith('.'):
                    continue
                i = xdis.decode(img, addr - base, addr, tab)
                pairs.append(('i', s, i))
                addr += i.size
            val = lambda e: xas.evaluate(e, lambda n: local[n] if n in local else sym(n))
            for kind, s, i in pairs:
                if kind == 'jt':
                    assert val(xas.parse_expr(s)) == i, '%s: jump table entry %s' % (name, s)
                    continue
                mn, _, ops = s.partition(' ')
                cands = isa.parse(mn, ops)
                assert i.op and any(op.name == i.op.name for op, _ in cands), \
                    '%s: %s decoded as %s' % (name, s, i.text())
                vals = next(v for op, v in cands if op.name == i.op.name)
                for k, v, got in zip(i.op.kinds, vals, i.ops):
                    if type(v) is int:
                        assert v == got, '%s: %s operand %d != %d' % (name, s, v, got)
                    elif v[2] == 'pc':
                        assert val(v[1]) == i.targets()[0], '%s: %s target' % (name, s)
                    else:
                        want = val(v[1])
                        if v[2] in ('cp', 'dp'):
                            want = (want - (cp if v[2] == 'cp' else dp)) // 4
                        assert want == got, '%s: %s value %d != %d' % (name, s, want, got)
                checked += 1
    return checked


def core_sources():
    inc = [os.path.join(FW, d) for d in ('core', 'usb', 'boards/m32')]
    srcs = sorted(glob.glob(os.path.join(FW, 'core', '*.c')))
    return [cc(f, '', inc) for f in srcs]


def runtime():
    return [os.path.join(XS1, 'crt0.s'), cc(os.path.join(XS1, 'rt.c'))]


def test_core():
    """firmware/core/*.c + platform/xs1 into one tile image (board/hal stubbed)."""
    core = core_sources()
    inc = [os.path.join(FW, d) for d in ('core', 'usb', 'boards/m32')]
    rt = runtime() + [cc(os.path.join(XS1, 'platform.c'), '', inc)]
    r, out = link('core', rt + core, stub=True)
    n = check_link(r, core + rt[1:])
    a = {k: e - s for k, s, e in r.areas()}
    print('core: %d files; %s; %d stubs; %d instructions cross-checked against clang output'
          % (len(core), r.summary(), len(r.stubs), n))
    print('core: sizes per object (code + cp + dp + bss bytes kept after gc):')
    per = {}
    for p in r.placed:
        if p.ctx is not None:
            o = r.objs[p.ctx].name
            per.setdefault(o, {k: 0 for k in a})[p.kind] += p.offs[-1] - p.addr
    for o, v in sorted(per.items(), key=lambda x: -sum(x[1].values())):
        print('  %-20s code %6d  cp %6d  dp %4d  bss %5d' % (o, v['code'], v['cp'], v['dp'], v['bss']))
    return True


def prog_files():
    inc = [os.path.join(TEST, 'include')]
    return runtime() + [os.path.join(TEST, 'simrt.s'), cc(os.path.join(TEST, 'simlib.c'), '', inc),
                        cc(os.path.join(TEST, 'prog.c'), '', inc)]


def test_image():
    import ximage
    files = prog_files()
    r0, out0 = link('prog_tile0', files)
    r1, out1 = link('prog_tile1', files)
    check_link(r0, files[1:])
    img = ximage.build([r0.image, r1.image])
    path = os.path.join(BUILD, 'prog_upgrade.bin')
    open(path, 'wb').write(img)
    print('image: test program %s; upgrade image %d bytes' % (r0.summary(), len(img)))
    ok = ximage.check(img)
    return ok


def run_sim(name, files):
    import xsim
    r, out = link(name, files)
    sim = xsim.Sim(r.image, syms={n: a for a, n, o in r.symbols()})
    old, sys.stdout = sys.stdout, open(os.devnull, 'w')
    try:
        why = sim.run(200_000_000)
    finally:
        sys.stdout = old
    return why, ''.join(sim.out), sim


def host_output(name, srcs, inc):
    exe = os.path.join(BUILD, name + '.host')
    subprocess.run(['cc', '-std=gnu11', '-w'] + ['-I' + i for i in inc] + srcs + ['-o', exe], check=True)
    return subprocess.run([exe], capture_output=True, text=True).stdout


def test_sim():
    ok = True
    inc = [os.path.join(TEST, 'include')]
    t = os.path.join(FW, 'test')
    tinc = [os.path.join(t, 'host'), os.path.join(FW, 'core'), os.path.join(FW, 'usb')]
    core = lambda *n: [os.path.join(FW, 'core', x + '.c') for x in n]
    drv = lambda *n: [os.path.join(FW, 'drivers', x + '.c') for x in n]
    progs = [('prog', [os.path.join(TEST, 'prog.c')], []),
             ('test_smartplay', [os.path.join(t, 'test_smartplay.c')] + core('smartplay'), tinc),
             ('test_core', [os.path.join(t, 'test_core.c'), os.path.join(t, 'host', 'fake_hal.c')]
              + core('midi', 'play', 'smartplay', 'settings', 'settings_store'), tinc),
             ('test_host', [os.path.join(t, 'test_host.c'), os.path.join(t, 'host', 'fake_hal.c')]
              + core('host', 'mcu', 'midi'), tinc),
             ('test_seg', [os.path.join(t, 'test_seg.c')] + drv('font16', 'st7565_seg')
              + [os.path.join(FW, 'ui', 'seg9.c')], tinc + [os.path.join(FW, 'drivers')]),
             ('test_panel', [os.path.join(t, 'test_panel.c')] + drv('shift595', 'ni_keybed', 'ni_sensor')
              + core('lightguide'), tinc + [os.path.join(FW, 'drivers')])]
    rt = runtime() + [os.path.join(TEST, 'simrt.s'), cc(os.path.join(TEST, 'simlib.c'), '', inc)]
    for name, srcs, sinc in progs:
        files = rt + [cc(f, name + '_', inc + sinc) for f in srcs]
        why, out, sim = run_sim(name, files)
        want = host_output(name, srcs, sinc)
        same = out == want
        ok &= same and why.startswith('exit')
        lines = out.count('\n')
        print('sim: %-15s %s, %d instructions, %d bytes stack, %d output lines, %s' % (
            name, why, sim.steps, sim.base + sim.size - 8 - sim.min_sp, lines,
            'identical to the native build' if same else 'DIFFERS from the native build'))
        if not same:
            import difflib
            sys.stdout.writelines(list(difflib.unified_diff(want.splitlines(1), out.splitlines(1),
                                                            'native', 'xsim'))[:20])
    return ok


def s49_images():
    """Build the two S49 harness test images (test/s49_tile0.c, test/s49_tile1.c)."""
    inc = [TEST]
    rt = runtime()
    out = []
    for n in ('s49_tile0', 's49_tile1'):
        r, path = link(n, rt + [cc(os.path.join(TEST, n + '.c'), '', inc)])
        out.append(path)
    return out


def test_s49():
    """The bring-up harness on the test images: 2 tiles, then tile 0 with the HostLink stub."""
    import s49run
    t0, t1 = s49_images()
    ok = True
    for tiles in ((t0, t1), (t0,)):
        h = s49run.Harness(*[open(p + '.bin', 'rb').read() for p in tiles])
        out = h.command('tick 5')
        want = ['din-out B0 7B 00']
        if len(tiles) == 1:             # the test tile 1 has no UART receiver: stub only
            out += h.command('din-in 90 3C 40') + h.command('in 0 B0 01 7F') + h.command('tick 5')
            want += ['din-out 90 3C 40', 'out 0 B0 01 7F']
        out += h.command('tick 2')
        scr = h.command('screen')
        leds = [x for x in h.command('leds')[1:-1] if not x.startswith('backlight')]
        want_leds = ['guide 0 127 0 0', 'led 3 127']
        disp = [x.split('|')[1:3] for x in scr[1:-1]]
        good = out == want and disp[0] == ['MY      ', 'KONTROL '] and disp[8] == ['        ', 'XS1 S49 '] \
            and all(d == [' ' * 8] * 2 for d in disp[1:8]) and leds == want_leds
        ok &= good
        print('s49: %s: MIDI %s; display 1 %r, display 9 %r, others blank; lights %s: %s' % (
            '2 tiles' if len(tiles) == 2 else 'tile 0 + HostLink stub', out, disp[0], disp[8],
            leds, 'ok' if good else 'DIFFERENT'))
    return ok


STACK_MARGIN = 512                      # bytes every stack must keep free (an overflow on XS1
                                        # corrupts memory without a fault)


def host_corpus():
    """Host-protocol messages for the firmware: every one test_host sends (HOST_CORPUS), then
    the longest of a few kinds (a 64-character message in four parts, DESCRIBE)."""
    test = os.path.join(FW, 'test')
    subprocess.run(['make', '-C', test, 'build/test_host'], capture_output=True, check=True)
    path = os.path.join(tempfile.mkdtemp(), 'corpus.txt')
    subprocess.run([os.path.join(test, 'build', 'test_host')], env=dict(os.environ, HOST_CORPUS=path),
                   capture_output=True, check=True)
    msgs = [bytes.fromhex(x) for x in open(path).read().split()]
    head = bytes([0xF0, 0x7D, 0x4D, 0x4B])
    msgs.append(head + bytes([0x00, 2, 0, 0x7F, 7]) + b'Any DAW' + bytes([3]) + b'1.0' + b'\xf7')
    for part in range(4):
        msgs.append(head + bytes([0x28, 1, part, 1 if part == 3 else 0, 64]) + b'W' * 64 + b'\xf7')
    msgs.append(head + bytes([0x07, 0xF7]))
    return msgs


def stack_check(name, *harnesses):
    """Print every stack's peak use over the harnesses' runs; the ones under STACK_MARGIN."""
    peaks = {}
    for h in harnesses:
        h.note_stacks()
        for k, (avail, used) in h.peaks.items():
            peaks[k] = (avail, max(used, peaks.get(k, (avail, 0))[1]))
    bad = ['%s %s: %d of %d bytes used, %d free' % (t, n, u, a, a - u)
           for (t, n), (a, u) in sorted(peaks.items()) if a - u < STACK_MARGIN]
    print('%s: stack peaks %s' % (name, ', '.join('%s/%s %d of %d' % (t[-1], n.split(':')[-1], u, a)
                                                 for (t, n), (a, u) in sorted(peaks.items()))))
    for x in bad:
        print('%s: FAILED stack margin under %d bytes: %s' % (name, STACK_MARGIN, x))
    return not bad


def test_fw():
    """Our S49 firmware (make BOARD=s49 DEV=1) on the harness with both of its tiles: splash,
    home screen, the CC page, a knob's CC on the 5-pin MIDI OUT, the menu; then with the
    HostLink stub (USB connected): a key on DIN and USB, a knob on the sensor, Scale on in the
    menu lighting the light guide."""
    import s49run
    r = subprocess.run(['make', '-C', FW, 'BOARD=s49', 'DEV=1', '-j8'], capture_output=True, text=True)
    if r.returncode:
        print('fw: FAILED to build\n' + r.stdout[-2000:] + r.stderr[-2000:])
        return False
    b = os.path.join(FW, 'build', 's49-dev')
    t0, t1 = (open(os.path.join(b, n), 'rb').read() for n in ('tile0.bin', 'tile1.bin'))
    h = s49run.Harness(t0, t1, os.path.join(b, 'tile0.map'), os.path.join(b, 'tile1.map'))
    shot = lambda: [x.split('|')[1:3] for x in h.command('screen')[1:-1]]
    push = ['event 1 25 1', 'tick 20', 'event 1 25 0', 'tick 40']
    steps = [('tick 100', None), ('screen', lambda d: d[0] == ['MY      ', 'KONTROL ']),
             ('tick 1300', None), ('screen', lambda d: d[0][0] == 'CH1     ' and d[1][1] == 'KONTROL '),
             ('turn 1', None), ('tick 30', None),
             ('screen', lambda d: d[0] == ['KNOBS   ', 'PAGE 1  '] and d[1] == ['CC14    ', '       0']),
             ('event 3 0 40', None), ('tick 30', None)] + [(c, None) for c in push] + \
            [('tick 2000', None), ('screen', lambda d: d[0][0] == 'MENU    ' and d[1][0].startswith('>'))]
    ok, din = True, []
    for cmd, check in steps:
        if check is None:
            din += [x for x in h.command(cmd) if x.startswith('din-out')]
        else:
            d = shot()
            if not check(d):
                ok = False
                print('fw: after %r the displays were %s' % (cmd, d))
    ok &= din == ['din-out B0 0E 28']
    h_both = h
    # tile 0 alone, USB connected through the stub
    h = s49run.Harness(t0, map0=os.path.join(b, 'tile0.map'))
    out = []
    for c in ['tick 1400', 'usb 1', 'tick 20', 'key 60 100', 'tick 10', 'keyoff 60', 'tick 10',
              'knob 0 640', 'tick 30']:
        out += [x for x in h.command(c) if not x.startswith('out 1')]
    want = ['din-out 90 3C 50', 'out 0 90 3C 50', 'din-out 80 3C 00', 'out 0 80 3C 00',
            'din-out B0 0E 0A', 'out 0 B0 0E 0A']
    ok &= out == want
    for c in ['tick 2000'] + push * 3 + ['event 2 0 1', 'tick 30'] + push + ['event 1 44 1', 'tick 20',
              'event 1 44 0', 'tick 50']:
        h.command(c)
    guide = {int(x.split()[1]): tuple(map(int, x.split()[2:])) for x in h.command('leds') if x.startswith('guide')}
    c_keys = [k for k, v in guide.items() if v[2] > 100]
    scale = sorted(k for k, v in guide.items() if v == (24, 24, 24))
    ok &= c_keys == [0, 12, 24, 36, 48] and scale[:6] == [2, 4, 5, 7, 9, 11]
    # the host protocol on the DAW cable: test_host's messages and the longest ones
    corpus = host_corpus()
    for m in corpus:
        h.b.link.usb_sysex(1, m)
        h.command('tick 5')
    answers = [p for _, p in h.b.link.usb_tx if p[0] >> 4 == 1]
    ok &= len(answers) > 20
    ok &= stack_check('fw', h_both, h)
    print('fw: host protocol: %d messages in, %d packets back on the DAW cable' % (len(corpus), len(answers)))
    print('fw: S49 firmware (tile 0 %d B, tile 1 %d B) in the harness: splash, home, CC page, '
          'menu %s; knob 1 -> %s; stub with USB: %s; light guide in C major: roots %s, scale %s...'
          % (len(t0), len(t1), 'ok' if ok else 'DIFFERENT', din, out, c_keys, scale[:7]))
    return ok


def test_standalone():
    """The S49 firmware as a standalone instrument, both tiles (tile 1 with its SPI flash
    driver on the emulated chip), no USB: boot; a key and a knob on the 5-pin MIDI OUT and the
    displays; Scale, Arp and Clock Out on in the menu, the light guide in C major, the arp on
    a held key; Play / Stop as MIDI clock start / stop; the settings in flash surviving a
    power cycle; the watchdog rebooting a hung main loop; 3 starts that never got stable
    giving safe mode, then a normal start; both page buttons at power-up giving update mode.
    Buttons whose input bits are unknown (encoder push, Back, Play, Stop) come as DEV events."""
    import s49run
    r = subprocess.run(['make', '-C', FW, 'BOARD=s49', 'DEV=1', '-j8'], capture_output=True, text=True)
    if r.returncode:
        print('standalone: FAILED to build\n' + r.stdout[-2000:] + r.stderr[-2000:])
        return False
    b = os.path.join(FW, 'build', 's49-dev')
    t0, t1 = (open(os.path.join(b, n), 'rb').read() for n in ('tile0.bin', 'tile1.bin'))
    h = s49run.Harness(t0, t1, os.path.join(b, 'tile0.map'), os.path.join(b, 'tile1.map'))
    fails, n = [], [0]

    def run(*cmds):
        out = []
        for c in cmds:
            out += h.command(c)
        return out

    def din(out, clock=False):
        return ' '.join(x[8:] for x in out if x.startswith('din-out')).replace(' F8', '' if not clock else ' F8')

    def check(what, cond, got):
        n[0] += 1
        if not cond:
            fails.append('%s: %s' % (what, got))

    shot = lambda: [x.split('|')[1:3] for x in h.command('screen')[1:-1]]
    guide = lambda: {int(x.split()[1]): tuple(map(int, x.split()[2:])) for x in h.command('leds')
                     if x.startswith('guide')}

    def c_major(g):
        roots = sorted(k for k, v in g.items() if v[2] > 100)
        scale = sorted(k for k, v in g.items() if v[0] > 15 and v[0] == v[1] == v[2])
        return roots == [0, 12, 24, 36, 48] and scale[:6] == [2, 4, 5, 7, 9, 11]

    btn = lambda ctl: ['event 1 %d 1' % ctl, 'tick 20', 'event 1 %d 0' % ctl, 'tick 40']
    push, back, enc = btn(25), btn(44), ['event 2 0 1', 'tick 30']
    run('tick 1400')
    d = shot()
    check('home', d[0][0] == 'CH1     ' and d[1][1] == 'KONTROL ', d[:2])
    o = din(run('key 60 100', 'tick 10', 'keyoff 60', 'tick 10', 'knob 0 640', 'tick 30'))
    check('key and knob on 5-pin', o == '90 3C 50 80 3C 00 B0 0E 0A', o)
    d = shot()
    check('knob display', d[0] == ['KNOB 1  ', '10      '], d[0])
    run(*push * 3, *enc, *push, *back, *back)                  # Scale & Chord > Scale: On
    run(*push, *enc, *push, *push, *enc * 11, *push, *back, *back, 'tick 200')   # Arp, Clock Out
    d = shot()
    check('home with scale and arp', d[0][1] == 'C MAJOR ' and d[1][1] == 'ARP 1/16', d[:3])
    o = din(run('key 60 100', 'tick 600'))
    check('arp on a held C', o.count('90 3C 50') >= 4, o)
    g = guide()
    check('light guide C major', c_major(g), sorted(g.items())[:8])
    run('keyoff 60', 'tick 100')
    for top in range(6):                    # every menu, every item scrolled past
        run(*push, *enc * top, *push, *enc * 24, *back, *back, *back)
    run('tick 50')
    for msg in ('F0 7D 4D 4B 00 02 00 7F 03 41 42 43 01 31 F7', 'F0 43 10 4C ' + '11 ' * 300 + 'F7'):
        run('din-in ' + msg, 'tick 120')    # SysEx into the 5-pin parser
    d = shot()
    check('home after the menu walk', d[0][1] == 'C MAJOR ', d[:2])
    o = din(run(*btn(9), 'tick 50', *btn(11)))
    check('Play / Stop -> clock start / stop', o == 'FA FC', o)
    run('tick 3000')                                           # the settings get saved
    flash = h.b.flash
    check('settings in flash at 0xF2000', flash.mem[0xF2000:0xF2004] != b'\xff' * 4, flash.mem[0xF2000:0xF2004])
    run('power', 'tick 1500')
    d = shot()
    check('settings after a power cycle', d[0][1] == 'C MAJOR ' and d[1][1] == 'ARP 1/16', d[:3])
    check('light guide after a power cycle', c_major(guide()), '')
    o = din(run(*btn(9)))
    check('Clock Out after a power cycle', o == 'FA', o)
    t = h.b.ms()
    o = run('hang', 'tick 2600')
    reset = [float(x.split()[2]) for x in o if x.startswith('reset at')]
    check('watchdog reboot 2..2.5 s after a hang', len(reset) == 1 and 2000 <= reset[0] - t <= 2500, o)
    run('tick 300')
    log = bytes(h.b.flash.mem[0xF4000:0xF4005])   # the first run passed 10 s: STABLE
    check('boot log: stable, then 2 unstable starts', log == b'\xb0\x5a\xb0\xb0\xff', log.hex())
    run('power', 'tick 300')
    log = bytes(h.b.flash.mem[0xF4000:0xF4006])
    check('boot log: a 3rd unstable start', log == b'\xb0\x5a\xb0\xb0\xb0\xff', log.hex())
    run('power', 'tick 300')
    d = shot()
    check('safe mode after 3 unstable starts', d[0] == ['SAFE    ', 'MODE    '], d[0])
    run('power', 'tick 1400')
    d = shot()
    check('normal start after safe mode', d[0][0] == 'CH1     ', d[0])
    run('btn-down 28', 'btn-down 29', 'power', 'tick 300')
    d = shot()
    check('both page buttons at power-up: update mode', d[0] == ['UPDATE  ', 'MODE    '], d[0])
    fl = h.b.flash
    check('NI flash untouched, no write refused', fl.violations == 0 and fl.refused == 0,
          (fl.violations, fl.refused))
    stacks_ok = stack_check('standalone', h)
    for f in fails:
        print('standalone: FAILED ' + f)
    print('standalone: S49 firmware, both tiles, no USB: %d of %d checks passed (boot, 5-pin key/knob, '
          'scale + arp + light guide, clock start/stop, settings across a power cycle, watchdog, '
          'safe mode, update combo)' % (n[0] - len(fails), n[0]))
    return not fails and stacks_ok


def test_usb():
    """Tile 1's USB above the wire, on the harness with both tiles: s49emu.UsbHost stands in
    for the SIE thread (usb_ulpi.c) and the host, so the DCD, TinyUSB, our descriptors, the
    MIDI and DFU code run as built for tile 1. Enumeration; MIDI to the host from a key and
    from the host to the 5-pin OUT; RESETINTODFU into update mode (the RAM flag kept over the
    chip reset, tile 0 showing UPDATE MODE); the DFU device downloading our own image (built
    here with ximage) into the upgrade slot of an XMOS flash layout and reading it back;
    RESETFROMDFU back; both page buttons at power-up giving update mode, and held 10 s the
    factory image (the upgrade image removed) without USB."""
    import s49run, ximage, s49emu
    r = subprocess.run(['make', '-C', FW, 'BOARD=s49', 'DEV=1', '-j8'], capture_output=True, text=True)
    if r.returncode:
        print('usb: FAILED to build\n' + r.stdout[-2000:] + r.stderr[-2000:])
        return False
    b = os.path.join(FW, 'build', 's49-dev')
    t0, t1 = (open(os.path.join(b, n), 'rb').read() for n in ('tile0.bin', 'tile1.bin'))
    tmp = tempfile.mkdtemp()
    old, new = os.path.join(tmp, 'old.bin'), os.path.join(tmp, 'new.bin')
    old_img = ximage.build([bytes(0x6000), bytes(0x4000)], ident=1)       # "the installed image"
    new_img = ximage.build([t1, t0], ident=2)                                # ours (word 4: not NI's)
    open(old, 'wb').write(old_img)
    open(new, 'wb').write(new_img)
    h = s49run.Harness(t0, t1, os.path.join(b, 'tile0.map'), os.path.join(b, 'tile1.map'))
    fails, n = [], [0]

    def run(*cmds):
        out = []
        for c in cmds:
            out += h.command(c)
        return out

    def check(what, cond, got):
        n[0] += 1
        if not cond:
            fails.append('%s: %s' % (what, got))

    shot = lambda: [x.split('|')[1:3] for x in h.command('screen')[1:-1]]
    slot = run('flash-layout ' + old)
    check('slot', slot == ['flash-layout slot 16000-36000'], slot)
    o = run('tick 1300', 'usb-enum')
    check('enumeration', len(o) == 2 and o[0].startswith('usb-device 17CC:1350 bcd 0090 "My Kontrol S49" serial ')
          and o[1] == 'usb-config 208 bytes, interfaces 0:01/01/00 1:01/03/00 2:03/00/00 3:FE/01/01', o)
    o = run('tick 20', 'key 60 100', 'tick 20', 'keyoff 60', 'tick 20', 'usb-out 29903C40', 'tick 20')
    check('MIDI both ways', o == ['din-out 90 3C 50', 'usb-in 0 90 3C 50', 'din-out 80 3C 00', 'usb-in 0 80 3C 00',
                                  'din-out 90 3C 40'], o)
    corpus, back = host_corpus(), []
    for m in corpus:                        # the host protocol over USB, cable 1 (DAW)
        p = s49emu.sysex_packets(1, m)
        for i in range(0, len(p), 64):
            h.b.host.bulk_out(0x01, p[i:i + 64])
        back += [x for x in run('tick 4') if x.startswith('usb-in 1')]
    check('host protocol over USB: answers on the DAW cable', len(back) > 10, len(back))
    o = run('usb-ctl 21 F2 0 3 0', 'tick 100')
    check('RESETINTODFU reboots', o[0] == 'usb-ctl ok ' and any(x.startswith('reset at') for x in o), o)
    check('tile 0 in update mode', shot()[0] == ['UPDATE  ', 'MODE    '], shot()[0])
    o = run('usb-enum')
    check('DFU device', len(o) == 2 and o[0].startswith('usb-device 17CC:1352') and
          o[1] == 'usb-config 27 bytes, interfaces 0:FE/01/02', o)
    o = run('dfu-dnload ' + new)
    fl = h.b.flash
    check('download', o == ['dfu-dnload status 0 state 2'], o)
    check('image in the slot', bytes(fl.mem[0x16000:0x16000 + len(new_img)]) == new_img, '')
    o = run('dfu-upload ' + os.path.join(tmp, 'up.bin'))
    check('upload', open(os.path.join(tmp, 'up.bin'), 'rb').read() == new_img, o)
    o = run('usb-ctl 21 F3 0 0 0', 'tick 1300', 'usb-enum')
    check('RESETFROMDFU', any(x.startswith('reset at') for x in o) and any(x.startswith('usb-device 17CC:1350') for x in o), o)
    run('btn-down 28', 'btn-down 29', 'power', 'tick 300')
    d = shot()
    check('page buttons at power-up: countdown', d[0] == ['UPDATE  ', 'MODE    '] and d[1][0] == 'FACTORY ', d[:2])
    o = run('btn-up 28', 'btn-up 29', 'tick 300', 'usb-enum')
    check('released: update mode', any(x.startswith('reset at') for x in o) and any(x.startswith('usb-device 17CC:1352') for x in o), o)
    o = run('btn-down 28', 'btn-down 29', 'power', 'tick 11000')
    check('held 10 s: the upgrade image removed, reboot', any(x.startswith('reset at') for x in o) and
          bytes(h.b.flash.mem[0x16000:0x17000]) == b'\xff' * 4096, o)
    fl = h.b.flash
    check('nothing outside the slot changed, nothing refused', fl.violations == 0 and fl.refused == 0,
          (fl.violations, fl.refused))
    stacks_ok = stack_check('usb', h)
    for f in fails:
        print('usb: FAILED ' + f)
    print('usb: tile 1 USB above the wire (s49emu.UsbHost): %d of %d checks passed (enumeration, MIDI, '
          'update mode, DFU download/upload of a %d-byte image, factory combo)' % (n[0] - len(fails), n[0], len(new_img)))
    return not fails and stacks_ok


def test_hybrid():
    """The hybrid bring-up image on the board model: our tile 0 (make BOARD=s49 HYBRID=1
    DEV=1, platform/xs1/ni_link.c and the probe) next to NI's tile 1 (segment 1 of NI's
    image, local only). Normal start: tile 1 goes through its start-up with us and keeps
    running; a key reaches the 5-pin OUT through NI's tile 1; the probe's input-bits message
    comes out; a probe command sent into the 5-pin IN (CC 80 = 3 on channel 16) starts the
    LED walk (chain C bit 0 lit, reported back). Update mode (NI's RAM flag set): NI's tile 1
    threads end up waiting where they do next to NI's own tile 0. NI's USB itself is not
    modelled (its ULPI wire), so enumeration and DFU over USB are not run."""
    if not os.path.exists(NI):
        print('hybrid: skipped (%s not present)' % os.path.relpath(NI, ROOT))
        return True
    import s49emu, xsim
    r = subprocess.run(['make', '-C', FW, 'BOARD=s49', 'HYBRID=1', 'DEV=1', '-j8'], capture_output=True, text=True)
    if r.returncode:
        print('hybrid: FAILED to build\n' + r.stdout[-2000:] + r.stderr[-2000:])
        return False
    b0 = os.path.join(FW, 'build', 's49-hybrid-dev')
    ours = open(os.path.join(b0, 'tile0.bin'), 'rb').read()
    syms = xsim.read_map(os.path.join(b0, 'tile0.map'))
    ni = s49emu.load_image(NI)
    fails, n = [], [0]

    def check(what, cond, got):
        n[0] += 1
        if not cond:
            fails.append('%s: %s' % (what, got))

    b = s49emu.Board(ours, ni[1], ui_syms=syms)
    seen = [0]

    def din():
        b.din_out.flush(b.sys.now)
        d = ' '.join('%02X' % x for _, x, _ in b.din_out.bytes[seen[0]:])
        seen[0] = len(b.din_out.bytes)
        return d

    why = b.run_ms(3000)
    check('runs 3 s with NI\'s tile 1', why == 'time limit', why)
    check('tile 1 started its 5 threads', sum(t.state != 'free' for t in b.usb.threads) == 5,
          [t.state for t in b.usb.threads])
    d = din()
    check('probe: input bits on the 5-pin OUT', 'BF 0F 01 BF 10 00 BF 10 00 BF 10 00 BF 10 00 BF 0E 00' in d, d[:80])
    b.key(60, 600)
    why = b.run_ms(3050)
    d = din()
    check('a key through NI\'s tile 1 to the 5-pin OUT', why == 'time limit' and '90 3C' in d, (why, d[-60:]))
    b.din_in.send(bytes([0xBF, 80, 3]), b.sys.now)
    b.run_ms(3300)
    for ch in b.chains:
        ch.reset_window(b.sys.now)
    why = b.run_ms(3400)
    d = din()
    lit = [i for i, v in enumerate(b.chains[2].brightness(b.sys.now)) if v > 60]
    check('probe command in through NI\'s tile 1: LED walk', why == 'time limit' and lit == [0] and
          'BF 0F 05 BF 10 02 BF 10 00 BF 10 00 BF 0E 02' in d, (why, lit, d[:80]))
    use = s49emu.stack_use(b.ui, os.path.join(b0, 'tile0.map'))
    print('hybrid: stack peaks ' + ', '.join('0/%s %d of %d' % (n.split(':')[-1], u, a) for n, a, u in use))
    check('every tile 0 stack keeps %d bytes free' % STACK_MARGIN, all(a - u >= STACK_MARGIN for _, a, u in use),
          [(n, a - u) for n, a, u in use if a - u < STACK_MARGIN])

    def update_mode(t0):
        bb = s49emu.Board(t0, ni[1])
        struct.pack_into('<I', bb.usb.ram, 0x1C3B4 - 0x10000, 0x11042011)   # NI's DFU flag
        why = bb.run_ms(1000)
        return why, sorted((t.num, t.state, t.pc) for t in bb.usb.threads if t.state != 'free')
    w1, nis = update_mode(ni[0])
    w2, mine = update_mode(ours)
    check('update mode: NI\'s tile 1 waits where it does next to NI\'s tile 0', w1 == w2 == 'time limit' and
          [(x, y) for x, y, _ in nis] == [(x, y) for x, y, _ in mine] and
          [p for _, s, p in nis if s == 'wait'] == [p for _, s, p in mine if s == 'wait'], (nis, mine))
    for f in fails:
        print('hybrid: FAILED ' + f)
    print('hybrid: our tile 0 next to NI\'s tile 1 (board model, no USB): %d of %d checks passed '
          '(start-up, probe out, key to 5-pin, probe command in, LED walk, update-mode start)'
          % (n[0] - len(fails), n[0]))
    return not fails


def test_emu():
    if not os.path.exists(NI):
        print('emu: skipped (%s not present)' % os.path.relpath(NI, ROOT))
        return True
    import s49emu
    b = s49emu.Board(*s49emu.load_image(NI)[:2])
    b.run_ms(3300)
    init = [0xA3, 0xA0, 0xC0, 0x24, 0x81, 0x03, 0x2C, 0x2E, 0x2F, 0xAF, 0xA4]
    ok_init = all([x for _, x in b.lcd.log[c][:11]] == init for c in range(3))
    want = [('     1/2', ' FACTORY')] + [('CC%d    ' % (14 + i), '        ') for i in range(8)]
    got = [(l1, l2) for _, l1, l2, _ in b.displays()]
    b.key(60, 600)
    b.run_ms(3320)
    b.din_out.flush(b.sys.now)
    din = [x for _, x, _ in b.din_out.bytes]
    note = any(din[i:i + 2] == [0x90, 0x3C] for i in range(len(din)))
    print('emu: LCD init on 3 controllers %s; displays %s; key 60 -> DIN %s (%d instructions)' % (
        'as documented' if ok_init else 'DIFFERENT', 'as expected' if got == want else 'DIFFERENT %r' % got,
        ' '.join('%02X' % x for x in din[-3:]) if note else 'NO NOTE', b.sys.steps))
    return ok_init and got == want and note


TESTS = {'roundtrip': test_roundtrip, 'core': test_core, 'image': test_image, 'sim': test_sim,
         's49': test_s49, 'fw': test_fw, 'standalone': test_standalone, 'usb': test_usb, 'hybrid': test_hybrid, 'emu': test_emu}
DEFAULT = ['roundtrip', 'core', 'image', 'sim', 's49', 'fw', 'standalone', 'usb', 'hybrid']


if __name__ == '__main__':
    names = sys.argv[1:] or DEFAULT
    ok = True
    for n in names:
        try:
            ok &= TESTS[n]()
        except subprocess.CalledProcessError as e:   # e.g. firmware sources mid-edit
            print('%s: FAILED, %s exited with %d' % (n, os.path.basename(e.cmd[0]), e.returncode))
            ok = False
    sys.exit(0 if ok else 1)
