/*
 * xoktests: tests of the Xok interface and of the ExOS services built on
 * it.  Prints one line per test and a summary; exits with the number
 * of failures.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <exos/exos.h>
#include <exos/fs.h>
#include <exos/fd.h>
#include <exos/net.h>
#include <xok/ipc.h>
#include <xok/xn.h>

static int npass, nfail;

#define CHECK(_name, _cond) do {					\
    if (_cond) { npass++; printf ("PASS %s\n", _name); }		\
    else { nfail++; printf ("FAIL %s (%s:%d)\n", _name, __FILE__, __LINE__); } \
  } while (0)

/* Run FN in a child; return its exit status. */
static int
in_child (int (*fn) (void))
{
  int st;
  pid_t p = fork ();
  if (p == 0)
    _exit (fn ());
  if (p < 0 || waitpid (p, &st, 0) != p)
    return -1;
  return WIFEXITED (st) ? WEXITSTATUS (st) : 128 + WTERMSIG (st);
}

/*
 * Capabilities.
 */
static int
user_caps (void)
{
  struct cap c;
  /* An unprivileged process cannot forge the root capability back. */
  if (setuid (1000) < 0)
    return 1;
  memset (&c, 0, sizeof (c));
  c.c_valid = 1;
  c.c_perm = CAP_ALL;
  c.c_len = 0;
  if (sys_cap_forge (EXOS_CAP, 5, &c) != -E_CAP_INSUFF)
    return 2;
  /* But it can derive a longer (weaker) name. */
  c.c_len = 4;
  c.c_name[0] = CAP_UID_TAG;
  c.c_name[1] = 1000 >> 8;
  c.c_name[2] = 1000 & 0xff;
  c.c_name[3] = 'x';
  c.c_perm = CAP_R;
  if (sys_cap_forge (EXOS_CAP, 5, &c) != 0)
    return 3;
  /* It cannot power off the machine. */
  if (sys_reboot (EXOS_CAP, 1) != -E_CAP_INSUFF)
    return 4;
  /* Nor read the disk raw. */
  if (sys_disk_request (EXOS_CAP, 0, 0, 1, 0, 1, NULL) != -E_CAP_INSUFF)
    return 5;
  return 0;
}

static void
test_caps (void)
{
  CHECK ("caps: user cannot regain root or use privileged calls",
	 in_child (user_caps) == 0);
}

/*
 * Physical memory and page tables.
 */
static void
test_memory (void)
{
  uintptr_t va = 0x70000000;
  int r = exos_page_alloc (va, PTE_W);
  xpte_t pte = exos_pte (va);
  ppn_t ppn = PTE_PPN (pte);

  CHECK ("mem: page allocation", r == 0 && (pte & PTE_P) && (pte & PTE_W));
  CHECK ("mem: exported ppage info",
	 ppages_info[ppn].pp_state == PP_USER
	 && ppages_info[ppn].pp_refcnt == 1
	 && ppages_info[ppn].pp_owner == __envid);
  /* Map the same physical page twice. */
  r = sys_self_insert_pte (EXOS_CAP, (pte & PTE_FRAME) | PTE_P | PTE_U,
			   va + PGSIZE);
  *(volatile int *) va = 1234;
  CHECK ("mem: sharing a physical page",
	 r == 0 && *(volatile int *) (va + PGSIZE) == 1234
	 && ppages_info[ppn].pp_refcnt == 2);
  /* Software bits and non-present entries belong to the library. */
  r = sys_self_insert_pte (EXOS_CAP, 0x12345000ULL | PTE_AVAIL1, va + PGSIZE);
  CHECK ("mem: non-present PTE values kept",
	 r == 0 && exos_pte (va + PGSIZE) == (0x12345000ULL | PTE_AVAIL1));
  /* Write-protect, then the fault upcall. */
  r = sys_mod_pte_range (EXOS_CAP, 0, 0, va, 1, 0, PTE_W);
  if (r != 0 || (exos_pte (va) & PTE_W))
    printf ("  mod_pte_range: r=%d pte=%llx\n", r, exos_pte (va));
  CHECK ("mem: mod_pte_range", r == 0 && !(exos_pte (va) & PTE_W));
  exos_page_unmap (va);
  exos_page_unmap (va + PGSIZE);
  CHECK ("mem: page freed when unmapped",
	 ppages_info[ppn].pp_state == PP_FREE);
  /* Requesting a specific physical page. */
  ppn_t want = 0;
  for (ppn_t p = sysinfo_page->si_nppages - 1; p > 256; p--)
    if (ppages_info[p].pp_state == PP_FREE)
      {
	want = p;
	break;
      }
  r = sys_self_insert_pte (EXOS_CAP, PPN2PTE (want) | PTE_P | PTE_W | PTE_U,
			   va);
  CHECK ("mem: allocation of a named physical page",
	 r == 0 && PTE_PPN (exos_pte (va)) == want);
  exos_page_unmap (va);
}

