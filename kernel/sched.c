/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	printk("eip=%04x:%08x\n\r",p->tss.cs&0xffff,p->tss.eip);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,};

long volatile jiffies=0;
long startup_time=0;
struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), };

long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
	last_task_used_math=current;
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
/**
 * 进程调度函数--这里linux代码是0.10版本的，调度原理比较简单：
 *
 * 1. 概括：linux操作系统对进程的调度就是对task_struct[] 的检索、遍历，具体调度逻辑如下
 * 2. 细节：从task_struct[]中找到时间片(task_struct->counter)最大的进程对象(task_struct)，然后进行
 * 	  调用，直到时间片为0才退出。之后再进行新一轮的调用。
 * 
 * 还有一个问题，task_struct->counter什么时候被设置的呢？
 * 	 --当task_struct[]中全部的进程的counter都为0时(即都耗尽完时间片了)，就进行新一轮的时间片分配，
 *   --每个进程分配多少是根据priority字段判断的，priority大的应该多分配点时间片。
 * 	   具体分配算法是：counter=counter/2 + priority;
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];

		// 1. 从task_struct[]中找到时间片(task_struct->counter)最大的进程对象(task_struct)，
		// 其中c是最大的counter值；next是最大的counter值对应的pid
		while (--i) {
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i; 
		}

		// 2. 若c不为null/0，说明当前有待执行的task_struct，则直接break，跳出循环执行switch_to(next)切换到task_struct[next]进程上；
		if (c) break; 

		// 3. 若c为null，则说明当前所有进程的时间片都用完了，就需要为所有进程重新分配时间片(counter)
		// 分配公式为：counter = counter/2 + priority
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}

	// 切换到task[next]进程
	switch_to(next);
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

// 当某个进程想访问CPU资源时，碰巧CPU资源被占用了，那么就会调用sleep_on()函数，让进程休眠。
void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();

	// 当调用上一步schedule()方法时，会进行进程切换，这里就会阻塞。
	
	// refrence：https://github.com/niubilityCode/linux-0.11/blob/master/kernel/sched.c
	// 只有当这个等待任务被唤醒时，调度程序才又返回到这里，表示本进程已被明确的唤醒(就
    // 续态)。既然大家都在等待同样的资源，那么在资源可用时，就有必要唤醒所有等待该该资源
    // 的进程。该函数嵌套调用，也会嵌套唤醒所有等待该资源的进程。这里嵌套调用是指一个
    // 进程调用了sleep_on()后就会在该函数中被切换掉，控制权呗转移到其他进程中。此时若有
    // 进程也需要使用同一资源，那么也会使用同一个等待队列头指针作为参数调用sleep_on()函数，
    // 并且也会陷入该函数而不会返回，最终形成一个等待链表，链表中存的都是等待唤醒的进程。只有当内核某处代码以队列头指针作为参数wake_up了队列，
    // 那么当系统切换去执行头指针所指的进程A时，该进程才会继续执行下面的代码，把队列后一个
    // 进程B置位就绪状态(唤醒)。而当轮到B进程执行时，它也才可能继续执行下面的代码。若它
    // 后面还有等待的进程C，那它也会把C唤醒等。
	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

// 唤醒进程: state=0
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;
unsigned char selected = 0;
struct task_struct * wait_on_floppy_select = NULL;

void floppy_select(unsigned int nr)
{
	if (nr>3)
		printk("floppy_select: nr>3\n\r");
	cli();
	while (selected)
		sleep_on(&wait_on_floppy_select);
	current_DOR &= 0xFC;
	current_DOR |= nr;
	outb(current_DOR,FD_DOR);
	sti();
}

void floppy_deselect(unsigned int nr)
{
	if (nr != (current_DOR & 3))
		printk("floppy_deselect: drive not selected\n\r");
	selected = 0;
	wake_up(&wait_on_floppy_select);
}

int ticks_to_floppy_on(unsigned int nr)
{
	unsigned char mask = 1<<(nr+4);

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	if (!(mask & current_DOR)) {
		current_DOR |= mask;
		if (!selected) {
			current_DOR &= 0xFC;
			current_DOR |= nr;
		}
		outb(current_DOR,FD_DOR);
		mon_timer[nr] = HZ;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl)
{
	if (cpl) //cpl是进程的状态标志，准确来说是CPU上核的状态(用户态 or 内核态)。utime和stime是用来统计用户进程和内核程序的执行时间的。
		current->utime++; //用户程序的运行时间，++
	else
		current->stime++; //内核程序的运行时间，++
	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	if ((--current->counter)>0) return;
	current->counter=0;
	if (!cpl) return;
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL; //将所有进程对象task[] 都置为NULL
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
	ltr(0);
	lldt(0);
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);
	set_system_gate(0x80,&system_call);
}
