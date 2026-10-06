/*
 * ExOS processes: fork (copy-on-write, using Xok's exposed page
 * tables), exec (a new environment with a fresh address space; read-only
 * program segments are mapped directly from XN's buffer cache, shared
 * by every process running the program), exit, wait, and signals
 * (delivered with Xok IPC).
 *
 * Process state lives in the library part of each environment's u-area,
 * readable by everybody: wait() and ps read it there.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <exos/fd.h>
#include <exos/fs.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>

/*
 * Information passed to a program started by exec, at the bottom of
 * its top stack page.
 */
#define XINFO_MAGIC 0x58494e46
#define XINFO_ADDR (USTACKTOP - PGSIZE)
#define XSTACK_PAGES 16

struct exec_info
{
  uint32_t magic;
  pid_t pid, ppid, pgid;
  uid_t uid, euid;
  gid_t gid, egid;
  mode_t umask;
  uint32_t sigignore;
  char cwd[256];
  struct fdtab fdtab;
};

/* Signal dispositions (private). */
static sighandler_t sig_handlers[NSIG];
static sigset_t sig_mask;

/* Children, for wait(). */
#define MAXCHILD 64
static pid_t children[MAXCHILD];

static void
child_add (pid_t pid)
{
  for (int i = 0; i < MAXCHILD; i++)
    if (children[i] == 0)
      {
	children[i] = pid;
	return;
      }
}

static void
child_del (pid_t pid)
{
  for (int i = 0; i < MAXCHILD; i++)
    if (children[i] == pid)
      children[i] = 0;
}

static int
ipc_signal (envid_t from, uint32_t sig, uint32_t b, uint32_t c, uint32_t * r1)
{
  if (sig == 0 || sig >= NSIG)
    return -EINVAL;
  /* Only processes of the same user (or root) may signal us. */
  volatile struct envinfo *fi = envinfo_of (from);
  volatile struct cap *fc = &fi->e_caps[EXOS_CAP];
  volatile struct cap *mc = &envinfo_of (__envid)->e_caps[EXOS_CAP];
  if (fi->e_id != from)
    return -EPERM;
  if (fc->c_len != 0
      && (fc->c_len != mc->c_len
	  || memcmp ((void *) fc->c_name, (void *) mc->c_name, fc->c_len)))
    return -EPERM;
  __atomic_or_fetch (&__proc->sigpending, 1u << sig, __ATOMIC_RELAXED);
  return 0;
}

static int
ipc_child (envid_t from, uint32_t pid, uint32_t b, uint32_t c, uint32_t * r1)
{
  __atomic_add_fetch (&__proc->child_events, 1, __ATOMIC_RELAXED);
  return 0;
}

void
exos_proc_init (int is_boot)
{
  struct exec_info *xi = (struct exec_info *) XINFO_ADDR;

  exos_ipc_register (IPC_SIGNAL, ipc_signal);
  exos_ipc_register (IPC_CHILD, ipc_child);
  memset ((void *) __proc, 0, sizeof (*__proc));
  __proc->magic = PROC_MAGIC;
  __proc->start_sec = time (NULL);
  if (is_boot)
    {
      struct cap w;
      memset (&w, 0, sizeof (w));
      w.c_valid = 1;
      w.c_perm = CAP_ALL;
      w.c_len = 1;
      w.c_name[0] = 'W';
      sys_cap_forge (CAP_ROOT, CAP_WORLD, &w);
      __proc->pid = 1;
      __proc->ppid = 0;
      __proc->pgid = 1;
      strcpy ((char *) __proc->name, "init");
      strcpy ((char *) __proc->args, "init");
    }
  else if (exos_mapped (XINFO_ADDR) && xi->magic == XINFO_MAGIC)
    {
      __proc->pid = xi->pid;
      __proc->ppid = xi->ppid;
      __proc->pgid = xi->pgid;
      __proc->uid = xi->uid;
      __proc->euid = xi->euid;
      __proc->gid = xi->gid;
      __proc->egid = xi->egid;
      __umask = xi->umask;
      for (int s = 1; s < NSIG; s++)
	if (xi->sigignore & (1u << s))
	  sig_handlers[s] = SIG_IGN;
      memcpy (__cwd, xi->cwd, sizeof (__cwd));
      memcpy (&__fdtab, &xi->fdtab, sizeof (__fdtab));
    }
  __proc->state = PROC_RUNNING;
}

void
exos_proc_setname (int argc, char **argv)
{
  size_t off = 0;
  const char *base = argc > 0 ? strrchr (argv[0], '/') : NULL;

  if (argc > 0)
    strlcpy ((char *) __proc->name, base ? base + 1 : argv[0],
	     sizeof (__proc->name));
  for (int i = 0; i < argc && off < sizeof (__proc->args) - 1; i++)
    {
      size_t l = strlen (argv[i]);
      if (off + l + 2 > sizeof (__proc->args))
	l = sizeof (__proc->args) - off - 2;
      memcpy ((char *) __proc->args + off, argv[i], l);
      off += l;
      ((char *) __proc->args)[off++] = ' ';
    }
  if (off)
    off--;
  ((char *) __proc->args)[off] = 0;
}

