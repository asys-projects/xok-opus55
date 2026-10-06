/*
 * ExOS POSIX interface: file descriptors, files, directories, paths.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <exos/fd.h>
#include <exos/fs.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define FAIL(_e) do { errno = (_e); return -1; } while (0)

/*
 * Paths: absolute, with "." and ".." resolved lexically.
 */
int
exos_abspath (const char *path, char *out, size_t n)
{
  char tmp[512];
  char *comp[64];
  int nc = 0;
  char *p, *save = NULL;

  if (path == NULL || *path == 0)
    return -ENOENT;
  if (path[0] == '/')
    {
      if (strlen (path) >= sizeof (tmp))
	return -ENAMETOOLONG;
      strcpy (tmp, path);
    }
  else
    {
      if (strlen (__cwd) + strlen (path) + 2 > sizeof (tmp))
	return -ENAMETOOLONG;
      strcpy (tmp, __cwd);
      strcat (tmp, "/");
      strcat (tmp, path);
    }
  for (p = strtok_r (tmp, "/", &save); p; p = strtok_r (NULL, "/", &save))
    {
      if (strcmp (p, ".") == 0)
	continue;
      if (strcmp (p, "..") == 0)
	{
	  if (nc > 0)
	    nc--;
	  continue;
	}
      if (nc >= 64)
	return -ENAMETOOLONG;
      comp[nc++] = p;
    }
  size_t len = 0;
  out[0] = 0;
  if (nc == 0)
    {
      if (n < 2)
	return -ENAMETOOLONG;
      strcpy (out, "/");
      return 0;
    }
  for (int i = 0; i < nc; i++)
    {
      size_t l = strlen (comp[i]);
      if (len + l + 2 > n)
	return -ENAMETOOLONG;
      out[len++] = '/';
      memcpy (out + len, comp[i], l);
      len += l;
      out[len] = 0;
    }
  return 0;
}

/* Split ABS into parent directory and last component. */
static int
split (const char *abs, struct cffs_ref *dir, char *name)
{
  char parent[256];
  const char *slash = strrchr (abs, '/');

  if (slash == NULL || slash[1] == 0)
    return -EEXIST;		/* The root. */
  if (strlen (slash + 1) > CFFS_NAMELEN)
    return -ENAMETOOLONG;
  strcpy (name, slash + 1);
  if (slash == abs)
    strcpy (parent, "/");
  else
    {
      memcpy (parent, abs, slash - abs);
      parent[slash - abs] = 0;
    }
  return cffs_namei (parent, dir);
}

static int
devfile (const char *abs)
{
  if (strcmp (abs, "/dev/null") == 0)
    return FT_NULL;
  if (strcmp (abs, "/dev/zero") == 0)
    return FT_ZERO;
  if (strcmp (abs, "/dev/console") == 0 || strcmp (abs, "/dev/tty") == 0)
    return FT_CONS;
  return 0;
}

int
open (const char *path, int flags, ...)
{
  char abs[256], name[CFFS_NAMELEN + 1];
  struct cffs_ref ref, dir;
  struct cffs_inode ino;
  struct file *f;
  mode_t mode = 0;
  int r, fd, dt;

  if (flags & O_CREAT)
    {
      va_list ap;
      va_start (ap, flags);
      mode = va_arg (ap, int);
      va_end (ap);
    }
  if ((r = exos_abspath (path, abs, sizeof (abs))) < 0)
    FAIL (-r);

  if ((dt = devfile (abs)) != 0)
    {
      if ((r = fd_alloc_file (dt, flags, &f)) < 0)
	FAIL (-r);
      if ((fd = fd_install (f, 0)) < 0)
	{
	  f->f_ref = 0;
	  FAIL (EMFILE);
	}
      return fd;
    }

  r = cffs_namei (abs, &ref);
  if (r == -ENOENT && (flags & O_CREAT))
    {
      if ((r = split (abs, &dir, name)) < 0)
	FAIL (-r);
      r = cffs_create (&dir, name, S_IFREG | (mode & 07777 & ~__umask),
		       __proc->euid, __proc->egid, &ref);
      if (r == -EEXIST && !(flags & O_EXCL))
	r = cffs_lookup (&dir, name, &ref);
    }
  else if (r == 0 && (flags & O_CREAT) && (flags & O_EXCL))
    FAIL (EEXIST);
  if (r < 0)
    FAIL (-r);
  if ((r = cffs_iget (&ref, &ino)) < 0)
    FAIL (-r);
  if ((ino.i_mode & S_IFMT) == S_IFDIR)
    {
      if ((flags & O_ACCMODE) != O_RDONLY)
	FAIL (EISDIR);
    }
  else if (flags & O_DIRECTORY)
    FAIL (ENOTDIR);
  if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY
      && (ino.i_mode & S_IFMT) == S_IFREG && ino.i_size)
    if ((r = cffs_truncate (&ref, 0)) < 0)
      FAIL (-r);

  if ((r = fd_alloc_file ((ino.i_mode & S_IFMT) == S_IFDIR ? FT_DIR : FT_FILE,
			  flags, &f)) < 0)
    FAIL (-r);
  f->u.file.ref = ref;
  strlcpy (f->u.file.path, abs, sizeof (f->u.file.path));
  if ((fd = fd_install (f, 0)) < 0)
    {
      f->f_ref = 0;
      FAIL (EMFILE);
    }
  if (flags & O_CLOEXEC)
    __fdtab.cloexec[fd] = 1;
  return fd;
}

