# XS1 tile startup, assembled with tools/xcore/xas.py and placed first by xld.py.
# The XMOS boot loader jumps to the start of the tile image (RAM base) on thread 0.
# Linker symbols: _cp/_dp (constant and data pool bases), _bss_start/_bss_end, _stack_top.
# Leaves chanend 0 allocated in xs1_chanend0 (rt.c).

	.text
	.globl	_start
_start:
	getr	r4, 2			# chanend 0, first: the tile link (link.h) gets its tokens
				# even while bss is still being cleared (NI's tile 1 speaks at once)
	ldap	r11, _cp
	set	cp, r11
	ldap	r11, _dp
	set	dp, r11
	ldap	r11, _stack_top
	set	sp, r11
	ldap	r11, _trap		# exceptions and kcall enter the kernel at kep
	set	kep, r11
	ldap	r11, _bss_start		# zero bss, one word at a time (both ends word aligned)
	mov	r0, r11
	ldap	r11, _bss_end
	ldc	r1, 0
.Lzero:
	lsu	r2, r0, r11
	bf	r2, .Lmain
	stw	r1, r0[0]
	add	r0, r0, 4
	bu	.Lzero
.Lmain:
	ldap	r11, xs1_chanend0
	stw	r4, r11[0]
	ldc	r0, 0			# main(0, 0)
	ldc	r1, 0
	bl	main
	.globl	_exit
_exit:				# main returned (r0 = status): park this thread
	clre
	waiteu
	bu	_exit

# Kernel entry point (kep must be 128-byte aligned; kcall enters at kep + 64). Any exception
# parks the thread here; r0..r11 and the saved spc/ssr/et are left for a debugger.
	.p2align	7
	.globl	_trap
_trap:
	clre
	waiteu
	bu	_trap
