#!/usr/bin/env python3
"""XMOS XS1 assembler for the assembly clang --target=xcore emits, no dependencies.

usage:
  xas.py FILE.s [-o FILE.o]                 assemble to a relocatable object (JSON)
  xas.py FILE.s --bin OUT [--base ADDR]     assemble and place every section in source order at
                                            ADDR (default 0x10000) as one raw blob, no linker

Syntax is the XMOS/LLVM one (`ldw r0, dp[x]`, `bt r1, .L3`, `bl f`, `ldaw r11, cp[.LCPI0_0]`),
taken from the opcode table in xdis.py, plus `mov a, b` (= add a, b, 0). A mnemonic suffix `.l`
forces the 32-bit (prefixed) form and `.s` the 16-bit form. Comments: `#`, `//`, `;`.
Directives: .text .data .bss .section .globl .local .weak .type .size .p2align .align .balign
.byte .short .long .quad .space .zero .ascii .asciiz .string .comm .lcomm .set/= .cc_top
.cc_bottom .jmptable .jmptable32; .file .ident .loc .cfi_* are ignored.

Operand values: `cp[sym]`/`dp[sym]` are word offsets from _cp/_dp, pc-relative operands (bt, bu,
bl, ldap) take a target address, or with a plain number the raw halfword offset as xdis prints it
(`bu -5`). Everything that depends on a symbol is left symbolic in the object with all encodings
that could hold it; the linker picks the shortest that fits (relaxation, see layout()).

Object file (JSON): {"format": "xobj1", "isa", "chunks": [{sec, kind, cc, align, items}],
"symbols": {name: [chunk, item] | int | {"e": expr}}, "globals", "weak", "types"}.
A chunk is one .cc_top block, or the rest of a section outside any block; the linker drops
unreferenced chunks. kind: code, cp, dp (dp data), bss. Items:
  ["b", hex]            fixed bytes            ["z", n]          n zero bytes (bss)
  ["a", n, fill]        align to n bytes        ["d", size, expr] data word with symbols
  ["i", [[op, size, operands], ...]]           instruction, candidate encodings shortest first;
                                               operand: int | ["x", expr, "pc"|"cp"|"dp"|"abs"]
  ["j", 2|4, [expr, ...]]                      .jmptable / .jmptable32
Expressions: int | ["s", name] | [op, a, b] | ["neg", a] | ["~", a].
"""
import argparse, json, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xdis

# u/F/B operand ranges per format; the 2rus/rus/l2rus/l3rus immediates are 0..11
LIMIT = {'ru6': 63, 'u6': 63, 'u10': 1023, 'lru6': 0xffff, 'lu6': 0xffff, 'lu10': 0xfffff}
REG = {'r%d' % i: i for i in range(12)}
REG_ANY = dict(REG, cp=12, dp=13, sp=14, lr=15)
CODE_FILL = b'\x00\x90'           # add r0, r0, 0: alignment padding in code


class AsmError(Exception):
    pass


# ---------------------------------------------------------------------------------------------
# Expressions

TOKEN = re.compile(r'\s*(?:(0[xX][0-9a-fA-F]+|0[bB][01]+|\d+)|([A-Za-z_.$][\w.$]*)|'
                   r"('(?:\\.|[^'])')|(<<|>>|[-+*/%&|^~()]))")
BINOPS = [('|',), ('^',), ('&',), ('<<', '>>'), ('+', '-'), ('*', '/', '%')]


def parse_expr(text):
    """Expression AST; constant subexpressions are folded to ints."""
    toks, pos = [], 0
    text = text.strip()
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise AsmError('bad expression: %r' % text)
        num, name, ch, op = m.groups()
        toks.append(('n', int(num, 0)) if num else ('s', name) if name else
                    ('n', ord(unescape(ch[1:-1]))) if ch else ('o', op))
        pos = m.end()
        while pos < len(text) and text[pos].isspace():
            pos += 1
    toks.append(('o', None))
    e, i = _binary(toks, 0, 0)
    if toks[i] != ('o', None):
        raise AsmError('bad expression: %r' % text)
    return e


