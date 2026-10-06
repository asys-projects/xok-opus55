/*
 * C-FFS: the ExOS library file system, on XN.
 *
 * Every metadata change is a call to XN with the bytes to modify; XN
 * checks it with the file system's own UDFs (cffs_templates.c).  Data
 * blocks are mapped writable and written back immediately; metadata is
 * written back by cffs_sync(), in an order that satisfies XN.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <exos/fs.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>

static int mounted;
static unsigned fsdev;
static uint32_t superblk;
static unsigned tid[CFFS_NTYPES];

#define INO_OFF(_slot) ((_slot) * CFFS_DIRENT_SIZE + CFFS_DIRENT_INODE)

static int
xe (int r)
{
  switch (-r)
    {
    case 0:
      return 0;
    case E_NO_MEM:
      return -ENOMEM;
    case E_ACCESS:
    case E_CAP_INSUFF:
    case E_CAP_INVALID:
      return -EACCES;
    case E_NOT_FREE:
    case E_FULL:
      return -ENOSPC;
    case E_BUSY:
      return -EBUSY;
    case E_NOT_FOUND:
      return -ENOENT;
    case E_NOT_INCORE:
      return -EIO;
    default:
      return -EIO;
    }
}

int
cffs_mounted (void)
{
  return mounted;
}

int
cffs_mount (void)
{
  struct xn_root root;

  if (mounted)
    return 0;
  for (unsigned d = 0; d < sysinfo_page->si_ndisk; d++)
    {
      if (!sysinfo_page->si_disk[d].d_xn)
	continue;
      exos_touch (&root, sizeof (root), 1);
      if (sys_xn_root_lookup (d, CFFS_ROOTNAME, &root) < 0)
	continue;
      fsdev = d;
      superblk = root.r_blk;
      for (int t = 0; t < CFFS_NTYPES; t++)
	{
	  int id = sys_xn_type_lookup (d, cffs_type_names[t]);
	  if (id < 0)
	    return -EIO;
	  tid[t] = id;
	}
      if (xnl_get (d, superblk, XN_NOPARENT) < 0)
	return -EIO;
      struct cffs_super *s = xnl_map (d, superblk, 0);
      if (s == NULL || s->s_magic != CFFS_MAGIC)
	return -EIO;
      mounted = 1;
      return 0;
    }
  return -ENODEV;
}

int
cffs_root (struct cffs_ref *r)
{
  if (!mounted && cffs_mount () < 0)
    return -ENODEV;
  r->dev = fsdev;
  r->blk = superblk;
  r->slot = 0;
  return 0;
}

static struct cffs_dirent *
dirent_ptr (const struct cffs_ref *r, int writable)
{
  uint8_t *p = xnl_map (r->dev, r->blk, writable);
  if (p == NULL)
    return NULL;
  return (struct cffs_dirent *) (p + r->slot * CFFS_DIRENT_SIZE);
}

int
cffs_iget (const struct cffs_ref *r, struct cffs_inode *ino)
{
  struct cffs_dirent *de = dirent_ptr (r, 0);

  if (de == NULL)
    return -EIO;
  memcpy (ino, &de->d_inode, sizeof (*ino));
  return 0;
}

/* Replace the inode at R with INO (a metadata modification). */
static int
iput (const struct cffs_ref *r, const struct cffs_inode *ino)
{
  struct xn_op op;
  int res;

  memset (&op, 0, sizeof (op));
  op.o_nmods = 1;
  op.o_mods[0].m_off = INO_OFF (r->slot);
  op.o_mods[0].m_len = sizeof (*ino);
  op.o_mods[0].m_data = (uint32_t) ino;
  for (int tries = 0; tries < 1000; tries++)
    {
      res = sys_xn_modify (EXOS_CAP, r->dev, r->blk, &op);
      if (res != -E_BUSY)
	break;
      sys_yield (-1);
    }
  return xe (res);
}

