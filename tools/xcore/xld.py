#!/usr/bin/env python3
"""XMOS XS1 linker for xas objects: one tile RAM image per run, no dependencies.

usage:
  xld.py [options] FILE... [@DESCRIPTION]
    FILE           xas objects (.o) or assembly (.s, assembled on the fly)
    -o OUT         tile RAM image, raw, loaded at the RAM base (default a.bin)
    -m MAP         map file: areas, placed blocks, symbols
    --entry SYM    entry point, placed first at the RAM base (default _start)
    --ram B:S      RAM base and size (default 0x10000:0x10000, one XS1-L16 tile)
    --stack N      minimum free bytes between the end of bss and the top of RAM (default 1024)
    --keep SYM     keep SYM's block even if unreferenced (repeatable)
    --no-gc        keep every block
    --stub-undefined  define every undefined symbol (size reports only): a function
                   `ldc r0, 0; retsp 0`, or a zero word in cp/dp when used cp/dp-relative
  @DESCRIPTION is a file holding more of the same arguments (one or more per line, # comments):
  the "linker script" of a tile, e.g. firmware/platform/xs1/tile0.ld.

Layout from the RAM base: code (entry block first), then the cp area (_cp; constants,
addressed cp-relative), then the dp area (_dp; initialised data, then bss). The image ends at
_bss_start; startup zeroes _bss_start.._bss_end and sets sp to _stack_top (RAM top - 8).
Data blocks are sorted small first so more cp/dp offsets fit the short (6-bit) encodings.
Blocks (.cc_top ... .cc_bottom, and the rest of each section) that nothing reachable from the
entry references are dropped. Instruction sizes are chosen here, see xas.layout().
Linker symbols: _cp _dp _bss_start _bss_end _image_end _stack_top _ram_base _ram_end.
"""
import argparse, json, os, shlex, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xas
from xas import AsmError

AREAS = ('code', 'cp', 'dp', 'bss')


class Obj:
    def __init__(self, path, data):
        self.path, self.name = path, os.path.basename(path)
        self.syms, self.chunks = data['symbols'], data['chunks']
        self.globals, self.weak, self.types = set(data['globals']), set(data['weak']), data['types']


def load(path, isa):
    if path.endswith('.s') or path.endswith('.S'):
        return Obj(path, xas.Assembler(isa).source(open(path).read(), path).obj())
    data = json.load(open(path))
    if data.get('format') != 'xobj1':
        raise AsmError('%s: not an xas object' % path)
    return Obj(path, data)


def refs(chunk):
    """Symbols an object chunk refers to: {name: 'cp'|'dp' if used cp/dp-relative, else ''}."""
    out = {}

    def add(e, mode):
        for s in xas.symbols_in(e, set()):
            out[s] = out.get(s) or (mode if mode in ('cp', 'dp') else '')
    for it in chunk['items']:
        if it[0] == 'd':
            add(it[2], '')
        elif it[0] == 'i':
            for v in it[1][0][2]:
                if type(v) is list:
                    add(v[1], v[2])
        elif it[0] == 'j':
            for e in it[2]:
                add(e, '')
    return out


