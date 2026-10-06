#ifndef _ASSERT_H
#define _ASSERT_H
void __assert_fail (const char *e, const char *f, int l) __attribute__ ((noreturn));
#ifdef NDEBUG
#define assert(_e) ((void)0)
#else
#define assert(_e) ((_e) ? (void)0 : __assert_fail (#_e, __FILE__, __LINE__))
#endif
#endif
