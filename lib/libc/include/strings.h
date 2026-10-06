#ifndef _STRINGS_H
#define _STRINGS_H
#include <string.h>
#define bzero(_p,_n) memset((_p),0,(_n))
#define bcopy(_s,_d,_n) memmove((_d),(_s),(_n))
#endif
