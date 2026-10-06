#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H
#include <limits.h>
#define MAXPATHLEN 256
#ifndef PATH_MAX
#define PATH_MAX 256
#endif
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
