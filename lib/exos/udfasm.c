/*
 * A tiny assembler for XN UDF programs (used by libFSes to build their
 * templates; also compiled into the host tool mkxnfs).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>
#include <exos/udfasm.h>

void
ua_init (struct udf_asm *a, struct udf_insn *code, unsigned max)
{
  memset (a, 0, sizeof (*a));
  a->code = code;
  a->max = max;
}

void
ua_op (struct udf_asm *a, int op, int ra, int rb, int rc, int imm)
{
  if (a->n >= a->max)
    {
      a->err = 1;
      return;
    }
  a->code[a->n].op = op;
  a->code[a->n].a = ra;
  a->code[a->n].b = rb;
  a->code[a->n].c = rc;
  a->code[a->n].imm = imm;
  a->n++;
}

int
ua_newlabel (struct udf_asm *a)
{
  if (a->nlabels >= UA_MAXLABELS)
    {
      a->err = 1;
      return 0;
    }
  a->labels[a->nlabels] = -1;
  return a->nlabels++;
}

void
ua_label (struct udf_asm *a, int label)
{
  a->labels[label] = a->n;
}

void
ua_br (struct udf_asm *a, int op, int ra, int rb, int label)
{
  if (a->nfix >= UA_MAXFIX)
    {
      a->err = 1;
      return;
    }
  a->fix[a->nfix].at = a->n;
  a->fix[a->nfix].label = label;
  a->nfix++;
  ua_op (a, op, ra, rb, 0, 0);
}

int
ua_finish (struct udf_asm *a)
{
  for (unsigned i = 0; i < a->nfix; i++)
    {
      int l = a->labels[a->fix[i].label];
      if (l < 0)
	return -1;
      a->code[a->fix[i].at].imm = l;
    }
  return a->err ? -1 : (int) a->n;
}
