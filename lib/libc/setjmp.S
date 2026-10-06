/*
 * ExOS libc: setjmp/longjmp for i386.
 * jmp_buf: ebx, esi, edi, ebp, esp, eip
 */
	.text
	.globl setjmp
setjmp:
	movl 4(%esp), %eax
	movl %ebx, 0(%eax)
	movl %esi, 4(%eax)
	movl %edi, 8(%eax)
	movl %ebp, 12(%eax)
	leal 4(%esp), %ecx
	movl %ecx, 16(%eax)
	movl (%esp), %ecx
	movl %ecx, 20(%eax)
	xorl %eax, %eax
	ret

	.globl longjmp
longjmp:
	movl 4(%esp), %edx
	movl 8(%esp), %eax
	testl %eax, %eax
	jnz 1f
	incl %eax
1:	movl 0(%edx), %ebx
	movl 4(%edx), %esi
	movl 8(%edx), %edi
	movl 12(%edx), %ebp
	movl 16(%edx), %esp
	jmp *20(%edx)
