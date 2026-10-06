/*
 * Xok CPU multiplexing.
 *
 * Each CPU is represented as a linear vector of time slices (the
 * quantum vector).  Environments allocate slices explicitly, by
 * position: position encodes an ordering and an approximate upper
 * bound on when a slice will run, so that applications can trade
 * latency for throughput.  The kernel cycles round-robin through the
 * vector.
 *
 * The beginning and the end of each slice are notified to the
 * environment with upcalls: the prologue (start of a slice) and the
 * epilogue (end of the slice, the environment must save its state and
 * yield within a grace period).  While an environment is in a robust
 * critical section (u_in_critical) the epilogue is deferred and the
 * environment is told it was interrupted.  An environment that
 * exceeds the grace period is preempted by the kernel and forfeits a
 * subsequent slice.
 *
 * sys_yield donates the rest of the slice to a named environment
 * (directed yield) or ends it.
 *
 * Timers: NUX exposes a single platform alarm.  Each CPU records its
 * next deadline; the alarm is programmed with the earliest one, and
 * the CPU receiving it forwards expired deadlines to the other CPUs
 * with IPIs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"

struct pcpu pcpus[XOK_MAXCPU];
static struct cap qcap[XOK_MAXCPU][NQUANTA];
static uint64_t programmed = UINT64_MAX;
static unsigned ncpus = 1;

/* Per-CPU scheduling requests, set while handling the current entry. */
static struct env *yield_to[XOK_MAXCPU];
static bool next_slot[XOK_MAXCPU];

#define QVEC(_c, _q) (sysinfo->si_qvec[(_c)][(_q)])

uint64_t
ktime (void)
{
  return timer_gettime ();
}

void
sched_init (void)
{
  ncpus = cpu_num ();
  if (ncpus > XOK_MAXCPU)
    ncpus = XOK_MAXCPU;
  sysinfo->si_ncpu = ncpus;
  sysinfo->si_nquanta = NQUANTA;
  sysinfo->si_tick_nsec = TICK_NSEC;
}

void
sched_init_cpu (void)
{
  unsigned id = cpu_id ();
  struct pcpu *pc;

  KASSERT (id < XOK_MAXCPU);
  pc = pcpus + id;
  memset (pc, 0, sizeof (*pc));
  pc->id = id;
  cpu_setdata (pc);
  fpu_init_cpu ();
}

/*
 * Program the platform alarm for the earliest CPU deadline.
 */
static void
timer_update (void)
{
  uint64_t min = UINT64_MAX, now, delta;

  for (unsigned c = 0; c < ncpus; c++)
    if (pcpus[c].next_deadline && pcpus[c].next_deadline < min)
      min = pcpus[c].next_deadline;
  if (min == UINT64_MAX)
    return;

  now = ktime ();
  if (min < programmed || programmed <= now)
    {
      delta = min > now ? min - now : 10000;
      if (delta < 10000)
	delta = 10000;
      if (delta > 1000000000ULL)
	delta = 1000000000ULL;
      timer_alarm ((uint32_t) delta);
      programmed = now + delta;
    }
}

/*
 * Called on the CPU receiving the platform alarm.
 */
void
sched_timer (void)
{
  uint64_t now = ktime ();
  unsigned me = cpu_id ();

  programmed = UINT64_MAX;
  revoke_timer ();
  for (unsigned c = 0; c < ncpus; c++)
    {
      if (c == me)
	continue;
      if (pcpus[c].next_deadline && pcpus[c].next_deadline <= now)
	{
	  __atomic_or_fetch (&pcpus[c].ipi_pending, IPI_TICK,
			     __ATOMIC_RELAXED);
	  cpu_ipi (c);
	}
    }
}

/*
 * Wake idle CPUs so that they re-evaluate wakeup predicates.
 */
void
sched_kick_idle (void)
{
  unsigned me = cpu_id ();

  for (unsigned c = 0; c < ncpus; c++)
    if (c != me && pcpus[c].cur == NULL)
      {
	__atomic_or_fetch (&pcpus[c].ipi_pending, IPI_RESCHED,
			   __ATOMIC_RELAXED);
	cpu_ipi (c);
      }
}