/*
 * Find the live environment running process PID.
 */
envid_t
exos_pid2env (pid_t pid)
{
  for (unsigned i = 1; i < NENV; i++)
    {
      volatile struct envinfo *ei = &((volatile struct envinfo *) UENVINFO)[i];
      volatile struct exos_proc *p;
      envid_t id = ei->e_id;

      if (ei->e_status == ENV_FREE || ei->e_status == ENV_DYING)
	continue;
      p = proc_of_env (id);
      if (p->magic != PROC_MAGIC || p->pid != pid)
	continue;
      if (p->state == PROC_RUNNING || p->state == PROC_ZOMBIE)
	return id;
    }
  return 0;
}

pid_t
getpid (void)
{
  return __proc->pid;
}

pid_t
getppid (void)
{
  return __proc->ppid;
}

uid_t
getuid (void)
{
  return __proc->uid;
}

uid_t
geteuid (void)
{
  return __proc->euid;
}

gid_t
getgid (void)
{
  return __proc->gid;
}

gid_t
getegid (void)
{
  return __proc->egid;
}

/*
 * setuid: a root process derives the user's capability from the root
 * capability and drops root.
 */
int
setuid (uid_t uid)
{
  struct cap c;

  if (__proc->euid != 0 && uid != __proc->uid)
    {
      errno = EPERM;
      return -1;
    }
  if (uid != 0)
    {
      memset (&c, 0, sizeof (c));
      c.c_valid = 1;
      c.c_perm = CAP_ALL;
      c.c_len = 3;
      c.c_name[0] = CAP_UID_TAG;
      c.c_name[1] = uid >> 8;
      c.c_name[2] = uid & 0xff;
      if (sys_cap_forge (EXOS_CAP, CAP_USER, &c) < 0
	  || sys_cap_forge (EXOS_CAP, EXOS_CAP, &c) < 0)
	{
	  errno = EPERM;
	  return -1;
	}
    }
  __proc->uid = __proc->euid = uid;
  return 0;
}

int
setgid (gid_t gid)
{
  if (__proc->euid != 0)
    {
      errno = EPERM;
      return -1;
    }
  __proc->gid = __proc->egid = gid;
  return 0;
}

int
setpgid (pid_t pid, pid_t pgid)
{
  if (pid == 0 || pid == __proc->pid)
    {
      __proc->pgid = pgid ? pgid : __proc->pid;
      return 0;
    }
  errno = EPERM;
  return -1;
}

pid_t
getpgrp (void)
{
  return __proc->pgid;
}

/*
 * Scheduling: give a new environment a time slice, spreading
 * processes over the CPUs.
 */
static int
give_quantum (envid_t child)
{
  static unsigned next_cpu;
  unsigned ncpu = sysinfo_page->si_ncpu;
  int r = -1;

  for (unsigned k = 0; k < ncpu && r < 0; k++)
    {
      unsigned cpu = (next_cpu + k) % ncpu;
      r = sys_quantum_alloc (EXOS_CAP, -1, cpu, child);
      if (r >= 0)
	next_cpu = cpu + 1;
    }
  return r;
}

/*
 * fork.
 */
static jmp_buf fork_jb;
static struct exos_proc fork_parent;
static envid_t fork_child_env;
void __exos_fork_child_entry (void);

/* Called by the child, on its exception stack, before anything else. */
void
__exos_fork_child_c (void)
{
  exos_init_upcalls ();
  longjmp (fork_jb, 1);
}

static int
dup_page (envid_t child, uintptr_t va, xpte_t pte)
{
  xpte_t flags = pte & (PTE_P | PTE_U | PTE_AVAIL | PTE_NX);
  ppn_t ppn = PTE_PPN (pte);

  if (ppages_info[ppn].pp_state == PP_BC)
    {
      /* A buffer cache page (e.g. shared program text): map it through
         XN, which checks our access to the block. */
      volatile struct bc_entry *b = &xn_registry[ppages_info[ppn].pp_bc - 1];
      return sys_xn_insert_pte (EXOS_CAP, b->bc_dev, b->bc_blk, va,
				(pte & PTE_W) != 0, EXOS_CAP, child);
    }

  if (pte & PTE_SHARE)
    {
      flags |= pte & PTE_W;
      if (sys_insert_pte (EXOS_CAP, (pte & PTE_FRAME) | flags, va,
			  EXOS_CAP, child) == 0)
	return 0;
      return sys_insert_pte (CAP_WORLD, (pte & PTE_FRAME) | flags, va,
			     EXOS_CAP, child);
    }
  if ((pte & PTE_W) || (pte & PTE_COW))
    {
      /* Copy-on-write, in the child first. */
      int r = sys_insert_pte (EXOS_CAP, (pte & PTE_FRAME) | flags | PTE_COW,
			      va, EXOS_CAP, child);
      if (r < 0)
	return r;
      if (pte & PTE_W)
	return sys_mod_pte_range (EXOS_CAP, 0, 0, va, 1, PTE_COW, PTE_W);
      return 0;
    }
  return sys_insert_pte (EXOS_CAP, (pte & PTE_FRAME) | flags, va, EXOS_CAP,
			 child);
}

