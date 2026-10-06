/*
 * Xok software regions.
 *
 * A software region is an area of memory that can only be read or
 * written through system calls.  It provides sub-page protection and
 * fault isolation for state shared by mutually distrustful processes
 * (ExOS uses them for pipes): every access is checked against the
 * region's guard capability (read access needs CAP_R, write access
 * CAP_W).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"

#define NSREG 256
#define SREG_MAXSIZE (64 * 1024)

struct sreg
{
  bool used;
  bool dead;			/* Destroyed, waiting for refs to drop. */
  uint32_t refs;		/* Wakeup predicates referencing it. */
  uint32_t size;
  struct cap guard;
  uint8_t *data;
};

static struct sreg sregs[NSREG];

int
sreg_create (struct env *cur, unsigned k, uint32_t size)
{
  struct cap *c = env_cap (cur, k);

  if (c == NULL)
    return -E_CAP_INVALID;
  if (size == 0 || size > SREG_MAXSIZE)
    return -E_INVAL;
  for (unsigned i = 0; i < NSREG; i++)
    if (!sregs[i].used)
      {
	sregs[i].dead = false;
	sregs[i].refs = 0;
	sregs[i].data = (uint8_t *) kmalloc (size);
	if (sregs[i].data == NULL)
	  return -E_NO_MEM;
	memset (sregs[i].data, 0, size);
	sregs[i].used = true;
	sregs[i].size = size;
	memcpy (&sregs[i].guard, c, sizeof (*c));
	return i;
      }
  return -E_NO_MEM;
}

static struct sreg *
sreg_get (struct env *cur, unsigned k, unsigned id, unsigned perm, int *err)
{
  struct sreg *s;

  if (id >= NSREG || !sregs[id].used || sregs[id].dead)
    {
      *err = -E_NOT_FOUND;
      return NULL;
    }
  s = sregs + id;
  *err = cap_check (cur, k, &s->guard, perm);
  return *err < 0 ? NULL : s;
}

int
sreg_destroy (struct env *cur, unsigned k, unsigned id)
{
  struct sreg *s;
  int r;

  s = sreg_get (cur, k, id, CAP_W, &r);
  if (s == NULL)
    return r;
  s->dead = true;
  if (s->refs == 0)
    {
      kfree (s->data, s->size);
      s->used = false;
    }
  return 0;
}

/*
 * Wakeup predicate support: reference a word of a region.
 */
volatile uint32_t *
sreg_word_ref (struct env *cur, unsigned k, unsigned id, unsigned off,
	       int *err)
{
  struct sreg *s = sreg_get (cur, k, id, CAP_R, err);

  if (s == NULL)
    return NULL;
  if ((off & 3) || off + 4 > s->size)
    {
      *err = -E_RANGE;
      return NULL;
    }
  s->refs++;
  return (volatile uint32_t *) (s->data + off);
}

void
sreg_word_unref (unsigned id)
{
  struct sreg *s = sregs + id;

  KASSERT (id < NSREG && s->used && s->refs > 0);
  if (--s->refs == 0 && s->dead)
    {
      kfree (s->data, s->size);
      s->used = false;
    }
}

int
sreg_read (struct env *cur, unsigned k, unsigned id, uint32_t off,
	   uaddr_t buf, uint32_t len)
{
  struct sreg *s;
  int r;

  s = sreg_get (cur, k, id, CAP_R, &r);
  if (s == NULL)
    return r;
  if (off > s->size || len > s->size - off)
    return -E_RANGE;
  return copyout (buf, s->data + off, len);
}

int
sreg_write (struct env *cur, unsigned k, unsigned id, uint32_t off,
	    uaddr_t buf, uint32_t len)
{
  struct sreg *s;
  int r;

  s = sreg_get (cur, k, id, CAP_W, &r);
  if (s == NULL)
    return r;
  if (off > s->size || len > s->size - off)
    return -E_RANGE;
  r = copyin (s->data + off, buf, len);
  if (r == 0)
    sched_kick_idle ();
  return r;
}
