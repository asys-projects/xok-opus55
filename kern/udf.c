/*
 * XN untrusted deterministic functions: verifier and safe interpreter.
 *
 * Programs are checked once, when their template is installed: every
 * opcode, register and jump target must be valid, and owns-udfs may not
 * use any instruction that gives access to information other than the
 * metadata block (credentials, arguments): this is what makes them
 * deterministic.  Execution is bounded (step budget), memory accesses
 * are bounds-checked against the metadata block, and division by zero
 * is a fault.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef XN_HOST
#include "udfhost.h"
#else
#include "xn.h"
#endif

int
udf_verify (const struct udf_insn *code, unsigned n, int kind)
{
  if (n > UDF_MAXINSNS)
    return -E_INVAL;
  for (unsigned pc = 0; pc < n; pc++)
    {
      const struct udf_insn *i = code + pc;

      if (i->op >= UDF_NOPS)
	return -E_INVAL;
      if (i->a >= UDF_NREGS || i->b >= UDF_NREGS || i->c >= UDF_NREGS)
	return -E_INVAL;
      switch (i->op)
	{
	case UDF_JMP:
	case UDF_BEQ:
	case UDF_BNE:
	case UDF_BLTU:
	case UDF_BGEU:
	  if (i->imm < 0 || (unsigned) i->imm > n)
	    return -E_INVAL;
	  break;
	case UDF_EMIT:
	  if (kind != UDF_KIND_OWNS)
	    return -E_INVAL;
	  break;
	case UDF_RET:
	  if (kind == UDF_KIND_OWNS)
	    return -E_INVAL;
	  break;
	case UDF_LDB:
	case UDF_LDH:
	case UDF_LDW:
	  if (i->c > (kind == UDF_KIND_ACL ? 1 : 0))
	    return -E_INVAL;
	  break;
	case UDF_ARG:
	  if (kind != UDF_KIND_ACL || i->imm < 0 || i->imm >= XN_NARGS)
	    return -E_INVAL;
	  break;
	case UDF_CRED:
	case UDF_DOMN:
	  if (kind != UDF_KIND_ACL)
	    return -E_INVAL;
	  break;
	default:
	  break;
	}
    }
  return 0;
}

static inline bool
meta_ok (uint32_t addr, unsigned size)
{
  return addr < XN_BLKSIZE && addr + size <= XN_BLKSIZE;
}

/*
 * Run a UDF.  Returns 0 and the return value in *RET, or -E_UDF if
 * the program faulted or exceeded its budget.
 */
