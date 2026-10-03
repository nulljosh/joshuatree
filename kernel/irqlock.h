/* 1.9.23: the one locking primitive this uniprocessor kernel needs. A
   ring-3 syscall already runs with IF clear (int 0x80 is an interrupt
   gate), so the only way shared kernel state gets touched by two tasks
   at once is the desktop (task 0, interrupts on) being preempted inside
   a kheap or vfs call and the ring-3 task then entering the same code
   through the gate. Close that by making those entry points cli
   critical sections that restore the caller's own IF on the way out, so
   a call made with interrupts off stays off and one made with them on
   gets them back. No spinlock, no counter: one CPU, one rule. */
#ifndef IRQLOCK_H
#define IRQLOCK_H
static inline unsigned int irq_save(void) {
    unsigned int f;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(unsigned int f) {
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}
#endif
