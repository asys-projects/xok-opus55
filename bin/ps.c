/*
 * ps: read every process' state from the exported environment table
 * and the u-areas (no kernel process table is involved).
 */
#include <stdio.h>
#include <string.h>
#include <exos/exos.h>

static const char *
state (uint32_t envst, uint32_t pst)
{
  if (pst == PROC_ZOMBIE)
    return "Z";
  switch (envst)
    {
    case ENV_RUNNABLE:
      return "R";
    case ENV_WAITING:
      return "S";
    case ENV_IPC_WAIT:
      return "I";
    case ENV_NOT_RUNNABLE:
      return "T";
    }
  return "?";
}

int
main (int argc, char **argv)
{
  int all = argc > 1 && !strcmp (argv[1], "-a");
  printf ("  PID  PPID   ENV    UID S CPU  PAGES  SLICES  COMMAND\n");
  for (unsigned i = 1; i < NENV; i++)
    {
      volatile struct envinfo *ei = &((volatile struct envinfo *) UENVINFO)[i];
      if (ei->e_status == ENV_FREE)
	continue;
      volatile struct exos_proc *p = proc_of_env (ei->e_id);
      if (p->magic != PROC_MAGIC)
	{
	  if (all)
	    printf ("    -     - %5x      - %s   -  %5u  %6llu  (raw env)\n",
		    ei->e_id, state (ei->e_status, 0), ei->e_npages,
		    ei->e_ticks);
	  continue;
	}
      if (p->state == PROC_EXECED)
	continue;
      printf ("%5d %5d %5x %6u %s %3s  %5u  %6llu  %s\n", p->pid, p->ppid,
	      ei->e_id, p->uid, state (ei->e_status, p->state),
	      ei->e_cpu >= 0 ? (char[]) { '0' + ei->e_cpu, 0 } : "-",
	      ei->e_npages, ei->e_ticks, (const char *) p->args);
    }
  return 0;
}
