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

struct refcnt {
  int refcnt;
  struct spinlock lock;
} *refcnts;

#define index_to_refcnts(pa) (((uint64)pa-(uint64)KERNBASE)/PGSIZE)

int refcnt(void* pa)
{
  if(pa < (void*)KERNBASE)
    panic("refcnt: not a ref tracking page");
  int i = index_to_refcnts(pa);
  struct refcnt* rc = &refcnts[i]; 
  acquire(&rc->lock);
  int cnt = rc->refcnt;
  release(&rc->lock);
  return cnt;
}

void pget(void* pa)
{
  // 只处理KERNBASE以上物理页的引用计数
  if(pa < (void*)KERNBASE){
    printf("%p", pa);
    panic("pget: not a ref tracking page");
  }
  int i = index_to_refcnts(pa);
  struct refcnt* rc = &refcnts[i]; 
  acquire(&rc->lock);
  rc->refcnt++;
  release(&rc->lock);
}

void pput(void* pa)
{
  // 只处理KERNBASE以上物理页的引用计数
 if(pa < (void*)KERNBASE)
    panic("pput: not a ref tracking page");
  int i = index_to_refcnts(pa);
  struct refcnt* rc = &refcnts[i]; 
  if(rc->refcnt==0)
    panic("pput: refcnt=0");
  acquire(&rc->lock);
  rc->refcnt--;
  release(&rc->lock);
}

void
kinit()
{
  int physpages = (PHYSTOP - PGROUNDUP((uint64)KERNBASE)) / PGSIZE;
  uint64 refcnt_size = physpages * sizeof(refcnts[0]);
  void* kmem_start = (void*)PGROUNDUP((uint64)end + refcnt_size); 

  refcnts=(struct refcnt*)end;

  initlock(&kmem.lock, "kmem");
  freerange(kmem_start, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    initlock(&refcnts[index_to_refcnts(p)].lock, "refcnt");
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 由调用者保证pa的引用计数为0
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

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
  
  if(r){
    refcnts[index_to_refcnts(r)].refcnt=1; // 重置引用计数
    memset((char*)r, 5, PGSIZE); // fill with junk
  }

  return (void*)r;
}