def link(paths, entry='_start', ram=(0x10000, 0x10000), stack=1024, keep=(), gc=True,
         stub=False, isa_name='xs1'):
    isa = xas.ISA(isa_name)
    objs = [load(p, isa) for p in paths]
    base, size = ram
    fixed = {'_stack_top': base + size - 8, '_ram_base': base, '_ram_end': base + size}
    markers = ('_cp', '_dp', '_bss_start', '_bss_end', '_image_end')

    # global symbols: name -> object index (strong definitions win over weak ones)
    glob = {}
    for i, o in enumerate(objs):
        for name in o.globals:
            if name not in o.syms:
                continue
            if name in glob:
                j = glob[name]
                if name in objs[j].weak:
                    glob[name] = i
                elif name not in o.weak:
                    raise AsmError('%s defined in %s and %s' % (name, objs[j].name, o.name))
            else:
                glob[name] = i
    stubs = {}

    def find(ctx, name):
        """(object index, definition) of name as seen from object ctx, or None."""
        o = objs[ctx]
        if name in o.syms:
            return ctx, o.syms[name]
        if name in glob:
            j = glob[name]
            return j, objs[j].syms[name]
        return None

    # garbage collection over blocks
    def chunk_of(ctx, name, seen, mode=''):
        """Blocks a reference to name from object ctx keeps alive."""
        if name in fixed or name in markers:
            return []
        f = find(ctx, name)
        if f is None:
            if stub:
                stubs.setdefault(name, (objs[ctx].name, {'cp': 'cp', 'dp': 'bss'}.get(mode, 'code')))
                return []
            raise AsmError('undefined symbol %s (referenced from %s)' % (name, objs[ctx].name))
        j, d = f
        if type(d) is list:
            return [(j, d[0])]
        if type(d) is dict and (j, name) not in seen:
            seen.add((j, name))
            return [c for s in xas.symbols_in(d['e'], set()) for c in chunk_of(j, s, seen)]
        return []

    live = set()
    work = []
    if find(0, entry) is None:
        raise AsmError('entry symbol %s not defined' % entry)
    for name in [entry] + list(keep):
        if find(0, name):
            work += chunk_of(0, name, set())
    for i, o in enumerate(objs):
        local = {d[0] for n, d in o.syms.items() if type(d) is list and not n.startswith('.L')}
        for k, c in enumerate(o.chunks):
            if c['kind'] and (not gc or (c['cc'] is None and k in local)):
                work.append((i, k))
    while work:
        ck = work.pop()
        if ck in live:
            continue
        live.add(ck)
        i, k = ck
        for name, mode in refs(objs[i].chunks[k]).items():
            work += chunk_of(i, name, set(), mode)

    # placement order
    j, d = find(0, entry)
    first = (j, d[0])
    order = {a: [] for a in AREAS}
    for i, o in enumerate(objs):
        for k, c in enumerate(o.chunks):
            if (i, k) in live and (i, k) != first:
                order[c['kind']].append((i, k))
    for a in ('cp', 'dp', 'bss'):
        order[a].sort(key=lambda ik: sum(it[1] if it[0] == 'z' else len(it[1]) // 2 if it[0] == 'b'
                                         else 4 for it in objs[ik[0]].chunks[ik[1]]['items']))
    order['code'].insert(0, first)

    placed, where, mark = [], {}, {}

    def marker(name):
        p = xas.Placed({'kind': 'bss', 'align': 4, 'items': [], 'sec': name}, None, isa)
        mark[name] = p
        placed.append(p)
    stub_chunks = {}
    for a in AREAS:
        if a == 'cp':
            marker('_cp')
        elif a == 'dp':
            marker('_dp')
        elif a == 'bss':
            marker('_bss_start')
        for i, k in order[a]:
            p = xas.Placed(objs[i].chunks[k], i, isa)
            where[(i, k)] = p
            placed.append(p)
        for name in sorted(n for n, (o, area) in stubs.items() if area == a):
            body = ['z', 4] if a == 'bss' else ['b', '00000000' if a == 'cp' else
                                                 (xas.encode(*isa.parse('ldc', 'r0, 0')[0]) +
                                                  xas.encode(*isa.parse('retsp', '0')[0])).hex()]
            c = {'kind': a, 'align': 4, 'sec': 'stub', 'cc': None, 'items': [body]}
            p = stub_chunks[name] = xas.Placed(c, None, isa)
            placed.append(p)
    marker('_bss_end')
    mark['_image_end'] = mark['_bss_start']

    def lookup(ctx, name):
        if ctx is not None:
            f = find(ctx, name)
            if f is not None:
                j, d = f
                if type(d) is int:
                    return d
                if type(d) is dict:
                    return xas.evaluate(d['e'], lambda s: lookup(j, s))
                p = where.get((j, d[0]))
                if p is None:
                    raise AsmError('%s is in a dropped block' % name)
                return p.offs[d[1]]
        if name in mark:
            return mark[name].offs[0]
        if name in fixed:
            return fixed[name]
        if name in stub_chunks:
            return stub_chunks[name].offs[0]
        raise AsmError('undefined symbol %s' % name)

    end = xas.layout(placed, base, lookup, isa)
    data = xas.emit(placed, isa, lookup)
    image = bytearray()
    for p, b in zip(placed, data):
        if p.kind != 'bss':
            image += bytes(p.addr - base - len(image)) + b
    img_end = mark['_bss_start'].offs[0]
    image += bytes(img_end - base - len(image))
    free = base + size - 8 - end
    if free < stack:
        raise AsmError('tile RAM overflow: %d bytes free for the stack, need %d' % (free, stack))
    return Result(objs, placed, where, mark, image, base, size, end, stubs, lookup, find, glob)