def _binary(toks, i, level):
    if level == len(BINOPS):
        return _unary(toks, i)
    a, i = _binary(toks, i, level + 1)
    while toks[i][0] == 'o' and toks[i][1] in BINOPS[level]:
        op = toks[i][1]
        b, i = _binary(toks, i + 1, level + 1)
        a = fold([op, a, b])
    return a, i


def _unary(toks, i):
    t, v = toks[i]
    if t == 'n':
        return v, i + 1
    if t == 's':
        return ['s', v], i + 1
    if v == '(':
        e, i = _binary(toks, i + 1, 0)
        if toks[i] != ('o', ')'):
            raise AsmError('missing )')
        return e, i + 1
    if v in ('-', '~', '+'):
        e, i = _unary(toks, i + 1)
        return (e if v == '+' else fold(['neg' if v == '-' else '~', e])), i
    raise AsmError('bad expression')


def fold(e):
    return evaluate(e, None) if all(type(a) is int for a in e[1:]) else e


def evaluate(e, sym):
    """Value of expression e; sym(name) gives a symbol's value."""
    if type(e) is int:
        return e
    op = e[0]
    if op == 's':
        return sym(e[1])
    a = evaluate(e[1], sym)
    if op == 'neg':
        return -a
    if op == '~':
        return ~a
    b = evaluate(e[2], sym)
    if op in '/%' and b == 0:
        raise AsmError('division by zero')
    return {'+': a + b, '-': a - b, '*': a * b, '/': int(a / b) if b else 0, '%': a % b if b else 0,
            '&': a & b, '|': a | b, '^': a ^ b, '<<': a << b, '>>': a >> b}[op]


def symbols_in(e, out):
    if type(e) is list:
        if e[0] == 's':
            out.add(e[1])
        else:
            for a in e[1:]:
                symbols_in(a, out)
    return out


def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c != '\\':
            out.append(ord(c))
            i += 1
            continue
        c = s[i + 1]
        if c in '01234567':
            m = re.match(r'[0-7]{1,3}', s[i + 1:])
            out.append(int(m.group(), 8) & 255)
            i += 1 + len(m.group())
        elif c == 'x':
            m = re.match(r'[0-9a-fA-F]{1,2}', s[i + 2:])
            out.append(int(m.group(), 16))
            i += 2 + len(m.group())
        else:
            out.append(ord({'n': '\n', 't': '\t', 'r': '\r', 'b': '\b', 'f': '\f', 'v': '\v',
                            'a': '\a', 'e': '\x1b'}.get(c, c)))
            i += 2
    return ''.join(map(chr, out))


# ---------------------------------------------------------------------------------------------
# Instructions: syntax templates from the xdis opcode table

def _template(syntax):
    """mnemonic, operand regex (spaces removed), and per capture: slot number and the cp/dp
    base it is relative to ('' when none)."""
    mn, _, rest = syntax.partition(' ')
    parts = re.split(r'\{(\d)\}', rest.replace(' ', ''))
    rx, slots = '', []
    for i, p in enumerate(parts):
        if i % 2 == 0:
            rx += re.escape(p)
        else:
            rx += '(.+?)'
            lit = parts[i - 1]
            slots.append((int(p), 'cp' if lit.endswith('cp[') else 'dp' if lit.endswith('dp[') else ''))
    return mn, re.compile(rx + '$'), slots


