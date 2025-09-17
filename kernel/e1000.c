#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
// 通过环形描述符数组+包内存空间数组共同确定包信息和包内容；
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
// 实际是一组寄存器
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();
  // 确保前面的操作同步到内存中

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  // 初始化所有描述符
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  // head和tail指针的位置初始化
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    // 接收端需要提前初始化接收缓冲区内存，并指定在描述符中，后续数据到达时才能直接通过描述符中的信息进行DMA
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
    // 这里是进行描述符中地址的赋值
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  // 接收缓冲区的头尾指针已经定义好，当前整个缓冲区内存都能存储/接受数据
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // 两个寄存器分别存储网卡的MAC地址，进行了端序处理

  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  acquire(&e1000_lock);
  uint32 index = regs[E1000_TDT];//尾指针，用于添加新的数据包；
  
  // 准入性判断
  if(!(tx_ring[index].status & E1000_TXD_STAT_DD)){
    // 说明这个位置还没有被发送，或者正在发送，而这个位置是尾部，说明缓冲区已经满了
    release(&e1000_lock);
    return -1;
  }

  // 清理原来的指针和指向的内存；
  if(tx_mbufs[index]){
    //对原来没有释放的内存进行释放；避免内存泄露
    mbuffree(tx_mbufs[index]);
    tx_mbufs[index] = 0;
  }

  // 进行数据包和控制信息的赋值
  tx_ring[index].addr = (uint64)m->head;
  tx_ring[index].length = m->len;
  // 配置cmd，EOP表示该buffer中含有一个完整的包，RS会告诉网卡在发送完成后，设置E1000_TXD_STAT_DD位
  tx_ring[index].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  
  tx_mbufs[index] = m;  // 添加包的实际内容到缓冲区中；
  //后续的发送就交由网卡来进行数据的发送了

  regs[E1000_TDT] = (regs[E1000_TDT]+1) % TX_RING_SIZE; //后移一位

  release(&e1000_lock);
  return 0;
}

static void
e1000_recv(void)
{
  // 被中断调用，可能接收多个包，需要对缓冲区中所有待接收的包都进行处理
  while(1){
    uint32 index = regs[E1000_RDT] +1 ;
    index %= RX_RING_SIZE;
    if(!(rx_ring[index].status & E1000_RXD_STAT_DD)){
      //说明当前遍历到的原始内存没有被存储，整个接收缓冲区都是空的，或者当前块正处于DMA写入的阶段，还没有写完。
      return;
    }
    
    // 运行到这里说明是有内容要处理的了；
    struct mbuf *m = rx_mbufs[index];//提取存储的内存；
    m->len = rx_ring[index].length;
    net_rx(m);//将取出的内存交由协议栈进行解包；

    //重新初始化这一片内存作为可接收的新内存
    rx_mbufs[index] = mbufalloc(0);
    if (!rx_mbufs[index])
      panic("e1000");
    rx_ring[index].addr = (uint64) rx_mbufs[index]->head;
    rx_ring[index].status = 0; 

    // 还需要进行循环向后遍历,会在前面的状态判断中终止
    regs[E1000_RDT] = index;//+1;
  }
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