void
sched_save (struct env *e, uctxt_t * u)
{
  if (u != UCTXT_IDLE && e != NULL)
    memcpy (&e->tf, u, sizeof (uctxt_t));
}

static void
env_leave (struct pcpu *pc)
{
  struct env *e = pc->cur;

  if (e == NULL)
    return;
  fpu_switch_out (e);
  e->cpu = -1;
  e->info->e_cpu = -1;
  pc->cur = NULL;
  pc->grace_end = 0;
  cpu_umap_exit ();
  sysinfo->si_cpu[pc->id].c_env = 0;
}

static void
env_enter (struct pcpu *pc, struct env *e)
{
  KASSERT (e->cpu == -1);
  pc->cur = e;
  e->cpu = pc->id;
  e->info->e_cpu = pc->id;
  cpu_umap_enter (&e->umap);
  fpu_switch_in (e);
  sysinfo->si_cpu[pc->id].c_env = e->id;
}

void
sched_switch_away (void)
{
  env_leave (mycpu ());
}

static bool
schedulable (struct env *e)
{
  if (e->cpu != -1)
    return false;
  if (e->status == ENV_WAITING && wk_eval (e))
    {
      wk_free (e);
      e->status = ENV_RUNNABLE;
      e->info->e_status = ENV_RUNNABLE;
    }
  return e->status == ENV_RUNNABLE;
}

static struct env *
pick (struct pcpu *pc)
{
  for (unsigned i = 1; i <= NQUANTA; i++)
    {
      unsigned q = (pc->q + i) % NQUANTA;
      envid_t id = QVEC (pc->id, q).q_env;
      struct env *e;

      if (id == 0)
	continue;
      e = env_lookup (id);
      if (e == NULL)
	continue;
      if (!schedulable (e))
	continue;
      if (e->penalty)
	{
	  e->penalty--;
	  continue;
	}
      pc->q = q;
      return e;
    }
  return NULL;
}

static void
kill_env (struct env *e, const char *why)
{
  kprintf ("xok: env %x killed: %s\n", e->id, why);
  env_destroy (e);
}

/*
 * Decide what runs next on this CPU and return its context.  Called
 * at the end of every kernel entry.
 */