static int
steal_page (void)
{
  /* A page of another user is protected by its guard capability. */
  ppn_t victim = 0;
  if (setuid (1001) < 0)
    return 1;
  for (ppn_t p = 1; p < sysinfo_page->si_nppages; p++)
    if (ppages_info[p].pp_state == PP_USER
	&& ppages_info[p].pp_owner == 0x101)	/* init's */
      {
	victim = p;
	break;
      }
  if (victim == 0)
    return 2;
  int r = sys_self_insert_pte (EXOS_CAP, PPN2PTE (victim) | PTE_P | PTE_U,
			       0x70000000);
  return r == -E_CAP_INSUFF ? 0 : 3;
}

static void
test_protection (void)
{
  CHECK ("mem: cannot map another user's page", in_child (steal_page) == 0);
}

/*
 * Sharing a page with another user, by granting a capability.
 */
static uintptr_t shared_va = 0x71000000;
static ppn_t shared_ppn;

static int
map_shared (void)
{
  ppn_t ppn = shared_ppn;
  /* The page was not inherited (no-fork): map it by name. */
  if (setuid (1003) < 0)
    return 1;
  if (sys_self_insert_pte (EXOS_CAP, PPN2PTE (ppn) | PTE_P | PTE_U,
			   shared_va) != -E_CAP_INSUFF)
    return 2;
  /* Wait for the parent to grant us the capability in slot 6. */
  for (int i = 0; i < 200 && !envinfo_of (__envid)->e_caps[6].c_valid; i++)
    usleep (10000);
  if (sys_self_insert_pte (6, PPN2PTE (ppn) | PTE_P | PTE_U, shared_va) != 0)
    return 3;
  return *(volatile int *) shared_va == 0x5ade ? 0 : 4;
}

static void
test_grant (void)
{
  struct cap s;
  int st;

  memset (&s, 0, sizeof (s));
  s.c_valid = 1;
  s.c_perm = CAP_R | CAP_GRANT;
  s.c_len = 1;
  s.c_name[0] = 'S';
  exos_page_alloc (shared_va, PTE_W | PTE_NOFORK);
  *(volatile int *) shared_va = 0x5ade;
  shared_ppn = PTE_PPN (exos_pte (shared_va));
  int r1 = sys_cap_forge (CAP_ROOT, 6, &s);
  int r2 = sys_ppage_acl (EXOS_CAP, PTE_PPN (exos_pte (shared_va)), &s);
  pid_t p = fork ();
  if (p == 0)
    _exit (map_shared ());
  usleep (300000);
  int r3 = sys_cap_grant (CAP_ROOT, exos_pid2env (p), 6, 6);
  waitpid (p, &st, 0);
  if (r1 || r2 || r3 || !WIFEXITED (st) || WEXITSTATUS (st))
    printf ("  grant: forge %d acl %d grant %d child %d\n", r1, r2, r3,
	    WEXITSTATUS (st));
  CHECK ("caps: page shared with another user by granting a capability",
	 r1 == 0 && r2 == 0 && r3 == 0 && WIFEXITED (st)
	 && WEXITSTATUS (st) == 0);
  exos_page_unmap (shared_va);
  sys_cap_clear (6);
}

/*
 * Accessed/dirty bits, INT redirection, raw disk.
 */
static volatile int int_seen;

static void
on_int80 (struct utf *utf)
{
  int_seen = utf->utf_eax + 1;
}

