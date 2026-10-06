/*
 * ExOS file descriptors and their object types: console (TTY line
 * discipline), C-FFS files and directories, pipes (on Xok software
 * regions), /dev/null and /dev/zero.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <exos/fd.h>
#include <exos/net.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>

struct fdtab __fdtab;
char __cwd[256] = "/";
mode_t __umask = 022;

void
file_lock (struct file *f)
{
  while (__atomic_exchange_n (&f->f_lock, 1, __ATOMIC_ACQUIRE))
    sys_yield (-1);
}

void
file_unlock (struct file *f)
{
  __atomic_store_n (&f->f_lock, 0, __ATOMIC_RELEASE);
}

int
file_index (struct file *f)
{
  return f - files;
}

struct file *
fd_file (int fd)
{
  int i;

  if (fd < 0 || fd >= NOFILE)
    return NULL;
  i = __fdtab.fd[fd];
  if (i < 0 || i >= NFILE)
    return NULL;
  if (files[i].f_ref == 0)
    return NULL;
  return &files[i];
}

int
fd_alloc_file (int type, int flags, struct file **fp)
{
  for (int i = 0; i < NFILE; i++)
    {
      uint32_t zero = 0;
      if (files[i].f_ref == 0
	  && __atomic_compare_exchange_n (&files[i].f_ref, &zero, 1, 0,
					  __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
	{
	  struct file *f = &files[i];
	  f->f_type = type;
	  f->f_flags = flags;
	  f->f_lock = 0;
	  f->f_off = 0;
	  memset (&f->u, 0, sizeof (f->u));
	  *fp = f;
	  return 0;
	}
    }
  return -ENFILE;
}

int
fd_install (struct file *f, int minfd)
{
  for (int fd = minfd; fd < NOFILE; fd++)
    if (__fdtab.fd[fd] < 0)
      {
	__fdtab.fd[fd] = file_index (f);
	__fdtab.cloexec[fd] = 0;
	return fd;
      }
  return -EMFILE;
}

void
file_ref (struct file *f)
{
  __atomic_add_fetch (&f->f_ref, 1, __ATOMIC_ACQ_REL);
}

void
file_unref (struct file *f)
{
  if (__atomic_sub_fetch (&f->f_ref, 1, __ATOMIC_ACQ_REL) == 0)
    {
      if (f->f_type < FT_NTYPES && exos_fops[f->f_type]
	  && exos_fops[f->f_type]->close)
	exos_fops[f->f_type]->close (f);
    }
}

void
fd_close_all (void)
{
  for (int fd = 0; fd < NOFILE; fd++)
    if (__fdtab.fd[fd] >= 0)
      close (fd);
}

/*
 * Console: a minimal TTY line discipline.
 */
static char linebuf[512];
static int lpos, llen;

static void
echo (const char *s, size_t n)
{
  sys_cputs (s, n);
}

static int
cons_getc_wait (int nonblock)
{
  for (;;)
    {
      uint32_t seen = sysinfo_page->si_cons_in;
      int c = sys_cgetc ();
      if (c >= 0)
	return c;
      if (nonblock)
	return -EAGAIN;
      exos_sleep_until_mem (&sysinfo_page->si_cons_in, WK_NE, seen, 0);
      if (__proc->sigpending)
	return -EINTR;
    }
}

static ssize_t
cons_read (struct file *f, void *buf, size_t n)
{
  if (lpos >= llen)
    {
      lpos = llen = 0;
      for (;;)
	{
	  int c = cons_getc_wait (f->f_flags & O_NONBLOCK);
	  if (c < 0)
	    return c;
	  if (c == '\r')
	    c = '\n';
	  if (c == 3)
	    {
	      echo ("^C\n", 3);
	      llen = 0;
	      raise (SIGINT);
	      return -EINTR;
	    }
	  if (c == 4)
	    {
	      if (llen == 0)
		return 0;
	      break;
	    }
	  if (c == '\b' || c == 0x7f)
	    {
	      if (llen > 0)
		{
		  llen--;
		  echo ("\b \b", 3);
		}
	      continue;
	    }
	  if (c == 21)
	    {
	      while (llen > 0)
		{
		  llen--;
		  echo ("\b \b", 3);
		}
	      continue;
	    }
	  if (llen < (int) sizeof (linebuf) - 1)
	    {
	      char ch = c;
	      linebuf[llen++] = ch;
	      echo (&ch, 1);
	    }
	  if (c == '\n')
	    break;
	}
    }
  size_t m = llen - lpos;
  if (m > n)
    m = n;
  memcpy (buf, linebuf + lpos, m);
  lpos += m;
  return m;
}

