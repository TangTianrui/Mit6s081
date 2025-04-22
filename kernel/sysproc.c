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

//该系统调用函数实现物理内存的分配和清除
//lab5.lazy只关心内存的懒分配
uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  struct proc *p=myproc();
  addr = p->sz;
  //由于lazy allocation, 在sbrk中只标记sz的扩大,而不实际分配物理内存
  //后续缺页错误时根据sz标记位再按需分配物理内存

  //printf("pre_mem size:%p to new_mem size:%p\n",addr,addr+n);
  if(n<0){
    if(-n>=addr){
      return -1;
    }
    else p->sz= uvmdealloc(p->pagetable, addr, addr + n);
    /*
    if(uvm_lazyfree(p->pagetable,addr,addr+n)<0){
      return -1;
    }    
    */
    //如果是释放内存，正常释放
    //实际上也不能正常释放，因为lazy alloc不会添加pte映射条目,所以growproc中的uvmunmap面对空条目会报错
  }
  else if(n>0){
    //如果是分配内存,直接操作进程的内存大小
    p->sz+=n;
  }

  //默认开辟4*4096(PGSIZE)=16374Bytes;
  //根据打印信息发现sh和echo会分配16*4096(PGSIZE)=65536Bytes的物理内存
  //printf("new_mem size:%d\n",p->sz);
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
