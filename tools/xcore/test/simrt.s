# xsim console: xsim intercepts calls to _sim_putc (prints r0); on hardware it just returns.
	.text
	.globl	_sim_putc
_sim_putc:
	retsp	0