static ssize_t
cons_write (struct file *f, const void *buf, size_t n)
{
  sys_cputs (buf, n);
  return n;
}

static int
cons_fstat (struct file *f, struct stat *st)
{
  memset (st, 0, sizeof (*st));
  st->st_mode = S_IFCHR | 0620;
  st->st_rdev = 1;
  return 0;
}

static int
cons_ready (struct file *f, int write)
{
  return write || lpos < llen || sysinfo_page->si_cons_in != 0;
}

static struct file_ops cons_ops = {
  .read = cons_read,
  .write = cons_write,
  .fstat = cons_fstat,
  .ready = cons_ready,
};

/*
 * /dev/null and /dev/zero.
 */
static ssize_t
null_read (struct file *f, void *buf, size_t n)
{
  if (f->f_type == FT_ZERO)
    {
      memset (buf, 0, n);
      return n;
    }
  return 0;
}

static ssize_t
null_write (struct file *f, const void *buf, size_t n)
{
  return n;
}

static int
null_fstat (struct file *f, struct stat *st)
{
  memset (st, 0, sizeof (*st));
  st->st_mode = S_IFCHR | 0666;
  st->st_rdev = f->f_type == FT_ZERO ? 5 : 3;
  return 0;
}

static struct file_ops null_ops = {
  .read = null_read,
  .write = null_write,
  .fstat = null_fstat,
};

/*
 * Regular files (C-FFS).
 */
static int
file_resolve (struct file *f)
{
  struct cffs_inode ino;

  if (cffs_iget (&f->u.file.ref, &ino) == 0)
    return 0;
  /* Our process does not know the path to the inode yet. */
  return cffs_namei (f->u.file.path, &f->u.file.ref);
}

static ssize_t
file_read (struct file *f, void *buf, size_t n)
{
  ssize_t r;

  if (file_resolve (f) < 0)
    return -EIO;
  file_lock (f);
  r = cffs_read (&f->u.file.ref, f->f_off, buf, n);
  if (r > 0)
    f->f_off += r;
  file_unlock (f);
  return r;
}

static ssize_t
file_write (struct file *f, const void *buf, size_t n)
{
  ssize_t r;

  if (file_resolve (f) < 0)
    return -EIO;
  file_lock (f);
  if (f->f_flags & O_APPEND)
    {
      struct cffs_inode ino;
      if (cffs_iget (&f->u.file.ref, &ino) == 0)
	f->f_off = ino.i_size;
    }
  r = cffs_write (&f->u.file.ref, f->f_off, buf, n);
  if (r > 0)
    f->f_off += r;
  file_unlock (f);
  return r;
}

static off_t
file_lseek (struct file *f, off_t off, int whence)
{
  struct cffs_inode ino;
  int64_t base = 0;

  if (file_resolve (f) < 0)
    return -EIO;
  if (whence == SEEK_CUR)
    base = f->f_off;
  else if (whence == SEEK_END)
    {
      if (cffs_iget (&f->u.file.ref, &ino) < 0)
	return -EIO;
      base = ino.i_size;
    }
  else if (whence != SEEK_SET)
    return -EINVAL;
  if (base + off < 0)
    return -EINVAL;
  f->f_off = base + off;
  return f->f_off;
}

static int
file_fstat (struct file *f, struct stat *st)
{
  struct cffs_inode ino;

  if (file_resolve (f) < 0 || cffs_iget (&f->u.file.ref, &ino) < 0)
    return -EIO;
  cffs_stat (&f->u.file.ref, &ino, st);
  return 0;
}

static int
always_ready (struct file *f, int write)
{
  return 1;
}

static struct file_ops file_ops = {
  .read = file_read,
  .write = file_write,
  .lseek = file_lseek,
  .fstat = file_fstat,
  .ready = always_ready,
};

static ssize_t
dir_read (struct file *f, void *buf, size_t n)
{
  return -EISDIR;
}

static struct file_ops dir_ops = {
  .read = dir_read,
  .lseek = file_lseek,
  .fstat = file_fstat,
};

/*
 * Pipes: the data is kept in a software region, readable and writable
 * only through the kernel, guarded by the creator's capability.  The
 * reader sleeps with a wakeup predicate on the region's write index.
 */
#define PIPE_HDR 64
#define PIPE_SIZE 16384
#define P_RPOS 0
#define P_WPOS 4
#define P_WCLOSED 8
#define P_RCLOSED 12

static int
pipe_hdr (struct file *f, uint32_t h[4])
{
  memset (h, 0, 16);
  return sys_sreg_read (EXOS_CAP, f->u.pipe.sreg, 0, h, 16);
}

