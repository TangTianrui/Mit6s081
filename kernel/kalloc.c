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

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

//通过hash表管理每个物理页的引用次数
struct cowlinks{
  struct spinlock lock;
  int count[PHYSTOP / PGSIZE];  // 引用计数
} cowlinks;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&cowlinks.lock, "cowlinks");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    cowlinks.count[(uint64)p / PGSIZE] = 1;//初始化计数为1,free后为0;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&cowlinks.lock);
  if(--cowlinks.count[(uint64)pa/PGSIZE]==0){
    //数量-1,如果没有其他进程在使用这个地址了,就可以清除了
    release(&cowlinks.lock);
    r=(struct run* )pa;
  
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
  }
  else{
    release(&cowlinks.lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    
    acquire(&cowlinks.lock);
    //分配时设置links的数目为1；
    cowlinks.count[(uint64)r/PGSIZE]=1;
    release(&cowlinks.lock);
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

//返回该物理地址的cow后副本数量；
int kcow_links(void* pa) {
  return cowlinks.count[(uint64)pa / PGSIZE];
}

//给对应物理地址增加cow的引用计数；
int kadd_cowlinks(void* pa) {
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    return -1;
  acquire(&cowlinks.lock);
  ++cowlinks.count[(uint64)pa / PGSIZE];
  release(&cowlinks.lock);
  return 0;
}