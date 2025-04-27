// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUCKETSZ 13
//简单的hash算法,(devid*素数+blocknum)%桶size
#define HASH(devid,blocknum) ((devid*2591+blocknum)%BUCKETSZ)

struct buf_bucket{
  struct spinlock lock;

  //int freebuf;维护不了,在操作b时候是独立的

  //维护桶中buf
  //对于结构体,这里不应该设置为指针,因为初始化的时候没有分配物理内存;
  struct buf head;
  //不要freelist了,反正要遍历timestamp,不用每次释放从head移到freelist;
  //struct buf *freelist;
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  struct buf_bucket bucket[BUCKETSZ];
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  //struct buf head;
} bcache;



void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  char lockname[10];
  //初始化每一个buf桶
  for(int i=0;i<BUCKETSZ;++i){
    snprintf(lockname,sizeof(lockname),"bcache_%d",i);
    initlock(&bcache.bucket[i].lock,lockname);
    //初始化使用的buf块链表
    bcache.bucket[i].head.prev=&bcache.bucket[i].head;
    bcache.bucket[i].head.next=&bcache.bucket[i].head;
    /*
    //初始化空闲buf块链表
    bcache.bucket[i].freelist->prev= bcache.bucket[i].freelist;
    bcache.bucket[i].freelist->next = bcache.bucket[i].freelist;    
    */
    //初始化空闲buf块数
    //bcache.bucket[i].freebuf=0;
  }
  // Create linked list of buffers
  //全部初始化到bucket[0]中;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    //这里应该需要初始化时间戳？
    /*
    acquire(&tickslock);
    b->timestamp=ticks;
    release(&tickslock);    
    */

    //从前往后依次插入到空闲链表中
    b->next = bcache.bucket[0].head.next;
    b->prev = &bcache.bucket[0].head;
    initsleeplock(&b->lock, "buffer");
    bcache.bucket[0].head.next->prev = b;
    bcache.bucket[0].head.next = b;
    //++bcache.bucket[0].freebuf;//增加空闲块的数量
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  /*
  acquire(&bcache.lock);
  for(int i=0;i<BUCKETSZ;++i){
    printf("%d:%d ",i,bcache.bucket[i].freebuf);
  }
  printf("\n");
  release(&bcache.lock);  
  */


  struct buf *b;
  //这里不用再获取bcache的锁导致串行了，直接根据hash到的bucketid直接对bucket进行操作

  int buckid=HASH(dev,blockno);

  acquire(&bcache.bucket[buckid].lock);
  //1.先在自己的bucket中寻找是否有对应的buf
  for(b=bcache.bucket[buckid].head.next;b!=&bcache.bucket[buckid].head;b=b->next){
    if(b->dev == dev && b->blockno == blockno){
      ++b->refcnt;
      //获取时钟锁
      acquire(&tickslock);
      b->timestamp=ticks;
      release(&tickslock);
      //如果找到了释放bucket锁
      release(&bcache.bucket[buckid].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  //2.如果没找到,再判断本bucket是否有空闲buf可以使用,根据时间戳选择时间戳最小(释放最久的块),进行分配
  struct buf *ret;
  ret=0;
  //if(bcache.bucket[buckid].freebuf>0)
  //{
    //根据时间戳选择空闲进行分配
    //printf("bucketid:%d:freebuf:%d-----\n",buckid,bcache.bucket[buckid].freebuf);
  for(b=bcache.bucket[buckid].head.next;b!=&bcache.bucket[buckid].head;b=b->next){
    //printf("%d-refcnt:%d\n",i++,b->refcnt);
    //在freelist中肯定是refcnt==0的
    if(b->refcnt==0&&(ret==0||b->timestamp<ret->timestamp)){
    //找到时间戳最久的buf去分配,实现LRU:本bucket的LRU,非全局的LRU
      ret=b;
    }
  }
  if(ret!=0){
    b=ret;    //分配： 
    b->dev=dev;
    b->blockno=blockno;
    b->valid=0;
    b->refcnt=1;
    acquire(&tickslock);
    b->timestamp=ticks;
    release(&tickslock);

    //--bcache.bucket[buckid].freebuf;
    release(&bcache.bucket[buckid].lock);
    acquiresleep(&b->lock);
    return b;      
  }

  //}

  //3.否则需要遍历其他bucket,找到空闲块进行分配
  //release(&bcache.bucket[buckid].lock);
  for(int i=buckid,cycle=0;cycle<BUCKETSZ;++cycle,i=(i+1)%BUCKETSZ){
    //printf("bget in i=%d\n",i);
    if(i==buckid)continue;
    acquire(&bcache.bucket[i].lock);
    //找到有空闲块的bucket,找到该bucket的最久buf
    for(b=bcache.bucket[i].head.next;b!=&bcache.bucket[i].head;b=b->next){
      if(b->refcnt==0&&(ret==0||b->timestamp<ret->timestamp)){
        ret=b;
      }
    }
    //移动到bucktid的buckt中
    //acquire(&bcache.bucket[buckid].lock);
    //要注意这个地方会不会死锁；buckid之前释放了锁,如果有进程释放该buck资源,后面有进程想要从bucktid中抢夺buf;
    //则可能导致此处acquire不到，而其他进程想要获取当前获取的bucket[i]的lock,也获取不到,则死锁;
    //分析下来,前面不能释放锁；
    if(ret==0) {
      release(&bcache.bucket[i].lock);
      continue;
    }
    //将该buf从bucket[i]中摘除
    ret->next->prev=ret->prev;
    ret->prev->next=ret->next;
    //--bcache.bucket[i].freebuf;
    release(&bcache.bucket[i].lock);

    //添加到buckid中
    bcache.bucket[buckid].head.next->prev=ret;
    ret->next=bcache.bucket[buckid].head.next;
    ret->prev=&bcache.bucket[buckid].head;
    bcache.bucket[buckid].head.next=ret;
    b=ret;
    //分配： 
    b->dev=dev;
    b->blockno=blockno;
    b->valid=0;
    b->refcnt=1;
    acquire(&tickslock);
    b->timestamp=ticks;
    release(&tickslock);

    release(&bcache.bucket[buckid].lock);
    acquiresleep(&b->lock);
    return b;
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  //不用再更新时间戳,因为bget中更新了;
  if(!b->valid) {
    //从磁盘中读取数据到缓存块
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  //在get的时候已经加过锁了,这里再次判断是否拿到了该buf的锁,才能执行写操作
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  //将buf->b中的内容写入对应的硬件设备当中
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int buckid=HASH(b->dev,b->blockno);
  acquire(&bcache.bucket[buckid].lock);
  //引用的数量-1
  //如果该块缓存的引用=0,则释放；
  --b->refcnt;
  //更新时间戳
  acquire(&tickslock);
  b->timestamp=ticks;
  release(&tickslock);

  release(&bcache.bucket[buckid].lock);
}

//引用数量+1
void
bpin(struct buf *b) {
  int buckid=HASH(b->dev,b->blockno);
  acquire(&bcache.bucket[buckid].lock);
  ++b->refcnt;
  release(&bcache.bucket[buckid].lock);
}

//引用数量-1
void
bunpin(struct buf *b) {
  int buckid=HASH(b->dev,b->blockno);
  acquire(&bcache.bucket[buckid].lock);
  --b->refcnt;
  release(&bcache.bucket[buckid].lock);
}


