#!/usr/bin/env python3
"""cc-compatible driver for XS1: `xcc.py [clang flags] -c FILE.c -o FILE.o` compiles to XS1
assembly (see compile_asm) and runs xas.py, so a Makefile's usual `$(CC) $(CFLAGS) -c` rule
works. Other invocations (no -c) are passed to clang unchanged. -MMD/-MD write FILE.d as gcc
does. The LLVM binaries: $XCORE_LLVM (a bin directory), default Homebrew LLVM 22.

LLVM's XCore backend bug worked around here: a stack object whose alignment is below 4
(a char array, a uint8_t whose address is taken) can get a frame offset that is not a
multiple of 4, and XCore addresses frame objects with `ldaw r, sp[offset / 4]`, which drops
the remainder: two such objects can then overlap (seen with two uint8_t[10] buffers at -Oz,
and at -O0 in the host tests). So C is compiled to LLVM IR, every alloca is given an
alignment of at least 4, and llc makes the assembly."""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LLVM = os.environ.get('XCORE_LLVM', '/opt/homebrew/opt/llvm@22/bin')
CLANG = os.environ.get('XCORE_CLANG', os.path.join(LLVM, 'clang'))
LLC = os.path.join(os.path.dirname(CLANG), 'llc')
ALLOCA = re.compile(r'(= alloca [^\n]*?, align )([12])\b')


def compile_asm(src, flags, out, dep=()):
    """C source -> XS1 assembly file `out`. Returns the exit status."""
    ll = out + '.ll'
    r = subprocess.call([CLANG, '--target=xcore'] + list(flags) + list(dep) + ['-S', '-emit-llvm', src, '-o', ll])
    if r:
        return r
    text = open(ll).read()
    open(ll, 'w').write(ALLOCA.sub(r'\g<1>4', text))
    sections = []
    if '-ffunction-sections' in flags:
        sections.append('-function-sections')
    if '-fdata-sections' in flags:
        sections.append('-data-sections')
    opt = '-O0' if '-O0' in flags else '-O2'
    r = subprocess.call([LLC, '-mtriple=xcore', opt] + sections + [ll, '-o', out])
    os.remove(ll)
    return r


def main():
    args = sys.argv[1:]
    if '-c' not in args:
        sys.exit(subprocess.call([CLANG, '--target=xcore'] + args))
    out = args[args.index('-o') + 1] if '-o' in args else None
    flags = [a for i, a in enumerate(args) if a != '-c' and a != '-o' and (i == 0 or args[i - 1] != '-o')]
    src = [a for a in flags if a.endswith('.c') or a.endswith('.s')]
    if len(src) != 1:
        sys.exit('xcc: one source file per -c, got %s' % src)
    flags.remove(src[0])
    out = out or os.path.splitext(os.path.basename(src[0]))[0] + '.o'
    asm = out[:-2] + '.s' if out.endswith('.o') else out + '.s'
    if src[0].endswith('.s'):
        asm = src[0]
    else:
        dep = []
        if ('-MMD' in flags or '-MD' in flags) and '-MF' not in flags:
            dep = ['-MF', out[:-2] + '.d', '-MT', out]
        r = compile_asm(src[0], flags, asm, dep)
        if r:
            sys.exit(r)
    sys.exit(subprocess.call([sys.executable, os.path.join(HERE, 'xas.py'), asm, '-o', out]))


if __name__ == '__main__':
    main()
