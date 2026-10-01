#ifndef KHEAP_H
#define KHEAP_H
void *kmalloc(unsigned int size);
void kfree(void *ptr);
int kheap_check(void);                 /* 1.9.23: serial "kheap: ok" or "kheap: CORRUPT", returns 1 if intact */
int kheap_stress_round(unsigned int seed, int side); /* 1.9.23: alloc/stamp/verify/free churn, 1 if every stamp held */
#endif