static void
test_misc (void)
{
  uintptr_t va = 0x72000000;
  exos_page_alloc (va, PTE_W);
  sys_vpt_refresh (va, 1);
  int clean = !(exos_pte (va) & PTE_D);
  *(volatile int *) va = 1;
  sys_vpt_refresh (va, 1);
  CHECK ("mem: hardware dirty bit exported", clean
	 && (exos_pte (va) & PTE_D) && (exos_pte (va) & PTE_A));
  exos_page_unmap (va);

  exos_set_int_handler (0x80, on_int80);
  asm volatile ("int $0x80"::"a" (41));
  CHECK ("events: INT instruction redirected to the application",
	 int_seen == 42);
  exos_set_int_handler (0x80, NULL);

  /* Raw disk access with the root capability: XN's superblock. */
  static volatile uint32_t done;
  va = 0x73000000;
  exos_page_alloc (va, PTE_W);
  done = 0;
  exos_touch ((void *) &done, 4, 1);
  int r = sys_disk_request (CAP_ROOT, 0, 0, 8, 0, PTE_PPN (exos_pte (va)),
			    &done);
  if (r == 0)
    exos_sleep_until_mem (&done, WK_NE, 0, 5000);
  CHECK ("disk: raw read with the root capability",
	 r == 0 && done == 1 && *(volatile uint32_t *) va == XN_MAGIC);
  exos_page_unmap (va);
}

/*
 * SMP: CPU-bound processes on every CPU.
 */
static int
spin_work (void)
{
  volatile uint32_t h = 0;
  uint64_t end = exos_time_ns () + 300000000ULL;
  int cpus = 0;
  while (exos_time_ns () < end)
    {
      h = h * 31 + 7;
      cpus |= 1 << envinfo_of (__envid)->e_cpu;
    }
  return cpus;
}

static void
test_smp (void)
{
  unsigned ncpu = sysinfo_page->si_ncpu;
  pid_t p[8];
  int seen = 0, ok = 1, st;
  uint64_t t0 = exos_time_ns ();

  for (unsigned i = 0; i < ncpu * 2 && i < 8; i++)
    if ((p[i] = fork ()) == 0)
      _exit (spin_work ());
  for (unsigned i = 0; i < ncpu * 2 && i < 8; i++)
    {
      if (waitpid (p[i], &st, 0) != p[i] || !WIFEXITED (st))
	ok = 0;
      else
	seen |= WEXITSTATUS (st);
    }
  uint64_t ms = (exos_time_ns () - t0) / 1000000;
  int used = __builtin_popcount (seen);
  printf ("  smp: %u CPUs, %d used, %d processes in %llu ms\n", ncpu, used,
	  ncpu * 2 > 8 ? 8 : ncpu * 2, ms);
  CHECK ("smp: processes run on every CPU", ok && used == (int) ncpu);

  /* Revoke the CPU of a running process. */
  pid_t q = fork ();
  if (q == 0)
    {
      volatile int x = 0;
      for (;;)
	x++;
    }
  usleep (200000);
  envid_t qe = exos_pid2env (q);
  unsigned before = uenv_of (qe)->u_epilogue_count;
  int r = 0;
  for (int i = 0; i < 20; i++)
    {
      int c = envinfo_of (qe)->e_cpu;
      if (c >= 0)
	r |= sys_cpu_revoke (EXOS_CAP, c, qe);
      usleep (5000);
    }
  CHECK ("sched: cpu_revoke", r == 0
	 && uenv_of (qe)->u_epilogue_count > before);
  kill (q, SIGKILL);
  waitpid (q, &st, 0);
}

/*
 * Copy-on-write fork.
 */
static volatile int cow_var = 1;

static int
cow_child (void)
{
  cow_var = 2;
  return cow_var == 2 ? 0 : 1;
}

static void
test_fork (void)
{
  int r = in_child (cow_child);
  CHECK ("fork: copy-on-write isolation", r == 0 && cow_var == 1);
  int st;
  pid_t p = fork ();
  if (p == 0)
    _exit (42);
  CHECK ("fork: exit status", waitpid (p, &st, 0) == p
	 && WIFEXITED (st) && WEXITSTATUS (st) == 42);
  CHECK ("wait: no children", wait (&st) == -1 && errno == ECHILD);
}

