#ifndef _TIME_H
#define _TIME_H
#include <sys/types.h>
struct timespec { time_t tv_sec; long tv_nsec; };
struct tm
{
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
typedef long clock_t;
#define CLOCKS_PER_SEC 1000000
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
time_t time (time_t *t);
clock_t clock (void);
int clock_gettime (clockid_t id, struct timespec *ts);
int nanosleep (const struct timespec *req, struct timespec *rem);
struct tm *gmtime (const time_t *t);
struct tm *gmtime_r (const time_t *t, struct tm *tm);
struct tm *localtime (const time_t *t);
char *asctime (const struct tm *tm);
char *ctime (const time_t *t);
size_t strftime (char *s, size_t max, const char *fmt, const struct tm *tm);
time_t mktime (struct tm *tm);
#endif
