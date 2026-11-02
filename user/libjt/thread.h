/* 8.1.0: threads, locks and condition variables for a Pi app, header only. The kernel gives each thread its own registers and
   stack and takes turns every 10 ms (arch/arm64/main.c, th_spawn and app_preempt); it does no locking itself. The locks are
   here, on the A72's load-acquire and store-release exclusives (it has no LSE atomics, so no casal or ldadd), and sleep on a
   futex word only when they must wait: Drepper's "Futexes Are Tricky" mutex, and a sequence-number condition variable.
   Pi only: the i386 kernel answers these calls with -ENOSYS. */
#ifndef JT_THREAD_H
#define JT_THREAD_H
#include "../jtsys.h"
#if defined(__aarch64__)

static inline int jt_cas(volatile int *p, int old, int nw) {   /* *p = nw if it was old; returns what it was */
    int prev, fail;
    __asm__ volatile ("1: ldaxr %w0, [%2]\n cmp %w0, %w3\n b.ne 2f\n stlxr %w1, %w4, [%2]\n cbnz %w1, 1b\n b 3f\n2: clrex\n3:"
                      : "=&r"(prev), "=&r"(fail) : "r"(p), "r"(old), "r"(nw) : "memory", "cc");
    return prev;
}
static inline int jt_xchg(volatile int *p, int nw) {
    int prev, fail;
    __asm__ volatile ("1: ldaxr %w0, [%2]\n stlxr %w1, %w3, [%2]\n cbnz %w1, 1b" : "=&r"(prev), "=&r"(fail) : "r"(p), "r"(nw) : "memory");
    return prev;
}
static inline int jt_fetch_add(volatile int *p, int d) {
    int prev, nw, fail;
    __asm__ volatile ("1: ldaxr %w0, [%3]\n add %w1, %w0, %w4\n stlxr %w2, %w1, [%3]\n cbnz %w2, 1b"
                      : "=&r"(prev), "=&r"(nw), "=&r"(fail) : "r"(p), "r"(d) : "memory");
    return prev;
}
static inline int jt_futex_wait(volatile int *p, int val) { return jt_syscall(JT_SYS_FUTEX, 0, (jt_arg)p, (jt_arg)(unsigned)val); }
static inline int jt_futex_wake(volatile int *p, int n) { return jt_syscall(JT_SYS_FUTEX, 1, (jt_arg)p, (jt_arg)n); }

typedef struct { volatile int v; } jt_mutex;   /* 0 free, 1 held, 2 held and someone may be asleep on it */
#define JT_MUTEX_INIT { 0 }
static inline void jt_mutex_lock(jt_mutex *m) {
    int c = jt_cas(&m->v, 0, 1);
    if (!c) return;
    if (c != 2) c = jt_xchg(&m->v, 2);
    while (c) { jt_futex_wait(&m->v, 2); c = jt_xchg(&m->v, 2); }
}
static inline void jt_mutex_unlock(jt_mutex *m) { if (jt_xchg(&m->v, 0) == 2) jt_futex_wake(&m->v, 1); }

typedef struct { volatile int seq; } jt_cond;
#define JT_COND_INIT { 0 }
static inline void jt_cond_wait(jt_cond *c, jt_mutex *m) {   /* a signal between the unlock and the sleep changes seq, so it is never lost */
    int s = c->seq;
    jt_mutex_unlock(m);
    jt_futex_wait(&c->seq, s);
    jt_mutex_lock(m);
}
static inline void jt_cond_signal(jt_cond *c) { jt_fetch_add(&c->seq, 1); jt_futex_wake(&c->seq, 1); }
static inline void jt_cond_broadcast(jt_cond *c) { jt_fetch_add(&c->seq, 1); jt_futex_wake(&c->seq, 0x7fffffff); }

struct jt_thread { volatile int done; void (*fn)(void *); void *arg; };
static inline void jt__thread_main(struct jt_thread *t) {   /* every thread starts here, so returning from fn ends the thread, not the app */
    t->fn(t->arg);
    jt_xchg(&t->done, 1);
    jt_futex_wake(&t->done, 1);
    jt_syscall(JT_SYS_THREAD_EXIT, 0, 0, 0);
    for (;;) { }
}
/* Start fn(arg) on its own stack (stack_size bytes at stack, rounded down to 16). Returns the thread's number or -errno. */
static inline int jt_thread_start(struct jt_thread *t, void (*fn)(void *), void *arg, void *stack, unsigned long stack_size) {
    t->done = 0; t->fn = fn; t->arg = arg;
    unsigned long top = ((unsigned long)stack + stack_size) & ~15UL;
    return jt_syscall(JT_SYS_THREAD_SPAWN, (jt_arg)(void *)jt__thread_main, top, (jt_arg)t);
}
static inline void jt_thread_join(struct jt_thread *t) { while (!t->done) jt_futex_wait(&t->done, 0); }

#endif
#endif