/* Give CHILD a private copy of the page at VA. */
#define FORK_TMP (UMMAP_TOP - 2 * PGSIZE)

static int
copy_page (envid_t child, uintptr_t va, xpte_t pte)
{
  int r = sys_self_insert_pte (EXOS_CAP, PTE_P | PTE_W | PTE_U, FORK_TMP);
  if (r < 0)
    return r;
  memcpy ((void *) FORK_TMP, (void *) va, PGSIZE);
  r = sys_insert_pte (EXOS_CAP, (vpt[FORK_TMP >> PGSHIFT] & PTE_FRAME)
		      | PTE_P | PTE_U | PTE_W | (pte & PTE_AVAIL & ~PTE_COW),
		      va, EXOS_CAP, child);
  sys_self_insert_pte (EXOS_CAP, 0, FORK_TMP);
  return r;
}

static int
dup_range (envid_t child, uintptr_t start, uintptr_t end, int copy)
{
  for (uintptr_t va = start; va < end; va += PGSIZE)
    {
      xpte_t pte;
      int r;

      /* Skip 2MB regions without page table (UVPT reads zeros). */
      pte = vpt[va >> PGSHIFT];
      if (!(pte & PTE_P) || (pte & PTE_NOFORK))
	continue;
      /* Pages pinned by the kernel (message rings, wakeup predicates)
         must stay ours: give the child a copy instead of sharing them
         copy-on-write. */
      if (!copy && !(pte & PTE_SHARE) && (pte & PTE_W)
	  && ppages_info[PTE_PPN (pte)].pp_state == PP_USER
	  && ppages_info[PTE_PPN (pte)].pp_pinned)
	r = copy_page (child, va, pte);
      else
	r = copy ? copy_page (child, va, pte) : dup_page (child, va, pte);
      if (r < 0)
	return r;
    }
  return 0;
}

extern char _end[];
extern void exos_mmap_regions (void (*fn) (uintptr_t, size_t, void *),
			       void *arg);

struct mmdup
{
  envid_t child;
  int err;
};

static void
mmap_dup_cb (uintptr_t va, size_t len, void *arg)
{
  struct mmdup *m = arg;
  if (m->err == 0)
    m->err = dup_range (m->child, va, va + len, 0);
}

pid_t
fork (void)
{
  envid_t child;
  struct utf utf;
  int r;
  uintptr_t sp;
  void *heap_end;

  fflush (NULL);
  memcpy (&fork_parent, (void *) __proc, sizeof (fork_parent));
  if (setjmp (fork_jb) != 0)
    {
      /* Child. */
      __envid = sys_getenvid ();
      xnl_fork_child ();
      memcpy ((void *) __proc, &fork_parent, sizeof (fork_parent));
      __proc->pid = __envid;
      __proc->ppid = fork_parent.pid;
      __proc->state = PROC_RUNNING;
      __proc->sigpending = 0;
      __proc->child_events = 0;
      __proc->start_sec = time (NULL);
      memset (children, 0, sizeof (children));
      return 0;
    }

  child = sys_env_alloc (EXOS_CAP);
  if (child < 0)
    {
      errno = EAGAIN;
      return -1;
    }
  fork_child_env = child;

  /* The child's exception stack: fresh pages. */
  for (uintptr_t a = XSTACKTOP - XSTACKSIZE; a < XSTACKTOP; a += PGSIZE)
    if ((r = sys_insert_pte (EXOS_CAP, PTE_P | PTE_W | PTE_U | PTE_NOFORK,
			     a, EXOS_CAP, child)) < 0)
      goto fail;

  /* The stack is copied right away: fork itself is running on it. */
  asm volatile ("movl %%esp, %0":"=r" (sp));
  if ((r = dup_range (child, PGROUNDDOWN (sp) - PGSIZE, USTACKTOP, 1)) < 0)
    goto fail;
  if ((r = dup_range (child, USTACKBOT, PGROUNDDOWN (sp) - PGSIZE, 0)) < 0)
    goto fail;
  /* Program and heap, copy-on-write. */
  heap_end = sbrk (0);
  if ((r = dup_range (child, UTEXT, PGROUNDUP ((uintptr_t) heap_end), 0)) < 0)
    goto fail;
  /* Anonymous mappings and shared regions. */
  {
    struct mmdup m = { child, 0 };
    exos_mmap_regions (mmap_dup_cb, &m);
    if ((r = m.err) < 0)
      goto fail;
  }
  if ((r = dup_range (child, USHARED, USHARED_TOP, 0)) < 0)
    goto fail;

  /* Open files are shared. */
  for (int fd = 0; fd < NOFILE; fd++)
    if (__fdtab.fd[fd] >= 0)
      file_ref (&files[__fdtab.fd[fd]]);

  /* Start the child on its exception stack, in the trampoline. */
  memset (&utf, 0, sizeof (utf));
  utf.utf_eip = (uint32_t) __exos_fork_child_entry;
  utf.utf_esp = XSTACKTOP - 64;
  if ((r = sys_env_set_tf (EXOS_CAP, child, &utf)) < 0)
    goto fail_files;
  if ((r = give_quantum (child)) < 0)
    goto fail_files;
  if ((r = sys_env_set_status (EXOS_CAP, child, ENV_RUNNABLE)) < 0)
    goto fail_files;
  child_add (child);
  return child;

fail_files:
  for (int fd = 0; fd < NOFILE; fd++)
    if (__fdtab.fd[fd] >= 0)
      file_unref (&files[__fdtab.fd[fd]]);
fail:
  sys_env_free (EXOS_CAP, child);
  errno = r == -E_NO_MEM ? ENOMEM : EAGAIN;
  return -1;
}

