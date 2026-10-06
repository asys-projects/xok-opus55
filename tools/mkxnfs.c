/*
 * mkxnfs: build a disk image holding an XN file system with a C-FFS
 * library file system, populated with files from the host.
 *
 * The image is laid out exactly as Xok's sys_xn_format would, the C-FFS
 * templates are produced by the same code the library OS uses, and
 * every metadata block is verified with the kernel's own UDF
 * interpreter before the image is written.
 *
 * usage: mkxnfs -o image -s size_mb [-d hostdir name...] [-f /path=hostfile]
 *               [-D /dir] [-c] [-v]
 *   -d dir    programs listed after the options are taken from DIR and
 *             installed in /bin
 *   -f p=h    install host file H as P
 *   -D dir    create directory DIR
 *   -c        check an existing image (fsck) instead of creating one
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <xok/xn.h>
#include <exos/cffs.h>
#include "udfhost.h"

static uint8_t *img;
static uint32_t nblocks;
static struct xn_super super;
static uint32_t next_blk;
static struct xn_template templates[CFFS_NTYPES];
static unsigned ids[CFFS_NTYPES] = { 0, 1, 2, 3, 4, 5 };
static int verbose;
static uint32_t now;

static void
die (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  fprintf (stderr, "mkxnfs: ");
  vfprintf (stderr, fmt, ap);
  fprintf (stderr, "\n");
  va_end (ap);
  exit (1);
}

static uint8_t *
blkp (uint32_t b)
{
  if (b >= nblocks)
    die ("block %u out of range", b);
  return img + (size_t) b * XN_BLKSIZE;
}

static uint32_t
balloc (unsigned n)
{
  uint32_t b = next_blk;
  next_blk += n;
  if (next_blk > nblocks)
    die ("disk full");
  return b;
}

/*
 * In-memory tree.
 */
struct node
{
  char name[CFFS_NAMELEN + 1];
  int isdir;
  uint16_t mode;
  const char *host;
  size_t size;
  struct node *child, *next;
  /* Layout. */
  uint32_t ndirblk, dirblk[CFFS_NDIRECT + CFFS_NPTRS];
  uint32_t dirind;
  uint32_t ndata, *data;
  uint32_t ind, dind, *dindptrs, ndindptrs;
};

static struct node root = {.name = "/",.isdir = 1,.mode = 040755 };

static struct node *
lookup_child (struct node *d, const char *name)
{
  for (struct node *c = d->child; c; c = c->next)
    if (strcmp (c->name, name) == 0)
      return c;
  return NULL;
}

static struct node *
mkpath (const char *path, int isdir, uint16_t mode)
{
  char buf[1024], *p, *save = NULL;
  struct node *d = &root, *n = NULL;

  strncpy (buf, path, sizeof (buf) - 1);
  buf[sizeof (buf) - 1] = 0;
  for (p = strtok_r (buf, "/", &save); p; p = strtok_r (NULL, "/", &save))
    {
      char *rest = save && *save ? save : NULL;
      if (strlen (p) > CFFS_NAMELEN)
	die ("name too long: %s", p);
      n = lookup_child (d, p);
      if (n == NULL)
	{
	  n = calloc (1, sizeof (*n));
	  strcpy (n->name, p);
	  n->isdir = rest ? 1 : isdir;
	  n->mode = n->isdir ? 040755 : mode;
	  n->next = d->child;
	  d->child = n;
	}
      if (rest && !n->isdir)
	die ("%s: not a directory", p);
      d = n;
    }
  if (n && isdir)
    n->mode = mode;
  return n;
}

static void
add_file (const char *path, const char *host, uint16_t mode)
{
  struct stat st;
  struct node *n;

  if (stat (host, &st) < 0)
    die ("cannot stat %s", host);
  n = mkpath (path, 0, mode);
  n->host = host;
  n->size = st.st_size;
}

/*
 * Layout: directories' blocks first, then the blocks of the files they
 * contain (explicit grouping), then subdirectories.
 */
static unsigned
count_children (struct node *d)
{
  unsigned n = 0;
  for (struct node *c = d->child; c; c = c->next)
    n++;
  return n;
}