static int
pipe_set (struct file *f, unsigned off, uint32_t v)
{
  return sys_sreg_write (EXOS_CAP, f->u.pipe.sreg, off, &v, 4);
}

static void
pipe_wait (struct file *f, unsigned off, uint32_t seen, unsigned closedoff)
{
  struct wk_term t[3];

  memset (t, 0, sizeof (t));
  t[0].wk_op = WK_NE;
  t[0].wk_lkind = WK_SREG32;
  t[0].wk_lval = (f->u.pipe.sreg << 16) | off;
  t[0].wk_rkind = WK_CONST;
  t[0].wk_rval = seen;
  t[0].wk_cap = EXOS_CAP;
  t[1].wk_op = WK_OR;
  t[2].wk_op = WK_NE;
  t[2].wk_lkind = WK_SREG32;
  t[2].wk_lval = (f->u.pipe.sreg << 16) | closedoff;
  t[2].wk_rkind = WK_CONST;
  t[2].wk_rval = 0;
  t[2].wk_cap = EXOS_CAP;
  exos_wkpred (t, 3);
}

static ssize_t
pipe_read (struct file *f, void *buf, size_t n)
{
  uint32_t h[4];

  if (f->u.pipe.wend)
    return -EBADF;
  if (n == 0)
    return 0;
  exos_touch (buf, n, 1);
  for (;;)
    {
      if (pipe_hdr (f, h) < 0)
	return -EIO;
      uint32_t avail = h[1] - h[0];
      if (avail)
	{
	  size_t m = n < avail ? n : avail;
	  uint32_t ro = h[0] % PIPE_SIZE;
	  size_t first = PIPE_SIZE - ro < m ? PIPE_SIZE - ro : m;
	  if (sys_sreg_read (EXOS_CAP, f->u.pipe.sreg, PIPE_HDR + ro, buf,
			     first) < 0)
	    return -EIO;
	  if (m > first)
	    sys_sreg_read (EXOS_CAP, f->u.pipe.sreg, PIPE_HDR,
			   (char *) buf + first, m - first);
	  pipe_set (f, P_RPOS, h[0] + m);
	  return m;
	}
      if (h[2])
	return 0;
      if (f->f_flags & O_NONBLOCK)
	return -EAGAIN;
      pipe_wait (f, P_WPOS, h[1], P_WCLOSED);
      if (__proc->sigpending)
	return -EINTR;
    }
}

static ssize_t
pipe_write (struct file *f, const void *buf, size_t n)
{
  uint32_t h[4];
  size_t done = 0;

  if (!f->u.pipe.wend)
    return -EBADF;
  file_lock (f);
  while (done < n)
    {
      if (pipe_hdr (f, h) < 0)
	{
	  file_unlock (f);
	  return done ? (ssize_t) done : -EIO;
	}
      if (h[3])
	{
	  file_unlock (f);
	  if (!done)
	    raise (SIGPIPE);
	  return done ? (ssize_t) done : -EPIPE;
	}
      uint32_t space = PIPE_SIZE - (h[1] - h[0]);
      if (space == 0)
	{
	  if (f->f_flags & O_NONBLOCK)
	    break;
	  pipe_wait (f, P_RPOS, h[0], P_RCLOSED);
	  continue;
	}
      size_t m = n - done < space ? n - done : space;
      uint32_t wo = h[1] % PIPE_SIZE;
      size_t first = PIPE_SIZE - wo < m ? PIPE_SIZE - wo : m;
      sys_sreg_write (EXOS_CAP, f->u.pipe.sreg, PIPE_HDR + wo,
		      (const char *) buf + done, first);
      if (m > first)
	sys_sreg_write (EXOS_CAP, f->u.pipe.sreg, PIPE_HDR,
			(const char *) buf + done + first, m - first);
      pipe_set (f, P_WPOS, h[1] + m);
      done += m;
    }
  file_unlock (f);
  if (done == 0 && (f->f_flags & O_NONBLOCK))
    return -EAGAIN;
  return done;
}

static void
pipe_close (struct file *f)
{
  struct file *peer = &files[f->u.pipe.peer];

  pipe_set (f, f->u.pipe.wend ? P_WCLOSED : P_RCLOSED, 1);
  if (peer->f_ref == 0)
    sys_sreg_destroy (EXOS_CAP, f->u.pipe.sreg);
}

static int
pipe_fstat (struct file *f, struct stat *st)
{
  memset (st, 0, sizeof (*st));
  st->st_mode = S_IFIFO | 0600;
  return 0;
}

