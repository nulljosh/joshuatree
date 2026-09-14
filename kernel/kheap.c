/* First-fit free-list heap, grown a physical frame at a time via pmm.
   v57 (0.57.0): the "no splitting" gap flagged by the ponytail note above turned
   out to be real, not hypothetical, once measured against the code that
   actually calls kmalloc. `build`'s codegen path (kernel.c) allocates one
   large scratch buffer (BUILD_BUF_SIZE, checked at the call site) per
   generation, frees it, then a handful of small allocations (each app's
   short serve response) run afterward; without splitting, the first small
   reuse of that freed big block ate the whole thing, whole-block-per-
   request, so every subsequent small alloc pulled in a fresh physical
   frame instead of reusing the rest of what was already free right next
   to it. Fixed by carving reused free blocks down to the requested size
   (plus a small header) whenever enough is left over to be worth a
   separate block, pushing the remainder back onto the free list as its
   own block, adjacent and lower-addressed than the piece just handed out,
   which keeps the coalescing invariant above intact (list stays in
   strict decreasing-address order: the remainder is placed exactly where
   the original block was, and the newly-allocated piece is carved from
   its low end, so both stay in their correct relative order). MIN_SPLIT
   avoids carving off slivers too small to ever satisfy a real request
   (their own header would outweigh the split payload).
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
/* Don't split off a remainder too small to ever satisfy a real request;
   its own header would outweigh what's left for a caller to actually use. */
#define MIN_SPLIT_PAYLOAD 8u

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

    struct block *prev = 0;
    for (struct block *b = heap_head; b; prev = b, b = b->next) {
        if (b->free && b->size >= size) {
            /* Split off the unused tail into its own free block when
               there's enough left to be worth it, instead of handing the
               whole block to a request that only needed part of it. The
               remainder sits at the higher address (the tail of b's old
               payload), b keeps the lower address, which is exactly the
               ordering kfree's coalescing walk already assumes: the
               remainder is spliced into the list ahead of b (in b's old
               list slot), b's own list position is otherwise unchanged. */
            if (b->size >= size + (u32)sizeof(struct block) + MIN_SPLIT_PAYLOAD) {
                struct block *rem = (struct block *)((u32)(b + 1) + size);
                rem->size = b->size - size - (u32)sizeof(struct block);
                rem->free = 1;
                rem->next = b;
                if (prev) prev->next = rem; else heap_head = rem;
                b->size = size;
            }
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
