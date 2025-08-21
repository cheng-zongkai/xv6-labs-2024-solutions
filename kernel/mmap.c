#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "memlayout.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

extern int argfd(int n, int *pfd, struct file **pf);
#define MIN(a, b) ((a) < (b) ? (a) : (b))

// void* mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
uint64
sys_mmap(void)
{
  struct vma new_vma;
  struct proc* p = myproc();

  // we can assume that addr will always be zero
  // so it's our job to determine vma's address
  argaddr(0, &new_vma.addr); 

  argaddr(1, &new_vma.len);
  argint(2, &new_vma.prot);
  argint(3, &new_vma.flags);
  
  if(new_vma.len==0)
    return -1;

  if(argfd(4, 0, &new_vma.f)<0)
    return -1;
  
  argaddr(5, (uint64*)&new_vma.offset);

  // mmap doesn't allow read/write mapping of a file opened read-only.
  if((new_vma.flags & MAP_SHARED) && !new_vma.f->writable && (new_vma.prot & PROT_WRITE))
    return -1;
  
  // find an unused region in the process's address space in which to map the file
  uint64 lowest_vma_addr = TRAPFRAME;
  for(int i = 0; i < NELEM(p->vma); i++){
    if(p->vma[i].len!=0){ // vaild vma
      lowest_vma_addr = MIN(lowest_vma_addr, p->vma[i].addr);
    }
  }
  // addr should be page-aligned
  new_vma.addr = PGROUNDDOWN(lowest_vma_addr - new_vma.len); 
  
  // increase the file's reference count 
  // so that the structure doesn't disappear when the file is closed
  new_vma.f=filedup(new_vma.f); 

  // find an unused vma slot
  int i;
  for(i = 0; i < NELEM(p->vma); i++){
    if(!p->vma[i].len)
      break;
  }
  // commit new_vma to process's vma list
  memmove(&p->vma[i], &new_vma, sizeof(struct vma));
  
  return new_vma.addr;
}

extern int munmap(struct proc* p, uint64 va, size_t len);
uint64
sys_munmap(void)
{
  uint64 va, len;

  argaddr(0, &va); 
  argaddr(1, &len); 

  return munmap(myproc(), va, len);
}

int munmap(struct proc* p, uint64 va, size_t len)
{
  struct vma* myvma;
  struct inode* ip;

  // find the vma which va falls in
  myvma = 0;
  for(int i = 0; i < NELEM(p->vma); i++){
    if(p->vma[i].len && p->vma[i].addr <= va && va < p->vma[i].addr + p->vma[i].len){
      myvma = &p->vma[i];
      break;
    }
  }

  // this va is not mmapped
  if(!myvma){
    printf("%p\n", (void*)va);
    panic("munmap: va is not mmapped");
    return -1;
  }
    
  va=PGROUNDDOWN(va);
  ip = myvma->f->ip;

  if(myvma->addr+myvma->len < va+len){
    panic("munmap: len exceeds range");
    return -1;
  }
  
  for(uint64 a=va; a < va+len; a+=PGSIZE){
    pte_t* pte = walk(p->pagetable, a, 0);
    if(pte==0 || (*pte & PTE_V) == 0) // this page was not used (so no memory allocated), no need to unmap it
      continue;
    if((myvma->flags & MAP_SHARED) && (*pte & PTE_D)){
      // write back to file
      uint64 size = ip->size;
      uint64 offset = myvma->offset + a - myvma->addr;
      if(offset < size){
        uint64 n = size - offset;
        if(n > PGSIZE)
          n=PGSIZE;
        begin_op();
        if(writei(ip, 0, PTE2PA(*pte), offset, n) < n)
          panic("munmap: writei");
        end_op();
      }
    }
    uvmunmap(p->pagetable, a, 1, 1);
  }

  // update this process's vma
  // only unmap at the start, or at the end, or the whole region
  
  myvma->len -= len;

  if(va == myvma->addr){
    // unmaps at the start
    myvma->addr+=len;
    myvma->offset+=len; // important, record file writeback offset when VMA's addr changed
  }else{
    // not at the start, unmaps to the end
    // myvma->addr didn't change, does nothing
  }

  if(myvma->len==0){
    // munmap removes all pages of a previous mmap, decrement the reference count of the corresponding struct file
    fileclose(myvma->f);
  }

  return 0;
}
