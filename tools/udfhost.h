/*
 * Host build shim for kern/udf.c (used by mkxnfs to verify the file
 * systems it creates with the kernel's own UDF interpreter).
 */
#ifndef UDFHOST_H
#define UDFHOST_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <xok/xn.h>
#include <xok/error.h>

#define UDF_KIND_OWNS 0
#define UDF_KIND_ACL  1
#define UDF_KIND_SIZE 2

struct xn_item
{
  uint32_t blk;
  uint32_t type;
};

struct udf_ctx
{
  const uint8_t *meta;
  const uint8_t *newmeta;
  const uint32_t *args;
  const struct cap *cred;
  struct xn_item *out;
  unsigned nout, maxout;
};

int udf_verify (const struct udf_insn *code, unsigned n, int kind);
int udf_run (const struct udf_insn *code, unsigned n, struct udf_ctx *c,
	     uint32_t * ret);
#endif