int
udf_run (const struct udf_insn *code, unsigned n, struct udf_ctx *c,
	 uint32_t * ret)
{
  uint32_t r[UDF_NREGS];
  unsigned pc = 0;
  uint32_t steps = 0;
  const uint8_t *m;

  memset (r, 0, sizeof (r));
  *ret = 0;
  while (pc < n)
    {
      const struct udf_insn *i = code + pc;
      uint32_t addr;

      if (++steps > UDF_MAXSTEPS)
	return -E_UDF;
      pc++;
      switch (i->op)
	{
	case UDF_END:
	  return 0;
	case UDF_LI:
	  r[i->a] = i->imm;
	  break;
	case UDF_MOV:
	  r[i->a] = r[i->b];
	  break;
	case UDF_ADD:
	  r[i->a] = r[i->b] + r[i->c];
	  break;
	case UDF_SUB:
	  r[i->a] = r[i->b] - r[i->c];
	  break;
	case UDF_MUL:
	  r[i->a] = r[i->b] * r[i->c];
	  break;
	case UDF_DIVU:
	  if (r[i->c] == 0)
	    return -E_UDF;
	  r[i->a] = r[i->b] / r[i->c];
	  break;
	case UDF_REMU:
	  if (r[i->c] == 0)
	    return -E_UDF;
	  r[i->a] = r[i->b] % r[i->c];
	  break;
	case UDF_AND:
	  r[i->a] = r[i->b] & r[i->c];
	  break;
	case UDF_OR:
	  r[i->a] = r[i->b] | r[i->c];
	  break;
	case UDF_XOR:
	  r[i->a] = r[i->b] ^ r[i->c];
	  break;
	case UDF_SHL:
	  r[i->a] = r[i->b] << (r[i->c] & 31);
	  break;
	case UDF_SHR:
	  r[i->a] = r[i->b] >> (r[i->c] & 31);
	  break;
	case UDF_ADDI:
	  r[i->a] = r[i->b] + (uint32_t) i->imm;
	  break;
	case UDF_ANDI:
	  r[i->a] = r[i->b] & (uint32_t) i->imm;
	  break;
	case UDF_LDB:
	  addr = r[i->b] + (uint32_t) i->imm;
	  m = (i->c && c->newmeta) ? c->newmeta : c->meta;
	  if (!meta_ok (addr, 1))
	    return -E_UDF;
	  r[i->a] = m[addr];
	  break;
	case UDF_LDH:
	  addr = r[i->b] + (uint32_t) i->imm;
	  m = (i->c && c->newmeta) ? c->newmeta : c->meta;
	  if (!meta_ok (addr, 2))
	    return -E_UDF;
	  r[i->a] = m[addr] | ((uint32_t) m[addr + 1] << 8);
	  break;
	case UDF_LDW:
	  addr = r[i->b] + (uint32_t) i->imm;
	  m = (i->c && c->newmeta) ? c->newmeta : c->meta;
	  if (!meta_ok (addr, 4))
	    return -E_UDF;
	  r[i->a] = m[addr] | ((uint32_t) m[addr + 1] << 8)
	    | ((uint32_t) m[addr + 2] << 16) | ((uint32_t) m[addr + 3] << 24);
	  break;
	case UDF_JMP:
	  pc = i->imm;
	  break;
	case UDF_BEQ:
	  if (r[i->a] == r[i->b])
	    pc = i->imm;
	  break;
	case UDF_BNE:
	  if (r[i->a] != r[i->b])
	    pc = i->imm;
	  break;
	case UDF_BLTU:
	  if (r[i->a] < r[i->b])
	    pc = i->imm;
	  break;
	case UDF_BGEU:
	  if (r[i->a] >= r[i->b])
	    pc = i->imm;
	  break;
	case UDF_EMIT:
	  {
	    uint32_t blk = r[i->a], cnt = r[i->b], type = r[i->c];
	    if (cnt == 0 || cnt > UDF_MAXEMIT || type >= XN_MAXTYPES)
	      return -E_UDF;
	    if (c->nout + cnt > c->maxout)
	      return -E_UDF;
	    for (uint32_t k = 0; k < cnt; k++)
	      {
		c->out[c->nout].blk = blk + k;
		c->out[c->nout].type = type;
		c->nout++;
	      }
	    break;
	  }
	case UDF_RET:
	  *ret = r[i->a];
	  return 0;
	case UDF_ARG:
	  r[i->a] = c->args[i->imm];
	  break;
	case UDF_CRED:
	  if (i->imm == -1)
	    r[i->a] = c->cred->c_len;
	  else if (i->imm == -2)
	    r[i->a] = c->cred->c_perm;
	  else if (i->imm >= 0 && i->imm < CAP_NAMELEN)
	    r[i->a] = c->cred->c_name[i->imm];
	  else
	    r[i->a] = 0;
	  break;
	case UDF_DOMN:
	  {
	    uint32_t off = r[i->b], len = r[i->c];
	    bool dom = true;
	    if (len > CAP_NAMELEN || !meta_ok (off, len))
	      return -E_UDF;
	    if (c->cred->c_len > len)
	      dom = false;
	    else
	      for (unsigned k = 0; k < c->cred->c_len; k++)
		if (c->cred->c_name[k] != c->meta[off + k])
		  dom = false;
	    r[i->a] = dom;
	    break;
	  }
	default:
	  return -E_UDF;
	}
    }
  return 0;
}
