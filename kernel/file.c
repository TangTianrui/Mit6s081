//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

#include "fcntl.h"

//设备驱动，为每个设备提供输入和输出函数的指针
struct devsw devsw[NDEV];

//文件表，通过自旋锁维护所有的文件数组
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
//分配文件容器，从文件数组中取出空容器,进行返回
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
//增加文件的引用数量
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
//把file中状态存入传入的addr中；
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

//用于处理mmap的懒分配
int allocmmap(uint64 evaddr,uint64 scause){
  struct proc *p=myproc();
  //struct vma evma;
  int i=0;
  //1.找到vaddr对应的vma;
  for(;i<VMASZ;++i){
    if(p->vma_[i].is_used==0)continue;
    if(p->vma_[i].addr<=evaddr&&evaddr<p->vma_[i].addr+p->vma_[i].len){
      break;
      //找到对应的evma
    }
  }
  if(i==VMASZ) return -1;

  //2.根据vma的起点addr和该vaddr的偏移，确定要从文件中复制到物理内存中的数据；
  char *mem=kalloc();
  if(mem==0) return -1;
  memset(mem,0,PGSIZE);

  int pages=(evaddr-p->vma_[i].addr)/PGSIZE;
  //借鉴file.c/fileread()->f->type==FD_INODE
  struct file *f=p->vma_[i].file_;
  int r=0;

  ilock(f->ip);
  r=readi(f->ip,0,(uint64)mem,p->vma_[i].offset+PGSIZE*pages,PGSIZE);
  iunlock(f->ip);
  if(r==0){
    kfree(mem);
    return -1;
  } 

  //对是否可以写等属性进行判断,判断缺页异常号和操作的eva是否匹配
  if(scause==13&&f->readable==0) return -1;
  if(scause==15&&f->writable==0) return -1;

  //3.根据vma的属性创建用户页表条目；pte
  int prot=PTE_U;
  if(p->vma_[i].prot & PROT_READ) prot|=PTE_R;
  if(p->vma_[i].prot & PROT_WRITE) prot|=PTE_W;
  if(p->vma_[i].prot & PROT_EXEC) prot|=PTE_X;

  if(mappages(p->pagetable,p->vma_[i].addr+PGSIZE*pages,PGSIZE,(uint64)mem,prot)!=0){
    kfree(mem);
    return -1;
  }
  
  return 0;

}