/*
 * Allocate a block of type TYPE, pointed to by the 32-bit word at
 * offset OFF of block PARENT.  If INO is given, OFF is inside the inode
 * of REF and the whole (updated) inode is written.
 */
static int
alloc_child (const struct cffs_ref *ref, struct cffs_inode *ino,
	     uint32_t parent, uint32_t off, uint32_t * ptr, unsigned type,
	     uint32_t hint, uint32_t * out)
{
  struct xn_op op;

  for (int tries = 0; tries < 64; tries++)
    {
      int b = xnl_alloc_block (ref->dev, hint), r;
      uint32_t nb;

      if (b < 0)
	return b;
      nb = b;
      memset (&op, 0, sizeof (op));
      op.o_child = nb;
      op.o_nchild = 1;
      op.o_ctype = tid[type];
      op.o_nmods = 1;
      *ptr = nb;
      if (ino)
	{
	  op.o_mods[0].m_off = INO_OFF (ref->slot);
	  op.o_mods[0].m_len = sizeof (*ino);
	  op.o_mods[0].m_data = (uint32_t) ino;
	}
      else
	{
	  op.o_mods[0].m_off = off;
	  op.o_mods[0].m_len = 4;
	  op.o_mods[0].m_data = (uint32_t) & nb;
	}
      r = sys_xn_alloc (EXOS_CAP, ref->dev, parent, &op);
      if (r == 0)
	{
	  xnl_set_parent (nb, parent);
	  *out = nb;
	  return 0;
	}
      *ptr = 0;
      if (r == -E_BUSY)
	{
	  sys_yield (-1);
	  continue;
	}
      if (r != -E_NOT_FREE)
	return xe (r);
      hint = nb + 1;
    }
  return -ENOSPC;
}

static int
read_ptr (unsigned dev, uint32_t blk, uint32_t idx, uint32_t * v)
{
  uint32_t *p = xnl_map (dev, blk, 0);
  if (p == NULL)
    return -EIO;
  *v = p[idx];
  return 0;
}

/*
 * Map file block BI of the inode at R to a disk block.  With ALLOC,
 * allocate missing blocks (TYPE is the type of data blocks: data or
 * directory).  Returns the block (0 = hole) and its parent.
 */
