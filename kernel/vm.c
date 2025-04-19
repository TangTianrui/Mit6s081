#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
//虚拟内存的初始化，初始化内核页表，应该lab中不用管
 void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  //只有trempoline不是直接映射;
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
  
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
//告诉硬件“从现在起，使用 kernel_pagetable 作为当前的页表”
//把内核页表添加到satp寄存器中;
//在进程切换时，satp 寄存器会被配置为当前进程的用户页表地址
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

//创建函数，使得将进程的内核页表副本载入到satp寄存器中
void
proc_inithart(pagetable_t kernel_pagetable_bak){
  w_satp(MAKE_SATP(kernel_pagetable_bak));//进程的内核页表副本载入satp寄存器
  sfence_vma();
  //printf("kernel_pagetable has been switched to process_kpgtbl_bak\n");
}

//用于创建进程的内核页表副本
pagetable_t
proc_kernel_pagetable_bak(struct proc *p){
  pagetable_t kernel_pagetable_bak;
  //1.分配一片空的页表
  kernel_pagetable_bak=(pagetable_t)kalloc();//分配了一个物理地址，但是页表是空的；
  if(kernel_pagetable_bak==0){//如果内存没有分配成功的话,返回0；
    return 0;
  }
  memset(kernel_pagetable_bak,0,PGSIZE);//整理物理内存并清0；

  //2.将kernal_pagetable的地址通过uvmmap添加映射到kernel_pagetable_back中；
  //参考vm.c/vminit()
  // uart registers
  uvmmap(kernel_pagetable_bak,UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  uvmmap(kernel_pagetable_bak,VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  uvmmap(kernel_pagetable_bak,CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  uvmmap(kernel_pagetable_bak,PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  uvmmap(kernel_pagetable_bak,KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  uvmmap(kernel_pagetable_bak,(uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  uvmmap(kernel_pagetable_bak,TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  //printf("    kernel_pagetable_bak has been create\n");
  return kernel_pagetable_bak;
  //return kernel_pagetable_bak;
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
//根据虚拟地址va和页表pagetable获取对应的pte条目；
//walk函数的后10位是被mask了的，因此walk函数返回的只是虚拟地址的页框，既所在页起点的物理地址
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)//虚拟地址超出范围，寻址失败；
    panic("walk");

  for(int level = 2; level > 0; level--) {//三级页表，这里寻两次
    pte_t *pte = &pagetable[PX(level, va)];//获取pte条目
    if(*pte & PTE_V) {//如果虚拟地址获取到了pte条目
      pagetable = (pagetable_t)PTE2PA(*pte);//pte条目转化为下一级的物理地址
    } else {//如果不存在该条目，则根据alloc标志位判断是否分配物理内存创建新的页表
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)//如果alloc标志位为0或者分配页表的物理内存出错，返回0；
        return 0;
      memset(pagetable, 0, PGSIZE);//初始化页表空间；
      *pte = PA2PTE(pagetable) | PTE_V;//基于物理地址，构造新建的pte条目；
    }
  }
  return &pagetable[PX(0, va)];//根据虚拟地址和检索的第三级页表起点,返回第三级页表寻址到的pte条目,既为目标PPN
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
//从虚拟地址和提供的页表中进行物理地址的寻址
//和kvmpa区别,kvmpa默认从内核页表中寻址,并且直接返回虚拟地址对应的物理地址
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)//分配的虚拟地址超过用户空间的虚拟地址
    return 0;

  pte = walk(pagetable, va, 0);//查询虚拟地址对应的三级pte
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);//三级pte指向的地址即为虚拟地址对应的页框物理地址，而不是虚拟地址指向的物理地址
  return pa;//返回页框，用户自己根据va添加页偏移
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
//对内核页表添加pte条目；
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  //调用为页表添加条目的函数，在内核页表中添加虚拟地址pte
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
//通过内核页表进行虚拟地址的寻址
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;//取出后12位,
  pte_t *pte;
  uint64 pa;
  pte = walk(myproc()->kernal_pagetable_bak, va, 0);//从内核页表中寻址
  if(pte == 0)//寻址失败
    panic("kvmpa");
  if((*pte & PTE_V) == 0)//寻到了未分配的地址，
    panic("kvmpa");
  pa = PTE2PA(*pte);//转为物理地址，其实此时并不完整，只有前44位ppn,没有后12位；
  
  //printf("kvmpa %p::%p\n",va,pa);//打印内核寻到的物理地址
  
  return pa+off;//前44位和后12位补全
}

uint64
kvmpa_kpgtbl(uint64 va)
{
  uint64 off = va % PGSIZE;//取出后12位,
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kernel_pagetable, va, 0);//从内核页表中寻址
  if(pte == 0)//寻址失败
    panic("kvmpa");
  if((*pte & PTE_V) == 0)//寻到了未分配的地址，
    panic("kvmpa");
  pa = PTE2PA(*pte);//转为物理地址，其实此时并不完整，只有前44位ppn,没有后12位；
  return pa+off;//前44位和后12位补全
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
//为虚拟地址创建pte条目
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);//向下取虚拟地址
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)//寻址失败
      return -1;
    if(*pte & PTE_V)//寻到的物理地址已经分配
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;//根据传递的权限参数配置pte条目
    if(a == last)//虚拟地址创建pte条目完成
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

//在进程的用户空间中，通过对内核页表的副本添加pte条目，使得进程可以通过该内核页表副本直接访问用户空间中的va指向的pa；
void
uvmmap(pagetable_t pagetable,uint64 va, uint64 pa, uint64 sz, int perm)
{
  //调用为页表添加条目的函数，在内核页表中添加虚拟地址pte
  if(mappages(pagetable, va, sz, pa, perm) != 0)
    panic("uvmmap");
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
//用户释放va指向的pa的物理内存后删除该某pte映射条目;
//freewalk不能释放pa的物理内存,unmap可以解除pte中va对pa的映射关系，并释放pa的物理内存资源；
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;//由于是用户空间，所以都是虚拟地址，需要用到进程的用户页表进行寻址映射；

  if((va % PGSIZE) != 0)//以页为单位进行物理内存的分配、寻址和释放
    panic("uvmunmap: not aligned");
  
  //由于需要释放多页物理内存，多次进行pte条目的解绑定
  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)//虚拟地址寻址没有寻到
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){//释放该va指向pa的物理内存；
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;//pte条目置空
  }
}