static void
layout (struct node *d)
{
  unsigned n = count_children (d);

  d->ndirblk = (n + CFFS_DIRENTS - 1) / CFFS_DIRENTS;
  if (d->ndirblk > CFFS_NDIRECT + CFFS_NPTRS)
    die ("directory %s too large", d->name);
  for (unsigned i = 0; i < d->ndirblk; i++)
    d->dirblk[i] = balloc (1);
  if (d->ndirblk > CFFS_NDIRECT)
    d->dirind = balloc (1);

  for (struct node *c = d->child; c; c = c->next)
    {
      if (c->isdir)
	continue;
      c->ndata = (c->size + XN_BLKSIZE - 1) / XN_BLKSIZE;
      c->data = calloc (c->ndata + 1, sizeof (uint32_t));
      for (unsigned i = 0; i < c->ndata; i++)
	c->data[i] = balloc (1);
      if (c->ndata > CFFS_NDIRECT)
	c->ind = balloc (1);
      if (c->ndata > CFFS_NDIRECT + CFFS_NPTRS)
	{
	  unsigned rest = c->ndata - CFFS_NDIRECT - CFFS_NPTRS;
	  c->ndindptrs = (rest + CFFS_NPTRS - 1) / CFFS_NPTRS;
	  if (c->ndindptrs > CFFS_NPTRS)
	    die ("file too large");
	  c->dind = balloc (1);
	  c->dindptrs = calloc (c->ndindptrs, sizeof (uint32_t));
	  for (unsigned i = 0; i < c->ndindptrs; i++)
	    c->dindptrs[i] = balloc (1);
	}
    }
  for (struct node *c = d->child; c; c = c->next)
    if (c->isdir)
      layout (c);
}

static void
fill_inode (struct cffs_inode *ino, struct node *n)
{
  memset (ino, 0, sizeof (*ino));
  ino->i_mode = n->mode;
  ino->i_nlink = 1;
  ino->i_uid = 0;
  ino->i_gid = 0;
  ino->i_atime = ino->i_mtime = ino->i_ctime = now;
  if (n->isdir)
    {
      ino->i_size = n->ndirblk * XN_BLKSIZE;
      for (unsigned i = 0; i < n->ndirblk && i < CFFS_NDIRECT; i++)
	ino->i_direct[i] = n->dirblk[i];
      ino->i_indirect = n->dirind;
    }
  else
    {
      ino->i_size = n->size;
      for (unsigned i = 0; i < n->ndata && i < CFFS_NDIRECT; i++)
	ino->i_direct[i] = n->data[i];
      ino->i_indirect = n->ind;
      ino->i_dindirect = n->dind;
    }
}

static void
put32 (uint8_t * p, uint32_t v)
{
  memcpy (p, &v, 4);
}

static void
emit (struct node *d)
{
  unsigned slot = 0;

  if (d->dirind)
    for (unsigned i = CFFS_NDIRECT; i < d->ndirblk; i++)
      put32 (blkp (d->dirind) + 4 * (i - CFFS_NDIRECT), d->dirblk[i]);

  for (struct node *c = d->child; c; c = c->next, slot++)
    {
      struct cffs_dirent *de = (struct cffs_dirent *)
	(blkp (d->dirblk[slot / CFFS_DIRENTS])
	 + (slot % CFFS_DIRENTS) * CFFS_DIRENT_SIZE);
      de->d_used = 1;
      de->d_namelen = strlen (c->name);
      strcpy (de->d_name, c->name);
      fill_inode (&de->d_inode, c);

      if (c->isdir)
	{
	  emit (c);
	  continue;
	}
      if (c->host)
	{
	  FILE *f = fopen (c->host, "rb");
	  if (f == NULL)
	    die ("cannot open %s", c->host);
	  for (unsigned i = 0; i < c->ndata; i++)
	    if (fread (blkp (c->data[i]), 1, XN_BLKSIZE, f) == 0 && ferror (f))
	      die ("read error on %s", c->host);
	  fclose (f);
	}
      if (c->ind)
	for (unsigned i = CFFS_NDIRECT;
	     i < c->ndata && i < CFFS_NDIRECT + CFFS_NPTRS; i++)
	  put32 (blkp (c->ind) + 4 * (i - CFFS_NDIRECT), c->data[i]);
      if (c->dind)
	{
	  for (unsigned j = 0; j < c->ndindptrs; j++)
	    {
	      put32 (blkp (c->dind) + 4 * j, c->dindptrs[j]);
	      for (unsigned k = 0; k < CFFS_NPTRS; k++)
		{
		  unsigned i = CFFS_NDIRECT + CFFS_NPTRS + j * CFFS_NPTRS + k;
		  if (i >= c->ndata)
		    break;
		  put32 (blkp (c->dindptrs[j]) + 4 * k, c->data[i]);
		}
	    }
	}
    }
}

/*
 * Verification with the kernel's UDF interpreter: traverse the tree
 * from the root catalogue, mark reachable blocks, compare with the free
 * map.
 */