static int
bmap (const struct cffs_ref *r, struct cffs_inode *ino, uint32_t bi,
      int alloc, uint32_t * blkp, uint32_t * parentp)
{
  int isdir = (ino->i_mode & S_IFMT) == S_IFDIR;
  unsigned dtype = isdir ? CFFS_T_DIR : CFFS_T_DATA;
  uint32_t hint = r->blk, ptr, res;
  int e;

  *blkp = 0;
  if (bi < CFFS_NDIRECT)
    {
      *parentp = r->blk;
      if (bi > 0 && ino->i_direct[bi - 1])
	hint = ino->i_direct[bi - 1] + 1;
      if (ino->i_direct[bi] == 0 && alloc)
	{
	  e = alloc_child (r, ino, r->blk, 0, &ino->i_direct[bi], dtype, hint,
			   &res);
	  if (e < 0)
	    return e;
	}
      *blkp = ino->i_direct[bi];
      return 0;
    }
  bi -= CFFS_NDIRECT;
  if (bi < CFFS_NPTRS)
    {
      if (ino->i_indirect == 0)
	{
	  if (!alloc)
	    return 0;
	  e = alloc_child (r, ino, r->blk, 0, &ino->i_indirect,
			   isdir ? CFFS_T_DIRIND : CFFS_T_IND,
			   ino->i_direct[CFFS_NDIRECT - 1] + 1, &res);
	  if (e < 0)
	    return e;
	}
      xnl_set_parent (ino->i_indirect, r->blk);
      if (xnl_get (r->dev, ino->i_indirect, r->blk) < 0)
	return -EIO;
      *parentp = ino->i_indirect;
      if (read_ptr (r->dev, ino->i_indirect, bi, &ptr) < 0)
	return -EIO;
      if (ptr == 0 && alloc)
	{
	  if (bi > 0)
	    read_ptr (r->dev, ino->i_indirect, bi - 1, &hint);
	  e = alloc_child (r, NULL, ino->i_indirect, bi * 4, &ptr, dtype,
			   hint ? hint + 1 : ino->i_indirect + 1, &res);
	  if (e < 0)
	    return e;
	}
      *blkp = ptr;
      return 0;
    }
  bi -= CFFS_NPTRS;
  if (isdir || bi >= CFFS_NPTRS * CFFS_NPTRS)
    return -EFBIG;
  if (ino->i_dindirect == 0)
    {
      if (!alloc)
	return 0;
      e = alloc_child (r, ino, r->blk, 0, &ino->i_dindirect, CFFS_T_DIND,
		       hint, &res);
      if (e < 0)
	return e;
    }
  xnl_set_parent (ino->i_dindirect, r->blk);
  if (xnl_get (r->dev, ino->i_dindirect, r->blk) < 0)
    return -EIO;
  {
    uint32_t ind;
    struct cffs_ref dref = *r;
    if (read_ptr (r->dev, ino->i_dindirect, bi / CFFS_NPTRS, &ind) < 0)
      return -EIO;
    if (ind == 0)
      {
	if (!alloc)
	  return 0;
	e = alloc_child (&dref, NULL, ino->i_dindirect,
			 (bi / CFFS_NPTRS) * 4, &ind, CFFS_T_IND,
			 ino->i_dindirect + 1, &res);
	if (e < 0)
	  return e;
      }
    xnl_set_parent (ind, ino->i_dindirect);
    if (xnl_get (r->dev, ind, ino->i_dindirect) < 0)
      return -EIO;
    *parentp = ind;
    if (read_ptr (r->dev, ind, bi % CFFS_NPTRS, &ptr) < 0)
      return -EIO;
    if (ptr == 0 && alloc)
      {
	e = alloc_child (&dref, NULL, ind, (bi % CFFS_NPTRS) * 4, &ptr,
			 CFFS_T_DATA, ind + 1, &res);
	if (e < 0)
	  return e;
      }
    *blkp = ptr;
  }
  return 0;
}

int
cffs_bmap_ro (const struct cffs_ref *r, uint32_t bi, uint32_t * blk,
	      uint32_t * parent)
{
  struct cffs_inode ino;

  if (cffs_iget (r, &ino) < 0)
    return -EIO;
  return bmap (r, &ino, bi, 0, blk, parent);
}

int
cffs_lookup (const struct cffs_ref *dir, const char *name,
	     struct cffs_ref *out)
{
  struct cffs_inode di;
  size_t len = strlen (name);
  uint32_t nblk;

