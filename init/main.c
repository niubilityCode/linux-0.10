/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

// linux操作系统启动时调用，初始化时间，得到一个全局的时间值中：
// startup_time=计算从1970年1月1日0时到现在经过的秒数。

// tops：该值startup_time后续给JIFFIES(一个系统的时钟滴答，每滴答一下是10ms，可以当做定时器)使用，
// 可以用来当CPU时间片单位。同时每隔10ms会引发一个定时器中断。
static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0); //从CMOS硬件中读取一个数，下面类似
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;
static long buffer_memory_end = 0;

struct drive_info { char dummy[32]; } drive_info;

// main函数，在操作系统启动时，执行该函数
void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */

	// 设置操作西系统的根文件地址
 	ROOT_DEV = ORIG_ROOT_DEV;
	// 设置操作系统驱动地址
 	drive_info = DRIVE_INFO;
	
	// 解析setup.s代码后获取系统内存参数
	// 设置系统的内存大小：系统本身内存(1M) + 扩展内存大小(参数*KB)
	memory_end = (1<<20) + (EXT_MEM_K<<10);
	memory_end &= 0xfffff000;

	// 控制操作系统的最大内存为16M
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
	// 设置告诉缓冲区的大小
	if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;
	
	// 进行内存控制器的初始化，加载内存驱动
	mem_init(buffer_memory_end,memory_end);

	// 进行异常函数的初始化
	trap_init();

	// 进行块设备驱动的初始化，加载块设备驱动
	blk_dev_init();

	// 进行字符设备驱动的初始化，加载字符设备驱动
	chr_dev_init();

	// 进行控制台设备的初始化，加载显示和传输设备的驱动
	tty_init();

	// 加载定时器驱动
	time_init(); 

	// 进行进程调度初始化
	sched_init();
	
	// 进行缓冲区初始化
	buffer_init(buffer_memory_end);
	
	// 尽心硬盘设备的初始化，加载硬盘驱动
	hd_init();

	// 尽心软盘设备的初始化，加载软盘驱动
	floppy_init();
	sti();

	// 从main方法开始直至这里，代码都是在内核态运行的(为了保证上面的初始化不被打断-即没有中断，所以才在内核态运行)。
	// 初始化完成之后就执行move_to_user_mode()切换到用户态了。后面在用户态执行就可以响应中断了。
	move_to_user_mode();
	
	// fork()创建0号进程，0号进程是所有进程的父进程
	if (!fork()) {		/* we count on this going ok */
		//init()函数自然而然的运行到fork()创建的0号进程上(fork()之后的代码都是运行到当前创建的新进程上的)
		// 因为fork()就会触发中断，进入中断处理程序，然后创建task_struct，然后复制父进程的一些数据，
		// 然后更改子进程的一些数据，关键在于init()运行在进程0的上、下文(寄存器值)中，所以我们才说：
		//【init()运行在进程0的上】
		init(); 
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;) pause();
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int i,j;

	setup((void *) &drive_info);
	if (!fork())
		_exit(execve("/bin/update",NULL,NULL));
	(void) open("/dev/tty0",O_RDWR,0); // 打开标准输入控制台
	(void) dup(0); // 打开标准输出控制台
	(void) dup(0); // 打开标准错误控制台
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-buffer_memory_end);
	printf(" Ok.\n\r");
	if ((i=fork())<0)
		printf("Fork failed in init\r\n");
	else if (!i) {
		close(0);close(1);close(2);
		setsid();
		(void) open("/dev/tty0",O_RDWR,0);
		(void) dup(0);
		(void) dup(0);
		_exit(execve("/bin/sh",argv,envp));
	}
	j=wait(&i);
	printf("child %d died with code %04x\n",j,i);
	sync();
	_exit(0);	/* NOTE! _exit, not exit() */
}
