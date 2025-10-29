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

#define NBUCKET 13  // number of hash buckets (prime number for better distribution)

struct hashbucket {
  struct spinlock lock;
  struct buf head;  // head of circular doubly-linked list
  uint ticks;       // for LRU timestamp
};

struct {
  struct buf buf[NBUF];
  struct hashbucket bucket[NBUCKET];
} bcache;

static int
hash(uint dev, uint blockno)
{
  return ((dev << 16) | blockno) % NBUCKET;
}

void
binit(void)
{
  struct buf *b;
  char lockname[16];

  // Initialize hash buckets
  for(int i = 0; i < NBUCKET; i++){
    snprintf(lockname, sizeof(lockname), "bcache%d", i);
    initlock(&bcache.bucket[i].lock, lockname);
    bcache.bucket[i].head.prev = &bcache.bucket[i].head;
    bcache.bucket[i].head.next = &bcache.bucket[i].head;
    bcache.bucket[i].ticks = 0;
  }

  // Distribute buffers across buckets in round-robin fashion
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    int bucket_id = (b - bcache.buf) % NBUCKET;
    b->next = bcache.bucket[bucket_id].head.next;
    b->prev = &bcache.bucket[bucket_id].head;
    initsleeplock(&b->lock, "buffer");
    bcache.bucket[bucket_id].head.next->prev = b;
    bcache.bucket[bucket_id].head.next = b;
    b->timestamp = 0;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int bucket_id = hash(dev, blockno);

  acquire(&bcache.bucket[bucket_id].lock);

  // Is the block already cached in this bucket?
  for(b = bcache.bucket[bucket_id].head.next; b != &bcache.bucket[bucket_id].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.bucket[bucket_id].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached in the target bucket.
  // First, try to recycle an unused buffer in the same bucket.
  b = 0;
  struct buf *lru = 0;
  for(struct buf *tmp = bcache.bucket[bucket_id].head.next; 
      tmp != &bcache.bucket[bucket_id].head; tmp = tmp->next){
    if(tmp->refcnt == 0) {
      if(!lru || tmp->timestamp < lru->timestamp) {
        lru = tmp;
      }
    }
  }

  if(lru) {
    b = lru;
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    release(&bcache.bucket[bucket_id].lock);
    acquiresleep(&b->lock);
    return b;
  }

  // No unused buffer in target bucket, search other buckets
  release(&bcache.bucket[bucket_id].lock);
  
  for(int i = 0; i < NBUCKET; i++) {
    if(i == bucket_id) continue;  // already checked
    
    acquire(&bcache.bucket[i].lock);
    lru = 0;
    for(struct buf *tmp = bcache.bucket[i].head.next; 
        tmp != &bcache.bucket[i].head; tmp = tmp->next){
      if(tmp->refcnt == 0) {
        if(!lru || tmp->timestamp < lru->timestamp) {
          lru = tmp;
        }
      }
    }
    
    if(lru) {
      // Acquire both locks in consistent order to avoid deadlock
      // Always acquire lower-numbered bucket lock first
      if(i < bucket_id) {
        // We already hold bucket i lock
        acquire(&bcache.bucket[bucket_id].lock);
      } else {
        // Release i, acquire in order: bucket_id then i
        release(&bcache.bucket[i].lock);
        acquire(&bcache.bucket[bucket_id].lock);
        acquire(&bcache.bucket[i].lock);
        
        // Re-check that lru is still valid (refcnt could have changed)
        if(lru->refcnt != 0) {
          release(&bcache.bucket[i].lock);
          release(&bcache.bucket[bucket_id].lock);
          continue;  // Try next bucket
        }
      }
      
      // Remove from source bucket
      lru->next->prev = lru->prev;
      lru->prev->next = lru->next;
      release(&bcache.bucket[i].lock);
      
      // Add to target bucket (we still hold bucket_id lock)
      lru->next = bcache.bucket[bucket_id].head.next;
      lru->prev = &bcache.bucket[bucket_id].head;
      bcache.bucket[bucket_id].head.next->prev = lru;
      bcache.bucket[bucket_id].head.next = lru;
      
      lru->dev = dev;
      lru->blockno = blockno;
      lru->valid = 0;
      lru->refcnt = 1;
      release(&bcache.bucket[bucket_id].lock);
      acquiresleep(&lru->lock);
      return lru;
    }
    release(&bcache.bucket[i].lock);
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
// Update timestamp for LRU.
void
brelse(struct buf *b)
{
  int bucket_id;
  
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  bucket_id = hash(b->dev, b->blockno);
  acquire(&bcache.bucket[bucket_id].lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // Update timestamp for LRU using per-bucket counter
    b->timestamp = ++bcache.bucket[bucket_id].ticks;
  }
  release(&bcache.bucket[bucket_id].lock);
}

void
bpin(struct buf *b) {
  int bucket_id = hash(b->dev, b->blockno);
  acquire(&bcache.bucket[bucket_id].lock);
  b->refcnt++;
  release(&bcache.bucket[bucket_id].lock);
}

void
bunpin(struct buf *b) {
  int bucket_id = hash(b->dev, b->blockno);
  acquire(&bcache.bucket[bucket_id].lock);
  b->refcnt--;
  release(&bcache.bucket[bucket_id].lock);
}


