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

//物理内存页
extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

//扩展为物理内存数组,为每个CPU维护
struct {
  struct spinlock lock;
  struct run *freelist;
  int free_pgsz;
} kmem[NCPU];

char locknamebuf[8];

void
kinit()
{
  for(int i=0;i<NCPU;++i){
    //初始化每一个CPU内存链表的锁;
    snprintf(locknamebuf,sizeof(locknamebuf),"keme_%d",i);
    initlock(&kmem[i].lock, locknamebuf);
    acquire(&kmem[i].lock);
    //初始化size
    kmem->free_pgsz=0;
    release(&kmem[i].lock);   
  }
  //初始化时由当前cpu,将所有空闲页分配到自己的内存链表中
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  //关闭cpu的中断;
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");


  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  //获取当前cpu的id,为了保证当前cpu的信息都是在当前cpu中执行并不会被修改,应当在使用cpu信息过程中关闭中断
  int cid=cpuid();

  acquire(&kmem[cid].lock);
  r->next = kmem[cid].freelist;
  kmem[cid].freelist = r;
  //空闲的内存页数量增加
  ++kmem[cid].free_pgsz;
  release(&kmem[cid].lock);

  //使用完之后打开中断
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r=0;
  push_off();
  int cid=cpuid();

  acquire(&kmem[cid].lock);
  if(kmem[cid].free_pgsz>0){
    r = kmem[cid].freelist;
    if(r){
      kmem[cid].freelist = r->next;
      --kmem[cid].free_pgsz;
    }
    release(&kmem[cid].lock);
  }
  else{//当前cpu的物理内存链表没有空闲页，需要抢夺
    release(&kmem[cid].lock);
    for(int i=0;i<NCPU;++i){
      if(i==cid) continue;//自己,不用判断了
      acquire(&kmem[i].lock);
      if(kmem[i].free_pgsz==0){
        release(&kmem[i].lock);
        continue; 
      }
      //找到了有空余资源的,需要绑定到当前cpu中;
      r=kmem[i].freelist;
      if(r){//获取了该cpu的一页物理内存
        kmem[i].freelist=r->next;//保持原本cpu的空闲物理页的连接关系
        --kmem[i].free_pgsz;
      }
      release(&kmem[i].lock);
      break;
    }
  }
  pop_off();
  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk    
  }
  return (void*)r;
}
