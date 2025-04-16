// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {//内存指针链表
  struct run *next;
};

struct {
  struct spinlock lock;//锁
  struct run *freelist;//释放的内存空间指针链表
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");//初始化内存锁
  freerange(end, (void*)PHYSTOP);//置空所有内存,end是起点，phystop是终点
  //压栈思想是小地址后分配,大地址先分配
  //所以end虽然地址更小,但是实际一直在栈底,除非所有内存都被分配完了,end才会被取出;
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);//找到内存页的起始指针，既通过PGROUNDUP去掉当前指针的后缀，
  //使得p是pa_start的内存页起点指针
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)//内存页的大小为4096字节
    kfree(p);
  //从低位向高位释放内存，最多到PHYSTOP-PGSIZE结束；
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  //1.释放的指针位置应该是页内存大小的倍数
  //2.释放的指针位置应该>end指针的位置：end就是内存的起始位置指针；
  //3.PHYSTOP是指内存的物理地址终点;  
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);//释放该页的内存，将该指针起始位置开始往后的页内存大小全部置1;

  r = (struct run*)pa;//获取当前内存的指针，并转化为空闲内存指针链表形式；
  //有点栈的感觉了,释放的时候，将空闲的内存页指针压栈;
  //分配内存的时候,从栈定取出空闲的内存页指针，并分配内存；
  acquire(&kmem.lock);//上锁
  r->next = kmem.freelist;//将原本的空闲内存指针链接到当前释放的内存页后面；
  kmem.freelist = r;//将当前释放了的内存页指针置为空闲内存指针的头；
  release(&kmem.lock);//解锁
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)//分配内存
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;//从空闲内存页的起点取分配的空间
  if(r)//1表示当前指针指向的空间空闲
    kmem.freelist = r->next;//空闲内存页往后移一页
  release(&kmem.lock);

  if(r)//将该页分配数据
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

//获取空闲内存的大小
uint64
kfreemem(void){
  //1.获取kmem.freelist是空闲的内存页起点
  struct run *r;
  uint64 freemem=0;

  acquire(&kmem.lock);
  r = kmem.freelist;//获得空闲内存页的起点
  //&&r!=(struct run *)end
  while(r)//这个判断条件有没有太简单了,因为end可能被分配了是吗，所以end就不在栈底了，不能作为判断条件
  {
    freemem+=PGSIZE;
    r=r->next;//后移到下一页空闲页指针;
  }

  release(&kmem.lock);

  return freemem;
}