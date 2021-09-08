/*
 *  linux/kernel/signal.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <signal.h>

volatile void do_exit(int error_code);

int sys_sgetmask()
{
	return current->blocked;
}

int sys_ssetmask(int newmask)
{
	int old=current->blocked;

	current->blocked = newmask & ~(1<<(SIGKILL-1));
	return old;
}

static inline void save_old(char * from,char * to)
{
	int i;

	verify_area(to, sizeof(struct sigaction));
	for (i=0 ; i< sizeof(struct sigaction) ; i++) {
		put_fs_byte(*from,to);
		from++;
		to++;
	}
}

static inline void get_new(char * from,char * to)
{
	int i;

	for (i=0 ; i< sizeof(struct sigaction) ; i++)
		*(to++) = get_fs_byte(from++);
}

// 做信号的预处理和设置
int sys_signal(int signum, long handler, long restorer)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL)
		return -1;
	
	// 指定信号处理句柄 handler给临时变量tmp
	tmp.sa_handler = (void (*)(int)) handler;
	tmp.sa_mask = 0;
	tmp.sa_flags = SA_ONESHOT | SA_NOMASK;
	tmp.sa_restorer = (void (*)(void)) restorer;
	handler = (long) current->sigaction[signum-1].sa_handler;

	// 将设置好的信号赋值给当前进程结构体 current(因为中断的肯定就是当前进程，所以这里用到了current)
	// 后面do_signal()方法中会再从current进程中取出sigaction[signum-1]位置的handler来执行。
	current->sigaction[signum-1] = tmp;
	return handler;
}

// 与sys_signal()函数类似，只是入参更多，可控性更强
int sys_sigaction(int signum, const struct sigaction * action,
	struct sigaction * oldaction)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL)
		return -1;
	tmp = current->sigaction[signum-1];
	get_new((char *) action,
		(char *) (signum-1+current->sigaction));
	if (oldaction)
		save_old((char *) &tmp,(char *) oldaction);
	if (current->sigaction[signum-1].sa_flags & SA_NOMASK)
		current->sigaction[signum-1].sa_mask = 0;
	else
		current->sigaction[signum-1].sa_mask |= (1<<(signum-1));
	return 0;
}

// do_signal()是中断处理程序中的"信号处理程序"，是属于中断处理程序之一。
void do_signal(long signr,long eax, long ebx, long ecx, long edx,
	long fs, long es, long ds,
	long eip, long cs, long eflags,
	unsigned long * esp, long ss)
{
	long sa_handler;
	long old_eip=eip;

	// 目的：找到中断结构体。
	// 因为中断一定是中断的当前进程，所以才使用 current进程，而current->sigaction是task_struct进程结构体中数组sigaction的首地址，
	// 所以”current->sigaction + signr - 1“ 就是数组中的位置
	struct sigaction * sa = current->sigaction + signr - 1;
	int longs;
	unsigned long * tmp_esp;

	// 取出信号句柄
	sa_handler = (long) sa->sa_handler;
	// 如果信号句柄为可忽略的，则不处理直接返回
	if (sa_handler==1)
		return;
	// 若信号句柄为空
	if (!sa_handler) {
		if (signr==SIGCHLD)
			return;
		else
			do_exit(1<<(signr-1));
	}
	if (sa->sa_flags & SA_ONESHOT)
		sa->sa_handler = NULL;

	// 这个设置很关键，实现方式很巧妙。这里并没有针对不同的信号做不同的处理(如写各种switch语句来处理不同的信号)，
	// 而是将信号处理函数sa_handler->赋值给eip寄存器指向的内存地址。然后等do_signal()方法执行结束，会继续执行ret_from_sys_call汇编代码剩下的部分，就是一些收尾工作，直到执行完最后一条指令iret，
	// iret指令会弹出ip寄存器的值(即之前更改好的eip的值)，在do_signal函数里，之前已经把这ip的值改为sa_handler的地址（有点缓冲区溢出攻击的感觉），所以结束系统调用后，就会从内核态->用户态，在用户态执行sa_handler函数，

	// tips：这里指的一提的是 do_signal()执行完之后->才执行sa_handler->然后回到用户进程。
	// tip：eip寄存器，更通用的叫法是PC寄存器，又叫有些机器中也称为指令指针IP(Instruction Pointer)。反正都是存储指令地址的存储器。参考:https://shimo.im/docs/qyPDGwHy3dkyRWvg
	*(&eip) = sa_handler;

	// 将原调用程序的用户堆栈指针向下扩展7(8)个字长(用来存放调用信号句柄的参数等)，
	// 7或者8代表下面即将执行的压栈的次数。
	longs = (sa->sa_flags & SA_NOMASK)?7:8;

	// 拓展用户栈栈顶
	*(&esp) -= longs;
	verify_area(esp,longs*4);
	tmp_esp=esp;

	// 压入用户栈，中断返回时执行
	put_fs_long((long) sa->sa_restorer,tmp_esp++);
	put_fs_long(signr,tmp_esp++);
	if (!(sa->sa_flags & SA_NOMASK))
		put_fs_long(current->blocked,tmp_esp++);
	put_fs_long(eax,tmp_esp++);
	put_fs_long(ecx,tmp_esp++);
	put_fs_long(edx,tmp_esp++);
	put_fs_long(eflags,tmp_esp++);

	// 用户程序的地址
	put_fs_long(old_eip,tmp_esp++);
	current->blocked |= sa->sa_mask;
}
