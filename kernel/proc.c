#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"


struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void wakeup1(struct proc *chan);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// initialize the proc table at boot time.
//系统运行时init进程的初始化
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");//获取锁
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");//为进程池的每一个进程创建锁

      // Allocate a page for the process's kernel stack.
      // Map it high in memory, followed by an invalid
      // guard page.
      char *pa = kalloc();//为进程分配内存空间(物理地址)
      if(pa == 0)
        panic("kalloc");
      uint64 va = KSTACK((int) (p - proc));//计算进程栈的虚拟地址起点；
      kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);//为该虚拟地址配置pte条目；
      p->kstack = va;//进程栈的起点就是虚拟地址;
  }
  kvminithart();
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
//获取cpu-id
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
//返回cpu状态指针；
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];//获取cpuid之后从cpu指针数组中取出当前正运行的cpu
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

//创建子进程
int
allocpid() {
  int pid;
  
  acquire(&pid_lock);//上锁
  pid = nextpid;//从进程编号中取出当前最小未被使用的进程编号
  nextpid = nextpid + 1;//待使用的进程编号++；
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
//创建进程并分配物理内存
static struct proc*
allocproc(void)
{
  //由于要增加一个内核页表副本，需要在创建进程时创建/复制内核页表副本；
  struct proc *p;

  //1.获取进程池的锁和指针；
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      //如果该位置的进程没有被使用，则可以分配；
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  //2.分配进程号
  p->pid = allocpid();//分配进程编号

  // Allocate a trapframe page.
  //3.分配trapframe的物理地址
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){//分配trapframe用于trap和系统调用
    release(&p->lock);
    return 0;
  }
  //4.创建用户页表
  // An empty user page table.
  p->pagetable = proc_pagetable(p);//创建进程的用户页表
  if(p->pagetable == 0){//如果进程的用户页表创建失败；
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  //!!!创建进程的内核页表副本
  p->kernal_pagetable_bak=proc_kernel_pagetable_bak(p);
  if(p->kernal_pagetable_bak==0){//如果进程的内核页表副本创建失败
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  //5.创建并初始化上下文；
  memset(&p->context, 0, sizeof(p->context));//为上下文清理内存空间，清空进程的内核上下文结构
  p->context.ra = (uint64)forkret;//ra return address 返回地址，既切换到当前进程时，从forkret()函数开始运行
  p->context.sp = p->kstack + PGSIZE;//设置内核上下文的栈指针

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
//释放进程的资源
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);//释放trapframe
  p->trapframe = 0;
  if(p->pagetable)//释放页表
    proc_freepagetable(p->pagetable, p->sz);
  if(p->kernal_pagetable_bak){//清除并释放进程的内核页表副本
    proc_free_kernel_pagetabel_bak(p->kernal_pagetable_bak);
    printf("    kernel_pagetable_bak has been freed\n");
  }
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
//创建进程的用户页表
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();//创建页表；
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  //为刚创建的用户页表创建pte条目
  //其中TEAMPOLINE是用户虚拟地址空间的最高位，是用户虚拟地址空间的起点;整个用户页表添加的映射都是用户空间地址的映射
  //意思是：在不同的进程中，通过索引相同的虚拟地址TRAMPLINE,借助想要跳转的进程页表，可以实现进程的切换；
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  //为TRAPFRAME创建进程的页表条目pte，便于trap或系统调用；
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;//页表创建结束
}

// Free a process's page table, and free the
// physical memory it refers to.
//清除页表
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  //1.首先清除用户栈和trap的页表条目；
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  //2.释放页表中的所有内存；
  uvmfree(pagetable, sz);
}

