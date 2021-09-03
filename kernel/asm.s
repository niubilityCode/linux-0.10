/*
 *  linux/kernel/asm.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * asm.s contains the low-level code for most hardware faults.
 * page_exception is handled by the mm, so that isn't here. This
 * file also handles (hopefully) fpu-exceptions due to TS-bit, as
 * the fpu must be properly saved/resored. This hasn't been tested.
 */

.globl _divide_error,_debug,_nmi,_int3,_overflow,_bounds,_invalid_op
.globl _device_not_available,_double_fault,_coprocessor_segment_overrun
.globl _invalid_TSS,_segment_not_present,_stack_segment
.globl _general_protection,_coprocessor_error,_irq13,_reserved

# Comment by Richard.ji
_divide_error:
	pushl $_do_divide_error
no_error_code: # 一个中断的处理流程就是三步：1.保存上下文 2.执行call命令，执行中断处理程序 3.恢复上下文
	xchgl %eax,(%esp) 
	pushl %ebx	# 1.保存上、下文到栈中(下面的所有pushl/push操作都是将寄存器中的值放入栈中)
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl $0		# "error code"
	lea 44(%esp),%edx
	pushl %edx
	movl $0x10,%edx
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	call *%eax	# 2.执行中断处理程序(对于每种类型的中断而言，eax的值都不一样：eax寄存器的值是实现存入的该中断处理程序的函数首地址)
	addl $8,%esp
	pop %fs	# 3.从栈中恢复上、下文(面的所有popl/pop操作都是将栈中的值恢复到寄存器中)
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	iret

_debug:
	pushl $_do_int3		# _do_debug
	jmp no_error_code

_nmi:
	pushl $_do_nmi # 中断处理函数入栈 (该中断处理函数是c语言写的，在trap.c中)
	jmp no_error_code #跳转到定义好的汇编代码区

_int3:
	pushl $_do_int3
	jmp no_error_code

_overflow:
	pushl $_do_overflow
	jmp no_error_code

_bounds:
	pushl $_do_bounds
	jmp no_error_code

_invalid_op:
	pushl $_do_invalid_op
	jmp no_error_code

math_emulate:
	popl %eax
	pushl $_do_device_not_available
	jmp no_error_code
_device_not_available:
	pushl %eax
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	jne math_emulate
	clts				# clear TS so that we can use math
	pushl %ecx
	pushl %edx
	push %ds
	movl $0x10,%eax
	mov %ax,%ds
	call _math_state_restore
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_coprocessor_segment_overrun:
	pushl $_do_coprocessor_segment_overrun
	jmp no_error_code

_reserved:
	pushl $_do_reserved
	jmp no_error_code

_irq13:
	pushl %eax
	xorb %al,%al
	outb %al,$0xF0
	movb $0x20,%al
	outb %al,$0x20
	jmp 1f
1:	jmp 1f
1:	outb %al,$0xA0
	popl %eax
_coprocessor_error:
	fnclex
	pushl $_do_coprocessor_error
	jmp no_error_code

_double_fault:
	pushl $_do_double_fault
error_code:
	xchgl %eax,4(%esp)		# error code <-> %eax
	xchgl %ebx,(%esp)		# &function <-> %ebx
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl %eax			# error code
	lea 44(%esp),%eax		# offset
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	call *%ebx
	addl $8,%esp
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	iret

_invalid_TSS:
	pushl $_do_invalid_TSS
	jmp error_code

_segment_not_present:
	pushl $_do_segment_not_present
	jmp error_code

_stack_segment:
	pushl $_do_stack_segment
	jmp error_code

_general_protection:
	pushl $_do_general_protection
	jmp error_code

