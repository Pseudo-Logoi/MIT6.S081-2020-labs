#include "param.h"
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"

//
// This file contains copyin_new() and copyinstr_new(), the
// replacements for copyin and coyinstr in vm.c.
//

static struct stats {
  int ncopyin;
  int ncopyinstr;
} stats;

int
statscopyin(char *buf, int sz) {
  int n;
  n = snprintf(buf, sz, "copyin: %d\n", stats.ncopyin);
  n += snprintf(buf+n, sz, "copyinstr: %d\n", stats.ncopyinstr);
  return n;
}

pte_t *walkforcopyin(pagetable_t pagetable, uint64 va, int alloc)
{
  for(int level = 2; level > 0; level--)
  {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V)
    {
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
// pagetable是进程的页表
// dst是内核空间虚拟地址
// srcva是用户空间虚拟地址
// len是拷贝的长度
int
copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  struct proc *p = myproc();

  if (srcva >= p->sz || srcva+len >= p->sz || srcva+len < srcva)
    return -1;

  pagetable_t pagetableKernel = p->pagetableKernel;

  uint64 curva = PGROUNDDOWN(srcva);
  uint64 endva = PGROUNDDOWN(srcva + len - 1);

  pte_t *kpte = walkforcopyin();

  memmove((void *) dst, (void *)srcva, len);
  stats.ncopyin++;   // XXX lock
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  struct proc *p = myproc();
  char *s = (char *) srcva;
  
  stats.ncopyinstr++;   // XXX lock
  for(int i = 0; i < max && srcva + i < p->sz; i++){
    dst[i] = s[i];
    if(s[i] == '\0')
      return 0;
  }
  return -1;
}
