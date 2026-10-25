#!/usr/bin/env python3
"""cc-compatible driver for XS1: `xcc.py [clang flags] -c FILE.c -o FILE.o` runs
clang --target=xcore -S, then xas.py, so a Makefile's usual `$(CC) $(CFLAGS) -c` rule works.
Other invocations (no -c) are passed to clang unchanged. -MMD/-MD write FILE.d as gcc does.
The clang binary: $XCORE_CLANG, default Homebrew LLVM 22."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('XCORE_CLANG', '/opt/homebrew/opt/llvm@22/bin/clang')


def main():
    args = sys.argv[1:]
    if '-c' not in args:
        sys.exit(subprocess.call([CLANG, '--target=xcore'] + args))
    out = args[args.index('-o') + 1] if '-o' in args else None
    flags = [a for i, a in enumerate(args) if a != '-c' and a != '-o' and (i == 0 or args[i - 1] != '-o')]
    src = [a for a in flags if a.endswith('.c') or a.endswith('.s')]
    if len(src) != 1:
        sys.exit('xcc: one source file per -c, got %s' % src)
    out = out or os.path.splitext(os.path.basename(src[0]))[0] + '.o'
    asm = out[:-2] + '.s' if out.endswith('.o') else out + '.s'
    if src[0].endswith('.s'):
        asm = src[0]
    else:
        dep = []
        if ('-MMD' in flags or '-MD' in flags) and '-MF' not in flags:
            dep = ['-MF', out[:-2] + '.d', '-MT', out]
        r = subprocess.call([CLANG, '--target=xcore'] + flags + dep + ['-S', '-o', asm])
        if r:
            sys.exit(r)
    r = subprocess.call([sys.executable, os.path.join(HERE, 'xas.py'), asm, '-o', out])
    sys.exit(r)


if __name__ == '__main__':
    main()