  if (cffs_iget (dir, &di) < 0)
    return -EIO;
  if ((di.i_mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  if (len > CFFS_NAMELEN)
    return -ENAMETOOLONG;
  nblk = di.i_size / XN_BLKSIZE;
  for (uint32_t i = 0; i < nblk; i++)
    {
      uint32_t blk, parent;
      uint8_t *p;

      if (bmap (dir, &di, i, 0, &blk, &parent) < 0)
	return -EIO;
      if (blk == 0)
	continue;
      if (xnl_get (dir->dev, blk, parent) < 0)
	return -EIO;
      p = xnl_map (dir->dev, blk, 0);
      if (p == NULL)
	return -EIO;
      for (unsigned s = 0; s < CFFS_DIRENTS; s++)
	{
	  struct cffs_dirent *de =
	    (struct cffs_dirent *) (p + s * CFFS_DIRENT_SIZE);
	  if (de->d_used && de->d_namelen == len
	      && memcmp (de->d_name, name, len) == 0)
	    {
	      out->dev = dir->dev;
	      out->blk = blk;
	      out->slot = s;
	      return 0;
	    }
	}
    }
  return -ENOENT;
}

/*
 * PATH is absolute and normalised.
 */
int
cffs_namei (const char *path, struct cffs_ref *r)
{
  char comp[CFFS_NAMELEN + 1];
  struct cffs_ref cur;
  int e;

  e = cffs_root (&cur);
  if (e < 0)
    return e;
  while (*path)
    {
      size_t n;
      while (*path == '/')
	path++;
      if (*path == 0)
	break;
      n = strcspn (path, "/");
      if (n > CFFS_NAMELEN)
	return -ENAMETOOLONG;
      memcpy (comp, path, n);
      comp[n] = 0;
      path += n;
      e = cffs_lookup (&cur, comp, &cur);
      if (e < 0)
	return e;
    }
  *r = cur;
  return 0;
}

static int
lock_blk (unsigned dev, uint32_t blk)
{
  for (int tries = 0; tries < 10000; tries++)
    {
      int r = sys_xn_lock (EXOS_CAP, dev, blk, 1);
      if (r == 0)
	return 0;
      if (r != -E_BUSY)
	return xe (r);
      sys_yield (-1);
    }
  return -EBUSY;
}

static void
unlock_blk (unsigned dev, uint32_t blk)
{
  sys_xn_lock (EXOS_CAP, dev, blk, 0);
}

static int
write_dirent (unsigned dev, uint32_t blk, unsigned slot,
	      const struct cffs_dirent *de, unsigned off, unsigned len)
{
  struct xn_op op;

  memset (&op, 0, sizeof (op));
  op.o_nmods = 1;
  op.o_mods[0].m_off = slot * CFFS_DIRENT_SIZE + off;
  op.o_mods[0].m_len = len;
  op.o_mods[0].m_data = (uint32_t) de + off;
  return xe (sys_xn_modify (EXOS_CAP, dev, blk, &op));
}

int
cffs_create (const struct cffs_ref *dir, const char *name, mode_t mode,
	     uid_t uid, gid_t gid, struct cffs_ref *out)
{
  struct cffs_inode di;
  struct cffs_dirent de;
  size_t len = strlen (name);
  uint32_t nblk;
  int e;

  if (len == 0 || len > CFFS_NAMELEN || strchr (name, '/'))
    return -EINVAL;
  if (cffs_lookup (dir, name, out) == 0)
    return -EEXIST;

  memset (&de, 0, sizeof (de));
  de.d_used = 1;
  de.d_namelen = len;
  memcpy (de.d_name, name, len);
  de.d_inode.i_mode = mode;
  de.d_inode.i_nlink = 1;
  de.d_inode.i_uid = uid;
  de.d_inode.i_gid = gid;
  de.d_inode.i_atime = de.d_inode.i_mtime = de.d_inode.i_ctime = time (NULL);

  for (int attempt = 0; attempt < 100; attempt++)
    {
      if (cffs_iget (dir, &di) < 0)
	return -EIO;
      nblk = di.i_size / XN_BLKSIZE;
      for (uint32_t i = 0; i <= nblk; i++)
	{
	  uint32_t blk, parent;
	  uint8_t *p;

	  if (i == nblk)
	    {
	      /* Grow the directory by one block (pointer and size are
	         updated by the same XN allocation when direct). */
	      di.i_size = (i + 1) * XN_BLKSIZE;
	      di.i_mtime = time (NULL);
	      e = bmap (dir, &di, i, 1, &blk, &parent);
	      if (e < 0)
		return e;
	      if (i >= CFFS_NDIRECT)
		{
		  e = iput (dir, &di);
		  if (e < 0)
		    return e;
		}
	    }
	  else if (bmap (dir, &di, i, 0, &blk, &parent) < 0 || blk == 0)
	    continue;
	  if (xnl_get (dir->dev, blk, parent) < 0)
	    return -EIO;
	  p = xnl_map (dir->dev, blk, 0);
	  if (p == NULL)
	    return -EIO;
	  for (unsigned s = 0; s < CFFS_DIRENTS; s++)
	    {
	      if (p[s * CFFS_DIRENT_SIZE] != 0)
		continue;
	      e = lock_blk (dir->dev, blk);
	      if (e < 0)
		return e;
	      if (p[s * CFFS_DIRENT_SIZE] != 0)
		{
		  unlock_blk (dir->dev, blk);
		  continue;
		}
	      e = write_dirent (dir->dev, blk, s, &de, 0, CFFS_DIRENT_SIZE);
	      unlock_blk (dir->dev, blk);
	      if (e < 0)
		return e;
	      out->dev = dir->dev;
	      out->blk = blk;
	      out->slot = s;
	      xnl_set_parent (blk, parent);
	      return 0;
	    }
	}
    }
  return -EBUSY;
}

ssize_t
cffs_read (const struct cffs_ref *r, off_t off, void *buf, size_t n)
{
  struct cffs_inode ino;
  size_t done = 0;

  if (cffs_iget (r, &ino) < 0)
    return -EIO;
  if (off >= (off_t) ino.i_size)
    return 0;
  if (off + n > ino.i_size)
    n = ino.i_size - off;
  while (done < n)
    {
      uint32_t bi = (off + done) / XN_BLKSIZE;
      uint32_t bo = (off + done) % XN_BLKSIZE;
      size_t m = XN_BLKSIZE - bo;
      uint32_t blk, parent;

      if (m > n - done)
	m = n - done;
      if (bmap (r, &ino, bi, 0, &blk, &parent) < 0)
	return done ? (ssize_t) done : -EIO;
      if (blk == 0)
	memset ((char *) buf + done, 0, m);
      else
	{
	  uint8_t *p;
	  if (xnl_get (r->dev, blk, parent) < 0)
	    return done ? (ssize_t) done : -EIO;
	  p = xnl_map (r->dev, blk, 0);
	  if (p == NULL)
	    return done ? (ssize_t) done : -errno;
	  memcpy ((char *) buf + done, p + bo, m);
	}
      done += m;
    }
  return done;
}

ssize_t
cffs_write (const struct cffs_ref *r, off_t off, const void *buf, size_t n)
{
  struct cffs_inode ino;
  size_t done = 0;
  int e = 0;

  if (cffs_iget (r, &ino) < 0)
    return -EIO;
  if ((ino.i_mode & S_IFMT) == S_IFDIR)
    return -EISDIR;
  while (done < n)
    {
      uint32_t bi = (off + done) / XN_BLKSIZE;
      uint32_t bo = (off + done) % XN_BLKSIZE;
      size_t m = XN_BLKSIZE - bo;
      uint32_t blk, parent;
      uint8_t *p;

      if (m > n - done)
	m = n - done;
      e = bmap (r, &ino, bi, 1, &blk, &parent);
      if (e < 0)
	break;
      if (xnl_get (r->dev, blk, parent) < 0)
	{
	  e = -EIO;
	  break;
	}
      p = xnl_map (r->dev, blk, 1);
      if (p == NULL)
	{
	  e = -errno;
	  break;
	}
      memcpy (p + bo, (const char *) buf + done, m);
      xnl_writeback (r->dev, blk, 0);
      done += m;
    }
  if (done)
    {
      if (off + done > ino.i_size)
	ino.i_size = off + done;
      ino.i_mtime = time (NULL);
      int r2 = iput (r, &ino);
      if (r2 < 0 && e == 0)
	e = r2;
    }
  return done ? (ssize_t) done : e;
}

/*
 * Free the block pointed to by the word at OFF in PARENT (or by the
 * inode field, if INO).
 */
static int
free_child (const struct cffs_ref *ref, struct cffs_inode *ino,
	    uint32_t parent, uint32_t off, uint32_t * ptr)
{
  struct xn_op op;
  uint32_t b = *ptr, zero = 0;
  int r;

  if (b == 0)
    return 0;
  xnl_unmap (ref->dev, b);
  /* XN needs metadata children in core to check they own nothing. */
  xnl_get (ref->dev, b, parent);
  xnl_unmap (ref->dev, b);
  memset (&op, 0, sizeof (op));
  op.o_child = b;
  op.o_nchild = 1;
  op.o_nmods = 1;
  *ptr = 0;
  if (ino)
    {
      op.o_mods[0].m_off = INO_OFF (ref->slot);
      op.o_mods[0].m_len = sizeof (*ino);
      op.o_mods[0].m_data = (uint32_t) ino;
    }
  else
    {
      op.o_mods[0].m_off = off;
      op.o_mods[0].m_len = 4;
      op.o_mods[0].m_data = (uint32_t) & zero;
    }
  for (int tries = 0; tries < 1000; tries++)
    {
      r = sys_xn_free (EXOS_CAP, ref->dev, parent, &op);
      if (r != -E_BUSY)
	break;
      sys_yield (-1);
    }
  if (r < 0)
    *ptr = b;
  return xe (r);
}

static int
free_ptrs (const struct cffs_ref *r, uint32_t blk, uint32_t from)
{
  uint32_t *p;
  int e;

  if (xnl_get (r->dev, blk, 0) < 0)
    return -EIO;
  for (uint32_t i = from; i < CFFS_NPTRS; i++)
    {
      p = xnl_map (r->dev, blk, 0);
      if (p == NULL)
	return -EIO;
      if (p[i] == 0)
	continue;
      uint32_t v = p[i];
      xnl_set_parent (v, blk);
      e = free_child (r, NULL, blk, i * 4, &v);
      if (e < 0)
	return e;
    }
  return 0;
}

/* Free the blocks of R beyond SIZE bytes. */
static int
free_blocks (const struct cffs_ref *r, off_t size)
{
  struct cffs_inode ino;
  uint32_t keep;
  int e;

  if (cffs_iget (r, &ino) < 0)
    return -EIO;
  keep = (size + XN_BLKSIZE - 1) / XN_BLKSIZE;

  /* Double indirect. */
  if (ino.i_dindirect)
    {
      uint32_t base = CFFS_NDIRECT + CFFS_NPTRS;
      uint32_t *p;
      xnl_set_parent (ino.i_dindirect, r->blk);
      if (xnl_get (r->dev, ino.i_dindirect, r->blk) < 0)
	return -EIO;
      for (uint32_t j = 0; j < CFFS_NPTRS; j++)
	{
	  uint32_t first = base + j * CFFS_NPTRS, ind;
	  p = xnl_map (r->dev, ino.i_dindirect, 0);
	  if (p == NULL)
	    return -EIO;
	  ind = p[j];
	  if (ind == 0)
	    continue;
	  xnl_set_parent (ind, ino.i_dindirect);
	  if (keep >= first + CFFS_NPTRS)
	    continue;
	  e = free_ptrs (r, ind, keep > first ? keep - first : 0);
	  if (e < 0)
	    return e;
	  if (keep <= first)
	    {
	      e = free_child (r, NULL, ino.i_dindirect, j * 4, &ind);
	      if (e < 0)
		return e;
	    }
	}
      if (keep <= base)
	{
	  e = free_child (r, &ino, r->blk, 0, &ino.i_dindirect);
	  if (e < 0)
	    return e;
	}
    }
  /* Single indirect. */
  if (ino.i_indirect && keep < CFFS_NDIRECT + CFFS_NPTRS)
    {
      xnl_set_parent (ino.i_indirect, r->blk);
      e = free_ptrs (r, ino.i_indirect,
		     keep > CFFS_NDIRECT ? keep - CFFS_NDIRECT : 0);
      if (e < 0)
	return e;
      if (keep <= CFFS_NDIRECT)
	{
	  e = free_child (r, &ino, r->blk, 0, &ino.i_indirect);
	  if (e < 0)
	    return e;
	}
    }
  /* Direct. */
  for (uint32_t i = keep; i < CFFS_NDIRECT; i++)
    if (ino.i_direct[i])
      {
	xnl_set_parent (ino.i_direct[i], r->blk);
	e = free_child (r, &ino, r->blk, 0, &ino.i_direct[i]);
	if (e < 0)
	  return e;
      }
  return 0;
}

int
cffs_truncate (const struct cffs_ref *r, off_t size)
{
  struct cffs_inode ino;
  int e;

  if (cffs_iget (r, &ino) < 0)
    return -EIO;
  if (size < (off_t) ino.i_size)
    {
      e = free_blocks (r, size);
      if (e < 0)
	return e;
      if (cffs_iget (r, &ino) < 0)
	return -EIO;
    }
  else if (size == (off_t) ino.i_size)
    return 0;
  ino.i_size = size;
  ino.i_mtime = time (NULL);
  return iput (r, &ino);
}

static int
dir_empty (const struct cffs_ref *d)
{
  uint32_t cookie = 0;
  char name[CFFS_NAMELEN + 1];
  struct cffs_ref c;

  return cffs_readdir (d, &cookie, name, &c) == 0;
}

int
cffs_unlink (const struct cffs_ref *dir, const char *name, int isdir)
{
  struct cffs_ref r;
  struct cffs_inode ino;
  struct cffs_dirent de;
  int e;

  e = cffs_lookup (dir, name, &r);
  if (e < 0)
    return e;
  if (cffs_iget (&r, &ino) < 0)
    return -EIO;
  if (isdir && (ino.i_mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  if (!isdir && (ino.i_mode & S_IFMT) == S_IFDIR)
    return -EISDIR;
  if (isdir && dir_empty (&r) != 1)
    return -ENOTEMPTY;
  e = free_blocks (&r, 0);
  if (e < 0)
    return e;
  memset (&de, 0, sizeof (de));
  e = lock_blk (r.dev, r.blk);
  if (e < 0)
    return e;
  e = write_dirent (r.dev, r.blk, r.slot, &de, 0, CFFS_DIRENT_SIZE);
  unlock_blk (r.dev, r.blk);
  return e;
}

int
cffs_readdir (const struct cffs_ref *dir, uint32_t * cookie, char *name,
	      struct cffs_ref *child)
{
  struct cffs_inode di;
  uint32_t nblk;

  if (cffs_iget (dir, &di) < 0)
    return -EIO;
  if ((di.i_mode & S_IFMT) != S_IFDIR)
    return -ENOTDIR;
  nblk = di.i_size / XN_BLKSIZE;
  while (*cookie / CFFS_DIRENTS < nblk)
    {
      uint32_t i = *cookie / CFFS_DIRENTS, s = *cookie % CFFS_DIRENTS;
      uint32_t blk, parent;
      uint8_t *p;

      if (bmap (dir, &di, i, 0, &blk, &parent) < 0)
	return -EIO;
      if (blk == 0)
	{
	  *cookie = (i + 1) * CFFS_DIRENTS;
	  continue;
	}
      if (xnl_get (dir->dev, blk, parent) < 0)
	return -EIO;
      p = xnl_map (dir->dev, blk, 0);
      if (p == NULL)
	return -EIO;
      for (; s < CFFS_DIRENTS; s++)
	{
	  struct cffs_dirent *de =
	    (struct cffs_dirent *) (p + s * CFFS_DIRENT_SIZE);
	  if (!de->d_used)
	    continue;
	  memcpy (name, de->d_name, de->d_namelen);
	  name[de->d_namelen] = 0;
	  child->dev = dir->dev;
	  child->blk = blk;
	  child->slot = s;
	  *cookie = i * CFFS_DIRENTS + s + 1;
	  return 1;
	}
      *cookie = (i + 1) * CFFS_DIRENTS;
    }
  return 0;
}

int
cffs_rename (const struct cffs_ref *odir, const char *oname,
	     const struct cffs_ref *ndir, const char *nname)
{
  struct cffs_ref r, t;
  struct cffs_inode ino;
  struct cffs_dirent de;
  size_t len = strlen (nname);
  int e;

  if (len == 0 || len > CFFS_NAMELEN)
    return -EINVAL;
  e = cffs_lookup (odir, oname, &r);
  if (e < 0)
    return e;
  if (cffs_iget (&r, &ino) < 0)
    return -EIO;
  if (cffs_lookup (ndir, nname, &t) == 0)
    {
      if (t.blk == r.blk && t.slot == r.slot)
	return 0;
      e = cffs_unlink (ndir, nname, (ino.i_mode & S_IFMT) == S_IFDIR);
      if (e < 0)
	return e;
    }
  if (odir->dev == ndir->dev && odir->blk == ndir->blk
      && odir->slot == ndir->slot)
    {
      /* Same directory: change the name only. */
      memset (&de, 0, sizeof (de));
      de.d_used = 1;
      de.d_namelen = len;
      memcpy (de.d_name, nname, len);
      e = lock_blk (r.dev, r.blk);
      if (e < 0)
	return e;
      e = write_dirent (r.dev, r.blk, r.slot, &de, 0, CFFS_DIRENT_INODE);
      unlock_blk (r.dev, r.blk);
      return e;
    }
  /* Across directories: XN has no "move" of block pointers between
     metadata blocks, so files are copied; directories cannot move. */
  if ((ino.i_mode & S_IFMT) == S_IFDIR)
    return -EXDEV;
  e = cffs_create (ndir, nname, ino.i_mode, ino.i_uid, ino.i_gid, &t);
  if (e < 0)
    return e;
  {
    static char buf[XN_BLKSIZE];
    for (off_t off = 0; off < (off_t) ino.i_size; off += XN_BLKSIZE)
      {
	ssize_t n = cffs_read (&r, off, buf, sizeof (buf));
	if (n <= 0)
	  break;
	if (cffs_write (&t, off, buf, n) != n)
	  {
	    cffs_unlink (ndir, nname, 0);
	    return -EIO;
	  }
      }
  }
  return cffs_unlink (odir, oname, 0);
}

int
cffs_setattr (const struct cffs_ref *r, int what, uint32_t v1, uint32_t v2)
{
  struct cffs_inode ino;

  if (cffs_iget (r, &ino) < 0)
    return -EIO;
  switch (what)
    {
    case CFFS_SET_MODE:
      ino.i_mode = (ino.i_mode & S_IFMT) | (v1 & 07777);
      break;
    case CFFS_SET_OWNER:
      if (v1 != (uint32_t) -1)
	ino.i_uid = v1;
      if (v2 != (uint32_t) -1)
	ino.i_gid = v2;
      break;
    case CFFS_SET_TIMES:
      ino.i_atime = v1;
      ino.i_mtime = v2;
      break;
    default:
      return -EINVAL;
    }
  ino.i_ctime = time (NULL);
  return iput (r, &ino);
}

void
cffs_stat (const struct cffs_ref *r, const struct cffs_inode *ino,
	   struct stat *st)
{
  memset (st, 0, sizeof (*st));
  st->st_dev = r->dev;
  st->st_ino = r->blk * CFFS_DIRENTS + r->slot;
  st->st_mode = ino->i_mode;
  st->st_nlink = ino->i_nlink;
  st->st_uid = ino->i_uid;
  st->st_gid = ino->i_gid;
  st->st_size = ino->i_size;
  st->st_blksize = XN_BLKSIZE;
  st->st_blocks = (ino->i_size + 511) / 512;
  st->st_atime = ino->i_atime;
  st->st_mtime = ino->i_mtime;
  st->st_ctime = ino->i_ctime;
}

int
cffs_sync (void)
{
  int r;

  if (!mounted)
    return 0;
  r = xnl_sync (fsdev);
  sys_xn_sync (fsdev);
  return r;
}
