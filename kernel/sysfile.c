//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// handleVMA(p, p->vmas[i], va);
int handleVMA(struct proc *p, struct vma* v, uint64 va)
{
  // 申请一个物理页面
  uint64 pa;
  if((pa = (uint64)kalloc()) == 0)
    return -1;

  // 先清空为0
  memset((void*)pa, 0, PGSIZE);

  // 将文件的内容复制到这个物理页面，复制4096个字节
  int npage = (va - v->addr) / PGSIZE;
  ilock(v->f->ip);
  if(readi(v->f->ip, 0, pa, v->offset + npage * PGSIZE, PGSIZE) < 0)
    return -1;
  iunlock(v->f->ip);

  if(mappages(p->pagetable, va, PGSIZE, pa, (v->port << 1) | PTE_U) < 0)// v->port << 1
  {
    kfree((void*)pa);
    return -1;
  }

  return 0;
}

uint64 sys_mmap(void)
{
  uint64 addr;
  int length, port, flags, fd, offset;
  struct file *f;
  if(argaddr(0, &addr) < 0 || argint(1, &length) < 0 || argint(2, &port) < 0||
     argint(3, &flags) < 0 || argfd(4, &fd, &f) < 0 || argint(5, &offset) < 0)
    return -1; // 0xffffffffffffffff
  
  printf("    before mmap: addr: %p, len: %d * PGSIZE + %d\n", addr, length / PGSIZE, length % PGSIZE);

  if(f->writable == 0 && port & PROT_WRITE && flags == MAP_SHARED)
    return -1;

  struct proc *p = myproc();

  if(addr < p->sz)
    addr = p->sz; // 用户不指定则自动分配

  // 找一个空闲的vma位置
  int i = 0;
  for(; i < 16; ++i)
  {
    if(p->vmas[i].length == 0)
    {
      p->vmas[i].addr = addr;    
      p->vmas[i].length = length;
      p->vmas[i].port = port;    
      p->vmas[i].flags = flags;
      p->vmas[i].fd = fd;
      p->vmas[i].f = f;
      p->vmas[i].offset = offset;
      break;
    }
  }
  if(i == 16)
    return -1; // 没有空闲位置

  p->sz += length; // 惰性分配

  filedup(f); // 文件增加引用，防止文件被释放

  printf("    after mmap: [%p, %p)\n", addr, addr + length);
  return addr;
}

uint64 sys_munmap(void)
{
  // 获取输入参数
  uint64 addr;
  int length;
  if(argaddr(0, &addr) < 0 || argint(1, &length) < 0 || length <= 0)
    return -1; // 0xffffffffffffffff

  printf("    before munmap: addr: %p, len: %d * PGSIZE + %d\n", addr, length / PGSIZE, length % PGSIZE);

  // 查找这个addr对应哪个vma
  struct proc *p = myproc();
  struct vma *targetvma;
  {
    int i = 0;
    for(; i < 16; ++i)
    {
      if(p->vmas[i].addr <= addr && (addr + length) <= (p->vmas[i].addr + p->vmas[i].length)) // 判断输入的范围需要满足条件
        break;
    }
    if(i == 16)
      return -1;
    targetvma = &p->vmas[i];
  }

  // 如果是share模式，则需要写回文件
  if(targetvma->flags == MAP_SHARED)
  {
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE; // 最多可以一次性写入的字节
    int curOff = targetvma->offset + (addr - targetvma->addr); // 根据输入地址计算文件起始偏移

    int tarLength = length;
    if(targetvma->f->ip->size < tarLength + curOff) // 要写入的内容超过了文件大小
      tarLength = targetvma->f->ip->size - curOff;
    tarLength = tarLength < 0 ? 0 : tarLength;

    printf("        tarLength: %d\n", tarLength);
    int i = 0; // i为已写入的在字节
    while(i < tarLength){
      int curLen = tarLength - i;
      if(curLen > max)
        curLen = max; // 如果写入的字节过多，则写max个字节

      printf("        start write file[%d]: start va %p, start offset %d, len %d\n", targetvma->fd, addr + i, curOff, curLen);
      begin_op();
      ilock(targetvma->f->ip);
      int r; // writei写入了多少字节
      if ((r = writei(targetvma->f->ip, 1, addr + i, curOff, curLen)) > 0)
      {
        curOff += r;
      }
      iunlock(targetvma->f->ip);
      end_op();
      printf("          end write file[%d]: file offset %d, ip size %d\n", targetvma->fd, targetvma->f->off, targetvma->f->ip->size);
      if(r != curLen){
        // error from writei
        break;
      }
      i += r;
    }
    if(i != tarLength)
    {
      printf("        wrong write file, %d\n", i);
      return -1;
    }
  }

  // 取消映射并释放物理页面
  // 暂不考虑对齐
  uvmunmap(p->pagetable, addr, length / PGSIZE, 1);

  // 因为不会在区域上打洞（题目要求），所以有且仅有以下几种情况：全部取消、取消前半部分、取消后半部分
  // 情况1：map的区域被全部取消
  if(addr == targetvma->addr && length == targetvma->length)
  {
    // 释放文件
    fileclose(targetvma->f);
    // 归零这个vma
    targetvma->length = 0; // 长度记为0，使得其可以被重用
    // 更新进程的大小
    int maxsz = 0, tempsz;
    for(int i = 0; i < 16; ++i)
    {
      if((tempsz = p->vmas[i].addr + p->vmas[i].length) > maxsz)
        maxsz = tempsz;
    }
    p->sz = maxsz;
  }
  // 情况2：取消前半部分，addr，length，offset都需要修改
  else if(addr == targetvma->addr) // addr相等，length相等，则是在取消前半部分
  {
    targetvma->addr += length;
    targetvma->length -= length;
    targetvma->offset += length;
  }
  // 情况3：取消后半部分，仅更新length即可
  else 
  {
    targetvma->length -= length;
  }

  printf("    after munmap: [%p, %p)\n", targetvma->addr, targetvma->addr + targetvma->length);
  return 0;
}