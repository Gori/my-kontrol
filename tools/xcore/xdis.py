#!/usr/bin/env python3
"""XMOS xCORE disassembler (XS1, optionally XS2/xCORE-200), no dependencies.

usage:
  xdis.py [--isa xs1|xs2] [--base ADDR] [--offset N] [--length N] [--bitrev] [--raw] FILE
  xdis.py --flash IMAGE [--isa ...]     split an xflash/upgrade image (bit-reversed SPI
                                        byte order) into its per-tile segments and disassemble each
  xdis.py --selftest                    decode->encode round trip over every 16-bit word

FILE is an ELF (EM_XCORE, executable sections are disassembled, symbols become labels) or,
with --raw or when it is not an ELF, a raw blob loaded at --base (default 0x10000, the XS1 RAM base).
Output: address, halfwords, xas-syntax instruction, and for pc-relative operands the
resolved target with a label. Undecodable halfwords are printed as `.short`.

The opcode table OPS is plain data: (name, format, bit pattern, syntax, operand kinds, isa).
`decode()` and `encode()` both read it, so the same table can drive an assembler.
Encodings come from the public XMOS XS1/XS2 architecture manuals; output syntax follows the
XMOS assembler (and LLVM's XCore backend, which it was cross-checked against for XS1).
"""
import argparse, struct, sys

# ---------------------------------------------------------------------------------------------
# Operand fields. Registers are r0..r11; three/two register operands share a packed field.

BITP = [32, 1, 2, 3, 4, 5, 6, 7, 8, 16, 24, 32]   # 'bitp' immediates encoded as 0..11
REGS = ['r%d' % i for i in range(12)] + ['cp', 'dp', 'sp', 'lr']


