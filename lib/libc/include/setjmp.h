#ifndef _SETJMP_H
#define _SETJMP_H
typedef unsigned long jmp_buf[6];
int setjmp (jmp_buf env) __attribute__ ((returns_twice));
void longjmp (jmp_buf env, int val) __attribute__ ((noreturn));
#endif