class ISA:
    def __init__(self, isa='xs1'):
        self.isa = isa
        self.mn = {}
        self.ops = {}
        for op in xdis.TABLE:
            if op.isa and op.isa != isa:
                continue
            mn, rx, slots = _template(op.syntax)
            self.mn.setdefault(mn, []).append((op, rx, slots))
            self.ops.setdefault((op.name, op.size), op)

    def parse(self, mn, text):
        """Candidate encodings [(op, operands)] for one instruction, shortest first; operands
        are ints or ['x', expr, mode]. Constant operands that cannot fit drop the candidate."""
        force = None
        if mn.endswith('.l') or mn.endswith('.s'):
            force = 4 if mn[-1] == 'l' else 2
            mn = mn[:-2]
        text = text.replace(' ', '').replace('\t', '')
        if mn == 'mov':
            mn, text = 'add', text + ',0'
        if mn not in self.mn:
            raise AsmError('unknown instruction %r' % mn)
        cands, matched = [], False
        for op, rx, slots in self.mn[mn]:
            if force and op.size != force:
                continue
            m = rx.match(text)
            if not m:
                continue
            vals = [None] * len(op.kinds)
            for g, (slot, base) in zip(m.groups(), slots):
                vals[slot] = operand(op.kinds[slot], g, base)
                if vals[slot] is None:
                    break
            else:
                matched = True
                if fit(op, vals, None, None) is not False:
                    cands.append((op, vals))
        if not cands:
            raise AsmError(('operand out of range: %s %s' if matched else 'bad operands: %s %s') % (mn, text))
        cands.sort(key=lambda c: c[0].size)
        return cands


def operand(kind, text, base):
    """Operand value from its source text, or None when it is not valid for this kind."""
    if kind == 'r':
        return REG.get(text)
    if kind == 'R':
        return REG_ANY.get(text)
    if text in REG_ANY:
        return None
    try:
        e = parse_expr(text)
    except AsmError:
        return None
    if type(e) is int:
        if kind == 'b':
            return xdis.BITP.index(e) if e in xdis.BITP else None
        if kind == 'F':
            return e if e >= 0 else None
        if kind == 'B':
            return -e if e <= 0 else None
        return e if e >= 0 else None
    if kind == 'b':
        return None
    return ['x', e, 'pc' if kind in 'FB' else base or 'abs']


def fit(op, vals, addr, value):
    """Final operand ints for op at addr, or False when a value does not fit. value(expr, mode)
    resolves symbolic operands (None: only check constants, symbolic ones pass)."""
    out = []
    for k, v in zip(op.kinds, vals):
        if type(v) is not int:
            if value is None:
                out.append(0)
                continue
            x = value(v[1], v[2])
            if v[2] == 'pc':
                d = x - (addr + op.size)
                if d & 1:
                    raise AsmError('odd branch target 0x%x' % x)
                v = d >> 1 if k == 'F' else -d >> 1
            else:
                v = x
        if k in 'uFB' and not 0 <= v <= LIMIT.get(op.fmt, 11):
            return False
        out.append(v)
    return out


def encode(op, vals):
    return struct.pack('<I' if op.size == 4 else '<H', op.val | xdis.pack(op.fmt, vals))


# ---------------------------------------------------------------------------------------------
# Source -> object

DATA = {'.byte': 1, '.short': 2, '.hword': 2, '.2byte': 2, '.long': 4, '.word': 4, '.int': 4,
        '.4byte': 4, '.quad': 8, '.8byte': 8}
IGNORE = {'.file', '.ident', '.loc', '.size', '.addrsig', '.addrsig_sym', '.hidden', '.protected',
          '.internal', '.p2alignl', '.p2alignw', '.loc_label'}


def section_kind(name, flags='', typ=''):
    if name.startswith('.cp.') or name.startswith('.rodata') or name == '.cp':
        return 'cp'
    if name.startswith('.dp.bss') or name.startswith('.bss') or ('d' in flags and typ == '@nobits'):
        return 'bss'
    if name.startswith('.dp.') or name.startswith('.data'):
        return 'dp'
    if name.startswith('.text'):
        return 'code'
    if 'a' not in flags:
        return None                    # debug info, notes: dropped
    return 'cp' if 'c' in flags else 'dp' if 'd' in flags else 'code'


def split_args(s):
    """Comma separated arguments; commas inside quotes or brackets do not split."""
    out, cur, q, depth = [], '', False, 0
    for i, c in enumerate(s):
        if q:
            cur += c
            if c == '"' and s[i - 1] != '\\':
                q = False
        elif c == '"':
            q, cur = True, cur + c
        elif c in '([':
            depth, cur = depth + 1, cur + c
        elif c in ')]':
            depth, cur = depth - 1, cur + c
        elif c == ',' and not depth:
            out.append(cur.strip())
            cur = ''
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def strip_comment(line):
    q = False
    for i, c in enumerate(line):
        if c == '"' and (i == 0 or line[i - 1] != '\\'):
            q = not q
        elif not q and (c in '#;' or line.startswith('//', i)):
            return line[:i]
    return line


