/*
 * A tiny assembler for XN UDF programs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef EXOS_UDFASM_H
#define EXOS_UDFASM_H

#include <xok/xn.h>

#define UA_MAXLABELS 64
#define UA_MAXFIX 128

struct udf_asm
{
  struct udf_insn *code;
  unsigned n, max;
  int labels[UA_MAXLABELS];
  unsigned nlabels;
  struct
  {
    unsigned at;
    int label;
  } fix[UA_MAXFIX];
  unsigned nfix;
  int err;
};

void ua_init (struct udf_asm *a, struct udf_insn *code, unsigned max);
void ua_op (struct udf_asm *a, int op, int ra, int rb, int rc, int imm);
int ua_newlabel (struct udf_asm *a);
void ua_label (struct udf_asm *a, int label);
/* Branch (or JMP) to LABEL. */
void ua_br (struct udf_asm *a, int op, int ra, int rb, int label);
/* Resolve labels; returns the number of instructions, or -1. */
int ua_finish (struct udf_asm *a);

#endif