/*
 * exec.
 */
#define EI_NIDENT 16
struct elf_ehdr
{
  uint8_t e_ident[EI_NIDENT];
  uint16_t e_type, e_machine;
  uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf_phdr
{
  uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags,
    p_align;
};

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

#define STAGE (UMMAP_TOP - 3 * PGSIZE)

int cffs_bmap_ro (const struct cffs_ref *r, uint32_t bi, uint32_t * blk,
		  uint32_t * parent);

/* Map a page of the program (copied from the file) into CHILD. */
static int
load_page (envid_t child, const struct cffs_ref *ref, uintptr_t va,
	   uint32_t seg_va, uint32_t seg_off, uint32_t filesz, int writable)
{
  int r;
  uintptr_t pva = PGROUNDDOWN (va);

  r = sys_self_insert_pte (EXOS_CAP, PTE_P | PTE_W | PTE_U, STAGE);
  if (r < 0)
    return -ENOMEM;
  memset ((void *) STAGE, 0, PGSIZE);
  /* File bytes that fall in this page. */
  uint32_t lo = pva < seg_va ? seg_va : pva;
  uint32_t hi = pva + PGSIZE;
  if (hi > seg_va + filesz)
    hi = seg_va + filesz;
  if (hi > lo)
    {
      ssize_t n = cffs_read (ref, seg_off + (lo - seg_va),
			     (char *) STAGE + (lo - pva), hi - lo);
      if (n != (ssize_t) (hi - lo))
	{
	  sys_self_insert_pte (EXOS_CAP, 0, STAGE);
	  return -ENOEXEC;
	}
    }
  r = sys_insert_pte (EXOS_CAP, (vpt[STAGE >> PGSHIFT] & PTE_FRAME)
		      | PTE_P | PTE_U | (writable ? PTE_W : 0), pva, EXOS_CAP,
		      child);
  sys_self_insert_pte (EXOS_CAP, 0, STAGE);
  return r < 0 ? -ENOMEM : 0;
}

static int
load_segment (envid_t child, const struct cffs_ref *ref,
	      const struct elf_phdr *ph)
{
  uint32_t start = PGROUNDDOWN (ph->p_vaddr);
  uint32_t end = PGROUNDUP (ph->p_vaddr + ph->p_memsz);
  int writable = ph->p_flags & PF_W;
  int shareable = !writable && (ph->p_offset % PGSIZE) == (ph->p_vaddr % PGSIZE);

  if (ph->p_vaddr < UTEXT || end > UHEAP_TOP || ph->p_filesz > ph->p_memsz)
    return -ENOEXEC;
  for (uint32_t va = start; va < end; va += PGSIZE)
    {
      int r;
      /* Read-only pages entirely backed by file blocks: map the buffer
         cache page itself, shared by everybody running this program. */
      if (shareable && va >= ph->p_vaddr
	  && va + PGSIZE <= ph->p_vaddr + ph->p_filesz)
	{
	  uint32_t bi = (ph->p_offset + (va - ph->p_vaddr)) / PGSIZE;
	  uint32_t blk, parent;
	  if (cffs_bmap_ro (ref, bi, &blk, &parent) == 0 && blk
	      && xnl_get (ref->dev, blk, parent) >= 0
	      && sys_xn_insert_pte (EXOS_CAP, ref->dev, blk, va, 0, EXOS_CAP,
				    child) == 0)
	    continue;
	}
      r = load_page (child, ref, va, ph->p_vaddr, ph->p_offset,
		     ph->p_filesz, writable);
      if (r < 0)
	return r;
    }
  return 0;
}

static int
setup_stack (envid_t child, char *const argv[], char *const envp[],
	     uint32_t * spp)
{
  struct exec_info *xi;
  int argc = 0, envc = 0, r;
  uint32_t strp, *ptrs;
  char *page;

  while (argv && argv[argc])
    argc++;
  while (envp && envp[envc])
    envc++;

  /* Lower stack pages: empty, allocated lazily by the child. */
  r = sys_self_insert_pte (EXOS_CAP, PTE_P | PTE_W | PTE_U, STAGE);
  if (r < 0)
    return -ENOMEM;
  page = (char *) STAGE;
  memset (page, 0, PGSIZE);
  xi = (struct exec_info *) page;
  xi->magic = XINFO_MAGIC;
  xi->pid = __proc->pid;
  xi->ppid = __proc->ppid;
  xi->pgid = __proc->pgid;
  xi->uid = __proc->uid;
  xi->euid = __proc->euid;
  xi->gid = __proc->gid;
  xi->egid = __proc->egid;
  xi->umask = __umask;
  for (int s = 1; s < NSIG; s++)
    if (sig_handlers[s] == SIG_IGN)
      xi->sigignore |= 1u << s;
  memcpy (xi->cwd, __cwd, sizeof (xi->cwd));
  memcpy (&xi->fdtab, &__fdtab, sizeof (__fdtab));
  for (int fd = 0; fd < NOFILE; fd++)
    if (xi->fdtab.cloexec[fd])
      xi->fdtab.fd[fd] = -1;

  /* Strings at the top of the page, pointers below them. */
  strp = PGSIZE;
  uint32_t uargv[256], uenvp[256];
  if (argc > 255 || envc > 255)
    goto toobig;
  for (int i = argc - 1; i >= 0; i--)
    {
      size_t l = strlen (argv[i]) + 1;
      if (strp < sizeof (*xi) + l + 16)
	goto toobig;
      strp -= l;
      memcpy (page + strp, argv[i], l);
      uargv[i] = XINFO_ADDR + strp;
    }
  for (int i = envc - 1; i >= 0; i--)
    {
      size_t l = strlen (envp[i]) + 1;
      if (strp < sizeof (*xi) + l + 16)
	goto toobig;
      strp -= l;
      memcpy (page + strp, envp[i], l);
      uenvp[i] = XINFO_ADDR + strp;
    }
  strp &= ~3u;
  uint32_t need = (argc + 1 + envc + 1 + 3) * 4;
  if (strp < sizeof (*xi) + need)
    goto toobig;
  ptrs = (uint32_t *) (page + strp - need);
  ptrs[0] = argc;
  ptrs[1] = XINFO_ADDR + strp - need + 12;
  ptrs[2] = ptrs[1] + (argc + 1) * 4;
  for (int i = 0; i < argc; i++)
    ptrs[3 + i] = uargv[i];
  ptrs[3 + argc] = 0;
  for (int i = 0; i < envc; i++)
    ptrs[4 + argc + i] = uenvp[i];
  ptrs[4 + argc + envc] = 0;
  *spp = XINFO_ADDR + strp - need;

  r = sys_insert_pte (EXOS_CAP, (vpt[STAGE >> PGSHIFT] & PTE_FRAME)
		      | PTE_P | PTE_U | PTE_W, XINFO_ADDR, EXOS_CAP, child);
  sys_self_insert_pte (EXOS_CAP, 0, STAGE);
  return r < 0 ? -ENOMEM : 0;
toobig:
  sys_self_insert_pte (EXOS_CAP, 0, STAGE);
  return -E2BIG;
}

int
execve (const char *path, char *const argv[], char *const envp[])
{
  char abs[256];
  struct cffs_ref ref;
  struct cffs_inode ino;
  struct elf_ehdr eh;
  struct elf_phdr ph[16];
  struct utf utf;
  envid_t child;
  uint32_t sp;
  int r;

  if (exos_abspath (path, abs, sizeof (abs)) < 0)
    {
      errno = ENAMETOOLONG;
      return -1;
    }
  if ((r = cffs_namei (abs, &ref)) < 0 || (r = cffs_iget (&ref, &ino)) < 0)
    {
      errno = -r;
      return -1;
    }
  if ((ino.i_mode & S_IFMT) != S_IFREG)
    {
      errno = EACCES;
      return -1;
    }
  if (!(ino.i_mode & 0111))
    {
      errno = EACCES;
      return -1;
    }
  if (cffs_read (&ref, 0, &eh, sizeof (eh)) != sizeof (eh)
      || memcmp (eh.e_ident, "\177ELF", 4) != 0 || eh.e_ident[4] != 1
      || eh.e_machine != 3 || eh.e_type != 2 || eh.e_phnum > 16
      || eh.e_phentsize != sizeof (struct elf_phdr))
    {
      errno = ENOEXEC;
      return -1;
    }
  if (cffs_read (&ref, eh.e_phoff, ph, eh.e_phnum * sizeof (ph[0]))
      != (ssize_t) (eh.e_phnum * sizeof (ph[0])))
    {
      errno = ENOEXEC;
      return -1;
    }

  child = sys_env_alloc (EXOS_CAP);
  if (child < 0)
    {
      errno = EAGAIN;
      return -1;
    }
  for (int i = 0; i < eh.e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_memsz)
      if ((r = load_segment (child, &ref, &ph[i])) < 0)
	goto fail;
  if ((r = setup_stack (child, argv, envp, &sp)) < 0)
    goto fail;
  /* Shared regions (the file table, the network configuration...). */
  for (uintptr_t a = USHARED; a < USHARED_TOP; a += PGSIZE)
    {
      xpte_t pte;
      if (a % (2 * 1024 * 1024) == 0 && !exos_mapped_2m (a))
	{
	  a += 2 * 1024 * 1024 - PGSIZE;
	  continue;
	}
      pte = vpt[a >> PGSHIFT];
      if (!(pte & PTE_P) || !(pte & PTE_SHARE))
	continue;
      if ((r = dup_page (child, a, pte)) < 0)
	goto fail;
    }
  memset (&utf, 0, sizeof (utf));
  utf.utf_eip = eh.e_entry;
  utf.utf_esp = sp;
  if ((r = sys_env_set_tf (EXOS_CAP, child, &utf)) < 0)
    goto fail;

  /* Point of no return.  Close close-on-exec descriptors; the others
     are inherited by the new environment. */
  for (int fd = 0; fd < NOFILE; fd++)
    if (__fdtab.fd[fd] >= 0 && __fdtab.cloexec[fd])
      close (fd);
  cffs_sync ();
  /* The new environment gets its own time slice; ours are released
     when we exit, once it has taken over. */
  give_quantum (child);
  __proc->exec_env = child;
  sys_env_set_status (EXOS_CAP, child, ENV_RUNNABLE);
  /* Stay until the new environment has published the process (our
     pid) in its u-area, so that the process never disappears. */
  sys_yield (child);
  exos_sleep_until_mem (&proc_of_env (child)->state, WK_NE, PROC_FREE,
			5000);
  __proc->state = PROC_EXECED;
  sys_env_free (0, 0);
  for (;;)
    sys_yield (-1);

fail:
  sys_env_free (EXOS_CAP, child);
  errno = r == -E_NO_MEM || r == -ENOMEM ? ENOMEM : (r < 0 ? -r : EIO);
  if (errno > 200)
    errno = ENOEXEC;
  return -1;
}

int
execv (const char *path, char *const argv[])
{
  return execve (path, argv, environ);
}

int
execvp (const char *file, char *const argv[])
{
  char buf[256];
  const char *pathenv;

  if (strchr (file, '/'))
    return execve (file, argv, environ);
  pathenv = getenv ("PATH");
  if (pathenv == NULL)
    pathenv = "/bin";
  while (*pathenv)
    {
      size_t n = strcspn (pathenv, ":");
      if (n + strlen (file) + 2 < sizeof (buf))
	{
	  memcpy (buf, pathenv, n);
	  buf[n] = '/';
	  strcpy (buf + n + 1, file);
	  execve (buf, argv, environ);
	  if (errno != ENOENT)
	    return -1;
	}
      pathenv += n;
      if (*pathenv == ':')
	pathenv++;
    }
  errno = ENOENT;
  return -1;
}

/*
 * exit and wait.
 */
extern void (*__exos_net_exit) (void) __attribute__ ((weak));

void
_exit (int status)
{
  volatile struct exos_proc *pp;
  envid_t parent;

  fd_close_all ();
  if (&__exos_net_exit && __exos_net_exit)
    __exos_net_exit ();
  cffs_sync ();
  exos_crit_enter ();
  __proc->status = (status & 0xff) << 8;
  __proc->state = PROC_ZOMBIE;
  for (unsigned c = 0; c < sysinfo_page->si_ncpu; c++)
    for (int q = 0; q < NQUANTA; q++)
      if (sysinfo_page->si_qvec[c][q].q_env == __envid)
	sys_quantum_free (EXOS_CAP, q, c);
  parent = __proc->ppid ? exos_pid2env (__proc->ppid) : 0;
  pp = parent ? proc_of_env (parent) : NULL;
  if (parent == 0 || pp->state != PROC_RUNNING)
    {
      /* Orphan: nobody will reap us. */
      sys_env_free (0, 0);
    }
  sys_ipc_call (parent, IPC_CHILD, __proc->pid, 0, 0, NULL);
  sys_env_set_status (0, 0, ENV_NOT_RUNNABLE);
  for (;;)
    sys_yield (-1);
}

static void
exit_signal (int sig)
{
  fd_close_all ();
  exos_crit_enter ();
  __proc->status = sig & 0x7f;
  __proc->state = PROC_ZOMBIE;
  for (unsigned c = 0; c < sysinfo_page->si_ncpu; c++)
    for (int q = 0; q < NQUANTA; q++)
      if (sysinfo_page->si_qvec[c][q].q_env == __envid)
	sys_quantum_free (EXOS_CAP, q, c);
  envid_t parent = __proc->ppid ? exos_pid2env (__proc->ppid) : 0;
  if (parent == 0)
    sys_env_free (0, 0);
  sys_ipc_call (parent, IPC_CHILD, __proc->pid, 0, 0, NULL);
  sys_env_set_status (0, 0, ENV_NOT_RUNNABLE);
  for (;;)
    sys_yield (-1);
}

pid_t
waitpid (pid_t pid, int *status, int options)
{
  for (;;)
    {
      int nchild = 0;
      struct wk_term t[WK_MAXTERMS];
      int nt = 0;

      for (unsigned i = 1; i < NENV; i++)
	{
	  volatile struct envinfo *ei =
	    &((volatile struct envinfo *) UENVINFO)[i];
	  volatile struct exos_proc *p;
	  envid_t e = ei->e_id;

	  if (ei->e_status == ENV_FREE || ei->e_status == ENV_DYING)
	    continue;
	  p = proc_of_env (e);
	  if (p->magic != PROC_MAGIC || p->ppid != __proc->pid
	      || e == __envid)
	    continue;
	  if (pid > 0 && p->pid != pid)
	    continue;
	  if (pid < -1 && p->pgid != -pid)
	    continue;
	  if (p->state == PROC_ZOMBIE)
	    {
	      pid_t cp = p->pid;
	      if (status)
		*status = p->status;
	      child_del (cp);
	      sys_env_free (EXOS_CAP, e);
	      return cp;
	    }
	  if (p->state != PROC_RUNNING)
	    continue;
	  nchild++;
	  if (nt + 2 < WK_MAXTERMS - 3)
	    {
	      if (nt)
		t[nt++].wk_op = WK_OR;
	      memset (&t[nt], 0, sizeof (t[nt]));
	      t[nt].wk_op = WK_NE;
	      t[nt].wk_lkind = WK_MEM32;
	      t[nt].wk_lval = (uint32_t) & p->state;
	      t[nt].wk_rkind = WK_CONST;
	      t[nt].wk_rval = PROC_RUNNING;
	      nt++;
	    }
	}
      /* Children destroyed without becoming zombies (SIGKILL). */
      for (int i = 0; i < MAXCHILD; i++)
	{
	  pid_t cp = children[i];
	  int found = 0;

	  if (cp == 0 || (pid > 0 && cp != pid))
	    continue;
	  if (exos_pid2env (cp))
	    continue;		/* Seen above. */
	  /* Still starting (fork), or in the middle of an exec? */
	  if (envinfo_of (cp)->e_id == cp
	      && envinfo_of (cp)->e_status != ENV_FREE)
	    found = 1;
	  for (unsigned k = 1; k < NENV && !found; k++)
	    {
	      volatile struct envinfo *ei =
		&((volatile struct envinfo *) UENVINFO)[k];
	      if (ei->e_status != ENV_FREE
		  && proc_of_env (ei->e_id)->magic == PROC_MAGIC
		  && proc_of_env (ei->e_id)->pid == cp)
		found = 1;
	    }
	  if (found)
	    {
	      nchild++;
	      continue;
	    }
	  child_del (cp);
	  if (status)
	    *status = SIGKILL;
	  return cp;
	}
      if (nchild == 0)
	{
	  errno = ECHILD;
	  return -1;
	}
      if (options & WNOHANG)
	return 0;
      /* Sleep until a child changes state; recheck periodically for
         children replaced by exec. */
      if (nt)
	t[nt++].wk_op = WK_OR;
      memset (&t[nt], 0, sizeof (t[nt]));
      t[nt].wk_op = WK_GE;
      t[nt].wk_lkind = WK_TIME;
      t[nt].wk_rkind = WK_CONST;
      t[nt].wk_rval = (uint32_t) (exos_time_ns () / 1000000) + 100;
      nt++;
      exos_wkpred (t, nt);
      if (__proc->sigpending & ~sig_mask)
	{
	  exos_sig_deliver ();
	  errno = EINTR;
	  return -1;
	}
    }
}

pid_t
wait (int *status)
{
  return waitpid (-1, status, 0);
}

/*
 * Signals.
 */
static int
sig_default_ignore (int sig)
{
  return sig == SIGCHLD || sig == SIGCONT || sig == SIGSTOP
    || sig == SIGTSTP;
}

void
exos_sig_deliver (void)
{
  uint32_t pend;

  while ((pend = __proc->sigpending & ~sig_mask) != 0)
    {
      int sig = __builtin_ctz (pend);
      sighandler_t h;

      __atomic_and_fetch (&__proc->sigpending, ~(1u << sig), __ATOMIC_RELAXED);
      if (sig == SIGKILL)
	exit_signal (sig);
      h = sig_handlers[sig];
      if (h == SIG_IGN)
	continue;
      if (h == SIG_DFL)
	{
	  if (sig_default_ignore (sig))
	    continue;
	  exit_signal (sig);
	}
      sig_mask |= 1u << sig;
      h (sig);
      sig_mask &= ~(1u << sig);
    }
}

void
exos_fault_signal (int sig, struct utf *utf)
{
  if (sig_handlers[sig] == SIG_DFL || sig_handlers[sig] == SIG_IGN)
    {
      char buf[160];
      int n = snprintf (buf, sizeof (buf),
			"%s[%d]: %s at eip %08x (address %08x, err %x, "
			"pte %llx)\n", __proc->name, __proc->pid,
			sig == SIGSEGV ? "segmentation fault" :
			sig == SIGFPE ? "arithmetic exception" :
			sig == SIGILL ? "illegal instruction" : "fault",
			utf->utf_eip, utf->utf_va, utf->utf_err,
			utf->utf_va < UXOK_BASE ? exos_pte (utf->utf_va) : 0ULL);
      sys_cputs (buf, n);
      exit_signal (sig);
    }
  sig_handlers[sig] (sig);
}

sighandler_t
signal (int sig, sighandler_t h)
{
  sighandler_t old;

  if (sig <= 0 || sig >= NSIG || sig == SIGKILL)
    {
      errno = EINVAL;
      return SIG_ERR;
    }
  old = sig_handlers[sig];
  sig_handlers[sig] = h;
  return old;
}

int
sigaction (int sig, const struct sigaction *sa, struct sigaction *old)
{
  if (sig <= 0 || sig >= NSIG)
    {
      errno = EINVAL;
      return -1;
    }
  if (old)
    {
      memset (old, 0, sizeof (*old));
      old->sa_handler = sig_handlers[sig];
    }
  if (sa)
    {
      if (sig == SIGKILL)
	{
	  errno = EINVAL;
	  return -1;
	}
      sig_handlers[sig] = sa->sa_handler;
    }
  return 0;
}

int
sigprocmask (int how, const sigset_t *set, sigset_t *old)
{
  if (old)
    *old = sig_mask;
  if (set)
    {
      if (how == SIG_BLOCK)
	sig_mask |= *set;
      else if (how == SIG_UNBLOCK)
	sig_mask &= ~*set;
      else
	sig_mask = *set;
      sig_mask &= ~(1u << SIGKILL);
    }
  if (__proc->sigpending & ~sig_mask)
    exos_sig_deliver ();
  return 0;
}

int
sigemptyset (sigset_t *s)
{
  *s = 0;
  return 0;
}

int
sigfillset (sigset_t *s)
{
  *s = ~0u;
  return 0;
}

int
sigaddset (sigset_t *s, int sig)
{
  *s |= 1u << sig;
  return 0;
}

int
sigdelset (sigset_t *s, int sig)
{
  *s &= ~(1u << sig);
  return 0;
}

int
sigismember (const sigset_t *s, int sig)
{
  return (*s >> sig) & 1;
}

int
raise (int sig)
{
  if (sig <= 0 || sig >= NSIG)
    {
      errno = EINVAL;
      return -1;
    }
  __atomic_or_fetch (&__proc->sigpending, 1u << sig, __ATOMIC_RELAXED);
  exos_sig_deliver ();
  return 0;
}

static int
kill_one (envid_t e, int sig)
{
  int r;

  if (e == __envid)
    return raise (sig);
  if (sig == 0)
    return 0;
  r = exos_ipc_send (e, IPC_SIGNAL, sig, 0, 0, NULL);
  if (r == 0)
    return 0;
  if (sig == SIGKILL && sys_env_free (EXOS_CAP, e) == 0)
    return 0;
  if (r == -EPERM || r == -E_CAP_INSUFF)
    errno = EPERM;
  else
    errno = ESRCH;
  return -1;
}

int
kill (pid_t pid, int sig)
{
  if (sig < 0 || sig >= NSIG)
    {
      errno = EINVAL;
      return -1;
    }
  if (pid > 0)
    {
      envid_t e = exos_pid2env (pid);
      if (e == 0 || proc_of_env (e)->state != PROC_RUNNING)
	{
	  errno = ESRCH;
	  return -1;
	}
      return kill_one (e, sig);
    }
  /* Process group. */
  pid_t pg = pid == 0 ? __proc->pgid : (pid == -1 ? 0 : -pid);
  int n = 0, self = 0;
  for (unsigned i = 1; i < NENV; i++)
    {
      volatile struct envinfo *ei = &((volatile struct envinfo *) UENVINFO)[i];
      volatile struct exos_proc *p;
      if (ei->e_status == ENV_FREE)
	continue;
      p = proc_of_env (ei->e_id);
      if (p->magic != PROC_MAGIC || p->state != PROC_RUNNING)
	continue;
      if (pg && p->pgid != pg)
	continue;
      if (!pg && p->pid == 1)
	continue;
      if (ei->e_id == __envid)
	{
	  self = 1;
	  continue;
	}
      if (kill_one (ei->e_id, sig) == 0)
	n++;
    }
  if (self)
    {
      raise (sig);
      n++;
    }
  if (n == 0)
    {
      errno = ESRCH;
      return -1;
    }
  return 0;
}

unsigned int
alarm (unsigned int s)
{
  return 0;
}

int
pause (void)
{
  while (!(__proc->sigpending & ~sig_mask))
    exos_sleep_until_mem (&__proc->sigpending, WK_NE, 0, 1000);
  exos_sig_deliver ();
  errno = EINTR;
  return -1;
}