class Chunk:
    def __init__(self, sec, kind, cc, idx):
        self.sec, self.kind, self.cc, self.idx = sec, kind, cc, idx
        self.items = []
        self.barrier = 0                # no byte merging into items before a label

    def add(self, it):
        if it[0] == 'b' and self.items and self.items[-1][0] == 'b' and self.barrier < len(self.items):
            self.items[-1][1] += it[1]
        else:
            self.items.append(it)


class Assembler:
    def __init__(self, isa='xs1'):
        self.isa = ISA(isa) if isinstance(isa, str) else isa
        self.chunks = []
        self.syms = {}                  # name -> (chunk index, item index) | int | {'e': expr}
        self.globals, self.weak, self.locals, self.types = set(), set(), set(), {}
        self.sec = None                 # (name, kind)
        self.cc = {}                    # section -> open .cc_top chunk
        self.rest = {}                  # section -> chunk outside .cc_top blocks
        self.pending = {}               # section -> alignment waiting for the next content
        self.where = ''

    # -- chunk bookkeeping
    def new_chunk(self, cc):
        c = Chunk(self.sec[0], self.sec[1], cc, len(self.chunks))
        self.chunks.append(c)
        return c

    def chunk(self):
        if self.sec is None:
            self.section('.text')
        name = self.sec[0]
        c = self.cc.get(name) or self.rest.get(name)
        if c is None:
            c = self.rest[name] = self.new_chunk(None)
        if name in self.pending:
            c.add(self.pending.pop(name))
        return c

    def emit(self, it):
        c = self.chunk()
        if c.kind == 'bss' and it[0] not in ('z', 'a'):
            if it[0] == 'b' and not any(it[1]):
                it = ['z', len(it[1])]
            else:
                raise AsmError('data in a nobits section')
        c.add(it)

    def label(self, name):
        if name in self.syms:
            raise AsmError('symbol %s defined twice' % name)
        c = self.chunk()
        c.barrier = len(c.items)
        self.syms[name] = (c.idx, len(c.items))

    def section(self, name, flags='', typ=''):
        self.sec = (name, section_kind(name, flags, typ))

    # -- statements
    def source(self, text, filename='<input>'):
        for n, line in enumerate(text.split('\n'), 1):
            self.where = '%s:%d' % (filename, n)
            try:
                self.line(strip_comment(line).strip())
            except AsmError as e:
                raise AsmError('%s: %s' % (self.where, e)) from None
        return self

    def line(self, s):
        if self.sec and self.sec[1] is None and not re.match(r'\.(section|text|data|bss)\b', s):
            return                      # contents of a dropped section (debug info)
        while True:
            m = re.match(r'([A-Za-z_.$][\w.$]*|\d+)\s*:(?!:)', s)
            if not m:
                break
            self.label(m.group(1))
            s = s[m.end():].strip()
        if not s:
            return
        m = re.match(r'([A-Za-z_.$][\w.$]*)\s*=\s*(.+)$', s)
        if m:
            return self.assign(m.group(1), m.group(2))
        if s[0] == '.':
            m = re.match(r'(\.[A-Za-z_]\w*)(.*)$', s)
            return self.directive(m.group(1).lower(), m.group(2).strip())
        m = re.match(r'([A-Za-z][\w]*(?:\.[ls])?)\s*(.*)$', s)
        if not m:
            raise AsmError('syntax error: %r' % s)
        self.insn(m.group(1).lower(), m.group(2))

    def assign(self, name, text):
        e = parse_expr(text)
        if name in self.syms:
            raise AsmError('symbol %s defined twice' % name)
        self.syms[name] = e if type(e) is int else {'e': e}

    def insn(self, mn, text):
        cands = self.isa.parse(mn, text)
        if all(type(v) is int for v in cands[0][1]):
            op, vals = cands[0]
            self.emit(['b', encode(op, vals)])
        else:
            self.emit(['i', cands])

    def directive(self, d, rest):
        a = split_args(rest) if rest else []
        if d in IGNORE or d.startswith('.cfi_'):
            return
        if d == '.text':
            return self.section('.text')
        if d == '.data':
            return self.section('.dp.data')
        if d == '.bss':
            return self.section('.dp.bss')
        if d == '.section':
            name = a[0].strip('"')
            flags = a[1].strip('"') if len(a) > 1 else ''
            return self.section(name, flags, a[2] if len(a) > 2 else '')
        if d in ('.globl', '.global'):
            self.globals.update(a)
            return
        if d == '.weak':
            self.globals.update(a)
            self.weak.update(a)
            return
        if d == '.local':
            self.locals.update(a)
            return
        if d == '.type':
            self.types[a[0]] = a[1].lstrip('@%') if len(a) > 1 else ''
            return
        if d in ('.p2align', '.align', '.balign'):
            n = parse_expr(a[0])
            n = 1 << n if d == '.p2align' else n
            fill = parse_expr(a[1]) if len(a) > 1 and a[1] else None
            if n > 1:
                it = ['a', n, fill]
                if self.sec and self.sec[1] and self.sec[0] not in self.cc:
                    # outside a .cc_top block: belongs to whatever comes next in this section,
                    # usually the next block
                    self.pending[self.sec[0]] = max(self.pending.get(self.sec[0], it), it, key=lambda x: x[1])
                else:
                    self.emit(it)
            return
        if d in DATA:
            size = DATA[d]
            for x in a:
                e = parse_expr(x)
                if type(e) is int:
                    if not -(1 << (8 * size - 1)) <= e < 1 << (8 * size):
                        raise AsmError('value out of range: %s' % x)
                    self.emit(['b', (e & ((1 << 8 * size) - 1)).to_bytes(size, 'little')])
                elif size == 8:
                    raise AsmError('.quad with symbols')
                else:
                    self.emit(['d', size, e])
            return
        if d in ('.space', '.zero', '.skip'):
            n = parse_expr(a[0])
            fill = parse_expr(a[1]) if len(a) > 1 else 0
            if type(n) is not int or n < 0:
                raise AsmError('bad size')
            if n:
                self.emit(['z', n] if self.sec and self.sec[1] == 'bss' else ['b', bytes([fill & 255]) * n])
            return
        if d in ('.ascii', '.asciiz', '.string'):
            for x in a:
                if not (x.startswith('"') and x.endswith('"')):
                    raise AsmError('bad string')
                b = unescape(x[1:-1]).encode('latin-1')
                self.emit(['b', b + (b'\0' if d != '.ascii' else b'')])
            return
        if d in ('.set', '.equ', '.equiv'):
            return self.assign(a[0], a[1])
        if d in ('.comm', '.lcomm'):
            name, size = a[0], parse_expr(a[1])
            align = parse_expr(a[2]) if len(a) > 2 else 4
            keep = self.sec
            self.section('.dp.bss')
            self.cc['.dp.bss'] = self.new_chunk(name + '.data')
            self.emit(['a', max(4, align), None])
            self.label(name)
            self.emit(['z', size])
            del self.cc['.dp.bss']
            if d == '.comm':
                self.globals.add(name)
            self.sec = keep
            return
        if d == '.cc_top':
            if self.sec is None:
                self.section('.text')
            if self.sec[1]:
                self.cc[self.sec[0]] = self.new_chunk(a[0])
            return
        if d == '.cc_bottom':
            self.cc.pop(self.sec[0] if self.sec else None, None)
            return
        if d in ('.jmptable', '.jmptable32'):
            self.emit(['j', 4 if d == '.jmptable32' else 2, [parse_expr(x) for x in a]])
            return
        raise AsmError('unknown directive %s' % d)

    # -- output
    def obj(self):
        def ser(it):
            if it[0] == 'b':
                return ['b', it[1].hex()]
            if it[0] == 'i':
                return ['i', [[op.name, op.size, vals] for op, vals in it[1]]]
            return it
        chunks = []
        for c in self.chunks:
            lead = [it[1] for it in c.items[:next((i for i, it in enumerate(c.items) if it[0] != 'a'), 0)]]
            align = max([2 if c.kind == 'code' else 4] + lead)      # later aligns are kept as items
            chunks.append({'sec': c.sec, 'kind': c.kind, 'cc': c.cc, 'align': align,
                           'items': [ser(it) for it in c.items]})
        syms = {k: (list(v) if type(v) is tuple else v) for k, v in self.syms.items()}
        return {'format': 'xobj1', 'isa': self.isa.isa, 'chunks': chunks, 'symbols': syms,
                'globals': sorted(self.globals - self.locals), 'weak': sorted(self.weak),
                'types': self.types}