/*
 * Signals (delivered with Xok IPC).
 */
static volatile int got_sig;

static void
on_usr1 (int s)
{
  got_sig = s;
}

static void
test_signals (void)
{
  int st;
  signal (SIGUSR1, on_usr1);
  raise (SIGUSR1);
  CHECK ("signal: handler", got_sig == SIGUSR1);

  pid_t p = fork ();
  if (p == 0)
    {
      for (;;)
	pause ();
    }
  usleep (100000);
  CHECK ("signal: kill delivers SIGTERM",
	 kill (p, SIGTERM) == 0 && waitpid (p, &st, 0) == p
	 && WIFSIGNALED (st) && WTERMSIG (st) == SIGTERM);

  p = fork ();
  if (p == 0)
    {
      volatile int x = 0;
      for (;;)
	x++;			/* Busy: never yields voluntarily. */
    }
  usleep (100000);
  CHECK ("signal: SIGKILL a busy process",
	 kill (p, SIGKILL) == 0 && waitpid (p, &st, 0) == p
	 && WTERMSIG (st) == SIGKILL);
}

/*
 * IPC: protected control transfer and message rings.
 */
static int
ipc_echo (envid_t from, uint32_t a, uint32_t b, uint32_t c, uint32_t * r1)
{
  *r1 = a + b + c;
  return 7;
}

static void
test_ipc (void)
{
  exos_ipc_register (9, ipc_echo);
  pid_t p = fork ();
  if (p == 0)
    {
      /* Child: call the parent. */
      envid_t parent = exos_pid2env (getppid ());
      uint32_t r1 = 0;
      int r = exos_ipc_send (parent, 9, 1, 2, 3, &r1);
      _exit (r == 7 && r1 == 6 ? 0 : 1);
    }
  int st;
  waitpid (p, &st, 0);
  CHECK ("ipc: protected control transfer",
	 WIFEXITED (st) && WEXITSTATUS (st) == 0);

  static struct msgring_ent ring[8] __attribute__ ((aligned (64)));
  memset (ring, 0, sizeof (ring));
  exos_touch (ring, sizeof (ring), 1);
  int r = sys_msgring_setring (ring, 8);
  p = fork ();
  if (p == 0)
    {
      envid_t parent = exos_pid2env (getppid ());
      int ok = sys_ipc_sendmsg (parent, "hello ring", 11);
      _exit (ok == 0 ? 0 : 1);
    }
  waitpid (p, &st, 0);
  exos_sleep_until_mem (&ring[0].m_flag, WK_NE, 0, 2000);
  CHECK ("ipc: message ring",
	 r == 0 && ring[0].m_flag && !strcmp ((char *) ring[0].m_buf,
					       "hello ring"));
  sys_msgring_delring ();
}

/*
 * Software regions and wakeup predicates.
 */
static void
test_sreg_wk (void)
{
  int id = sys_sreg_create (EXOS_CAP, 64);
  char buf[16];
  int r1 = sys_sreg_write (EXOS_CAP, id, 8, "region", 7);
  memset (buf, 0, sizeof (buf));
  int r2 = sys_sreg_read (EXOS_CAP, id, 8, buf, 7);
  CHECK ("sreg: read/write", id >= 0 && r1 == 0 && r2 == 0
	 && !strcmp (buf, "region"));
  CHECK ("sreg: bounds", sys_sreg_read (EXOS_CAP, id, 60, buf, 8)
	 == -E_RANGE);
  sys_sreg_destroy (EXOS_CAP, id);

  uint64_t t0 = exos_time_ns ();
  struct wk_term t;
  memset (&t, 0, sizeof (t));
  t.wk_op = WK_GE;
  t.wk_lkind = WK_TIME;
  t.wk_rkind = WK_CONST;
  t.wk_rval = t0 / 1000000 + 100;
  while (exos_time_ns () / 1000000 < t0 / 1000000 + 100)
    sys_wkpred (&t, 1);
  uint64_t dt = (exos_time_ns () - t0) / 1000000;
  CHECK ("wkpred: timed sleep", dt >= 100 && dt < 1000);
}

/*
 * Scheduling.
 */