static uint8_t *reach;
static struct xn_item items[UDF_MAXEMIT];

static void
check_block (uint32_t b, unsigned type, unsigned depth)
{
  struct udf_ctx c;
  uint32_t ret;
  struct xn_template *t = &templates[type];

  if (depth > 64)
    die ("tree too deep");
  if (reach[b])
    die ("block %u reachable twice", b);
  reach[b] = 1;
  if (t->t_nowns == 0)
    return;
  memset (&c, 0, sizeof (c));
  c.meta = blkp (b);
  c.out = items;
  c.maxout = UDF_MAXEMIT;
  if (udf_run (t->t_owns, t->t_nowns, &c, &ret) < 0)
    die ("owns-udf of %s failed on block %u", t->t_name, b);
  struct xn_item *mine = malloc (c.nout * sizeof (*mine) + 1);
  memcpy (mine, items, c.nout * sizeof (*mine));
  unsigned n = c.nout;
  for (unsigned i = 0; i < n; i++)
    {
      if (mine[i].blk < super.s_data_start || mine[i].blk >= nblocks)
	die ("block %u owns invalid block %u", b, mine[i].blk);
      check_block (mine[i].blk, mine[i].type, depth + 1);
    }
  free (mine);
}

static int
fm_isfree (uint32_t b)
{
  uint32_t *fm = (uint32_t *) blkp (super.s_freemap_start);
  return (fm[b / 32] >> (b % 32)) & 1;
}

static void
verify (void)
{
  struct xn_root *roots = (struct xn_root *) blkp (super.s_rootcat_start);
  unsigned nused = 0, nreach = 0;

  memcpy (templates, blkp (super.s_typecat_start), 0);
  for (unsigned t = 0; t < XN_MAXTYPES && t < CFFS_NTYPES; t++)
    memcpy (&templates[t], blkp (super.s_typecat_start + t),
	    sizeof (struct xn_template));
  for (unsigned t = 0; t < CFFS_NTYPES; t++)
    {
      struct xn_template *tp = &templates[t];
      uint8_t zero[XN_BLKSIZE];
      struct udf_ctx c;
      uint32_t ret;

      if (udf_verify (tp->t_owns, tp->t_nowns, UDF_KIND_OWNS) < 0
	  || udf_verify (tp->t_acl, tp->t_nacl, UDF_KIND_ACL) < 0)
	die ("template %s does not verify", tp->t_name);
      memset (zero, 0, sizeof (zero));
      memset (&c, 0, sizeof (c));
      c.meta = zero;
      c.out = items;
      c.maxout = UDF_MAXEMIT;
      if (tp->t_nowns
	  && (udf_run (tp->t_owns, tp->t_nowns, &c, &ret) < 0 || c.nout))
	die ("template %s: zeroed metadata owns blocks", tp->t_name);
      if (verbose)
	printf ("  type %u %-12s owns %3u insns, acl %3u insns\n", t,
		tp->t_name, tp->t_nowns, tp->t_nacl);
    }

  reach = calloc (nblocks, 1);
  for (unsigned r = 0; r < XN_MAXROOTS; r++)
    if (roots[r].r_flags & XN_RF_USED)
      for (uint32_t b = roots[r].r_blk; b < roots[r].r_blk + roots[r].r_nblocks;
	   b++)
	check_block (b, roots[r].r_type, 0);
  for (uint32_t b = super.s_data_start; b < nblocks; b++)
    {
      if (!fm_isfree (b))
	nused++;
      if (reach[b])
	nreach++;
      if (reach[b] && fm_isfree (b))
	die ("block %u reachable but free", b);
    }
  if (nused != nreach)
    printf ("mkxnfs: warning: %u blocks allocated, %u reachable\n", nused,
	    nreach);
  printf ("mkxnfs: verified: %u blocks used of %u\n", nreach, nblocks);
}