# ---------------------------------------------------------------------------------------------
# Layout and relaxation (used by the linker and by --bin)

class Item:
    __slots__ = ('t', 'a', 'b', 'size', 'choice')

    def __init__(self, it, isa):
        self.t, self.choice = it[0], 0
        if self.t == 'b':
            self.a = bytes.fromhex(it[1])
            self.size = len(self.a)
        elif self.t == 'z':
            self.size = it[1]
        elif self.t == 'a':
            self.a, self.b, self.size = it[1], it[2], 0
        elif self.t == 'd':
            self.a, self.b, self.size = it[1], it[2], it[1]
        elif self.t == 'i':
            self.a = [(isa.ops[(n, s)], vals) for n, s, vals in it[1]]
            self.size = self.a[0][0].size
        elif self.t == 'j':
            self.a, self.b = it[1], it[2]
            self.choice = set()         # .jmptable entries routed through a trampoline
            self.size = it[1] * len(it[2])


class Placed:
    """A chunk being laid out: its items, object context for symbol lookup, and addresses
    (offs[i] = address of item i, offs[-1] = end)."""

    def __init__(self, c, ctx, isa):
        self.c, self.ctx = c, ctx
        self.kind, self.align = c['kind'], c['align']
        self.items = [Item(it, isa) for it in c['items']]
        self.addr, self.offs = 0, []