def dec3(w):
    """3r field (bits 10..0): op1,op2,op3 each 0..11, or None. The high register bits are
    packed as op1 + 3*op2 + 9*op3 (the XS2 manual's format pages print the weights in the other
    order; this order is the one the XMOS toolchain and LLVM use)."""
    c = (w >> 6) & 31
    if c >= 27:
        return None
    return [(c % 3) << 2 | (w >> 4) & 3, (c // 3 % 3) << 2 | (w >> 2) & 3, (c // 9) << 2 | w & 3]


def enc3(a, b, c):
    return ((a >> 2) + (b >> 2) * 3 + (c >> 2) * 9) << 6 | (a & 3) << 4 | (b & 3) << 2 | c & 3


def dec2(w):
    """2r field (bits 10..5 and 3..0; bit 4 belongs to the opcode): op1 (bits 3..2), op2 (1..0)."""
    c = (w >> 6) & 31
    if c < 27:
        return None
    if (w >> 5) & 1:
        if c == 31:
            return None
        c += 5
    c -= 27
    return [(c % 3) << 2 | (w >> 2) & 3, (c // 3) << 2 | w & 3]


def enc2(a, b):
    c = (a >> 2) + 3 * (b >> 2)
    return ((c % 5) + 27) << 6 | (c >= 5) << 5 | (a & 3) << 2 | b & 3


# Formats: words, and how operands are packed. Each format names the operand slots in
# encoding order; the 'kinds' string of an entry types them (r reg, u unsigned, b bitp,
# R reg incl. cp/dp/sp/lr, F/B forward/backward pc-relative halfword offset from the next
# instruction).
FMT = {
    # 16-bit
    '3r':    (1, 3), '2rus': (1, 3), '2r': (1, 2), 'r2r': (1, 2), 'rus': (1, 2),
    '1r':    (1, 1), '0r': (1, 0), 'ru6': (1, 2), 'u6': (1, 1), 'u10': (1, 1),
    # 32-bit: 11111 prefix (long) or 111100 prefix (PFIX, extends the immediate)
    'l3r':   (2, 3), 'l2rus': (2, 3), 'l2r': (2, 2), 'lr2r': (2, 2), 'l0r': (2, 0),
    'l4r':   (2, 4), 'l3rus': (2, 4), 'l4rus': (2, 5), 'l5r': (2, 5), 'l6r': (2, 6),
    'lru6':  (2, 2), 'lu6': (2, 1), 'lu10': (2, 1),
}


def operands(fmt, w):
    """Raw operand values in encoding order (op1, op2, ...), or None when the operand field
    is not valid for this format. w is the instruction: halfword 0 in bits 15..0, halfword 1
    in bits 31..16."""
    lo, hi = w & 0xffff, w >> 16
    if fmt in ('3r', '2rus', 'l3r', 'l2rus'):
        return dec3(lo)
    if fmt in ('2r', 'rus', 'l2r'):
        return dec2(lo)
    if fmt in ('r2r', 'lr2r'):
        o = dec2(lo)
        return o and [o[1], o[0]]
    if fmt == '1r':
        r = lo & 15
        return [r] if r < 12 else None
    if fmt in ('0r', 'l0r'):
        return []
    if fmt == 'ru6':
        return [(lo >> 6) & 15, lo & 63]
    if fmt == 'u6':
        return [lo & 63]
    if fmt == 'u10':
        return [lo & 1023]
    if fmt == 'lru6':
        return [(hi >> 6) & 15, (lo & 1023) << 6 | hi & 63]
    if fmt == 'lu6':
        return [(lo & 1023) << 6 | hi & 63]
    if fmt == 'lu10':
        return [(lo & 1023) << 10 | hi & 1023]
    if fmt in ('l4r', 'l3rus'):
        o, r = dec3(lo), hi & 15
        return o + [r] if o and r < 12 else None
    if fmt in ('l5r', 'l4rus'):
        o, p = dec3(lo), dec2(hi)
        return o + p if o and p else None
    if fmt == 'l6r':
        o, p = dec3(lo), dec3(hi)
        return o + p if o and p else None
    raise KeyError(fmt)


def pack(fmt, ops):
    """Inverse of operands(): the operand bits of an instruction word."""
    if fmt in ('3r', '2rus', 'l3r', 'l2rus'):
        return enc3(*ops)
    if fmt in ('2r', 'rus', 'l2r'):
        return enc2(*ops)
    if fmt in ('r2r', 'lr2r'):
        return enc2(ops[1], ops[0])
    if fmt == '1r':
        return ops[0]
    if fmt in ('0r', 'l0r'):
        return 0
    if fmt == 'ru6':
        return ops[0] << 6 | ops[1]
    if fmt in ('u6', 'u10'):
        return ops[0]
    if fmt == 'lru6':
        return (ops[1] >> 6) | (ops[0] << 6 | ops[1] & 63) << 16
    if fmt == 'lu6':
        return (ops[0] >> 6) | (ops[0] & 63) << 16
    if fmt == 'lu10':
        return (ops[0] >> 10) | (ops[0] & 1023) << 16
    if fmt in ('l4r', 'l3rus'):
        return enc3(*ops[:3]) | ops[3] << 16
    if fmt in ('l5r', 'l4rus'):
        return enc3(*ops[:3]) | enc2(*ops[3:]) << 16
    if fmt == 'l6r':
        return enc3(*ops[:3]) | enc3(*ops[3:]) << 16
    raise KeyError(fmt)


# ---------------------------------------------------------------------------------------------
# Opcode table. pattern: 16 (or 16|16) characters, 0/1 = fixed opcode bits, '.' = operand bits,
# halfword 0 first, written msb first as in the manuals. syntax: {n} is operand n in encoding
# order. isa: 'xs1' (XS1 only), 'xs2' (xCORE-200 only), '' (both).

OPS = [
    # name      fmt      pattern                                 syntax                         kinds  isa
    ('STWI',    '2rus',  '00000...........',                   'stw {0}, {1}[{2}]',            'rru', ''),
    ('LDWI',    '2rus',  '00001...........',                   'ldw {0}, {1}[{2}]',            'rru', ''),
    ('ADD',     '3r',    '00010...........',                   'add {0}, {1}, {2}',            'rrr', ''),
    ('SUB',     '3r',    '00011...........',                   'sub {0}, {1}, {2}',            'rrr', ''),
    ('SHL',     '3r',    '00100...........',                   'shl {0}, {1}, {2}',            'rrr', ''),
    ('SHR',     '3r',    '00101...........',                   'shr {0}, {1}, {2}',            'rrr', ''),
    ('EQ',      '3r',    '00110...........',                   'eq {0}, {1}, {2}',             'rrr', ''),
    ('AND',     '3r',    '00111...........',                   'and {0}, {1}, {2}',            'rrr', ''),
    ('OR',      '3r',    '01000...........',                   'or {0}, {1}, {2}',             'rrr', ''),
    ('LDW',     '3r',    '01001...........',                   'ldw {0}, {1}[{2}]',            'rrr', ''),
    ('LD16S',   '3r',    '10000...........',                   'ld16s {0}, {1}[{2}]',          'rrr', ''),
    ('LD8U',    '3r',    '10001...........',                   'ld8u {0}, {1}[{2}]',           'rrr', ''),
    ('ADDI',    '2rus',  '10010...........',                   'add {0}, {1}, {2}',            'rru', ''),
    ('SUBI',    '2rus',  '10011...........',                   'sub {0}, {1}, {2}',            'rru', ''),
    ('SHLI',    '2rus',  '10100...........',                   'shl {0}, {1}, {2}',            'rrb', ''),
    ('SHRI',    '2rus',  '10101...........',                   'shr {0}, {1}, {2}',            'rrb', ''),
    ('EQI',     '2rus',  '10110...........',                   'eq {0}, {1}, {2}',             'rru', ''),
    ('TSETR',   '3r',    '10111...........',                   'set t[{2}]:r{0}, {1}',         'urr', 'xs1'),
    ('LSS',     '3r',    '11000...........',                   'lss {0}, {1}, {2}',            'rrr', ''),
    ('LSU',     '3r',    '11001...........',                   'lsu {0}, {1}, {2}',            'rrr', ''),
    # ru6 / lru6 (register 12..15 in the ru6 register slot selects the u6 group below)
    ('STWDP',   'ru6',   '010100..........',                   'stw {0}, dp[{1}]',             'Ru', ''),
    ('STWSP',   'ru6',   '010101..........',                   'stw {0}, sp[{1}]',             'Ru', ''),
    ('LDWDP',   'ru6',   '010110..........',                   'ldw {0}, dp[{1}]',             'Ru', ''),
    ('LDWSP',   'ru6',   '010111..........',                   'ldw {0}, sp[{1}]',             'Ru', ''),
    ('LDAWDP',  'ru6',   '011000..........',                   'ldaw {0}, dp[{1}]',            'Ru', ''),
    ('LDAWSP',  'ru6',   '011001..........',                   'ldaw {0}, sp[{1}]',            'Ru', ''),
    ('LDC',     'ru6',   '011010..........',                   'ldc {0}, {1}',                 'Ru', ''),
    ('LDWCP',   'ru6',   '011011..........',                   'ldw {0}, cp[{1}]',             'Ru', ''),
    ('BRFT',    'ru6',   '011100..........',                   'bt {0}, {1}',                  'rF', ''),
    ('BRBT',    'ru6',   '011101..........',                   'bt {0}, {1}' ,                 'rB', ''),
    ('BRFF',    'ru6',   '011110..........',                   'bf {0}, {1}',                  'rF', ''),
    ('BRBF',    'ru6',   '011111..........',                   'bf {0}, {1}' ,                 'rB', ''),
    ('SETCI',   'ru6',   '111010..........',                   'setc res[{0}], {1}',           'ru', ''),
    # u6 / lu6
    ('BRFU',    'u6',    '0111001100......',                   'bu {0}',                       'F', ''),
    ('BLAT',    'u6',    '0111001101......',                   'blat {0}',                     'u', ''),
    ('EXTDP',   'u6',    '0111001110......',                   'extdp {0}',                    'u', ''),
    ('KCALLI',  'u6',    '0111001111......',                   'kcall {0}',                    'u', ''),
    ('BRBU',    'u6',    '0111011100......',                   'bu {0}' ,                      'B', ''),
    ('ENTSP',   'u6',    '0111011101......',                   'entsp {0}',                    'u', ''),
    ('EXTSP',   'u6',    '0111011110......',                   'extsp {0}',                    'u', ''),
    ('RETSP',   'u6',    '0111011111......',                   'retsp {0}',                    'u', ''),
    ('CLRSR',   'u6',    '0111101100......',                   'clrsr {0}',                    'u', ''),
    ('SETSR',   'u6',    '0111101101......',                   'setsr {0}',                    'u', ''),
    ('KENTSP',  'u6',    '0111101110......',                   'kentsp {0}',                   'u', ''),
    ('KRESTSP', 'u6',    '0111101111......',                   'krestsp {0}',                  'u', ''),
    ('GETSR',   'u6',    '0111111100......',                   'getsr r11, {0}',               'u', ''),
    ('LDAWCP',  'u6',    '0111111101......',                   'ldaw r11, cp[{0}]',            'u', ''),
    ('DUALENTSP', 'u6',  '0111111110......',                   'dualentsp {0}',                'u', 'xs2'),
    # u10 / lu10
    ('BLRF',    'u10',   '110100..........',                   'bl {0}',                       'F', ''),
    ('BLRB',    'u10',   '110101..........',                   'bl {0}' ,                      'B', ''),
    ('LDAPF',   'u10',   '110110..........',                   'ldap r11, {0}',                'F', ''),
    ('LDAPB',   'u10',   '110111..........',                   'ldap r11, {0}' ,               'B', ''),
    ('BLACP',   'u10',   '111000..........',                   'bla cp[{0}]',                  'u', ''),
    ('LDWCPL',  'u10',   '111001..........',                   'ldw r11, cp[{0}]',             'u', ''),
    # 2r / r2r / rus: opcode in bits 15..11 and bit 4
    ('TINITPC', '2r',    '00000......0....',                   'init t[{1}]:pc, {0}',          'rr', 'xs1'),
    ('BYTEREV', '2r',    '00000......0....',                   'byterev {0}, {1}',             'rr', 'xs2'),
    ('GETST',   '2r',    '00000......1....',                   'getst {0}, res[{1}]',          'rr', ''),
    ('TINITDP', '2r',    '00001......0....',                   'init t[{1}]:dp, {0}',          'rr', 'xs1'),
    ('CLZ',     '2r',    '00001......0....',                   'clz {0}, {1}',                 'rr', 'xs2'),
    ('OUTT',    'r2r',   '00001......1....',                   'outt res[{0}], {1}',           'rr', ''),
    ('TINITSP', '2r',    '00010......0....',                   'init t[{1}]:sp, {0}',          'rr', 'xs1'),
    ('BITREV',  '2r',    '00010......0....',                   'bitrev {0}, {1}',              'rr', 'xs2'),
    ('SETD',    'r2r',   '00010......1....',                   'setd res[{0}], {1}',           'rr', ''),
    ('TINITCP', '2r',    '00011......0....',                   'init t[{1}]:cp, {0}',          'rr', 'xs1'),
    ('TSETMR',  '2r',    '00011......1....',                   'tsetmr {0}, {1}',              'rr', 'xs1'),
    ('EET',     '2r',    '00100......1....',                   'eet {0}, res[{1}]',            'rr', ''),
    ('ANDNOT',  '2r',    '00101......0....',                   'andnot {0}, {1}',              'rr', ''),
    ('EEF',     '2r',    '00101......1....',                   'eef {0}, res[{1}]',            'rr', ''),
    ('SEXT',    '2r',    '00110......0....',                   'sext {0}, {1}',                'rr', ''),
    ('SEXTI',   'rus',   '00110......1....',                   'sext {0}, {1}',                'rb', ''),
    ('GETTS',   '2r',    '00111......0....',                   'getts {0}, res[{1}]',          'rr', ''),
    ('SETPT',   'r2r',   '00111......1....',                   'setpt res[{0}], {1}',          'rr', ''),
    ('ZEXT',    '2r',    '01000......0....',                   'zext {0}, {1}',                'rr', ''),
    ('ZEXTI',   'rus',   '01000......1....',                   'zext {0}, {1}',                'rb', ''),
    ('OUTCT',   '2r',    '01001......0....',                   'outct res[{0}], {1}',          'rr', ''),
    ('OUTCTI',  'rus',   '01001......1....',                   'outct res[{0}], {1}',          'ru', ''),
    ('GETR',    'rus',   '10000......0....',                   'getr {0}, {1}',                'ru', ''),
    ('INCT',    '2r',    '10000......1....',                   'inct {0}, res[{1}]',           'rr', ''),
    ('NOT',     '2r',    '10001......0....',                   'not {0}, {1}',                 'rr', ''),
    ('INT',     '2r',    '10001......1....',                   'int {0}, res[{1}]',            'rr', ''),
    ('NEG',     '2r',    '10010......0....',                   'neg {0}, {1}',                 'rr', ''),
    ('ENDIN',   '2r',    '10010......1....',                   'endin {0}, res[{1}]',          'rr', ''),
    ('MKMSK',   '2r',    '10100......0....',                   'mkmsk {0}, {1}',               'rr', ''),
    ('MKMSKI',  'rus',   '10100......1....',                   'mkmsk {0}, {1}',               'rb', ''),
    ('OUT',     'r2r',   '10101......0....',                   'out res[{0}], {1}',            'rr', ''),
    ('OUTSHR',  'r2r',   '10101......1....',                   'outshr res[{0}], {1}',         'rr', ''),
    ('IN',      '2r',    '10110......0....',                   'in {0}, res[{1}]',             'rr', ''),
    ('INSHR',   '2r',    '10110......1....',                   'inshr {0}, res[{1}]',          'rr', ''),
    ('PEEK',    '2r',    '10111......0....',                   'peek {0}, res[{1}]',           'rr', ''),
    ('TESTCT',  '2r',    '10111......1....',                   'testct {0}, res[{1}]',         'rr', ''),
    ('SETPSC',  'r2r',   '11000......0....',                   'setpsc res[{0}], {1}',         'rr', ''),
    ('TESTWCT', '2r',    '11000......1....',                   'testwct {0}, res[{1}]',        'rr', ''),
    ('CHKCT',   '2r',    '11001......0....',                   'chkct res[{0}], {1}',          'rr', ''),
    ('CHKCTI',  'rus',   '11001......1....',                   'chkct res[{0}], {1}',          'ru', ''),
    # 1r: bits 10..5 = 111111
    ('EDU',     '1r',    '000001111110....',                   'edu res[{0}]',                 'r', ''),
    ('EEU',     '1r',    '000001111111....',                   'eeu res[{0}]',                 'r', ''),
    ('WAITET',  '1r',    '000011111110....',                   'waitet {0}',                   'r', ''),
    ('WAITEF',  '1r',    '000011111111....',                   'waitef {0}',                   'r', ''),
    ('FREER',   '1r',    '000101111110....',                   'freer res[{0}]',               'r', ''),
    ('MJOIN',   '1r',    '000101111111....',                   'mjoin res[{0}]',               'r', ''),
    ('TSTART',  '1r',    '000111111110....',                   'start t[{0}]',                 'r', ''),
    ('MSYNC',   '1r',    '000111111111....',                   'msync res[{0}]',               'r', ''),
    ('BLA',     '1r',    '001001111110....',                   'bla {0}',                      'r', ''),
    ('BAU',     '1r',    '001001111111....',                   'bau {0}',                      'r', ''),
    ('BRU',     '1r',    '001011111110....',                   'bru {0}',                      'r', ''),
    ('SETSP',   '1r',    '001011111111....',                   'set sp, {0}',                  'r', ''),
    ('SETDP',   '1r',    '001101111110....',                   'set dp, {0}',                  'r', ''),
    ('SETCP',   '1r',    '001101111111....',                   'set cp, {0}',                  'r', ''),
    ('DGETREG', '1r',    '001111111110....',                   'dgetreg {0}',                  'r', ''),
    ('SETEV',   '1r',    '001111111111....',                   'setev res[{0}], r11',          'r', ''),
    ('KCALL',   '1r',    '010001111110....',                   'kcall {0}',                    'r', ''),
    ('SETV',    '1r',    '010001111111....',                   'setv res[{0}], r11',           'r', ''),
    ('ECALLF',  '1r',    '010011111110....',                   'ecallf {0}',                   'r', ''),
    ('ECALLT',  '1r',    '010011111111....',                   'ecallt {0}',                   'r', ''),
    ('CLRPT',   '1r',    '100001111110....',                   'clrpt res[{0}]',               'r', ''),
    ('SYNCR',   '1r',    '100001111111....',                   'syncr res[{0}]',               'r', ''),
    ('GETTIME', '1r',    '100011111110....',                   'gettime {0}',                  'r', 'xs2'),
    ('ELATE',   '1r',    '100011111111....',                   'elate {0}',                    'r', 'xs2'),
    # 0r
    ('WAITEU',  '0r',    '0000011111101100',                   'waiteu',                       '', ''),
    ('CLRE',    '0r',    '0000011111101101',                   'clre',                         '', ''),
    ('SSYNC',   '0r',    '0000011111101110',                   'ssync',                        '', ''),
    ('FREET',   '0r',    '0000011111101111',                   'freet',                        '', ''),
    ('DCALL',   '0r',    '0000011111111100',                   'dcall',                        '', ''),
    ('KRET',    '0r',    '0000011111111101',                   'kret',                         '', 'xs1'),
    ('DRET',    '0r',    '0000011111111110',                   'dret',                         '', 'xs1'),
    ('SETKEP',  '0r',    '0000011111111111',                   'set kep, r11',                 '', ''),
    ('LDSPC',   '0r',    '0000111111101100',                   'ldw spc, sp[1]',               '', ''),
    ('STSPC',   '0r',    '0000111111101101',                   'stw spc, sp[1]',               '', ''),
    ('LDSSR',   '0r',    '0000111111101110',                   'ldw ssr, sp[2]',               '', ''),
    ('STSSR',   '0r',    '0000111111101111',                   'stw ssr, sp[2]',               '', ''),
    ('STSED',   '0r',    '0000111111111100',                   'stw sed, sp[3]',               '', ''),
    ('STET',    '0r',    '0000111111111101',                   'stw et, sp[4]',                '', ''),
    ('GETED',   '0r',    '0000111111111110',                   'get r11, ed',                  '', ''),
    ('GETET',   '0r',    '0000111111111111',                   'get r11, et',                  '', ''),
    ('DENTSP',  '0r',    '0001011111101100',                   'dentsp',                       '', 'xs1'),
    ('DRESTSP', '0r',    '0001011111101101',                   'drestsp',                      '', 'xs1'),
    ('GETID',   '0r',    '0001011111101110',                   'get r11, id',                  '', ''),
    ('GETKEP',  '0r',    '0001011111101111',                   'get r11, kep',                 '', ''),
    ('GETKSP',  '0r',    '0001011111111100',                   'get r11, ksp',                 '', ''),
    ('LDSED',   '0r',    '0001011111111101',                   'ldw sed, sp[3]',               '', ''),
    ('LDET',    '0r',    '0001011111111110',                   'ldw et, sp[4]',                '', ''),
    ('NOP',     '0r',    '0001011111111111',                   'nop',                          '', 'xs2'),
    # l3r / l2rus: 11111 + 3r field, then a fixed second halfword
    ('STW',     'l3r',   '11111...........|0000011111101100',  'stw {0}, {1}[{2}]',            'rrr', ''),
    ('XOR',     'l3r',   '11111...........|0000111111101100',  'xor {0}, {1}, {2}',            'rrr', ''),
    ('ASHR',    'l3r',   '11111...........|0001011111101100',  'ashr {0}, {1}, {2}',           'rrr', ''),
    ('LDAWF',   'l3r',   '11111...........|0001111111101100',  'ldaw {0}, {1}[{2}]',           'rrr', ''),
    ('LDAWB',   'l3r',   '11111...........|0010011111101100',  'ldaw {0}, {1}[-{2}]',          'rrr', ''),
    ('LDA16F',  'l3r',   '11111...........|0010111111101100',  'lda16 {0}, {1}[{2}]',          'rrr', ''),
    ('LDA16B',  'l3r',   '11111...........|0011011111101100',  'lda16 {0}, {1}[-{2}]',         'rrr', ''),
    ('MUL',     'l3r',   '11111...........|0011111111101100',  'mul {0}, {1}, {2}',            'rrr', ''),
    ('DIVS',    'l3r',   '11111...........|0100011111101100',  'divs {0}, {1}, {2}',           'rrr', ''),
    ('DIVU',    'l3r',   '11111...........|0100111111101100',  'divu {0}, {1}, {2}',           'rrr', ''),
    ('ST16',    'l3r',   '11111...........|1000011111101100',  'st16 {0}, {1}[{2}]',           'rrr', ''),
    ('ST8',     'l3r',   '11111...........|1000111111101100',  'st8 {0}, {1}[{2}]',            'rrr', ''),
    ('CRC',     'l3r',   '11111...........|1010111111101100',  'crc32 {0}, {1}, {2}',          'rrr', ''),
    ('TSETR',   'l3r',   '11111...........|1011011111101100',  'set t[{2}]:r{0}, {1}',         'urr', 'xs2'),
    ('REMS',    'l3r',   '11111...........|1100011111101100',  'rems {0}, {1}, {2}',           'rrr', ''),
    ('REMU',    'l3r',   '11111...........|1100111111101100',  'remu {0}, {1}, {2}',           'rrr', ''),
    ('OUTPW',   'l3r',   '11111...........|1100111111101101',  'outpw res[{1}], {0}, {2}',     'rrr', 'xs2'),
    ('LSATS',   'l3r',   '11111...........|1110011111101100',  'lsats {0}, {1}, {2}',          'rrr', 'xs2'),
    ('ASHRI',   'l2rus', '11111...........|1001011111101100',  'ashr {0}, {1}, {2}',           'rrb', ''),
    ('LDAWFI',  'l2rus', '11111...........|1001111111101100',  'ldaw {0}, {1}[{2}]',           'rru', ''),
    ('LDAWBI',  'l2rus', '11111...........|1010011111101100',  'ldaw {0}, {1}[-{2}]',          'rru', ''),
    ('LDDSP',   'l2rus', '11111...........|1110111111101100',  'ldd {0}, {1}, sp[{2}]',        'rru', 'xs2'),
    ('STDSP',   'l2rus', '11111...........|1111011111101100',  'std {0}, {1}, sp[{2}]',        'rru', 'xs2'),
    ('OUTPWI',  'l2rus', '11111...........|1001011111101101',  'outpw res[{1}], {0}, {2}',     'rrb', ''),
    ('UNZIP',   'l2rus', '11111...........|1001111111101101',  'unzip {0}, {1}, {2}',          'rru', 'xs2'),
    ('INPW',    'l2rus', '11111...........|1001011111101110',  'inpw {0}, res[{1}], {2}',      'rrb', ''),
    ('ZIP',     'l2rus', '11111...........|1001111111101110',  'zip {0}, {1}, {2}',            'rru', 'xs2'),
    # l2r / lr2r: 11111 + 2r field (bit 4 is opcode), then a fixed second halfword
    ('BITREV',  'l2r',   '11111......0....|0000011111101100',  'bitrev {0}, {1}',              'rr', 'xs1'),
    ('BYTEREV', 'l2r',   '11111......1....|0000011111101100',  'byterev {0}, {1}',             'rr', 'xs1'),
    ('TINITSP', 'l2r',   '11111......0....|0000011111101100',  'init t[{1}]:sp, {0}',          'rr', 'xs2'),
    ('TINITPC', 'l2r',   '11111......1....|0000011111101100',  'init t[{1}]:pc, {0}',          'rr', 'xs2'),
    ('TINITCP', 'l2r',   '11111......0....|0000011111101101',  'init t[{1}]:cp, {0}',          'rr', 'xs2'),
    ('CLZ',     'l2r',   '11111......0....|0000111111101100',  'clz {0}, {1}',                 'rr', 'xs1'),
    ('TINITDP', 'l2r',   '11111......0....|0000111111101100',  'init t[{1}]:dp, {0}',          'rr', 'xs2'),
    ('SETCLK',  'lr2r',  '11111......1....|0000111111101100',  'setclk res[{0}], {1}',         'rr', ''),
    ('TSETMR',  'l2r',   '11111......0....|0000111111101101',  'tsetmr {0}, {1}',              'rr', 'xs2'),
    ('TINITLR', 'l2r',   '11111......0....|0001011111101100',  'init t[{1}]:lr, {0}',          'rr', ''),
    ('GETPS',   'l2r',   '11111......1....|0001011111101100',  'get {0}, ps[{1}]',             'rr', ''),
    ('SETPS',   'lr2r',  '11111......0....|0001111111101100',  'set ps[{0}], {1}',             'rr', ''),
    ('GETD',    'l2r',   '11111......1....|0001111111101100',  'getd {0}, res[{1}]',           'rr', ''),
    ('TESTLCL', 'l2r',   '11111......0....|0010011111101100',  'testlcl {0}, res[{1}]',        'rr', ''),
    ('SETTW',   'lr2r',  '11111......1....|0010011111101100',  'settw res[{0}], {1}',          'rr', ''),
    ('SETRDY',  'lr2r',  '11111......0....|0010111111101100',  'setrdy res[{0}], {1}',         'rr', ''),
    ('SETC',    'l2r',   '11111......1....|0010111111101100',  'setc res[{0}], {1}',           'rr', ''),
    ('SETN',    'lr2r',  '11111......0....|0011011111101100',  'setn res[{0}], {1}',           'rr', ''),
    ('GETN',    'l2r',   '11111......1....|0011011111101100',  'getn {0}, res[{1}]',           'rr', ''),
    # l0r (xCORE-200 moved these from 0r)
    ('KRET',    'l0r',   '1111111111101100|0000011111101100',  'kret',                         '', 'xs2'),
    ('DRET',    'l0r',   '1111111111101100|0000111111101100',  'dret',                         '', 'xs2'),
    ('DENTSP',  'l0r',   '1111111111101100|0001011111101100',  'dentsp',                       '', 'xs2'),
    ('DRESTSP', 'l0r',   '1111111111101100|0001111111101100',  'drestsp',                      '', 'xs2'),
    # l4r / l3rus: 3r field, then opcode with a fourth register in bits 3..0
    ('CRC8',    'l4r',   '11111...........|000001111110....',  'crc8 {3}, {0}, {1}, {2}',      'rrrr', ''),
    ('MACCU',   'l4r',   '11111...........|000001111111....',  'maccu {0}, {3}, {1}, {2}',     'rrrr', ''),
    ('MACCS',   'l4r',   '11111...........|000011111110....',  'maccs {0}, {3}, {1}, {2}',     'rrrr', ''),
    ('CRCN',    'l4r',   '11111...........|000011111111....',  'crcn {0}, {3}, {1}, {2}',      'rrrr', 'xs2'),
    ('STD',     'l4r',   '11111...........|000101111110....',  'std {0}, {3}, {1}[{2}]',       'rrrr', 'xs2'),
    ('STDI',    'l3rus', '11111...........|000101111111....',  'std {0}, {3}, {1}[{2}]',       'rrur', 'xs2'),
    ('LDD',     'l4r',   '11111...........|001001111110....',  'ldd {0}, {3}, {1}[{2}]',       'rrrr', 'xs2'),
    ('LDDI',    'l3rus', '11111...........|001001111111....',  'ldd {0}, {3}, {1}[{2}]',       'rrur', 'xs2'),
    # l5r / l4rus: 3r field, then opcode in bits 15..11 and 4 with a 2r field
    ('LDIVU',   'l5r',   '11111...........|00000......0....',  'ldivu {0}, {3}, {4}, {1}, {2}', 'rrrrr', ''),
    ('LADD',    'l5r',   '11111...........|00000......1....',  'ladd {3}, {0}, {1}, {2}, {4}', 'rrrrr', ''),
    ('LSUB',    'l5r',   '11111...........|00001......0....',  'lsub {3}, {0}, {1}, {2}, {4}', 'rrrrr', ''),
    ('XOR4',    'l5r',   '11111...........|00001......1....',  'xor4 {0}, {3}, {1}, {2}, {4}', 'rrrrr', 'xs2'),
    ('LEXTRACT', 'l4rus', '11111...........|00011......0....', 'lextract {0}, {3}, {1}, {2}, {4}', 'rrrrb', 'xs2'),
    ('LINSERT', 'l4rus', '11111...........|00011......1....',  'linsert {0}, {3}, {1}, {2}, {4}', 'rrrrb', 'xs2'),
    ('CRC32_INC', 'l4rus', '11111...........|00101......1....', 'crc32_inc {0}, {3}, {1}, {2}, {4}', 'rrrrb', 'xs2'),
    # l6r
    ('LMUL',    'l6r',   '11111...........|00000...........',  'lmul {0}, {3}, {1}, {2}, {4}, {5}', 'rrrrrr', ''),
]

# lru6/lu6/lu10: every ru6/u6/u10 instruction with a 111100 (PFIX) halfword in front
PFIX = '111100..........|'
OPS += [(n, 'l' + f, PFIX + p, s, k, i) for (n, f, p, s, k, i) in list(OPS) if f in ('ru6', 'u6', 'u10')]


def _mask(pat):
    bits = pat.replace('|', '')
    halves = [bits[:16], bits[16:]] if len(bits) == 32 else [bits]
    m = v = 0
    for h, half in enumerate(halves):
        for i, ch in enumerate(half):
            b = 1 << (15 - i + 16 * h)
            if ch != '.':
                m |= b
                v |= b if ch == '1' else 0
    return m, v


class Op:
    __slots__ = ('name', 'fmt', 'mask', 'val', 'syntax', 'kinds', 'isa', 'size')

    def __init__(self, name, fmt, pat, syntax, kinds, isa):
        self.name, self.fmt, self.syntax, self.kinds, self.isa = name, fmt, syntax, kinds, isa
        self.mask, self.val = _mask(pat)
        self.size = 2 * FMT[fmt][0]
        assert FMT[fmt][1] == len(kinds), name


TABLE = [Op(*e) for e in OPS]


def table(isa):
    """Lookup: (size, key halfword) -> candidate ops; 16-bit ops keyed by top 5 bits of the
    halfword, 32-bit ops keyed by top 5 bits of the second halfword."""
    t = {}
    for op in TABLE:
        if op.isa and op.isa != isa:
            continue
        key = (op.val >> (27 if op.size == 4 else 11)) & 31
        for k in ([key] if op.mask >> (27 if op.size == 4 else 11) & 31 == 31 else range(32)):
            t.setdefault((op.size, k), []).append(op)
    return t


class Insn:
    __slots__ = ('addr', 'size', 'word', 'op', 'ops')

    def __init__(self, addr, size, word, op, ops):
        self.addr, self.size, self.word, self.op, self.ops = addr, size, word, op, ops

    def targets(self):
        """pc-relative targets (byte addresses) of this instruction."""
        nxt = self.addr + self.size
        return [nxt + 2 * v if k == 'F' else nxt - 2 * v
                for k, v in zip(self.op.kinds, self.ops) if k in 'FB'] if self.op else []

    def text(self):
        if not self.op:
            return '.short 0x%04x' % (self.word & 0xffff)
        vals = []
        for k, v in zip(self.op.kinds, self.ops):
            vals.append(REGS[v] if k in 'rR' else str(BITP[v] if k == 'b' else -v if k == 'B' else v))
        return self.op.syntax.format(*vals)


def _match(cands, w):
    for op in cands:
        if w & op.mask == op.val:
            o = operands(op.fmt, w)
            # ru6 register slot: r0..r11 only, except 'R' (cp/dp/sp/lr allowed); 12..15 of the
            # 0111xx group are the u6 instructions
            if o is not None and all(v < 12 for k, v in zip(op.kinds, o) if k == 'r'):
                return op, o
    return None


def decode(buf, off, addr, tab):
    """Decode one instruction at buf[off:]. Returns an Insn (op None for an unknown halfword)."""
    h0 = struct.unpack_from('<H', buf, off)[0]
    if off + 4 <= len(buf) and (h0 >> 11 == 31 or h0 >> 10 == 0x3c):
        h1 = struct.unpack_from('<H', buf, off + 2)[0]
        w = h0 | h1 << 16
        m = _match(tab.get((4, h1 >> 11), ()), w)
        if m:
            return Insn(addr, 4, w, *m)
    m = _match(tab.get((2, h0 >> 11), ()), h0)
    return Insn(addr, 2, h0, *(m or (None, None)))


def encode(name, ops, isa='xs1', long=False):
    """Instruction bytes for table entry `name` (manual mnemonic) with raw operand values in
    encoding order. long=True picks the PFIX/long variant when both exist."""
    for op in TABLE:
        if op.name == name and (not op.isa or op.isa == isa) and (op.size == 4) == long:
            w = op.val | pack(op.fmt, ops)
            return struct.pack('<I' if op.size == 4 else '<H', w)
    raise KeyError(name)


def disasm(buf, base, isa='xs1'):
    tab = table(isa)
    out, off = [], 0
    while off + 2 <= len(buf):
        i = decode(buf, off, base + off, tab)
        out.append(i)
        off += i.size
    return out


# ---------------------------------------------------------------------------------------------
# Input: ELF, raw, or an xflash image (SPI byte order is bit-reversed).

REV8 = bytes(int('{:08b}'.format(i)[::-1], 2) for i in range(256))


def read_elf(data):
    """Executable sections and symbols of an ELF32 little-endian file."""
    (shoff,) = struct.unpack_from('<I', data, 32)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 46)
    sh = [struct.unpack_from('<10I', data, shoff + i * shentsize) for i in range(shnum)]
    secs, syms = [], {}
    for name, typ, flags, addr, off, size, link, _, _, entsize in sh:
        if typ == 1 and flags & 4:
            secs.append((addr, data[off:off + size]))
        if typ == 2:
            stroff = sh[link][4]
            for j in range(0, size, 16):
                n, val, _, info, _, shn = struct.unpack_from('<IIIBBH', data, off + j)
                s = data[stroff + n:data.index(b'\0', stroff + n)].decode(errors='replace')
                if s and shn and (info & 15) in (0, 2):
                    syms.setdefault(val, s)
    return secs, syms


def flash_segments(img):
    """Per-tile code segments of an xflash/upgrade image whose bytes are in SPI (bit-reversed)
    order. Layout as observed: 13-word header, word 6 = segment count, then per segment
    {byte offset, flags, size in words}; each segment ends with a checksum word."""
    d = img.translate(REV8)
    n = struct.unpack_from('<I', d, 24)[0]
    segs = []
    for k in range(n):
        off, flags, words = struct.unpack_from('<3I', d, 28 + 12 * k)
        segs.append((off, flags, d[off:off + 4 * words]))
    return segs


def listing(insns, syms=None, out=sys.stdout):
    syms = dict(syms or {})
    calls = {t for i in insns if i.op and i.op.name in ('BLRF', 'BLRB') for t in i.targets()}
    for i in insns:
        for t in i.targets():
            if t not in syms:
                syms[t] = ('f_%x' if t in calls else 'L_%x') % t
    for i in insns:
        if i.addr in syms:
            out.write('\n%s:\n' % syms[i.addr])
        hw = ' '.join('%04x' % (i.word >> s & 0xffff) for s in range(0, 8 * i.size, 16))
        t = i.text()
        tg = i.targets()
        if tg:
            t = '%-32s ; -> %s' % (t, ', '.join('0x%x <%s>' % (x, syms[x]) for x in tg))
        out.write('%8x:  %-10s %s\n' % (i.addr, hw, t))


def selftest():
    bad = 0
    for isa in ('xs1', 'xs2'):
        tab = table(isa)
        for h in range(65536):
            if h >> 11 == 31 or h >> 10 == 0x3c:
                continue
            i = decode(struct.pack('<H', h), 0, 0, tab)
            if i.op and encode(i.op.name, i.ops, isa, False) != struct.pack('<H', h):
                bad += 1
                print('mismatch', isa, hex(h), i.op.name, i.ops)
    for op in TABLE:   # every 32-bit entry with sample operands
        if op.size == 4:
            ops = [{'r': 5, 'R': 13, 'u': 7, 'b': 9, 'F': 0x123, 'B': 0x45}[k] for k in op.kinds]
            ops = [v if op.fmt not in ('lu10',) else 0x1234 for v in ops]
            b = encode(op.name, ops, op.isa or 'xs1', True)
            i = decode(b, 0, 0, table(op.isa or 'xs1'))
            if (i.op is not op and i.op.name != op.name) or i.ops != ops:
                bad += 1
                print('mismatch', op.name, ops, i.op and i.op.name, i.ops)
    print('selftest:', 'ok' if not bad else '%d failures' % bad)
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file', nargs='?')
    ap.add_argument('--isa', default='xs1', choices=('xs1', 'xs2'))
    ap.add_argument('--base', type=lambda s: int(s, 0), default=0x10000)
    ap.add_argument('--offset', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--length', type=lambda s: int(s, 0))
    ap.add_argument('--raw', action='store_true', help='treat FILE as a raw blob even if it is an ELF')
    ap.add_argument('--bitrev', action='store_true', help='bit-reverse every byte first (SPI flash order)')
    ap.add_argument('--flash', action='store_true', help='FILE is an xflash image: disassemble each segment')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if not a.file:
        ap.error('FILE required')
    data = open(a.file, 'rb').read()
    if a.flash:
        for k, (off, flags, seg) in enumerate(flash_segments(data)):
            print('; segment %d: image offset 0x%x, flags 0x%08x, %d bytes (last word: checksum)'
                  % (k, off, flags, len(seg)))
            listing(disasm(seg[:-4], a.base, a.isa))
        return
    if a.bitrev:
        data = data.translate(REV8)
    if data[:4] == b'\x7fELF' and not a.raw:
        secs, syms = read_elf(data)
        for addr, body in secs:
            listing(disasm(body, addr, a.isa), syms)
        return
    data = data[a.offset:a.offset + a.length if a.length else None]
    listing(disasm(data, a.base, a.isa))


if __name__ == '__main__':
    main()