uctxt_t *
sched_return (void)
{
  struct pcpu *pc = mycpu ();
  struct env *e;
  uint64_t now;

again:
  now = ktime ();
  sysinfo->si_nsec = now;
  sysinfo->si_ticks = now / TICK_NSEC;
  __atomic_and_fetch (&pc->ipi_pending, 0, __ATOMIC_RELAXED);
  e = pc->cur;

  if (e != NULL)
    {
      if (e->status == ENV_DYING)
	{
	  env_leave (pc);
	  env_reap (e);
	  e = NULL;
	}
      else if (e->status != ENV_RUNNABLE)
	{
	  env_leave (pc);
	  e = NULL;
	}
    }

  if (yield_to[pc->id] != NULL)
    {
      struct env *t = yield_to[pc->id];

      yield_to[pc->id] = NULL;
      if (e != NULL)
	{
	  env_leave (pc);
	  e = NULL;
	}
      if (t->status != ENV_FREE && schedulable (t))
	{
	  env_enter (pc, t);
	  e = t;
	  /* A directed yield starts a (partial) slice for the target;
	     an IPC reply simply resumes the caller. */
	  e->need_prologue = !(e->ipc_pending || e->ipc_return);
	  e->ipc_return = false;
	}
    }

  if (next_slot[pc->id])
    {
      next_slot[pc->id] = false;
      if (e != NULL)
	{
	  env_leave (pc);
	  e = NULL;
	}
    }

  if (e != NULL && pc->grace_end && now >= pc->grace_end)
    {
      /* Grace period exceeded: preempt, and forfeit a slice. */
      e->penalty++;
      e->u->u_excess_count++;
      e->in_epilogue = false;
      e->u->u_in_epilogue = 0;
      env_leave (pc);
      e = NULL;
    }
  else if (e != NULL && now >= pc->slice_end && !pc->grace_end)
    {
      if (e->u->u_in_critical)
	{
	  e->interrupted = true;
	  e->u->u_interrupted = 1;
	  pc->grace_end = now + GRACE_NSEC;
	}
      else if (e->u->u_entepilogue && !e->in_epilogue)
	{
	  if (upcall_push (e, UPC_EPILOGUE, 0, 0, 0, NULL) < 0)
	    {
	      kill_env (e, "cannot deliver epilogue");
	      goto again;
	    }
	  e->in_epilogue = true;
	  e->u->u_in_epilogue = 1;
	  e->u->u_epilogue_count++;
	  pc->grace_end = now + GRACE_NSEC;
	}
      else
	{
	  /* No epilogue handler: the kernel saves the context. */
	  env_leave (pc);
	  e = NULL;
	}
    }

  if (e == NULL)
    {
      e = pick (pc);
      if (e != NULL)
	{
	  pc->slice_end = now + TICK_NSEC;
	  pc->grace_end = 0;
	  env_enter (pc, e);
	  e->need_prologue = true;
	  QVEC (pc->id, pc->q).q_ticks++;
	  sysinfo->si_cpu[pc->id].c_q = pc->q;
	}
    }

  if (e == NULL)
    {
      sysinfo->si_cpu[pc->id].c_idle_ticks++;
      pc->next_deadline = now + TICK_NSEC;
      timer_update ();
      return UCTXT_IDLE;
    }

  if (e->ipc_pending)
    {
      e->ipc_pending = false;
      e->need_prologue = false;
      if (upcall_push (e, UPC_IPC, 0, 0, 0, e->ipc_args) < 0)
	{
	  kill_env (e, "cannot deliver IPC upcall");
	  goto again;
	}
      e->u->u_ipc_count++;
    }
  else if (e->need_prologue)
    {
      e->need_prologue = false;
      if (e->u->u_entprologue)
	{
	  if (upcall_push (e, UPC_PROLOGUE, 0, 0, 0, NULL) < 0)
	    {
	      kill_env (e, "cannot deliver prologue");
	      goto again;
	    }
	  e->u->u_prologue_count++;
	}
    }

  if (e->revoke_pending && !e->revoke_delivered && e->u->u_entrevoke)
    {
      uint32_t args[5] = { e->u->u_revoke_npages, 0, 0, 0, 0 };
      e->revoke_delivered = true;
      if (upcall_push (e, UPC_REVOKE, 0, 0, 0, args) < 0)
	{
	  kill_env (e, "cannot deliver revocation");
	  goto again;
	}
    }

  pc->next_deadline = pc->grace_end ? pc->grace_end : pc->slice_end;
  timer_update ();
  e->info->e_ticks = e->info->e_ticks + 1;
  memcpy (&pc->retframe, &e->tf, sizeof (uctxt_t));
  return &pc->retframe;
}

/*
 * End the current env's slice.  If TARGET is given, donate the rest of
 * the slice to it.
 */
void
sched_yield_to (struct env *target)
{
  struct pcpu *pc = mycpu ();
  struct env *e = pc->cur;

  if (e != NULL)
    {
      e->in_epilogue = false;
      e->u->u_in_epilogue = 0;
      e->interrupted = false;
      e->u->u_yield_count++;
    }
  pc->grace_end = 0;
  if (target != NULL && target != e)
    yield_to[pc->id] = target;
  else
    next_slot[pc->id] = true;
}

/*
 * Remove every reference to E from the schedules.
 */
void
sched_env_freed (struct env *e)
{
  for (unsigned c = 0; c < ncpus; c++)
    {
      for (unsigned q = 0; q < NQUANTA; q++)
	if (QVEC (c, q).q_env == e->id)
	  {
	    QVEC (c, q).q_env = 0;
	    QVEC (c, q).q_ticks = 0;
	    memset (&qcap[c][q], 0, sizeof (struct cap));
	  }
      if (yield_to[c] == e)
	yield_to[c] = NULL;
    }
  e->info->e_nquanta = 0;
}