def value_fn(p, lookup):
    """value(expr, mode) for operands of chunk p: an address, or for cp/dp a word offset."""
    def value(e, mode):
        v = evaluate(e, lambda s: lookup(p.ctx, s))
        if mode in ('cp', 'dp'):
            d = v - lookup(p.ctx, '_' + mode)
            if d & 3 or d < 0:
                raise AsmError('%s is not a word in the %s area (offset %d)' % (e, mode, d))
            v = d >> 2
        return v
    return value


def jt_entries(it, addr, value, isa):
    """Encodings (op, operands) of a jump table at addr. .jmptable32: one long bu per entry.
    .jmptable: one short bu per entry; an entry in it.choice branches to a long bu placed
    after the table instead (one per distinct target; always within reach of a short bu since
    trampoline k serves an entry at index >= k of a table of at most 32 entries)."""
    out, tramp, n, size = [], [], len(it.b), it.a
    for i, e in enumerate(it.b):
        x, at = value(e, 'pc'), addr + size * i
        if size == 2 and i in it.choice:
            if x not in tramp:
                tramp.append(x)
            x = addr + 2 * n + 4 * tramp.index(x)
        d = x - at - size
        out.append((isa.ops[('BRFU' if d >= 0 else 'BRBU', size)], [abs(d) >> 1]))
    for k, x in enumerate(tramp):
        d = x - (addr + 2 * n + 4 * k) - 4
        out.append((isa.ops[('BRFU' if d >= 0 else 'BRBU', 4)], [abs(d) >> 1]))
    return out


