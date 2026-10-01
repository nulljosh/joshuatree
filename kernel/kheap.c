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
#include "irqlock.h"
#include "serial.h"

typedef unsigned int u32;

struct block {
    u32 size;          /* payload size, not including this header */
    int free;
    struct block *next;
};

/* v74 (0.66.0): 8MB, matching paging.c's BASE_MAP_TABLES (2 tables) now
   that the kernel image itself crosses 4MB; frames inside the base map
   are already reachable, anything past it still gets mapped on demand. */
#define IDENTITY_MAP_LIMIT 0x800000
#define ALIGN(x) (((x) + 3u) & ~3u)
/* Don't split off a remainder too small to ever satisfy a real request;
   the minimum allocation is 4 bytes after ALIGN(), so splittable remainders
   need at least 4 bytes of payload to ever be reused. Reduced from 8 in v76
   to improve fragmentation recovery without sacrificing correctness. */
#define MIN_SPLIT_PAYLOAD 4u

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
        else if (frame != heap_limit) {
            /* v64 (0.61.0): the frame pmm handed back is NOT the one right
               after the current region, and this code used to bump
               heap_limit anyway, as if it were. Real, found by a triple
               fault, present on v62 too: task_create() interleaves a
               stack kmalloc with paging_new_task_directory()'s three pmm
               frames, so the second task's 4KB stack got carved across a
               gap that was actually the first task's page directory, and
               the fabricated frame (zeros plus task_b's EIP) landed on
               PDE 0-3 of that directory; the first `pop` on the new task's
               stack then faulted under a directory that no longer mapped
               low memory. The bytes between heap_next and the old limit
               can't join a block that continues into the new frame, so
               park them as their own free block when there's room for a
               header plus something usable, otherwise let them go, and
               start the next carve at the new frame. */
            u32 tail = heap_limit - heap_next;
            if (tail >= sizeof(struct block) + MIN_SPLIT_PAYLOAD) {
                struct block *t = (struct block *)heap_next;
                t->size = tail - (u32)sizeof(struct block);
                t->free = 1;
                t->next = heap_head;
                heap_head = t;
            }
            heap_next = frame;
        }
        heap_limit = frame + 4096;
    }
    return 1;
}

static void *kmalloc_locked(u32 size) {
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

static void kfree_locked(void *ptr) {
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
       whole list once so one free can close a run of several.
       v64 (0.61.0): adjacency is checked, not assumed. grow_heap can now
       start a fresh region when pmm hands back a non-contiguous frame
       (its parked tail block breaks the strict decreasing-address order
       for that one pair), and merging two list neighbours that aren't
       memory neighbours would fold whatever sits in the gap, another
       subsystem's page directory in the case that found this, into a
       free block. Two blocks merge only when the lower one's payload ends
       exactly at the higher one's header. */
    struct block *prev = 0;
    struct block *cur = heap_head;
    while (cur && cur->next) {
        struct block *nxt = cur->next;
        if (cur->free && nxt->free && (u32)nxt + sizeof(struct block) + nxt->size == (u32)cur) {
            nxt->size += (u32)sizeof(struct block) + cur->size;
            if (prev) prev->next = nxt; else heap_head = nxt;
            cur = nxt;
        } else {
            prev = cur;
            cur = cur->next;
        }
    }
}

/* 1.9.23: the public entry points are cli critical sections (irqlock.h).
   Task 0 runs with interrupts on and a timer tick can land anywhere inside
   the free-list walk above; the ring-3 task it switches to then enters the
   same walk through a syscall. Two walkers on one singly linked list is a
   double handout or a lost block, so neither walker can be interrupted. */
void *kmalloc(u32 size) { unsigned int f = irq_save(); void *p = kmalloc_locked(size); irq_restore(f); return p; }
void kfree(void *ptr) { unsigned int f = irq_save(); kfree_locked(ptr); irq_restore(f); }

/* 1.9.23: consistency walk, the proof behind tools/checks/ring3stress-check.py.
   Every header must sit inside the frame-backed region, carry a sane size
   and flag, and sit above its list successor (the decreasing-address
   invariant kfree's coalescing relies on). A cycle shows up as the walk
   running past any plausible block count. Logs one serial line, returns 1
   when the heap is intact. */
int kheap_check(void) {
    unsigned int f = irq_save();
    u32 n = 0, bad = 0;
    for (struct block *b = heap_head; b; b = b->next) {
        if (++n > 200000u) { bad = 1; break; }
        if ((u32)b < 0x100000u || (u32)b + sizeof(struct block) > heap_limit) { bad = 2; break; }
        if (b->size > 0x1000000u || (b->free != 0 && b->free != 1)) { bad = 3; break; }
        if (b->next && (u32)b->next >= (u32)b) { bad = 4; break; }
    }
    irq_restore(f);
    if (bad) { static const char *why[] = { "", "cycle", "header outside heap", "bad size or flag", "order" }; serial_puts("kheap: CORRUPT "); serial_puts(why[bad]); serial_puts("\n"); return 0; }
    serial_puts("kheap: ok\n");
    return 1;
}

/* 1.9.23: one round of churn for the stress check. Each side keeps a
   rotating set of live blocks across rounds (desktop and syscall sets are
   separate), so the free list changes shape between a preempted walk and
   its resumption instead of snapping back. A round frees the oldest held
   block after re-checking its stamp (a second owner of the same block
   would have overwritten it), then allocates a fresh one of a mixed size
   and stamps it with a pattern derived from its own address. Returns 1
   when every stamp held. */
#define STRESS_HOLD 24
struct stress_set { u32 *p[STRESS_HOLD]; u32 sz[STRESS_HOLD]; u32 at; };
static struct stress_set stress_sets[2];
int kheap_stress_round(u32 seed, int side) {
    struct stress_set *st = &stress_sets[side & 1];
    int ok = 1;
    for (int n = 0; n < 4; n++) {
        u32 i = st->at++ % STRESS_HOLD;
        if (st->p[i]) { for (u32 j = 0; j < st->sz[i] / 4; j++) if (st->p[i][j] != ((u32)st->p[i] ^ j)) ok = 0; kfree(st->p[i]); st->p[i] = 0; }
        u32 sz = 16 + ((seed * 2654435761u >> ((n + (int)i) % 13)) & 0x7F0);
        u32 *q = (u32 *)kmalloc(sz);
        if (q) for (u32 j = 0; j < sz / 4; j++) q[j] = (u32)q ^ j;
        st->p[i] = q; st->sz[i] = sz;
    }
    if (!ok) serial_puts("kheap: CORRUPT stamp mismatch\n");
    return ok;
}
