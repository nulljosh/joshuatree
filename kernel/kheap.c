/* First-fit free-list heap, grown a physical frame at a time via pmm.
   ponytail: no splitting a larger free block on reuse, a whole block goes
   to a smaller request rather than being carved up, add real splitting if
   a workload's block-size mix ever makes that waste matter.
   v34 (0.34.0): used to hard-stop at 0x400000 (a frame handed back at or
   past there wasn't mapped by anything, so growth just stopped and
   kmalloc silently returned 0). Real fix, not a bigger constant: a frame
   past the base 4MB now gets identity-mapped on demand via
   paging_map_region() (already built for exactly this, PCI framebuffers
   used it first), the same real OOM only happening when pmm itself is
   out of frames or paging.c is out of spare page tables for new regions
   (MAX_EXTRA_TABLES, 16MB worth), not at an arbitrary 4MB line nothing
   about physical memory actually enforces. */
#include "kheap.h"
#include "pmm.h"
#include "paging.h"

typedef unsigned int u32;

struct block {
    u32 size;          /* payload size, not including this header */
    int free;
    struct block *next;
};

#define IDENTITY_MAP_LIMIT 0x400000
#define ALIGN(x) (((x) + 3u) & ~3u)

static struct block *heap_head = 0;
static u32 heap_next  = 0; /* next free byte to hand out */
static u32 heap_limit = 0; /* end of the currently frame-backed region */

static int grow_heap(u32 need) {
    while (heap_next + need > heap_limit) {
        u32 frame = pmm_alloc_frame();
        if (frame == 0) return 0; /* real OOM, no physical memory left at all */
        if (frame >= IDENTITY_MAP_LIMIT && !paging_map_region(frame, 4096)) {
            pmm_free_frame(frame); /* out of spare page tables (see paging.c's MAX_EXTRA_TABLES), give the frame back rather than leak it */
            return 0;
        }
        if (heap_limit == 0) heap_next = frame;
        heap_limit = frame + 4096;
    }
    return 1;
}

void *kmalloc(u32 size) {
    size = ALIGN(size);

    for (struct block *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            b->free = 0;
            return (void *)(b + 1);
        }
    }

    u32 need = size + sizeof(struct block);
    if (!grow_heap(need)) return 0;

    struct block *b = (struct block *)heap_next;
    heap_next += need;
    b->size = size;
    b->free = 0;
    b->next = heap_head;
    heap_head = b;
    return (void *)(b + 1);
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct block *b = (struct block *)ptr - 1;
    b->free = 1;

    /* Coalesce adjacent free blocks. The list is exactly in decreasing-
       address order: every new block is carved at the current heap_next
       (always higher than anything carved before) and pushed onto the
       head, so a node and its immediate successor in the list are always
       memory-adjacent, higher address first. Merge into the lower-address
       side (its header is the one actually sitting at the merged block's
       start) whenever both sides of a boundary are free, walking the
       whole list once so one free can close a run of several. */
    struct block *prev = 0;
    struct block *cur = heap_head;
    while (cur && cur->next) {
        struct block *nxt = cur->next;
        if (cur->free && nxt->free) {
            nxt->size += (u32)sizeof(struct block) + cur->size;
            if (prev) prev->next = nxt; else heap_head = nxt;
            cur = nxt;
        } else {
            prev = cur;
            cur = cur->next;
        }
    }
}