def layout(placed, base, lookup, isa):
    """Assign addresses to the placed chunks (in order from base) and pick instruction sizes.
    Every instruction starts at its shortest encoding and only grows until all operands fit,
    so this converges. lookup(ctx, name) returns a symbol's current address. Returns the end."""
    while True:
        addr = base
        for p in placed:
            addr = -(-addr // p.align) * p.align
            p.addr, offs = addr, []
            for it in p.items:
                offs.append(addr)
                if it.t == 'a':
                    it.size = -addr % it.a
                addr += it.size
            offs.append(addr)
            p.offs = offs
        changed = False
        for p in placed:
            value = value_fn(p, lookup)
            for i, it in enumerate(p.items):
                if it.t == 'i':
                    op, vals = it.a[it.choice]
                    if fit(op, vals, p.offs[i], value) is not False:
                        continue
                    for j in range(it.choice + 1, len(it.a)):
                        op, vals = it.a[j]
                        if op.size >= it.size and fit(op, vals, p.offs[i], value) is not False:
                            break
                    else:
                        op, vals = it.a[-1]
                        raise AsmError('%s: operand out of range: %s %s'
                                       % (p.c['sec'], op.syntax, vals))
                    it.choice, it.size, changed = j, op.size, True
                elif it.t == 'j' and it.a == 2:
                    for k, (op, vals) in enumerate(jt_entries(it, p.offs[i], value, isa)[:len(it.b)]):
                        if vals[0] > 63 and k not in it.choice:
                            it.choice.add(k)
                            changed = True
                    it.size = 2 * len(it.b) + 4 * len({value(it.b[k], 'pc') for k in it.choice})
        if not changed:
            return addr


def emit(placed, isa, lookup):
    """Bytes of each placed chunk after layout() (b'' for bss)."""
    out = []
    for p in placed:
        if p.kind == 'bss':
            out.append(b'')
            continue
        value, buf = value_fn(p, lookup), bytearray()
        for i, it in enumerate(p.items):
            addr = p.offs[i]
            if it.t == 'b':
                buf += it.a
            elif it.t == 'z':
                buf += bytes(it.size)
            elif it.t == 'a' and it.size:
                fill = CODE_FILL if p.kind == 'code' and it.b is None else bytes([it.b or 0]) * 2
                buf += (fill * (it.size // 2 + 1))[addr & 1:][:it.size]
            elif it.t == 'd':
                v = value(it.b, 'abs')
                if not -(1 << (8 * it.a - 1)) <= v < 1 << (8 * it.a):
                    raise AsmError('data value out of range: %s = %d' % (it.b, v))
                buf += (v & ((1 << 8 * it.a) - 1)).to_bytes(it.a, 'little')
            elif it.t == 'i':
                op, vals = it.a[it.choice]
                buf += encode(op, fit(op, vals, addr, value))
            elif it.t == 'j':
                for op, vals in jt_entries(it, addr, value, isa):
                    if vals[0] > LIMIT[op.fmt]:
                        raise AsmError('jump table entry out of range')
                    buf += encode(op, vals)
            assert len(buf) == p.offs[i + 1] - p.addr, (p.c['sec'], it.t)
        out.append(bytes(buf))
    return out


def link_flat(obj, base, isa):
    """Place every chunk of one object in source order at base (no linker): for --bin."""
    keep = [c for c in obj['chunks'] if c['kind']]
    placed = [Placed(c, None, isa) for c in keep]
    where = {id(c): p for c, p in zip(keep, placed)}

    def lookup(ctx, name):
        v = obj['symbols'].get(name)
        if v is None:
            raise AsmError('undefined symbol %s' % name)
        if type(v) is int:
            return v
        if type(v) is dict:
            return evaluate(v['e'], lambda s: lookup(ctx, s))
        return where[id(obj['chunks'][v[0]])].offs[v[1]]
    layout(placed, base, lookup, isa)
    return b''.join(emit(placed, isa, lookup))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file')
    ap.add_argument('-o', '--output')
    ap.add_argument('--bin', help='write a raw blob instead of an object')
    ap.add_argument('--base', type=lambda s: int(s, 0), default=0x10000)
    ap.add_argument('--isa', default='xs1', choices=('xs1', 'xs2'))
    a = ap.parse_args()
    isa = ISA(a.isa)
    try:
        obj = Assembler(isa).source(open(a.file).read(), a.file).obj()
        if a.bin:
            open(a.bin, 'wb').write(link_flat(obj, a.base, isa))
            return
        out = a.output or os.path.splitext(a.file)[0] + '.o'
        with open(out, 'w') as f:
            json.dump(obj, f, separators=(',', ':'))
    except AsmError as e:
        sys.exit('xas: %s' % e)


if __name__ == '__main__':
    main()