class Result:
    def __init__(self, objs, placed, where, mark, image, base, size, end, stubs, lookup, find, glob):
        self.objs, self.placed, self.where, self.mark = objs, placed, where, mark
        self.image, self.base, self.size, self.end, self.stubs = image, base, size, end, stubs
        self.lookup, self.find, self.glob = lookup, find, glob

    def areas(self):
        """[(area, start, end)]"""
        m = self.mark
        bounds = [self.base, m['_cp'].offs[0], m['_dp'].offs[0], m['_bss_start'].offs[0], m['_bss_end'].offs[0]]
        return [(a, bounds[i], bounds[i + 1]) for i, a in enumerate(AREAS)]

    def symbols(self):
        """[(address, name, object)] of all labels in placed blocks, sorted by address."""
        out = []
        for i, o in enumerate(self.objs):
            for name, d in o.syms.items():
                if type(d) is list and (i, d[0]) in self.where and not name.startswith('.L'):
                    out.append((self.where[(i, d[0])].offs[d[1]], name, o.name))
        out += [(p.offs[0], n, 'linker') for n, p in self.mark.items()]
        return sorted(out)

    def summary(self):
        a = {n: e - s for n, s, e in self.areas()}
        free = self.base + self.size - 8 - self.end
        return ('code %d  cp %d  dp %d  bss %d  image %d  stack/free %d  (of %d)'
                % (a['code'], a['cp'], a['dp'], a['bss'], len(self.image), free, self.size))

    def write_map(self, f):
        f.write('# xld map\n# %s\n\nareas:\n' % self.summary())
        for n, s, e in self.areas():
            f.write('  %-5s 0x%05x 0x%05x %6d\n' % (n, s, e, e - s))
        f.write('  stack 0x%05x 0x%05x %6d\n' % (self.end, self.base + self.size - 8,
                                                  self.base + self.size - 8 - self.end))
        f.write('\nblocks:  address  size  area  object  section  block\n')
        for p in self.placed:
            if p.ctx is not None:
                f.write('  0x%05x %5d  %-4s  %s  %s  %s\n' % (p.addr, p.offs[-1] - p.addr, p.kind,
                                                            self.objs[p.ctx].name, p.c['sec'],
                                                            p.c['cc'] or '-'))
        if self.stubs:
            f.write('\nstubs (code: ldc r0, 0; retsp 0, cp/bss: a zero word):\n')
            for n, (o, area) in sorted(self.stubs.items()):
                f.write('  0x%05x %-4s %s (from %s)\n' % (self.lookup(None, n), area, n, o))
        f.write('\nsymbols:\n')
        for addr, name, o in self.symbols():
            f.write('  0x%05x %s %s\n' % (addr, name, o))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], fromfile_prefix_chars='@')
    ap.convert_arg_line_to_args = lambda line: shlex.split(line, comments=True)
    ap.add_argument('files', nargs='+')
    ap.add_argument('-o', '--output', default='a.bin')
    ap.add_argument('-m', '--map')
    ap.add_argument('--entry', default='_start')
    ap.add_argument('--ram', default='0x10000:0x10000')
    ap.add_argument('--stack', type=lambda s: int(s, 0), default=1024)
    ap.add_argument('--keep', action='append', default=[])
    ap.add_argument('--no-gc', action='store_true')
    ap.add_argument('--stub-undefined', action='store_true')
    ap.add_argument('--isa', default='xs1', choices=('xs1', 'xs2'))
    a = ap.parse_args()
    b, s = (int(x, 0) for x in a.ram.split(':'))
    try:
        r = link(a.files, a.entry, (b, s), a.stack, a.keep, not a.no_gc, a.stub_undefined, a.isa)
    except AsmError as e:
        sys.exit('xld: %s' % e)
    open(a.output, 'wb').write(r.image)
    if a.map:
        with open(a.map, 'w') as f:
            r.write_map(f)
    if r.stubs:
        print('xld: stubbed %d undefined symbols: %s' % (len(r.stubs), ' '.join(sorted(r.stubs))),
              file=sys.stderr)
    print('%s: %s' % (a.output, r.summary()))


if __name__ == '__main__':
    main()