static void
test_sched (void)
{
  volatile struct Uenv *u = __uenv;
  unsigned ep = u->u_epilogue_count, pr = u->u_prologue_count;
  uint64_t end = exos_time_ns () + 100000000ULL;
  volatile unsigned long x = 0;
  while (exos_time_ns () < end)
    x++;
  CHECK ("sched: prologue/epilogue upcalls",
	 u->u_epilogue_count > ep && u->u_prologue_count > pr);
  int q = sys_quantum_alloc (EXOS_CAP, -1, 0, __envid);
  CHECK ("sched: quantum allocation", q >= 0
	 && sysinfo_page->si_qvec[0][q].q_env == __envid);
  CHECK ("sched: quantum free", sys_quantum_free (EXOS_CAP, q, 0) == 0
	 && sysinfo_page->si_qvec[0][q].q_env == 0);
}

/*
 * Visible revocation.
 */
static int
bc_window_mappings (void)
{
  int n = 0;
  for (uintptr_t va = BCWIN; va < BCWIN_TOP; va += PGSIZE)
    {
      if (va % 0x200000 == 0 && !exos_mapped_2m (va))
	{
	  va += 0x200000 - PGSIZE;
	  continue;
	}
      if (exos_mapped (va))
	n++;
    }
  return n;
}

static int
read_file (const char *p)
{
  char buf[4096];
  int fd = open (p, O_RDONLY), n, t = 0;
  while (fd >= 0 && (n = read (fd, buf, sizeof (buf))) > 0)
    t += n;
  close (fd);
  return t;
}

static int
refuse_revocation (void)
{
  int fds[2];
  /* No revocation handler: the kernel will repossess. */
  __uenv->u_entrevoke = 0;
  read_file ("/bin/sh");
  int before = bc_window_mappings ();
  if (before < 16)
    return 2;
  usleep (1500000);
  /* The kernel took 16 mappings, all in the declared range, and our
     program text (also buffer cache pages) is untouched. */
  return __uenv->u_nrepossessed == 16
    && bc_window_mappings () == before - 16 ? 0 : 1;
  (void) fds;
}

static void
test_revocation (void)
{
  read_file ("/bin/ls");
  int before = bc_window_mappings ();
  int r = sys_debug (DBG_REVOKE, __envid);
  for (int i = 0; i < 20 && __uenv->u_revoke_npages; i++)
    sys_yield (-1);
  CHECK ("revoke: library releases its buffer cache mappings",
	 before > 0 && r == 0 && __uenv->u_revoke_npages == 0
	 && bc_window_mappings () == 0);
  pid_t p = fork ();
  if (p == 0)
    _exit (refuse_revocation ());
  usleep (500000);
  r = sys_debug (DBG_REVOKE, exos_pid2env (p));
  int st;
  waitpid (p, &st, 0);
  CHECK ("revoke: abort protocol repossesses mappings",
	 r == 0 && WIFEXITED (st) && WEXITSTATUS (st) == 0);
}

/*
 * Pipes.
 */
static void
test_pipes (void)
{
  int fds[2], st;
  char buf[64];
  CHECK ("pipe: create", pipe (fds) == 0);
  pid_t p = fork ();
  if (p == 0)
    {
      close (fds[0]);
      for (int i = 0; i < 1000; i++)
	write (fds[1], "0123456789", 10);
      _exit (0);
    }
  close (fds[1]);
  long total = 0;
  ssize_t n;
  while ((n = read (fds[0], buf, sizeof (buf))) > 0)
    total += n;
  close (fds[0]);
  waitpid (p, &st, 0);
  CHECK ("pipe: 10000 bytes and EOF", total == 10000);
}

/*
 * File system.
 */