// create an empty user page table.
// returns 0 if out of memory.
//用户创建一个空页表，分配物理内存；
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();//分配物理内存
  memset(mem, 0, PGSIZE);
  //用户的地址空间从0开始，所以创建0对应的用户页表条目；
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);//将其他用户的状态转移到当前新建的用户中？
}


//根据n是正/负调用dealloc还是alloc
// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
//注意：该函数的返回值是新的sz，既扩大后的sz
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
//重新分配大小并添加pte，因为新分配了物理地址需要创建对应的pte，才能在使用时找到
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
//递归释放页表结构本身，既把页表项pte递归清空；
//但是如果pte指向的pa分配了物理内存，freewalk不能释放该pa,且由于释放了pte,导致该pa的寻址可能丢失；
//所以应当在freewalk之前unmap掉,unmap可以解除pte中va对pa的映射关系，并释放pa的物理内存资源；
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){//遍历页表的所有pte条目
    pte_t pte = pagetable[i];//根据索引得到第i个pte条目
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){//pte条目valid,
      //没有读/写/执行权限->表示指向下一级页表，而不是指向虚拟地址对应的物理地址。
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);//pte条目转化为pagtable；
      freewalk((pagetable_t)child);//递归遍历该pagetable的每一个pte条目
      pagetable[i] = 0;//将该pte条目置为0；
    } else if(pte & PTE_V){//pte条目有效，但是检索到了第三级，说明执行错误
      panic("freewalk: leaf");
    }//如果本身为0则跳过
  }
  kfree((void*)pagetable);//因为一个页表有512个pte条目,每一个条目8bytes,共4096bytes,正好占一页物理内存；
}


//下面是内核/用户态复制/传递，数据/内存的函数了

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

//将用户页表项复制到内核页表副本中
int
cp_u2k_kpgtbl(pagetable_t kpgtbl,pagetable_t pgtbl,uint64 start,uint64 end){
  pte_t *upte,*kpte;
  start=PGROUNDUP(start);//取下一页的页框地址
  for(int i=start;i<end;i+=PGSIZE){
    if((upte=walk(pgtbl,i,0))<0){
      panic("cp_u2k_kpgtbl upte walk no exist");
      return -1;
    }
    if((kpte=walk(kpgtbl,i,1))<0){
      panic("cp_u2k_kpgtbl kpte walk error");
      return -1;
    }    
    uint64 pa=PTE2PA(*upte);//取出用户虚拟地址对应的物理地址
    uint64 flag=PTE_FLAGS(*upte)&(~PTE_U);//取出标志位并取消PTE_U;
    //此处不能直接用mappages()进行绑定，应当参考mappages提取底层实现，否则对同样的虚拟地址mappages会报remap错误；
    /*
    if(mappages(kpgtbl,i,PGSIZE,pa,flag)<0){
      panic("cp_u2k_kpgtbl kpte walk error");
      return -1;
    }    
    */
    *kpte=PA2PTE(pa)|flag;//修改内核页表副本的虚拟地址i的映射条目pte,使其和用户虚拟地址指向相同的地址;
  }
  return 0;
}


// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
//内核向用户态传递参数，dstva是通过argvaddr()获取的实参地址，src是内核想要传递的内存起始指针，len是传递的字节数；
//pagetable应该就是当前进程的根页表；
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
//原本的copyin函数,根据传入的用户页表和虚拟地址,检索对应的物理地址;
//再将物理地址上的数据复制到内核提供的目标地址中；
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len){ 
  return copyin_new(pagetable,dst,srcva,len);
}

/*
{
  uint64 n, va0, pa0=0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  if(pa0!=0){
    printf("copyin %p:%p-->%p:%p\n",srcva,pa0,(uint64)dst,kvmpa((uint64)dst));//打印虚拟地址以及对应复制到的内容
  }  

  
  return 0;
}
*/


// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  return copyinstr_new(pagetable,dst,srcva,max);
}
/*
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}
*/


const static char *pre[]={"..",".. ..",".. .. .."};//代表层级的前缀字符串输出；

//深度优先打印页表
void vmprint(pagetable_t pagetable,uint64 pg_dep){
  //printf("vmprint\n");
  //if(pg_dep>2) return;//pte只有3层页表
  if(pg_dep==0){
    printf("page table %p\n",pagetable);
  }//第一次调用，打印提示词；
  
  for(int i=0;i<512;++i){
    pte_t pte=pagetable[i];//遍历所有页表
    if(pte&PTE_V){//pte条目有效
      uint64 child=PTE2PA(pte);//将pte条目转化为下一级的页表
      printf("%s%d: pte %p pa %p\n",pre[pg_dep],i,pte,child);//打印输出
      if((pte&(PTE_R|PTE_W|PTE_X))==0){//说明没有到最后一级页表
        vmprint((pagetable_t)child,1+pg_dep);//递归调用；
      }
    }
  }
  return;
}