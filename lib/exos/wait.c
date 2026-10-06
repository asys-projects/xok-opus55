/*
 * ExOS sleeping: wakeup predicates, with the add-ons registered by
 * library modules (e.g. TCP timers) OR'ed in.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <string.h>

#define MAX_WK_ADDONS 4
static struct
{
  wk_addon_t fn;
  void (*handler) (void);
} wk_addons[MAX_WK_ADDONS];
static int nwk_addons;

void
exos_add_wk (wk_addon_t fn, void (*handler) (void))
{
  if (nwk_addons < MAX_WK_ADDONS)
    {
      wk_addons[nwk_addons].fn = fn;
      wk_addons[nwk_addons].handler = handler;
      nwk_addons++;
    }
}

uint64_t
exos_time_ns (void)
{
  uint64_t ns;
  sys_gettime (&ns);
  return ns;
}

/*
 * Install the predicate T (n terms, sum of products) plus the add-ons
 * and sleep.  Returns when the predicate may have become true (callers
 * re-check their condition).
 */
int
exos_wkpred (struct wk_term *t, int n)
{
  struct wk_term all[WK_MAXTERMS];
  int m = n;

  if (n > WK_MAXTERMS)
    return -1;
  memcpy (all, t, n * sizeof (*t));
  for (int i = 0; i < nwk_addons && m < WK_MAXTERMS - 1; i++)
    {
      all[m].wk_op = WK_OR;
      int k = wk_addons[i].fn (all + m + 1, WK_MAXTERMS - m - 1);
      if (k > 0)
	m += k + 1;
    }
  int r = sys_wkpred (all, m);
  for (int i = 0; i < nwk_addons; i++)
    if (wk_addons[i].handler)
      wk_addons[i].handler ();
  return r;
}

static int
check (volatile uint32_t * addr, uint32_t op, uint32_t val)
{
  uint32_t v = *addr;
  switch (op)
    {
    case WK_EQ:
      return v == val;
    case WK_NE:
      return v != val;
    case WK_LT:
      return v < val;
    case WK_LE:
      return v <= val;
    case WK_GT:
      return v > val;
    case WK_GE:
      return v >= val;
    case WK_AND:
      return (v & val) != 0;
    case WK_ANDZ:
      return (v & val) == 0;
    }
  return 0;
}

/*
 * Sleep until (*ADDR OP VAL) holds, or TIMEOUT_MS milliseconds elapse
 * (0 = no timeout).  Returns 0, or -1 on timeout.
 */
int
exos_sleep_until_mem (volatile uint32_t * addr, uint32_t op, uint32_t val,
		      uint32_t timeout_ms)
{
  uint32_t deadline = 0;

  if (timeout_ms)
    deadline = (uint32_t) (exos_time_ns () / 1000000) + timeout_ms;
  for (;;)
    {
      struct wk_term t[3];
      int n = 0;

      if (check (addr, op, val))
	return 0;
      if (timeout_ms && (int32_t) ((uint32_t) (exos_time_ns () / 1000000)
				   - deadline) >= 0)
	return -1;
      memset (t, 0, sizeof (t));
      t[n].wk_op = op;
      t[n].wk_lkind = WK_MEM32;
      t[n].wk_lval = (uint32_t) addr;
      t[n].wk_rkind = WK_CONST;
      t[n].wk_rval = val;
      n++;
      if (timeout_ms)
	{
	  t[n++].wk_op = WK_OR;
	  t[n].wk_op = WK_GE;
	  t[n].wk_lkind = WK_TIME;
	  t[n].wk_rkind = WK_CONST;
	  t[n].wk_rval = deadline;
	  n++;
	}
      if (exos_wkpred (t, n) < 0)
	sys_yield (-1);
    }
}

int
usleep (unsigned long us)
{
  uint32_t deadline = (uint32_t) (exos_time_ns () / 1000000) + (us + 999) / 1000;
  for (;;)
    {
      struct wk_term t;
      if ((int32_t) ((uint32_t) (exos_time_ns () / 1000000) - deadline) >= 0)
	return 0;
      memset (&t, 0, sizeof (t));
      t.wk_op = WK_GE;
      t.wk_lkind = WK_TIME;
      t.wk_rkind = WK_CONST;
      t.wk_rval = deadline;
      exos_wkpred (&t, 1);
    }
}

unsigned int
sleep (unsigned int s)
{
  usleep ((unsigned long) s * 1000000);
  return 0;
}