static void
test_fs (void)
{
  int fd = open ("/tmp/t1", O_CREAT | O_RDWR | O_TRUNC, 0644);
  char buf[100];
  CHECK ("fs: create", fd >= 0);
  CHECK ("fs: write", write (fd, "hello, xn", 9) == 9);
  CHECK ("fs: seek/read", lseek (fd, 7, SEEK_SET) == 7
	 && read (fd, buf, 10) == 2 && !memcmp (buf, "xn", 2));
  close (fd);
  struct stat st;
  CHECK ("fs: stat", stat ("/tmp/t1", &st) == 0 && st.st_size == 9);
  CHECK ("fs: mkdir", mkdir ("/tmp/dir", 0755) == 0);
  CHECK ("fs: rename across directories",
	 rename ("/tmp/t1", "/tmp/dir/t2") == 0
	 && stat ("/tmp/t1", &st) < 0 && stat ("/tmp/dir/t2", &st) == 0);
  CHECK ("fs: rmdir non-empty fails", rmdir ("/tmp/dir") < 0
	 && errno == ENOTEMPTY);
  CHECK ("fs: unlink", unlink ("/tmp/dir/t2") == 0);
  CHECK ("fs: rmdir", rmdir ("/tmp/dir") == 0);

  /* Many entries: the directory grows. */
  int ok = 1;
  mkdir ("/tmp/many", 0755);
  for (int i = 0; i < 40 && ok; i++)
    {
      char name[64];
      snprintf (name, sizeof (name), "/tmp/many/file%d", i);
      int f = open (name, O_CREAT | O_WRONLY, 0644);
      ok = f >= 0 && write (f, name, strlen (name)) > 0;
      close (f);
    }
  int count = 0;
  DIR *d = opendir ("/tmp/many");
  while (d && readdir (d))
    count++;
  if (d)
    closedir (d);
  CHECK ("fs: 40 files in a directory", ok && count == 40);
  for (int i = 0; i < 40; i++)
    {
      char name[64];
      snprintf (name, sizeof (name), "/tmp/many/file%d", i);
      unlink (name);
    }
  CHECK ("fs: cleanup", rmdir ("/tmp/many") == 0);

  /* A large file: direct, indirect and double indirect blocks. */
  unsigned free0 = 0, free1 = 0;
  fd = open ("/tmp/big", O_CREAT | O_RDWR | O_TRUNC, 0644);
  static char blk[4096];
  ok = fd >= 0;
  for (int i = 0; i < 1100 && ok; i++)	/* 4.4 MB */
    {
      memset (blk, i & 0xff, sizeof (blk));
      ok = write (fd, blk, sizeof (blk)) == sizeof (blk);
    }
  CHECK ("fs: write 4.4MB (double indirect)", ok);
  ok = lseek (fd, 1099 * 4096L, SEEK_SET) == 1099 * 4096L
    && read (fd, blk, 4096) == 4096 && blk[0] == (char) (1099 & 0xff)
    && lseek (fd, 13 * 4096L, SEEK_SET) == 13 * 4096L
    && read (fd, blk, 4096) == 4096 && blk[100] == 13;
  CHECK ("fs: read back large file", ok);
  close (fd);
  for (unsigned b = 0; b < xn_cat_super (0)->s_nblocks; b++)
    if ((xn_freemap (0)[b / 32] >> (b % 32)) & 1)
      free0++;
  CHECK ("fs: truncate", truncate ("/tmp/big", 5000) == 0
	 && stat ("/tmp/big", &st) == 0 && st.st_size == 5000);
  unlink ("/tmp/big");
  sync ();
  for (unsigned b = 0; b < xn_cat_super (0)->s_nblocks; b++)
    if ((xn_freemap (0)[b / 32] >> (b % 32)) & 1)
      free1++;
  CHECK ("fs: blocks returned to XN free map after sync", free1 > free0 + 1000);
}

static int
user_fs (void)
{
  if (setuid (1000) < 0)
    return 1;
  /* Root's directory: cannot create files there. */
  if (open ("/bin/evil", O_CREAT | O_WRONLY, 0644) >= 0)
    return 2;
  if (errno != EACCES)
    return 3;
  /* Root's file: cannot write it. */
  if (open ("/etc/motd", O_WRONLY) >= 0)
    {
      int fd = open ("/etc/motd", O_WRONLY);
      if (write (fd, "x", 1) == 1)
	return 4;
    }
  /* /tmp is world-writable. */
  int fd = open ("/tmp/userfile", O_CREAT | O_WRONLY, 0600);
  if (fd < 0 || write (fd, "mine", 4) != 4)
    return 5;
  close (fd);
  return 0;
}

