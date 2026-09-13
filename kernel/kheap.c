/* First-fit free-list heap, grown a physical frame at a time via pmm.
   ponytail: no coalescing on free, and the heap can only grow inside
   paging.c's identity-mapped first 4MB (a frame handed back at or past
   0x400000 isn't mapped yet, so growth just stops there and kmalloc
   returns 0 -- fine until something above 4MB needs to be heap-backed,
   which is exactly what the higher-half move later in v2 has to fix). */
#include "kheap.h"
#include "pmm.h"

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
        if (frame == 0 || frame >= IDENTITY_MAP_LIMIT) return 0; /* OOM or unmapped */
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
}
