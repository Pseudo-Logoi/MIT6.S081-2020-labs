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

// 所有内存
// PHYSTOP = KERNBASE + 128*1024*1024
// 页面数量：128*1024*1024 / 4096 = 32768
// 这个int数组占用了64个page：32768 * 8 / 4096
struct {
  struct spinlock lock;
  int count[32768];
} kmemRef;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kmemRef.lock, "kmemRef");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  struct run *r;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  {
    memset(p, 1, PGSIZE);

    r = (struct run*)p;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree: wrong pa\n");

  // 根据引用计数释放页面
  acquire(&kmemRef.lock);
  int index = ((uint64)pa - KERNBASE) / PGSIZE;
  if(kmemRef.count[index] == 0) // 引用计数为0，错误，多次释放页面
  { 
    release(&kmemRef.lock);
    panic("kfree: kfree again\n");
  }
  kmemRef.count[index]--;
  if(kmemRef.count[index] != 0) // 还有其他引用
  {
    release(&kmemRef.lock);
    return;
  }
  release(&kmemRef.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  struct run *r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
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
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
  {
    memset((char*)r, 5, PGSIZE); // fill with junk

    acquire(&kmemRef.lock);
    kmemRef.count[((uint64)r - KERNBASE) / PGSIZE] = 1; // 新页面的引用计数是1
    release(&kmemRef.lock);
  }
  return (void*)r;
}

// pa页面的引用计数 增加n个
void addRefCount(void *pa, int n)
{
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("addRefCount: wrong pa\n");

  acquire(&kmemRef.lock);
  kmemRef.count[((uint64)pa - KERNBASE) / PGSIZE] += n; // 新页面的引用计数是1
  release(&kmemRef.lock);
}

int getRefCount(void  *pa)
{
  return kmemRef.count[((uint64)pa - KERNBASE) / PGSIZE];
}