static int
other_user_fs (void)
{
  if (setuid (1001) < 0)
    return 1;
  int fd = open ("/tmp/userfile", O_RDONLY);
  char b[8];
  /* Mode 0600 of uid 1000: no access. */
  if (fd >= 0 && read (fd, b, 4) == 4)
    return 2;
  return 0;
}

static void
test_fs_protection (void)
{
  CHECK ("fs: XN acl-uf enforces UNIX permissions", in_child (user_fs) == 0);
  CHECK ("fs: other users cannot read a 0600 file",
	 in_child (other_user_fs) == 0);
  unlink ("/tmp/userfile");
}

/*
 * XN directly: UDF verification of metadata updates.
 */
static void
test_xn (void)
{
  struct cffs_ref r;
  struct xn_op op;
  uint32_t bogus;
  int idx;

  int fd = open ("/tmp/xn", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  write (fd, "data", 4);
  close (fd);
  CHECK ("xn: namei", cffs_namei ("/tmp/xn", &r) == 0);
  idx = sys_xn_lookup (r.dev, r.blk);
  CHECK ("xn: directory block in the registry", idx >= 0
	 && (xn_registry[idx].bc_state & BC_META));

  /* Claim a block we do not own by writing a pointer to it. */
  bogus = xn_cat_super (r.dev)->s_data_start;	/* The superblock! */
  memset (&op, 0, sizeof (op));
  op.o_nmods = 1;
  op.o_mods[0].m_off = r.slot * CFFS_DIRENT_SIZE + CFFS_DIRENT_INODE
    + CFFS_I_DIRECT + 4;
  op.o_mods[0].m_len = 4;
  op.o_mods[0].m_data = (uint32_t) & bogus;
  CHECK ("xn: owns-udf rejects a forged block pointer",
	 sys_xn_modify (EXOS_CAP, r.dev, r.blk, &op) == -E_BOGUS_UPDATE);
  int rr = sys_xn_insert_pte (EXOS_CAP, r.dev, r.blk, 0x70000000, 1, 0, 0);
  if (rr != -E_ACCESS)
    printf ("  xn_insert_pte: %d\n", rr);
  CHECK ("xn: metadata cannot be mapped writable", rr == -E_ACCESS);
  /* Allocating a block that is not free. */
  memset (&op, 0, sizeof (op));
  op.o_child = bogus;
  op.o_nchild = 1;
  op.o_ctype = sys_xn_type_lookup (r.dev, "cffs-data");
  op.o_nmods = 1;
  op.o_mods[0].m_off = r.slot * CFFS_DIRENT_SIZE + CFFS_DIRENT_INODE
    + CFFS_I_DIRECT + 4;
  op.o_mods[0].m_len = 4;
  op.o_mods[0].m_data = (uint32_t) & bogus;
  CHECK ("xn: allocation of a used block rejected",
	 sys_xn_alloc (EXOS_CAP, r.dev, r.blk, &op) == -E_NOT_FREE);
  unlink ("/tmp/xn");
}

/*
 * Network: packet filter protection.
 */
static int
dpf_conflict (void)
{
  struct ring rg;
  struct dpf_atom a[8];
  int n;
  if (setuid (1002) < 0)
    return 1;
  if (ring_create (&rg, 4) < 0)
    return 2;
  /* netd (root) owns ICMP to our address: a user cannot capture it. */
  n = dpf_build_ip (a, 1, netcfg->ip, 0, 0, 0);
  return sys_dpf_insert (EXOS_CAP, a, n, rg.id) == -E_CONFLICT ? 0 : 3;
}

static void
test_net (void)
{
  if (!netcfg->configured)
    {
      printf ("SKIP net: not configured\n");
      return;
    }
  CHECK ("net: DPF protects another user's packets",
	 in_child (dpf_conflict) == 0);
}

int
main (int argc, char **argv)
{
  test_caps ();
  test_memory ();
  test_grant ();
  test_misc ();
  test_smp ();
  test_protection ();
  test_fork ();
  test_signals ();
  test_ipc ();
  test_sreg_wk ();
  test_sched ();
  test_revocation ();
  test_pipes ();
  test_fs ();
  test_fs_protection ();
  test_xn ();
  test_net ();
  printf ("xoktests: %d passed, %d failed\n", npass, nfail);
  return nfail;
}
