/*
 * ExOS program entry point.
 *
 * Environments created by ExOS start with %esp pointing to
 * [argc, argv, envp].  The boot environment, loaded by the boot loader,
 * starts with %esp == 0: it runs on a static bootstrap stack.
 */
	.section .text.start, "ax"
	.globl _start
_start:
	testl %esp, %esp
	jnz 1f
	movl $__boot_stack_top, %esp
	pushl $0
	pushl $0
	pushl $0
1:	movl %esp, %eax
	pushl $0		/* Fake return address / frame end. */
	xorl %ebp, %ebp
	pushl %eax
	call __exos_start
	ud2

	.bss
	.align 16
	.globl __boot_stack
__boot_stack:
	.space 16384
__boot_stack_top:
