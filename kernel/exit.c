/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <asm/segment.h>

int sys_pause(void);
int sys_close(int fd);

// 1. 清空任务描述表中的对应进程表项task[i]，
// 2. 释放对应的内存页(代码段、数据段、堆栈)，
// 3. 让内核重新调度进程schedule()
void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	for (i=1 ; i<NR_TASKS ; i++)
		if (task[i]==p) {
			task[i]=NULL;
			free_page((long)p);
			schedule();
			return;
		}
	panic("trying to release non-existent task");
}

// 向进程p发送sig信号。使用场景：
// 1. 进程间通信
// 2. 子进程 -> 父进程发送SIGCHLD信号 等等
static inline int send_sig(long sig,struct task_struct * p,int priv)
{
	if (!p || sig<1 || sig>32)
		return -EINVAL;
	if (priv || (current->euid==p->euid) || suser())
		p->signal |= (1<<(sig-1));
	else
		return -EPERM;
	return 0;
}

// 关闭会话
static void kill_session(void)
{
	struct task_struct **p = NR_TASKS + task;
	
	// 排除task[0]，因为task[0]是所有进程的父进程，不能被关闭。
	while (--p > &FIRST_TASK) {
		if (*p && (*p)->session == current->session)
			(*p)->signal |= 1<<(SIGHUP-1);
	}
}

/*
 * XXX need to check permissions needed to send signals to process
 * groups, etc. etc.  kill() permissions semantics are tricky!
 */
// 向对应的进程/进程组 发送信号。
// tips：我们在终端执行的kill -n 命令对应的就会执行sys_kill()方法
int sys_kill(int pid,int sig)
{
	struct task_struct **p = NR_TASKS + task;
	int err, retval = 0;

	if (!pid) while (--p > &FIRST_TASK) { //1. pid=0，则给“当前进程”的“进程组”发送sig信号
		if (*p && (*p)->pgrp == current->pid) 
			if (err=send_sig(sig,*p,1))
				retval = err;
	} else if (pid>0) while (--p > &FIRST_TASK) { //2. pid>0，则给对应的“pid进程”发送sig信号
		if (*p && (*p)->pid == pid) 
			if (err=send_sig(sig,*p,0))
				retval = err;
	} else if (pid == -1) while (--p > &FIRST_TASK) //3. pid=-1，则给所有进程(刨除task[0]进程)发送sig信号
		if (err = send_sig(sig,*p,0))
			retval = err;
	else while (--p > &FIRST_TASK) //4. pid<-1，则给“进程组”号=-pid的进程组发送sig信号
		if (*p && (*p)->pgrp == -pid)
			if (err = send_sig(sig,*p,0))
				retval = err;
	return retval;
}

// 子进程 -> 父进程 发送SIGCHLD信号。如果找不到父进程，则释放当前的进程(current)
static void tell_father(int pid)
{
	int i;

	if (pid)
		for (i=0;i<NR_TASKS;i++) {
			if (!task[i])
				continue;
			if (task[i]->pid != pid)
				continue;
			task[i]->signal |= (1<<(SIGCHLD-1));
			return;
		}
/* if we don't find any fathers, we just release ourselves */
	release(current);
}

// 
int do_exit(long code)
{
	int i;

	// 释放ldt内存段
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));

	for (i=0 ; i<NR_TASKS ; i++)
		if (task[i] && task[i]->father == current->pid)
			task[i]->father = 0;
	
	// 如果进程有已经打开的文件，则进行关闭
	for (i=0 ; i<NR_OPEN ; i++)
		if (current->filp[i])
			sys_close(i);
	iput(current->pwd);
	current->pwd=NULL;
	iput(current->root);
	current->root=NULL;
	if (current->leader && current->tty >= 0)
		tty_table[current->tty].pgrp = 0;

	// 如果进程使用了协处理器，则将协处理器置为NULL
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	if (current->leader)
		kill_session();
	current->state = TASK_ZOMBIE;
	current->exit_code = code;

	// 向父进程发送SIGCHLD信号
	tell_father(current->father);

	// 重新进行进程调度
	schedule();
	return (-1);	/* just to suppress warnings */
}

int sys_exit(int error_code)
{
	return do_exit((error_code&0xff)<<8);
}

// 处理进程收到的pid信号，根据不同的pid信号，做不同的操作。

// 应用场景：当子进程 -> 父进程 发送完SIGCHLD信号之后，子进程进入僵死状态(TASK_ZOMBIE)，
// 		    然后父进程就会调用sys_waitpid()函数，找到僵死状态的子进程，然后release()掉子进程
int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options)
{
	int flag;
	struct task_struct ** p;

	verify_area(stat_addr,4);
repeat:
	flag=0;
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p || *p == current)
			continue;
		if ((*p)->father != current->pid)
			continue;
		if (pid>0) {
			if ((*p)->pid != pid)
				continue;
		} else if (!pid) {
			if ((*p)->pgrp != current->pgrp)
				continue;
		} else if (pid != -1) {
			if ((*p)->pgrp != -pid)
				continue;
		}
		switch ((*p)->state) {
			case TASK_STOPPED:
				if (!(options & WUNTRACED))
					continue;
				put_fs_long(0x7f,stat_addr);
				return (*p)->pid;
			case TASK_ZOMBIE:
				current->cutime += (*p)->utime;
				current->cstime += (*p)->stime;
				flag = (*p)->pid;
				put_fs_long((*p)->exit_code,stat_addr);
				release(*p);
				return flag;
			default:
				flag=1;
				continue;
		}
	}
	if (flag) {
		if (options & WNOHANG)
			return 0;
		current->state=TASK_INTERRUPTIBLE;
		schedule();
		if (!(current->signal &= ~(1<<(SIGCHLD-1))))
			goto repeat;
		else
			return -EINTR;
	}
	return -ECHILD;
}


