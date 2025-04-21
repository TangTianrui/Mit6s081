#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  
  //backtrace();
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_sigalarm(void){
  struct proc *p=myproc();
  int alarm_interval_;
  uint64 alarm_func_;
  
  printf("sigalarm\n");

  if(argint(0,&alarm_interval_)<0){
    return -1;
  }
  if(argaddr(1,&alarm_func_)<0){
    return -1;
  }

  p->alarm_interval=alarm_interval_;
  //此处直接将用户虚拟地址赋值保存即可，因为该函数也是由用户模式进行调用的，所以不用转为物理地址由内核处理;
  p->alarm_func=alarm_func_;

  /*
  //根据用户地址和用户页表进行寻址
  uint64 func_;
  if(fetchaddr(alarm_func_,(uint64*)&func_)<0){
    printf("fetch failed\n");
    return -1;
  } 

  //如果给进程时钟中断函数传递的是函数的物理地址
  //那么返回用户空间后调用该地址的函数,会导致访问非用户虚拟地址空间的地址，导致缺页错误
  //p->alarm_func=func_;
  */
  
  //打印输出当前的函数地址
  //printf("interval: %d: func:%p\n",p->alarm_interval,p->alarm_func);  

  return 0;
}

//用于主动恢复用户空间,将保存的用户寄存器进行恢复
uint64
sys_sigreturn(void){
  
  

  printf("sigreturn\n");
  return 0;
}