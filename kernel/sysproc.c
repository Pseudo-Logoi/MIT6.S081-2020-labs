#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64 sys_exit(void) {
    int n;
    if (argint(0, &n) < 0) return -1;
    exit(n);
    return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
    uint64 p;
    if (argaddr(0, &p) < 0) return -1;
    return wait(p);
}

uint64 sys_sbrk(void) {
    // 获取输入参数
    int n;
    if (argint(0, &n) < 0) return -1;

    // 更新进程的sz，如果扩张，进改变sz
    struct proc* p = myproc();
    uint64 oldsz   = p->sz;
    uint64 newsz   = oldsz + n;
    p->sz          = newsz;

    // 如果收缩，根据收缩大小将pte设置为无效
    if (newsz < oldsz) {
        uint64 startpage = PGROUNDUP(newsz), endpage = PGROUNDDOWN(oldsz);
        uvmunmap(p->pagetable, startpage, (endpage - startpage) / PGSIZE + 1, 1);
    }

    return oldsz;
}

uint64 sys_sleep(void) {
    int n;
    uint ticks0;

    if (argint(0, &n) < 0) return -1;
    acquire(&tickslock);
    ticks0 = ticks;
    while (ticks - ticks0 < n) {
        if (myproc()->killed) {
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

uint64 sys_kill(void) {
    int pid;

    if (argint(0, &pid) < 0) return -1;
    return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
    uint xticks;

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
    return xticks;
}
