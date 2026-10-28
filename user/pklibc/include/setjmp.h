#ifndef PK_SETJMP_H
#define PK_SETJMP_H
typedef unsigned long jmp_buf[22];   /* x19-x30, sp, d8-d15 (setjmp.S) */
int setjmp(jmp_buf b) __attribute__((returns_twice)); int _setjmp(jmp_buf b) __attribute__((returns_twice)); void longjmp(jmp_buf b, int v) __attribute__((noreturn));
#endif
