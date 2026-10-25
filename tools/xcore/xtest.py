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
emu        (not in the default set, ~1 min) run NI's S49 image on the s49emu board model: LCD
           init sequence, standalone display text, a key press giving a note on DIN MIDI OUT
"""
import glob, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import xdis, xas

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
    subprocess.run([CLANG] + CFLAGS + ['-I' + i for i in inc] + ['-S', src, '-o', out], check=True)
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
    rt = runtime() + [cc(os.path.join(XS1, 'platform.c'))]
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
        why = sim.run()
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
    progs = [('prog', [os.path.join(TEST, 'prog.c')], []),
             ('test_smartplay', [os.path.join(t, 'test_smartplay.c')] + core('smartplay'), tinc),
             ('test_core', [os.path.join(t, 'test_core.c'), os.path.join(t, 'host', 'fake_hal.c')]
              + core('midi', 'play', 'smartplay', 'settings', 'settings_store'), tinc),
             ('test_host', [os.path.join(t, 'test_host.c'), os.path.join(t, 'host', 'fake_hal.c')]
              + core('host', 'mcu', 'midi'), tinc)]
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
        leds = h.command('leds')[1:-1]
        want_leds = ['guide 0 127 0 0', 'led 3 127']
        disp = [x.split('|')[1:3] for x in scr[1:-1]]
        good = out == want and disp[0] == ['MY      ', 'KONTROL '] and disp[8] == ['        ', 'XS1 S49 '] \
            and all(d == [' ' * 8] * 2 for d in disp[1:8]) and leds == want_leds
        ok &= good
        print('s49: %s: MIDI %s; display 1 %r, display 9 %r, others blank; lights %s: %s' % (
            '2 tiles' if len(tiles) == 2 else 'tile 0 + HostLink stub', out, disp[0], disp[8],
            leds, 'ok' if good else 'DIFFERENT'))
    return ok


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
         's49': test_s49, 'emu': test_emu}
DEFAULT = ['roundtrip', 'core', 'image', 'sim', 's49']


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
