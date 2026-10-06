/*
 * ExOS libc: memory allocator.
 *
 * A simple segregated free-list allocator over sbrk(): small requests
 * are served from power-of-two size classes, large ones from a sorted,
 * coalescing free list.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

#define ALIGN 16
#define NCLASSES 8		/* 16 .. 2048 */
#define MINCLASS 4		/* log2(16) */

struct hdr
{
  size_t size;			/* Usable size; low bit: in use. */
  size_t cls;			/* Size class, or NCLASSES for large. */
  struct hdr *next;		/* Free list link. */
  size_t pad;
};

static struct hdr *freelist[NCLASSES];
static struct hdr *largefree;

static void *
more (size_t n)
{
  void *p = sbrk (n);
  if (p == (void *) -1)
    return NULL;
  return p;
}

static int
class_of (size_t n)
{
  for (int c = 0; c < NCLASSES; c++)
    if (n <= ((size_t) 1 << (c + MINCLASS)))
      return c;
  return NCLASSES;
}

void *
malloc (size_t n)
{
  struct hdr *h, **pp;
  int c;

  if (n == 0)
    n = 1;
  n = (n + ALIGN - 1) & ~(size_t) (ALIGN - 1);
  c = class_of (n);
  if (c < NCLASSES)
    {
      size_t sz = (size_t) 1 << (c + MINCLASS);
      if (freelist[c] == NULL)
	{
	  /* Carve a 4KB (or larger) chunk into objects. */
	  size_t chunk = 4096;
	  size_t each = sizeof (struct hdr) + sz;
	  if (chunk < each * 4)
	    chunk = each * 4;
	  char *p = more (chunk);
	  if (p == NULL)
	    {
	      errno = ENOMEM;
	      return NULL;
	    }
	  for (size_t off = 0; off + each <= chunk; off += each)
	    {
	      h = (struct hdr *) (p + off);
	      h->size = sz;
	      h->cls = c;
	      h->next = freelist[c];
	      freelist[c] = h;
	    }
	}
      h = freelist[c];
      freelist[c] = h->next;
      h->size |= 1;
      return h + 1;
    }

  /* Large: first fit in the address-ordered list. */
  for (pp = &largefree; *pp; pp = &(*pp)->next)
    if ((*pp)->size >= n)
      {
	h = *pp;
	if (h->size >= n + sizeof (struct hdr) + 4096)
	  {
	    struct hdr *r = (struct hdr *) ((char *) (h + 1) + n);
	    r->size = h->size - n - sizeof (struct hdr);
	    r->cls = NCLASSES;
	    r->next = h->next;
	    h->size = n;
	    *pp = r;
	  }
	else
	  *pp = h->next;
	h->size |= 1;
	return h + 1;
      }
  size_t total = (n + sizeof (struct hdr) + 4095) & ~(size_t) 4095;
  h = more (total);
  if (h == NULL)
    {
      errno = ENOMEM;
      return NULL;
    }
  h->size = (total - sizeof (struct hdr)) | 1;
  h->cls = NCLASSES;
  return h + 1;
}

void
free (void *p)
{
  struct hdr *h, **pp;

  if (p == NULL)
    return;
  h = (struct hdr *) p - 1;
  h->size &= ~(size_t) 1;
  if (h->cls < NCLASSES)
    {
      h->next = freelist[h->cls];
      freelist[h->cls] = h;
      return;
    }
  /* Insert sorted and coalesce with neighbours. */
  for (pp = &largefree; *pp && *pp < h; pp = &(*pp)->next)
    ;
  h->next = *pp;
  *pp = h;
  if (h->next
      && (char *) (h + 1) + h->size == (char *) h->next)
    {
      h->size += sizeof (struct hdr) + h->next->size;
      h->next = h->next->next;
    }
  if (pp != &largefree)
    {
      struct hdr *prev = (struct hdr *) ((char *) pp
					 - offsetof (struct hdr, next));
      if ((char *) (prev + 1) + prev->size == (char *) h)
	{
	  prev->size += sizeof (struct hdr) + h->size;
	  prev->next = h->next;
	}
    }
}

void *
calloc (size_t n, size_t s)
{
  size_t t = n * s;
  if (s && t / s != n)
    return NULL;
  void *p = malloc (t);
  if (p)
    memset (p, 0, t);
  return p;
}

void *
realloc (void *p, size_t n)
{
  struct hdr *h;
  void *q;

  if (p == NULL)
    return malloc (n);
  if (n == 0)
    {
      free (p);
      return NULL;
    }
  h = (struct hdr *) p - 1;
  if ((h->size & ~(size_t) 1) >= n)
    return p;
  q = malloc (n);
  if (q == NULL)
    return NULL;
  memcpy (q, p, h->size & ~(size_t) 1);
  free (p);
  return q;
}
