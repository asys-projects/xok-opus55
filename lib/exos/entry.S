/*
 * ExOS upcall entry and context resumption.
 *
 * Xok enters every upcall with %esp pointing to a struct utf (the
 * interrupted context, see <xok/env.h>) and %eax holding the kind of
 * event.  The C dispatcher handles the event; the context is then
 * resumed entirely at user level, without entering the kernel.
 */

#define UTF_EFLAGS 32
#define UTF_EIP    36
#define UTF_ESP    40

	.text
	.globl __exos_upcall
__exos_upcall:
	cld
	movl %esp, %eax
	pushl %eax
	call __exos_upcall_c
	addl $4, %esp
	/* Fall through: resume the context in the utf at (%esp). */

	.globl __exos_resume
__exos_resume:
	movl UTF_ESP(%esp), %eax
	subl $8, %eax
	movl UTF_EIP(%esp), %ebx
	movl %ebx, 4(%eax)
	movl UTF_EFLAGS(%esp), %ebx
	movl %ebx, 0(%eax)
	movl %eax, UTF_ESP(%esp)
	popal
	addl $8, %esp		/* Skip eflags and eip. */
	popl %esp		/* Interrupted stack, minus eflags/eip. */
	popfl
	ret

/*
 * __exos_resume_utf(struct utf *): resume a context saved anywhere.
 */
	.globl __exos_resume_utf
__exos_resume_utf:
	movl 4(%esp), %esp
	jmp __exos_resume