static void
create (uint32_t size_mb)
{
  uint32_t fmblocks;
  struct xn_root *r;
  struct cffs_super *cs;
  uint32_t superblk;

  nblocks = size_mb * 256;
  img = calloc (nblocks, XN_BLKSIZE);
  if (img == NULL)
    die ("out of memory");
  fmblocks = (nblocks + XN_BLKSIZE * 8 - 1) / (XN_BLKSIZE * 8);
  super.s_magic = XN_MAGIC;
  super.s_version = XN_VERSION;
  super.s_nblocks = nblocks;
  super.s_typecat_start = 1;
  super.s_rootcat_start = 1 + XN_MAXTYPES;
  super.s_freemap_start = 2 + XN_MAXTYPES;
  super.s_freemap_nblocks = fmblocks;
  super.s_data_start = 2 + XN_MAXTYPES + fmblocks;
  super.s_clean = 1;
  super.s_gen = 0;
  next_blk = super.s_data_start;

  if (cffs_build_templates (templates, ids) < 0)
    die ("cannot build C-FFS templates (program too large?)");
  for (unsigned t = 0; t < CFFS_NTYPES; t++)
    memcpy (blkp (super.s_typecat_start + ids[t]), &templates[t],
	    sizeof (struct xn_template));

  superblk = balloc (1);
  r = (struct xn_root *) blkp (super.s_rootcat_start);
  strcpy (r->r_name, CFFS_ROOTNAME);
  r->r_blk = superblk;
  r->r_nblocks = 1;
  r->r_type = ids[CFFS_T_SUPER];
  r->r_flags = XN_RF_USED;
  r->r_guard.c_valid = 1;
  r->r_guard.c_perm = CAP_ALL;
  r->r_guard.c_len = 0;

  layout (&root);
  emit (&root);
  cs = (struct cffs_super *) blkp (superblk);
  cs->s_magic = CFFS_MAGIC;
  cs->s_version = CFFS_VERSION;
  cs->s_ctime = now;
  fill_inode (&cs->s_root, &root);

  /* Free map: everything before next_blk is used. */
  for (uint32_t b = 0; b < fmblocks * XN_BLKSIZE * 8; b++)
    if (b >= next_blk && b < nblocks)
      {
	uint32_t *fm = (uint32_t *) blkp (super.s_freemap_start);
	fm[b / 32] |= 1u << (b % 32);
      }
  memcpy (blkp (0), &super, sizeof (super));
}

int
main (int argc, char **argv)
{
  const char *out = NULL, *bindir = NULL;
  uint32_t size = 64;
  int check = 0, opt;
  char *files[256];
  int nfiles = 0;
  char *dirs[64];
  int ndirs = 0;

  now = time (NULL);
  while ((opt = getopt (argc, argv, "o:s:d:f:D:cv")) != -1)
    switch (opt)
      {
      case 'o':
	out = optarg;
	break;
      case 's':
	size = atoi (optarg);
	break;
      case 'd':
	bindir = optarg;
	break;
      case 'f':
	files[nfiles++] = optarg;
	break;
      case 'D':
	dirs[ndirs++] = optarg;
	break;
      case 'c':
	check = 1;
	break;
      case 'v':
	verbose = 1;
	break;
      default:
	die ("usage: mkxnfs -o image -s MB [-d bindir progs...] "
	     "[-f /path=hostfile] [-D /dir] [-c] [-v]");
      }
  if (out == NULL)
    die ("no output image");

  if (check)
    {
      FILE *f = fopen (out, "rb");
      struct stat st;
      if (f == NULL || stat (out, &st) < 0)
	die ("cannot open %s", out);
      nblocks = st.st_size / XN_BLKSIZE;
      img = malloc (st.st_size);
      if (fread (img, 1, st.st_size, f) != (size_t) st.st_size)
	die ("read error");
      fclose (f);
      memcpy (&super, img, sizeof (super));
      if (super.s_magic != XN_MAGIC)
	die ("not an XN file system");
      verify ();
      return 0;
    }

  mkpath ("/bin", 1, 040755);
  mkpath ("/etc", 1, 040755);
  mkpath ("/tmp", 1, 041777);
  mkpath ("/home", 1, 040755);
  for (int i = 0; i < ndirs; i++)
    mkpath (dirs[i], 1, 040755);
  if (bindir)
    for (int i = optind; i < argc; i++)
      {
	char path[512], host[1024];
	const char *base = strrchr (argv[i], '/');
	base = base ? base + 1 : argv[i];
	snprintf (path, sizeof (path), "/bin/%s", base);
	snprintf (host, sizeof (host), "%s/%s", bindir, base);
	add_file (path, strdup (host), 0100755);
      }
  for (int i = 0; i < nfiles; i++)
    {
      char *eq = strchr (files[i], '=');
      if (eq == NULL)
	die ("bad -f argument %s", files[i]);
      *eq = 0;
      add_file (files[i], eq + 1, 0100644);
    }

  create (size);
  verify ();

  FILE *f = fopen (out, "wb");
  if (f == NULL)
    die ("cannot create %s", out);
  if (fwrite (img, XN_BLKSIZE, nblocks, f) != nblocks)
    die ("write error");
  fclose (f);
  printf ("mkxnfs: %s: %u MB, %u blocks used\n", out, size,
	  next_blk - super.s_data_start);
  return 0;
}