int
sched_quantum_alloc (struct env *cur, unsigned k, int q, unsigned cpu,
		     envid_t id)
{
  struct env *e;
  struct cap *c;
  int r;

  if (cpu >= ncpus)
    return -E_INVAL;
  c = env_cap (cur, k);
  if (c == NULL)
    return -E_CAP_INVALID;
  r = env_control (cur, k, id, &e);
  if (r < 0)
    return r;

  if (q < 0)
    {
      for (q = 0; q < NQUANTA; q++)
	if (QVEC (cpu, q).q_env == 0)
	  break;
      if (q == NQUANTA)
	return -E_NO_QUANTUM;
    }
  else if (q >= NQUANTA)
    return -E_INVAL;
  if (QVEC (cpu, q).q_env != 0 && env_lookup (QVEC (cpu, q).q_env))
    return -E_NO_QUANTUM;

  memcpy (&qcap[cpu][q], c, sizeof (struct cap));
  QVEC (cpu, q).q_env = e->id;
  QVEC (cpu, q).q_ticks = 0;
  e->info->e_nquanta++;
  sched_kick_idle ();
  return q;
}

int
sched_quantum_free (struct env *cur, unsigned k, int q, unsigned cpu)
{
  struct env *e;
  int r;

  if (cpu >= ncpus || q < 0 || q >= NQUANTA)
    return -E_INVAL;
  if (QVEC (cpu, q).q_env == 0)
    return -E_NO_QUANTUM;
  e = env_lookup (QVEC (cpu, q).q_env);
  if (e != cur && e != NULL)
    {
      r = cap_check (cur, k, &qcap[cpu][q], CAP_W);
      if (r < 0)
	return r;
    }
  if (e)
    e->info->e_nquanta--;
  QVEC (cpu, q).q_env = 0;
  memset (&qcap[cpu][q], 0, sizeof (struct cap));
  return 0;
}

int
sched_quantum_set (struct env *cur, unsigned k, int q, unsigned cpu,
		   envid_t id)
{
  struct env *old, *e;
  int r;

  if (cpu >= ncpus || q < 0 || q >= NQUANTA)
    return -E_INVAL;
  if (QVEC (cpu, q).q_env == 0)
    return -E_NO_QUANTUM;
  r = cap_check (cur, k, &qcap[cpu][q], CAP_W);
  if (r < 0)
    return r;
  r = env_control (cur, k, id, &e);
  if (r < 0)
    return r;
  old = env_lookup (QVEC (cpu, q).q_env);
  if (old)
    old->info->e_nquanta--;
  QVEC (cpu, q).q_env = e->id;
  e->info->e_nquanta++;
  sched_kick_idle ();
  return 0;
}

/*
 * Revoke CPU CPU from environment ID: if it is running there, its
 * slice ends now (it receives the epilogue).
 */
int
sched_cpu_revoke (struct env *cur, unsigned k, unsigned cpu, envid_t id)
{
  struct env *e;
  bool ok = false;

  if (cpu >= ncpus)
    return -E_INVAL;
  e = env_lookup (id);
  if (e == NULL)
    return -E_BAD_ENV;
  if (e->cpu != (int) cpu)
    return 0;
  for (unsigned q = 0; q < NQUANTA; q++)
    if (QVEC (cpu, q).q_env == id
	&& cap_check (cur, k, &qcap[cpu][q], CAP_W) == 0)
      ok = true;
  if (!ok)
    return -E_CAP_INSUFF;
  pcpus[cpu].slice_end = ktime ();
  pcpus[cpu].next_deadline = pcpus[cpu].slice_end;
  if (cpu == cpu_id ())
    return 0;
  __atomic_or_fetch (&pcpus[cpu].ipi_pending, IPI_REVOKE, __ATOMIC_RELAXED);
  cpu_ipi (cpu);
  return 0;
}
