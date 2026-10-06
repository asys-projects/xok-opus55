/*
 * Xok wakeup predicates.
 *
 * Predicates are checked at installation: every memory operand is
 * translated to a physical address through the environment's page
 * table, and its page is pinned so that it cannot be reused while the
 * predicate references it.  Evaluation reads physical memory directly:
 * no address space switch is needed to decide whether a sleeping
 * environment can run.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"

struct wkop
{
  uint8_t kind;
  uint32_t val;			/* Constant, offset in page, or region id. */
  pfn_t pfn;
  volatile uint32_t *sreg;	/* WK_SREG32: kernel address. */
};

volatile uint32_t *sreg_word_ref (struct env *cur, unsigned k, unsigned id,
				  unsigned off, int *err);
void sreg_word_unref (unsigned id);

struct wkpred
{
  unsigned n;
  struct
  {
    uint8_t op;
    struct wkop l, r;
  } t[WK_MAXTERMS];
};

static int
wk_operand (struct env *e, struct wkop *o, uint8_t kind, uint32_t val,
	    unsigned k)
{
  xpte_t pte;
  int r;

  o->kind = kind;
  switch (kind)
    {
    case WK_SREG32:
      o->pfn = PFN_INVALID;
      o->val = val >> 16;
      o->sreg = sreg_word_ref (e, k, val >> 16, val & 0xffff, &r);
      if (o->sreg == NULL)
	{
	  o->kind = WK_CONST;
	  return r;
	}
      return 0;
    case WK_CONST:
    case WK_TIME:
      o->val = val;
      o->pfn = PFN_INVALID;
      return 0;
    case WK_MEM32:
      if ((val & 3) || val >= UTOP)
	return -E_INVAL;
      if (val >= UXOK_BASE)
	{
	  /* Kernel exported structures (e.g. the buffer cache). */
	  if (!export_fault (e, val))
	    return -E_FAULT;
	  {
	    hal_l1p_t l1p;
	    unsigned prot;
	    if (!hal_umap_getl1p (&e->umap.hal, val & ~PAGE_MASK, false, &l1p))
	      return -E_FAULT;
	    hal_l1e_unbox (hal_l1e_get (l1p), &o->pfn, &prot);
	  }
	}
      else if ((val & ~PAGE_MASK) == UAREA)
	o->pfn = e->upfn;
      else
	{
	  pte = env_getpte (e, val & ~(uint32_t) PAGE_MASK);
	  if (!(pte & PTE_P))
	    return -E_FAULT;
	  o->pfn = PTE_PPN (pte);
	}
      o->val = val & PAGE_MASK;
      pmem_pin (o->pfn);
      return 0;
    default:
      return -E_INVAL;
    }
}

static void
wk_op_release (struct wkop *o)
{
  if (o->kind == WK_MEM32 && o->pfn != PFN_INVALID)
    pmem_unpin (o->pfn);
  else if (o->kind == WK_SREG32)
    sreg_word_unref (o->val);
  o->kind = WK_CONST;
}

static void
wk_release (struct wkpred *w)
{
  for (unsigned i = 0; i < w->n; i++)
    {
      wk_op_release (&w->t[i].l);
      wk_op_release (&w->t[i].r);
    }
}

void
wk_free (struct env *e)
{
  if (e->wk == NULL)
    return;
  wk_release (e->wk);
  kmem_free (0, (vaddr_t) e->wk, sizeof (struct wkpred));
  e->wk = NULL;
}

int
wk_install (struct env *e, uaddr_t uterms, unsigned n)
{
  struct wk_term terms[WK_MAXTERMS];
  struct wkpred *w;
  int r;

  if (n == 0 || n > WK_MAXTERMS)
    return -E_INVAL;
  if (copyin (terms, uterms, n * sizeof (struct wk_term)) < 0)
    return -E_FAULT;

  w = (struct wkpred *) kmem_alloc (0, sizeof (*w));
  if (w == NULL)
    return -E_NO_MEM;
  w->n = 0;
  for (unsigned i = 0; i < n; i++)
    {
      w->t[i].op = terms[i].wk_op;
      w->t[i].l.kind = w->t[i].r.kind = WK_CONST;
      w->t[i].l.pfn = w->t[i].r.pfn = PFN_INVALID;
      if (terms[i].wk_op > WK_ANDZ)
	{
	  r = -E_INVAL;
	  goto fail;
	}
      w->n = i + 1;
      if (terms[i].wk_op == WK_OR)
	continue;
      r = wk_operand (e, &w->t[i].l, terms[i].wk_lkind, terms[i].wk_lval,
		      terms[i].wk_cap);
      if (r < 0)
	goto fail;
      r = wk_operand (e, &w->t[i].r, terms[i].wk_rkind, terms[i].wk_rval,
		      terms[i].wk_cap);
      if (r < 0)
	goto fail;
    }
  wk_free (e);
  e->wk = w;
  return 0;

fail:
  wk_release (w);
  kmem_free (0, (vaddr_t) w, sizeof (*w));
  return r;
}

static uint32_t
wk_value (struct wkop *o)
{
  uint32_t v;
  uint8_t *p;

  switch (o->kind)
    {
    case WK_CONST:
      return o->val;
    case WK_TIME:
      return (uint32_t) (ktime () / 1000000);
    case WK_SREG32:
      return *o->sreg;
    case WK_MEM32:
      p = pfn_get (o->pfn);
      v = __atomic_load_n ((uint32_t *) (p + o->val), __ATOMIC_ACQUIRE);
      pfn_put (o->pfn, p);
      return v;
    }
  return 0;
}

bool
wk_eval (struct env *e)
{
  struct wkpred *w = e->wk;
  bool prod = true;

  if (w == NULL)
    return true;
  for (unsigned i = 0; i < w->n; i++)
    {
      uint32_t l, r;
      bool t;

      if (w->t[i].op == WK_OR)
	{
	  if (prod)
	    return true;
	  prod = true;
	  continue;
	}
      if (!prod)
	continue;
      l = wk_value (&w->t[i].l);
      r = wk_value (&w->t[i].r);
      switch (w->t[i].op)
	{
	case WK_EQ:
	  t = l == r;
	  break;
	case WK_NE:
	  t = l != r;
	  break;
	case WK_LT:
	  t = l < r;
	  break;
	case WK_LE:
	  t = l <= r;
	  break;
	case WK_GT:
	  t = l > r;
	  break;
	case WK_GE:
	  t = l >= r;
	  break;
	case WK_AND:
	  t = (l & r) != 0;
	  break;
	case WK_ANDZ:
	  t = (l & r) == 0;
	  break;
	default:
	  t = false;
	}
      prod = prod && t;
    }
  return prod;
}