int
creat (const char *path, mode_t mode)
{
  return open (path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int
close (int fd)
{
  struct file *f = fd_file (fd);

  if (f == NULL)
    FAIL (EBADF);
  __fdtab.fd[fd] = -1;
  file_unref (f);
  return 0;
}

ssize_t
read (int fd, void *buf, size_t n)
{
  struct file *f = fd_file (fd);
  ssize_t r;

  if (f == NULL || (f->f_flags & O_ACCMODE) == O_WRONLY)
    FAIL (EBADF);
  if (exos_fops[f->f_type] == NULL || exos_fops[f->f_type]->read == NULL)
    FAIL (EINVAL);
  r = exos_fops[f->f_type]->read (f, buf, n);
  if (r < 0)
    FAIL (-r);
  return r;
}

ssize_t
write (int fd, const void *buf, size_t n)
{
  struct file *f = fd_file (fd);
  ssize_t r;

  if (f == NULL || (f->f_flags & O_ACCMODE) == O_RDONLY)
    FAIL (EBADF);
  if (exos_fops[f->f_type] == NULL || exos_fops[f->f_type]->write == NULL)
    FAIL (EINVAL);
  r = exos_fops[f->f_type]->write (f, buf, n);
  if (r < 0)
    FAIL (-r);
  return r;
}

off_t
lseek (int fd, off_t off, int whence)
{
  struct file *f = fd_file (fd);
  off_t r;

  if (f == NULL)
    FAIL (EBADF);
  if (exos_fops[f->f_type]->lseek == NULL)
    FAIL (ESPIPE);
  r = exos_fops[f->f_type]->lseek (f, off, whence);
  if (r < 0)
    FAIL (-r);
  return r;
}

int
fstat (int fd, struct stat *st)
{
  struct file *f = fd_file (fd);
  int r;

  if (f == NULL)
    FAIL (EBADF);
  if (exos_fops[f->f_type]->fstat == NULL)
    FAIL (EINVAL);
  r = exos_fops[f->f_type]->fstat (f, st);
  if (r < 0)
    FAIL (-r);
  return 0;
}

int
stat (const char *path, struct stat *st)
{
  char abs[256];
  struct cffs_ref ref;
  struct cffs_inode ino;
  int r;

  if ((r = exos_abspath (path, abs, sizeof (abs))) < 0)
    FAIL (-r);
  if (devfile (abs))
    {
      memset (st, 0, sizeof (*st));
      st->st_mode = S_IFCHR | 0666;
      return 0;
    }
  if ((r = cffs_namei (abs, &ref)) < 0 || (r = cffs_iget (&ref, &ino)) < 0)
    FAIL (-r);
  cffs_stat (&ref, &ino, st);
  return 0;
}

int
lstat (const char *path, struct stat *st)
{
  return stat (path, st);
}

int
access (const char *path, int mode)
{
  struct stat st;
  unsigned bits;

  if (stat (path, &st) < 0)
    return -1;
  if (mode == F_OK || __proc->euid == 0)
    return 0;
  bits = st.st_uid == __proc->euid ? (st.st_mode >> 6) & 7 : st.st_mode & 7;
  if ((mode & R_OK && !(bits & 4)) || (mode & W_OK && !(bits & 2))
      || (mode & X_OK && !(bits & 1)))
    FAIL (EACCES);
  return 0;
}

int
dup2 (int fd, int nfd)
{
  struct file *f = fd_file (fd);

  if (f == NULL || nfd < 0 || nfd >= NOFILE)
    FAIL (EBADF);
  if (fd == nfd)
    return nfd;
  if (__fdtab.fd[nfd] >= 0)
    close (nfd);
  file_ref (f);
  __fdtab.fd[nfd] = file_index (f);
  __fdtab.cloexec[nfd] = 0;
  return nfd;
}

int
dup (int fd)
{
  struct file *f = fd_file (fd);
  int nfd;

  if (f == NULL)
    FAIL (EBADF);
  file_ref (f);
  nfd = fd_install (f, 0);
  if (nfd < 0)
    {
      file_unref (f);
      FAIL (EMFILE);
    }
  return nfd;
}

int
fcntl (int fd, int cmd, ...)
{
  struct file *f = fd_file (fd);
  va_list ap;
  int arg, nfd;

  if (f == NULL)
    FAIL (EBADF);
  va_start (ap, cmd);
  arg = va_arg (ap, int);
  va_end (ap);
  switch (cmd)
    {
    case F_DUPFD:
      file_ref (f);
      nfd = fd_install (f, arg);
      if (nfd < 0)
	{
	  file_unref (f);
	  FAIL (EMFILE);
	}
      return nfd;
    case F_GETFD:
      return __fdtab.cloexec[fd] ? FD_CLOEXEC : 0;
    case F_SETFD:
      __fdtab.cloexec[fd] = (arg & FD_CLOEXEC) != 0;
      return 0;
    case F_GETFL:
      return f->f_flags;
    case F_SETFL:
      f->f_flags = (f->f_flags & O_ACCMODE) | (arg & ~O_ACCMODE);
      return 0;
    }
  FAIL (EINVAL);
}

int
isatty (int fd)
{
  struct file *f = fd_file (fd);
  if (f == NULL)
    {
      errno = EBADF;
      return 0;
    }
  if (f->f_type != FT_CONS)
    {
      errno = ENOTTY;
      return 0;
    }
  return 1;
}

int
ioctl (int fd, unsigned long req, ...)
{
  va_list ap;
  void *arg;

  va_start (ap, req);
  arg = va_arg (ap, void *);
  va_end (ap);
  if (req == TIOCGWINSZ && isatty (fd))
    {
      struct winsize *w = arg;
      w->ws_row = 25;
      w->ws_col = 80;
      w->ws_xpixel = w->ws_ypixel = 0;
      return 0;
    }
  if (req == FIONBIO)
    {
      struct file *f = fd_file (fd);
      if (f == NULL)
	FAIL (EBADF);
      if (*(int *) arg)
	f->f_flags |= O_NONBLOCK;
      else
	f->f_flags &= ~O_NONBLOCK;
      return 0;
    }
  FAIL (ENOTTY);
}

int
mkdir (const char *path, mode_t mode)
{
  char abs[256], name[CFFS_NAMELEN + 1];
  struct cffs_ref dir, ref;
  int r;

  if ((r = exos_abspath (path, abs, sizeof (abs))) < 0
      || (r = split (abs, &dir, name)) < 0
      || (r = cffs_create (&dir, name, S_IFDIR | (mode & 07777 & ~__umask),
			   __proc->euid, __proc->egid, &ref)) < 0)
    FAIL (-r);
  return 0;
}

static int
remove_entry (const char *path, int isdir)
{
  char abs[256], name[CFFS_NAMELEN + 1];
  struct cffs_ref dir;
  int r;

  if ((r = exos_abspath (path, abs, sizeof (abs))) < 0
      || (r = split (abs, &dir, name)) < 0
      || (r = cffs_unlink (&dir, name, isdir)) < 0)
    FAIL (-r);
  return 0;
}

int
unlink (const char *path)
{
  return remove_entry (path, 0);
}

int
rmdir (const char *path)
{
  return remove_entry (path, 1);
}

int
remove (const char *path)
{
  struct stat st;
  if (stat (path, &st) < 0)
    return -1;
  return remove_entry (path, S_ISDIR (st.st_mode));
}

int
rename (const char *from, const char *to)
{
  char a1[256], a2[256], n1[CFFS_NAMELEN + 1], n2[CFFS_NAMELEN + 1];
  struct cffs_ref d1, d2;
  int r;

  if ((r = exos_abspath (from, a1, sizeof (a1))) < 0
      || (r = exos_abspath (to, a2, sizeof (a2))) < 0
      || (r = split (a1, &d1, n1)) < 0 || (r = split (a2, &d2, n2)) < 0
      || (r = cffs_rename (&d1, n1, &d2, n2)) < 0)
    FAIL (-r);
  return 0;
}

int
link (const char *a, const char *b)
{
  /* C-FFS embeds inodes in directory entries: no hard links. */
  FAIL (EMLINK);
}

int
chdir (const char *path)
{
  char abs[256];
  struct stat st;
  int r;

  if ((r = exos_abspath (path, abs, sizeof (abs))) < 0)
    FAIL (-r);
  if (stat (abs, &st) < 0)
    return -1;
  if (!S_ISDIR (st.st_mode))
    FAIL (ENOTDIR);
  strcpy (__cwd, abs);
  return 0;
}

char *
getcwd (char *buf, size_t size)
{
  if (buf == NULL)
    {
      size = 256;
      buf = malloc (size);
      if (buf == NULL)
	return NULL;
    }
  if (strlen (__cwd) + 1 > size)
    {
      errno = ERANGE;
      return NULL;
    }
  strcpy (buf, __cwd);
  return buf;
}

static int
path_setattr (const char *path, int what, uint32_t a, uint32_t b)
{
  char abs[256];
  struct cffs_ref ref;
  int r;

  if ((r = exos_abspath (path, abs, sizeof (abs))) < 0
      || (r = cffs_namei (abs, &ref)) < 0
      || (r = cffs_setattr (&ref, what, a, b)) < 0)
    FAIL (r == -EACCES ? EPERM : -r);
  return 0;
}

int
chmod (const char *path, mode_t mode)
{
  return path_setattr (path, CFFS_SET_MODE, mode, 0);
}

int
chown (const char *path, uid_t uid, gid_t gid)
{
  return path_setattr (path, CFFS_SET_OWNER, uid, gid);
}

int
utime (const char *path, const void *times)
{
  time_t now = time (NULL);
  return path_setattr (path, CFFS_SET_TIMES, now, now);
}

mode_t
umask (mode_t m)
{
  mode_t old = __umask;
  __umask = m & 0777;
  return old;
}

int
truncate (const char *path, off_t len)
{
  char abs[256];
  struct cffs_ref ref;
  int r;

  if ((r = exos_abspath (path, abs, sizeof (abs))) < 0
      || (r = cffs_namei (abs, &ref)) < 0
      || (r = cffs_truncate (&ref, len)) < 0)
    FAIL (-r);
  return 0;
}

int
ftruncate (int fd, off_t len)
{
  struct file *f = fd_file (fd);
  int r;

  if (f == NULL || f->f_type != FT_FILE)
    FAIL (EBADF);
  if ((r = cffs_truncate (&f->u.file.ref, len)) < 0)
    FAIL (-r);
  return 0;
}

int
fsync (int fd)
{
  cffs_sync ();
  return 0;
}

void
sync (void)
{
  cffs_sync ();
}

/*
 * Directories.
 */
struct _DIR
{
  int fd;
  struct dirent de;
};

DIR *
opendir (const char *path)
{
  int fd = open (path, O_RDONLY | O_DIRECTORY);
  DIR *d;

  if (fd < 0)
    return NULL;
  d = malloc (sizeof (*d));
  if (d == NULL)
    {
      close (fd);
      return NULL;
    }
  d->fd = fd;
  return d;
}

struct dirent *
readdir (DIR *d)
{
  struct file *f = fd_file (d->fd);
  struct cffs_ref child;
  struct cffs_inode ino;
  uint32_t cookie;
  int r;

  if (f == NULL || f->f_type != FT_DIR)
    {
      errno = EBADF;
      return NULL;
    }
  cookie = f->f_off;
  if (cffs_iget (&f->u.file.ref, &ino) < 0
      && cffs_namei (f->u.file.path, &f->u.file.ref) < 0)
    return NULL;
  r = cffs_readdir (&f->u.file.ref, &cookie, d->de.d_name, &child);
  f->f_off = cookie;
  if (r <= 0)
    return NULL;
  d->de.d_ino = child.blk * CFFS_DIRENTS + child.slot;
  d->de.d_type = DT_UNKNOWN;
  if (cffs_iget (&child, &ino) == 0)
    d->de.d_type = (ino.i_mode & S_IFMT) == S_IFDIR ? DT_DIR : DT_REG;
  return &d->de;
}

void
rewinddir (DIR *d)
{
  struct file *f = fd_file (d->fd);
  if (f)
    f->f_off = 0;
}

int
closedir (DIR *d)
{
  int r = close (d->fd);
  free (d);
  return r;
}

/*
 * Miscellaneous.
 */
int
gethostname (char *name, size_t len)
{
  strlcpy (name, "xok", len);
  return 0;
}

long
sysconf (int name)
{
  switch (name)
    {
    case _SC_PAGESIZE:
      return PGSIZE;
    case _SC_NPROCESSORS_ONLN:
      return sysinfo_page->si_ncpu;
    case _SC_CLK_TCK:
      return 1000000000 / sysinfo_page->si_tick_nsec;
    }
  errno = EINVAL;
  return -1;
}

int
nanosleep (const struct timespec *req, struct timespec *rem)
{
  usleep (req->tv_sec * 1000000 + req->tv_nsec / 1000);
  if (rem)
    rem->tv_sec = rem->tv_nsec = 0;
  return 0;
}

int
system (const char *cmd)
{
  char *argv[] = { "sh", "-c", (char *) cmd, NULL };
  int status;
  pid_t pid = fork ();

  if (pid < 0)
    return -1;
  if (pid == 0)
    {
      execv ("/bin/sh", argv);
      _exit (127);
    }
  if (waitpid (pid, &status, 0) < 0)
    return -1;
  return status;
}