//清除进程的内核页表副本
void
proc_free_kernel_pagetabel_bak(pagetable_t kernel_pagetable_bak){
  //递归清除三级条目
  for(int i=0;i<512;++i){
    pte_t pte=kernel_pagetable_bak[i];//取出条目
    if(pte&PTE_V){//条目有效，进行清除，并且将对应的pa释放
      kernel_pagetable_bak[i]=0;//清除页表条目的物理地址上的内容；
      //uvmunmap(kernel_pagetable_bac,child,1,0);//清除页表的映射
      if((pte&(PTE_R|PTE_W|PTE_X))==0){//如果是1或2级pte条目，则对其子条目进行递归的清除；
        uint64 child=PTE2PA(pte);//转为下一级页表的起始物理地址；
        proc_free_kernel_pagetabel_bak(((pagetable_t)child));//递归清除页表
      }
    }
  }
  kfree(kernel_pagetable_bak);//释放物理内存;
}


// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
//用户的初始化
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  np->parent = p;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold p->lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    // this code uses pp->parent without holding pp->lock.
    // acquiring the lock first could cause a deadlock
    // if pp or a child of pp were also in exit()
    // and about to try to lock p.
    if(pp->parent == p){
      // pp->parent can't change between the check and the acquire()
      // because only the parent changes it, and we're the parent.
      acquire(&pp->lock);
      pp->parent = initproc;
      // we should wake up init here, but that would require
      // initproc->lock, which would be a deadlock, since we hold
      // the lock on one of init's children (pp). this is why
      // exit() always wakes init (before acquiring any locks).
      release(&pp->lock);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  // we might re-parent a child to init. we can't be precise about
  // waking up init, since we can't acquire its lock once we've
  // acquired any other proc lock. so wake up init whether that's
  // necessary or not. init may miss this wakeup, but that seems
  // harmless.
  acquire(&initproc->lock);
  wakeup1(initproc);
  release(&initproc->lock);

  // grab a copy of p->parent, to ensure that we unlock the same
  // parent we locked. in case our parent gives us away to init while
  // we're waiting for the parent lock. we may then race with an
  // exiting parent, but the result will be a harmless spurious wakeup
  // to a dead or wrong process; proc structs are never re-allocated
  // as anything else.
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);
  
  // we need the parent's lock in order to wake it up from wait().
  // the parent-then-child rule says we have to lock it first.
  acquire(&original_parent->lock);

  acquire(&p->lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup1(original_parent);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&original_parent->lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  // hold p->lock for the whole time to avoid lost
  // wakeups from a child's exit().
  acquire(&p->lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      // this code uses np->parent without holding np->lock.
      // acquiring the lock first would cause a deadlock,
      // since np might be an ancestor, and we already hold p->lock.
      if(np->parent == p){
        // np->parent can't change between the check and the acquire()
        // because only the parent changes it, and we're the parent.
        acquire(&np->lock);
        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&p->lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&p->lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&p->lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &p->lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
//进程的调度器
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();//获取cpu信息
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();//允许中断
    
    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;

        found = 1;
      }
      release(&p->lock);
    }
#if !defined (LAB_FS)
    if(found == 0) {
      intr_on();
      asm volatile("wfi");
    }
#else
    ;
#endif
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
  if(lk != &p->lock){  //DOC: sleeplock0
    acquire(&p->lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &p->lock){
    release(&p->lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
//唤醒前置进程为chan的所有进程为就绪状态；
void
wakeup(void *chan)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
    }
    release(&p->lock);
  }
}

// Wake up p if it is sleeping in wait(); used by exit().
// Caller must hold p->lock.
static void
wakeup1(struct proc *p)
{
  if(!holding(&p->lock))
    panic("wakeup1");
  if(p->chan == p && p->state == SLEEPING) {
    //如果进程正在等待它自己，就把它唤醒
    p->state = RUNNABLE;
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;
  //kill 是通过轮询进行的 
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){//从休眠态转移到就绪？为啥
        // Wake process from sleep().
        p->state = RUNNABLE;
        //这么做是为了让它能被调度运行，从而尽快检查到自己已经被 kill，然后退出
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
