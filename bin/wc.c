#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

static void
count (FILE *f, long *l, long *w, long *c)
{
  int ch, in = 0;
  *l = *w = *c = 0;
  while ((ch = fgetc (f)) != EOF)
    {
      (*c)++;
      if (ch == '\n')
	(*l)++;
      if (isspace (ch))
	in = 0;
      else if (!in)
	{
	  in = 1;
	  (*w)++;
	}
    }
}

static int fl, fw, fc;

static void
show (long l, long w, long c, const char *name)
{
  if (fl)
    printf ("%7ld", l);
  if (fw)
    printf ("%s%7ld", fl ? " " : "", w);
  if (fc)
    printf ("%s%7ld", fl || fw ? " " : "", c);
  if (name)
    printf (" %s", name);
  printf ("\n");
}

int
main (int argc, char **argv)
{
  long l, w, c, tl = 0, tw = 0, tc = 0;
  int i = 1, nfiles;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
    for (char *p = argv[i] + 1; *p; p++)
      if (*p == 'l')
	fl = 1;
      else if (*p == 'w')
	fw = 1;
      else if (*p == 'c')
	fc = 1;
  if (!fl && !fw && !fc)
    fl = fw = fc = 1;
  nfiles = argc - i;
  if (nfiles == 0)
    {
      count (stdin, &l, &w, &c);
      show (l, w, c, NULL);
      return 0;
    }
  for (; i < argc; i++)
    {
      FILE *f = fopen (argv[i], "r");
      if (f == NULL)
	{
	  fprintf (stderr, "wc: %s: %s\n", argv[i], strerror (errno));
	  continue;
	}
      count (f, &l, &w, &c);
      fclose (f);
      show (l, w, c, argv[i]);
      tl += l, tw += w, tc += c;
    }
  if (nfiles > 1)
    show (tl, tw, tc, "total");
  return 0;
}
