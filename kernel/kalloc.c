// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end, int cpuNo);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

#define LOCKNAMELEN 10

struct {
  struct spinlock lock;
  char lockname[LOCKNAMELEN];
  struct run *freelist;
  int freePGnum;
} kmem[NCPU];

void
kinit()
{
  uint64 endPG = PGROUNDUP((uint64)end); // 第一个可以使用的page
  uint64 pageNum = (PHYSTOP - endPG) / PGSIZE; // 系统中在总page数量

  for(int i = 0; i < NCPU; ++i)
  {
    snprintf(kmem[i].lockname, LOCKNAMELEN, "kmem%d", i);
    initlock(&kmem[i].lock, kmem[i].lockname);
    // 每个核心的起始页面和终止页面：[currStartPG, currEndPG)
    uint64 currStartPG = endPG + (pageNum * i / NCPU) * PGSIZE;
    uint64 currEndPG = endPG + (pageNum * (i + 1) / NCPU) * PGSIZE;
    freerange((void*)currStartPG, (void*)currEndPG, i);
  }
}

void
freerange(void *pa_start, void *pa_end, int id)
{
  for(char *p = (char*)pa_start; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  {
    memset(p, 1, PGSIZE);
    struct run *r = (struct run*)p;
    r->next = kmem[id].freelist;
    kmem[id].freelist = r;
    kmem[id].freePGnum++;
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

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // 获取当前CPU的id
  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  kmem[id].freePGnum++;
  release(&kmem[id].lock);
}

// 当前CPU没有剩余的空间了，从剩余空间最多的CPU获取空间
int getmemFromOtherCPU(int curid)
{
  int maxFreePGnum = 0, tarid = 0;
  
  for(int i = 0; i < NCPU; ++i)
  {
    if(i == curid)
      continue;
    else
    {
      acquire(&kmem[i].lock);
      if(kmem[i].freePGnum > maxFreePGnum)
      {
        maxFreePGnum = kmem[i].freePGnum;
        tarid = i;
      }
      release(&kmem[i].lock);
    }
  }

  int getSuccess = 1;
  if(maxFreePGnum == 0) // 别的CPU也没有空间了
    return 0;
  else
  {
    acquire(&kmem[tarid].lock);
    acquire(&kmem[curid].lock);
    struct run *quickp = kmem[tarid].freelist, *slowp = kmem[tarid].freelist;
    while(quickp) // 快慢指针获取链表的一半
    {
      if(quickp->next == 0 || quickp->next->next == 0)
        break;
      quickp = quickp->next->next;
      slowp = slowp->next;
    }
    kmem[curid].freelist = slowp->next;
    slowp->next = 0;
    kmem[curid].freePGnum = kmem[tarid].freePGnum / 2;
    kmem[tarid].freePGnum -= kmem[curid].freePGnum;

    if(kmem[curid].freePGnum == 0)
      getSuccess = 0;
    release(&kmem[tarid].lock);
    release(&kmem[curid].lock);
  }
  
  return getSuccess;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // 获取当前CPU的id
  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
  {
    kmem[id].freelist = r->next;
    kmem[id].freePGnum--;
  }
  release(&kmem[id].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  else
  {
    if(getmemFromOtherCPU(id))
    {
      acquire(&kmem[id].lock);
      r = kmem[id].freelist;
      if(r)
      {
        kmem[id].freelist = r->next;
        kmem[id].freePGnum--;
      }
      release(&kmem[id].lock);
      memset((char*)r, 5, PGSIZE); // fill with junk
    }
    else
      return 0;
  }
  return (void*)r;
}
