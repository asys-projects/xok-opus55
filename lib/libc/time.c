/*
 * ExOS libc: time.  Wall clock time = RTC time at boot (published by
 * Xok in the sysinfo page) + time since boot.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <exos/sys.h>
#include <xok/sysinfo.h>
#include <xok/mmu.h>

static uint64_t
now_ns (void)
{
  uint64_t ns;
  if (sys_gettime (&ns) < 0)
    return sysinfo_page->si_nsec;
  return ns;
}

time_t
time (time_t *t)
{
  time_t v = sysinfo_page->si_boot_unix + now_ns () / 1000000000ULL;
  if (t)
    *t = v;
  return v;
}

int
gettimeofday (struct timeval *tv, void *tz)
{
  uint64_t ns = now_ns ();
  tv->tv_sec = sysinfo_page->si_boot_unix + ns / 1000000000ULL;
  tv->tv_usec = (ns % 1000000000ULL) / 1000;
  return 0;
}

int
clock_gettime (clockid_t id, struct timespec *ts)
{
  uint64_t ns = now_ns ();
  ts->tv_sec = ns / 1000000000ULL;
  ts->tv_nsec = ns % 1000000000ULL;
  if (id == CLOCK_REALTIME)
    ts->tv_sec += sysinfo_page->si_boot_unix;
  return 0;
}

clock_t
clock (void)
{
  return (clock_t) (now_ns () / 1000);
}

struct tm *
gmtime_r (const time_t *tp, struct tm *tm)
{
  long t = *tp;
  long days = t / 86400, rem = t % 86400;
  if (rem < 0)
    {
      rem += 86400;
      days--;
    }
  tm->tm_hour = rem / 3600;
  tm->tm_min = (rem % 3600) / 60;
  tm->tm_sec = rem % 60;
  tm->tm_wday = (4 + days) % 7;
  if (tm->tm_wday < 0)
    tm->tm_wday += 7;
  /* Civil from days (Howard Hinnant). */
  long z = days + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned) (z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = (long) yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  tm->tm_year = y - 1900;
  tm->tm_mon = m - 1;
  tm->tm_mday = d;
  static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  tm->tm_yday = cum[m - 1] + d - 1 + (leap && m > 2);
  tm->tm_isdst = 0;
  return tm;
}

struct tm *
gmtime (const time_t *t)
{
  static struct tm tm;
  return gmtime_r (t, &tm);
}

struct tm *
localtime (const time_t *t)
{
  return gmtime (t);
}

time_t
mktime (struct tm *tm)
{
  int y = tm->tm_year + 1900, m = tm->tm_mon + 1;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned) (y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + (long) doe - 719468;
  return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

static const char *wday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul",
  "Aug", "Sep", "Oct", "Nov", "Dec"
};

char *
asctime (const struct tm *tm)
{
  static char buf[32];
  snprintf (buf, sizeof (buf), "%s %s %2d %02d:%02d:%02d %d\n",
	    wday[tm->tm_wday], mon[tm->tm_mon], tm->tm_mday, tm->tm_hour,
	    tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
  return buf;
}

char *
ctime (const time_t *t)
{
  return asctime (gmtime (t));
}

size_t
strftime (char *s, size_t max, const char *fmt, const struct tm *tm)
{
  size_t n = 0;
  char tmp[32];

  for (; *fmt && n + 1 < max; fmt++)
    {
      if (*fmt != '%')
	{
	  s[n++] = *fmt;
	  continue;
	}
      fmt++;
      switch (*fmt)
	{
	case 'Y':
	  snprintf (tmp, sizeof (tmp), "%d", tm->tm_year + 1900);
	  break;
	case 'm':
	  snprintf (tmp, sizeof (tmp), "%02d", tm->tm_mon + 1);
	  break;
	case 'd':
	  snprintf (tmp, sizeof (tmp), "%02d", tm->tm_mday);
	  break;
	case 'H':
	  snprintf (tmp, sizeof (tmp), "%02d", tm->tm_hour);
	  break;
	case 'M':
	  snprintf (tmp, sizeof (tmp), "%02d", tm->tm_min);
	  break;
	case 'S':
	  snprintf (tmp, sizeof (tmp), "%02d", tm->tm_sec);
	  break;
	case 'a':
	  snprintf (tmp, sizeof (tmp), "%s", wday[tm->tm_wday]);
	  break;
	case 'b':
	  snprintf (tmp, sizeof (tmp), "%s", mon[tm->tm_mon]);
	  break;
	case 'Z':
	  snprintf (tmp, sizeof (tmp), "GMT");
	  break;
	case '%':
	  snprintf (tmp, sizeof (tmp), "%%");
	  break;
	default:
	  tmp[0] = 0;
	}
      for (char *p = tmp; *p && n + 1 < max; p++)
	s[n++] = *p;
    }
  s[n] = 0;
  return n;
}
