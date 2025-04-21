#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

//三个汇编函数的地址：定义在汇编函数trampoline.S中
//1.其中trampoline用于定位/偏移
//2.uservec用于保存用户寄存器向量
//3.userret用于恢复寄存器
extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  //由于是用户trap，所以检查是不是用户模式陷入的
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  //控制陷入trap（异常/中断）时PC跳转到的地址。
  //0.将 trap handler 的入口地址设置为 kernelvec
  //1.因为进入该函数代表此时：用户已经触发陷入，转为管理模式进行陷入处理了；
  //2.如果在管理模式的陷入处理程序中再次触发陷入，则应当调用内核陷入，所以此处设置stvec寄存器为kerneltrap()所在地址；
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  //通过read保存用户pc到epc寄存器中
  p->trapframe->epc = r_sepc();
  
  //判断陷入标志是否为8，表示系统调用；
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    //不理解：
    //1.RISC-V 指令是 定长 4 字节（32 位）
    //2.在 trap 返回之前，把用户态的 PC 往前推进一条指令，避免重复执行之前引发 trap 的那条指令
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    //保存完寄存器状态，执行系统调用函数之前打开中断，在系统函数中可以被中断；
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
    //如果是中断，则根据中断码，再后续执行中断的处理，如果也不是中断，那么是用户陷入异常，应当杀死进程；
  } else {
    //用户陷入异常，杀死进程
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  //时钟中断的处理程序
  if(which_dev == 2){
    if(p->alarm_interval!=0&&++p->alarm_last>=p->alarm_interval){
      printf("alarm in trap!!!\n");
      p->alarm_last=0;

      //为了解决下述问题，在跳转回用户模式的时钟中断处理函数之前，保存当前的用户程序寄存器等状态等待恢复；
      //恢复可以在中断处理函数执行之后实现，也可以在中断处理函数中实现，本实验中在中断处理函数中调用sys_sigreturn()恢复；
      //1.tf_bak和tf都是指针类型,导致直接复制了地址而不是值,后续tf改变也反应到了tf_bak中;
      //2.此处有两种实现思路：分别是直接操作地址进行内存的复制，或者通过解引用进行逐字段的赋值；
      //3.一个偏底层一个更具有解释意义;但一般解引用更安全
      *(p->alarm_tf_bak)=*(p->trapframe);
      //memmove(p->alarm_tf_bak,p->trapframe,sizeof(struct trapframe));

      //将中断后返回用户空间后执行的地址改为了传入的时钟中断函数,但是原本用户正在执行的地址被覆盖了;
      //因此进入时钟中断函数后，无法正确返回用户程序；
      p->trapframe->epc=p->alarm_func;
    }
    yield();
  }

  //准备用户陷入的返回
  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  //因为要操作并恢复用户寄存器，关闭中断
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  //0.设置trap向量为trampoline页中的uservec地址，从而支持用户态 trap的进入处理
  //1.既恢复用户模式之后，如果触发陷入，应当调用usertrap()；
  //2.所以此处stvec寄存器应当设置为usertrap()所在地址；
  //3.此处TRAMPOLINE是蹦床页页框地址，后面uservec和trampoline是地址偏移，定位到usertrap()所在地址;
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.

  //保存内核页表，当用户模式陷入时，会通过该寄存器装载内核页表
  //要切换成哪个内核页表（kernel_satp）；
  p->trapframe->kernel_satp = r_satp();         // kernel page table

  //保存该进程的内核栈顶地址；p->kstack是创建进程时开辟的进程栈物理地址；
  //要用哪个内核栈（kernel_sp）；
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack

  //恢复为用户模式后，如果陷入，应当调用usertrap函数
  //trap 后跳转到哪里处理（kernel_trap）
  p->trapframe->kernel_trap = (uint64)usertrap;

  //保存当前执行的cpu
  //这是哪一个CPU（kernel_hartid）
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  //恢复状态标志
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  //装载标志位
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  //装载用户的程序指令位置:pc
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  //装载用户页表
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  //fn根据trampoline.S中生成的userret和trampoline函数地址偏移，确定userret()函数的地址指针；
  //解引用该函数指针,调用userret函数，返回用户程序；
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING){
    
    struct proc *p=myproc();
    if(p->alarm_interval!=0&&++p->alarm_last>=p->alarm_interval){
      printf("alarm in kerneltrap!!!\n");
      p->alarm_last=0;
    }    
    yield();
  }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