static int
pipe_ready (struct file *f, int write)
{
  uint32_t h[4];
  if (pipe_hdr (f, h) < 0)
    return 1;
  if (write)
    return h[3] || (h[1] - h[0]) < PIPE_SIZE;
  return h[2] || h[1] != h[0];
}

static int
pipe_wk (struct file *f, int write, struct wk_term *t, int max)
{
  uint32_t h[4];
  if (max < 3 || pipe_hdr (f, h) < 0)
    return 0;
  memset (t, 0, 3 * sizeof (*t));
  t[0].wk_op = WK_NE;
  t[0].wk_lkind = WK_SREG32;
  t[0].wk_lval = (f->u.pipe.sreg << 16) | (write ? P_RPOS : P_WPOS);
  t[0].wk_rkind = WK_CONST;
  t[0].wk_rval = write ? h[0] : h[1];
  t[0].wk_cap = EXOS_CAP;
  t[1].wk_op = WK_OR;
  t[2].wk_op = WK_NE;
  t[2].wk_lkind = WK_SREG32;
  t[2].wk_lval = (f->u.pipe.sreg << 16) | (write ? P_RCLOSED : P_WCLOSED);
  t[2].wk_rkind = WK_CONST;
  t[2].wk_rval = 0;
  t[2].wk_cap = EXOS_CAP;
  return 3;
}

static struct file_ops pipe_ops = {
  .read = pipe_read,
  .write = pipe_write,
  .fstat = pipe_fstat,
  .close = pipe_close,
  .ready = pipe_ready,
  .wk = pipe_wk,
};

int
pipe (int fds[2])
{
  struct file *r, *w;
  int sreg, e, fr, fw;

  sreg = sys_sreg_create (EXOS_CAP, PIPE_HDR + PIPE_SIZE);
  if (sreg < 0)
    {
      errno = ENOMEM;
      return -1;
    }
  if ((e = fd_alloc_file (FT_PIPE, O_RDONLY, &r)) < 0)
    goto fail;
  if ((e = fd_alloc_file (FT_PIPE, O_WRONLY, &w)) < 0)
    {
      r->f_ref = 0;
      goto fail;
    }
  r->u.pipe.sreg = w->u.pipe.sreg = sreg;
  r->u.pipe.peer = file_index (w);
  w->u.pipe.peer = file_index (r);
  r->u.pipe.wend = 0;
  w->u.pipe.wend = 1;
  fr = fd_install (r, 0);
  fw = fr < 0 ? fr : fd_install (w, 0);
  if (fr < 0 || fw < 0)
    {
      if (fr >= 0)
	__fdtab.fd[fr] = -1;
      r->f_ref = w->f_ref = 0;
      e = -EMFILE;
      goto fail;
    }
  fds[0] = fr;
  fds[1] = fw;
  return 0;
fail:
  sys_sreg_destroy (EXOS_CAP, sreg);
  errno = -e;
  return -1;
}

struct file_ops *exos_fops[FT_NTYPES] = {
  [FT_CONS] = &cons_ops,
  [FT_FILE] = &file_ops,
  [FT_DIR] = &dir_ops,
  [FT_PIPE] = &pipe_ops,
  [FT_NULL] = &null_ops,
  [FT_ZERO] = &null_ops,
};

/*
 * Initialisation.  The boot environment creates the shared file table
 * and the console descriptors.
 */
void
exos_fd_init (int is_boot)
{
  if (!is_boot)
    return;
  for (int fd = 0; fd < NOFILE; fd++)
    __fdtab.fd[fd] = -1;
  for (uintptr_t a = FILETAB; a < FILETAB + FILETAB_SIZE; a += PGSIZE)
    if (sys_self_insert_pte (CAP_WORLD, PTE_P | PTE_W | PTE_U | PTE_SHARE,
			     a) < 0)
      exos_panic ("cannot allocate the file table");
  memset ((void *) FILETAB, 0, FILETAB_SIZE);
  /* The network configuration page, filled by netd. */
  if (sys_self_insert_pte (CAP_WORLD, PTE_P | PTE_W | PTE_U | PTE_SHARE,
			   NETCFG) == 0)
    {
      memset ((void *) NETCFG, 0, PGSIZE);
      ((volatile struct netcfg_page *) NETCFG)->magic = NETCFG_MAGIC;
    }
  for (int fd = 0; fd < 3; fd++)
    {
      struct file *f;
      if (fd_alloc_file (FT_CONS, O_RDWR, &f) < 0)
	exos_panic ("cannot open the console");
      fd_install (f, fd);
    }
}

void
fd_fork_child (void)
{
  /* Nothing: the parent took the references for us. */
}
