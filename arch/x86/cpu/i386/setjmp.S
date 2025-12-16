/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Written by H. Peter Anvin <hpa@zytor.com>
 * Brought in from Linux v4.4 and modified for U-Boot
 * From Linux arch/um/sys-i386/setjmp.S
 */

#define _REGPARM

/*
 * The jmp_buf is assumed to contain the following, in order:
 *	%ebx
 *	%esp
 *	%ebp
 *	%esi
 *	%edi
 *	<return address>
 */

	.text
	.align 4
	.globl setjmp
	.type setjmp, @function
setjmp:
#ifdef _REGPARM
	movl %eax, %edx
#else
	movl 4(%esp), %edx
#endif
	popl %ecx		/* Return address, and adjust the stack */
	xorl %eax, %eax		/* Return value */
	movl %ebx, (%edx)
	movl %esp, 4(%edx)	/* Post-return %esp! */
	pushl %ecx		/* Make the call/return stack happy */
	movl %ebp, 8(%edx)
	movl %esi, 12(%edx)
	movl %edi, 16(%edx)
	movl %ecx, 20(%edx)	/* Return address */
	ret

	/* Provide function size if needed */
	.size setjmp, .-setjmp

	.align 4
	.globl longjmp
	.type longjmp, @function
longjmp:
#ifdef _REGPARM
	xchgl %eax, %edx
#else
	movl 4(%esp), %edx	/* jmp_ptr address */
	movl 8(%esp), %eax	/* Return value */
#endif
	movl (%edx), %ebx
	movl 4(%edx), %esp
	movl 8(%edx), %ebp
	movl 12(%edx), %esi
	movl 16(%edx), %edi
	test %eax, %eax
	jnz nz
	inc %eax
nz:
	jmp *20(%edx)

	.size longjmp, .-longjmp

	.align 4
	.globl initjmp
	.type initjmp, @function
initjmp:
	pushl	%edi

#ifdef _REGPARM
	movl	%eax, %edi	/* jump buffer */
	/* %edx - entry point address */
	/* %ecx - stack base */
	movl 8(%esp), %eax	/* stack size */
#else
	movl	8(%esp), %edi	/* jump buffer */
	movl	12(%esp), %edx	/* entry point address */
	movl	16(%esp), %ecx	/* stack base */
	movl	20(%esp), %eax	/* stack size */
#endif
	addl	%ecx, %eax
	subl	$4, %eax	/* Emulate return address pushed to stack */

	movl	%edx, 20(%edi)	/* Return address */
	movl	%eax, 4(%edi)	/* Post-return %esp! */

	xorl	%eax, %eax
	movl	%eax, 0(%edi)	/* ebx */
	movl	%eax, 8(%edi)	/* ebp */
	movl	%eax, 12(%edi)	/* esi */
	movl	%eax, 16(%edi)	/* edi */

	pop	%edi
	ret

	.size initjmp, .-initjmp
