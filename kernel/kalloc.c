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

// Per-CPU free list
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  char* names[NCPU]={"kmem0","kmem1","kmem2","kmem3","kmem4","kmem5","kmem6","kmem7"};
  for(int i = 0; i < NCPU; i++){
      initlock(&kmem[i].lock, names[i]);
  }
  freerange(end, (void*)PHYSTOP); // First give all free memory to the CPU running freerange.

  // int id = cpuid(); 
  // initlock(&kmem[id].lock, names[id]);

  // uint64 size_per_cpu = (PHYSTOP-PGROUNDUP((uint64)end)) / NCPU;
  // char* base = (char*)PGROUNDUP((uint64)end);
  // freerange(base + id*size_per_cpu, base + (id+1)*size_per_cpu);

}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
    panic("kfree");
  }

  push_off();
  int id = cpuid();
  pop_off();

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

}

static int list_alloc_n(struct run **freelist, struct run** freehead, struct run** freetail, int max){
  struct run* r = *freelist;
  struct run* n = r, *t;
  int i;
  for(i = 0; i < max && n; i++){
    t=n;
    n=n->next;
  }
  *freelist=n;
  *freehead=r;
  if(freetail)
    *freetail=t;
  return i;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r=0, *tail;

  push_off();

  int id = cpuid();
  int n_alloc;
  // int stealed=0;

  acquire(&kmem[id].lock);

  if((n_alloc=list_alloc_n(&kmem[id].freelist, &r, 0, 1))==0){
    // stealed=1;
    release(&kmem[id].lock);
    for(int i = 0; i < NCPU && !n_alloc; i++){
      if(i==id) 
        continue;
      acquire(&kmem[i].lock);
      n_alloc = list_alloc_n(&kmem[i].freelist, &r, &tail, 1);
      release(&kmem[i].lock);
    }
    acquire(&kmem[id].lock);
  }
  
  if(n_alloc > 1){
    tail->next = kmem[id].freelist;
    kmem[id].freelist = r->next;
  }
  release(&kmem[id].lock);

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}