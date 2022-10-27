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

#define BUFBUCKETN 13 // 为什么哈希表素数个桶可以降低散列冲突？
#define LOCKNAMELEN 10
#define HASH(no)   no%BUFBUCKETN

struct {
  struct buf buf[NBUF];
  struct spinlock htlock[BUFBUCKETN];
  char htlockname[BUFBUCKETN][LOCKNAMELEN]; // 每个锁的名字的空间
  struct buf ht[BUFBUCKETN];
} bcache;

void
binit(void)
{
  // 初始化hashtable和每个bucket的锁
  for(int i = 0; i < BUFBUCKETN; ++i)
  {
    snprintf(bcache.htlockname[i], LOCKNAMELEN, "bcache%d", i);
    initlock(&bcache.htlock[i], bcache.htlockname[i]);
    bcache.ht[i].prev = &bcache.ht[i];
    bcache.ht[i].next = &bcache.ht[i];
  }

  // 初始化每个bucket的buf
  uint currbucket;
  for(struct buf *b = bcache.buf; b < bcache.buf+NBUF; b++)
  {
    // 头插到对应的bucket
    currbucket = HASH((uint)(b - bcache.buf));
    b->next = bcache.ht[currbucket].next;
    b->prev = &bcache.ht[currbucket];
    bcache.ht[currbucket].next->prev = b;
    bcache.ht[currbucket].next = b;

    // 初始化buf的睡眠锁
    initsleeplock(&b->lock, "buffer");

  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucketNum = HASH(blockno);

  // 先检查其是否被当前的桶缓存
  acquire(&bcache.htlock[bucketNum]); // 获取对应桶的锁
  for(b = bcache.ht[bucketNum].next; b != &bcache.ht[bucketNum]; b = b->next)
  {
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.htlock[bucketNum]); // 找到了就释放对应桶的锁
      acquiresleep(&b->lock); // 并获取睡眠锁
      return b;
    }
  }

  // 如果其没有被缓存，则需要找一个使用时间最早且引用为0的（每个bufket中最早，不是全局最早）
  struct buf *tar = 0; // 目标buf
  for(b = bcache.ht[bucketNum].next; b != &bcache.ht[bucketNum]; b = b->next)
  {
    // 在当前链中找到时间戳最小的无引用buf
    if(b->refcnt == 0 && (tar == 0 || b->ticks < tar->ticks))
    {
      tar = b;
    }
  }

  uint nextbucketNum = (bucketNum + 1) % BUFBUCKETN; // 下一个桶
  while(tar == 0) // 如果在当前桶没有找到
  {
    if(nextbucketNum == bucketNum)
      break;
    
    acquire(&bcache.htlock[nextbucketNum]);

    // 在下一个链中找到时间戳最小的无引用buf
    for(b = bcache.ht[nextbucketNum].next; b != &bcache.ht[nextbucketNum]; b = b->next)
    {
      if(b->refcnt == 0 && (tar == 0 || b->ticks < tar->ticks))
        tar = b;
    }

    if(tar)
    {
      // 将tar从原链中取出
      tar->prev->next = tar->next;
      tar->next->prev = tar->prev;
      // 将其头插入当前链
      tar->next = bcache.ht[bucketNum].next;
      tar->prev = &bcache.ht[bucketNum];
      bcache.ht[bucketNum].next->prev = tar;
      bcache.ht[bucketNum].next = tar;
    }

    release(&bcache.htlock[nextbucketNum]);

    nextbucketNum = (nextbucketNum + 1) % BUFBUCKETN;
  }

  if(tar) // 如果在所有链中找到了位置
  {
    tar->dev = dev;
    tar->blockno = blockno;
    tar->valid = 0;
    tar->refcnt = 1;
    release(&bcache.htlock[bucketNum]);
    acquiresleep(&tar->lock);
    return tar;
  }

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  uint bucketNum = HASH(b->blockno);

  acquire(&bcache.htlock[bucketNum]);

  b->refcnt--;

  // 每个buf记录自己的最后被访问时间
  acquire(&tickslock);
  b->ticks = ticks;
  release(&tickslock);

  release(&bcache.htlock[bucketNum]);

  releasesleep(&b->lock);
}

// log层调用，保证当前块不会被直接写入磁盘
void
bpin(struct buf *b) {
  acquire(&bcache.htlock[HASH(b->blockno)]);
  b->refcnt++;
  release(&bcache.htlock[HASH(b->blockno)]);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.htlock[HASH(b->blockno)]);
  b->refcnt--;
  release(&bcache.htlock[HASH(b->blockno)]);
}


