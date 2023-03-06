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
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
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

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0; // 初始指向null
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);

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

  // 查看是否有空位
  uint16 start_pos = regs[E1000_TDT], cur_pos;
  uint16 mbuf_num = 0;
  struct mbuf *cur_mbuf = m;
  for(; cur_mbuf != 0; cur_mbuf = cur_mbuf->next, mbuf_num++) {
    if(mbuf_num >= TX_RING_SIZE) {
      release(&e1000_lock);
      return -1;
    }

    cur_pos = (start_pos + mbuf_num) % TX_RING_SIZE;

    if(!(tx_ring[cur_pos].status & E1000_TXD_STAT_DD))  { // E1000尚未完成先前相应的传输请求
      release(&e1000_lock);
      return -1;
    }

    if(tx_mbufs[cur_pos]) { // 使用mbuffree()释放从该描述符传输的最后一个mbuf（如果有）
      mbuffree(tx_mbufs[cur_pos]);
    }
  }

  // 填写描述符
  cur_mbuf = m;
  for(uint16 cur_count = 0; cur_count < mbuf_num; cur_count++, cur_mbuf = cur_mbuf->next) {
    cur_pos = (start_pos + cur_count) % TX_RING_SIZE;

    tx_ring[cur_pos].addr = (uint64)cur_mbuf->head;
    tx_ring[cur_pos].length = cur_mbuf->len;
    tx_ring[cur_pos].cmd = E1000_TXD_CMD_RS;
    if(cur_count == mbuf_num - 1) tx_ring[cur_pos].cmd |= E1000_TXD_CMD_EOP;
    tx_ring[cur_pos].status = 0;
    // tx_ring[cur_pos].cso;
    // tx_ring[cur_pos].css;
    // tx_ring[cur_pos].special;

    // 保存指向mbuf的指针，以便后续释放
    tx_mbufs[cur_pos] = cur_mbuf;
  }

  // 更新到下一个位置
  regs[E1000_TDT] = (start_pos + mbuf_num) % TX_RING_SIZE;

  // 成功
  release(&e1000_lock);
  return 0;
}



static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //

  uint16 cur_pos = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

  while(rx_ring[cur_pos].status & E1000_RXD_STAT_DD) {
    rx_mbufs[cur_pos]->len = rx_ring[cur_pos].length;
    net_rx(rx_mbufs[cur_pos]);

    rx_mbufs[cur_pos] = mbufalloc(0);
    rx_ring[cur_pos].addr = (uint64) rx_mbufs[cur_pos]->head;
    rx_ring[cur_pos].status = 0;

    cur_pos = (cur_pos + 1) % RX_RING_SIZE;
  }

  regs[E1000_RDT] = (cur_pos + RX_RING_SIZE - 1) % RX_RING_SIZE;

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
