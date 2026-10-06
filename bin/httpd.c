/*
 * httpd: an event-driven web server for ExOS, in the spirit of Cheetah.
 *
 * A single process serves every connection from a poll() loop, over the
 * application-level TCP of ExOS (the network stack runs in this
 * process), and keeps a cache of documents in memory, indexed by path
 * and validated with the file's modification time and size.
 *
 * It serves files below the document root (default /www), directory
 * listings, and /status: a page showing what the exokernel exposes
 * (environments, quantum vectors, physical memory, buffer cache,
 * network interfaces).
 *
 * usage: httpd [-p port] [-r root] [-n max_requests]
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <exos/exos.h>
#include <exos/net.h>
#include <xok/xn.h>

#define MAXCONN 32
#define REQMAX 4096
#define CACHE_ENTRIES 32
#define CACHE_MAXFILE (256 * 1024)

static const char *docroot = "/www";
static long served, max_requests = -1;

struct conn
{
  int fd;
  char req[REQMAX];
  size_t reqlen;
  char *out;			/* Response being sent. */
  size_t outlen, outoff;
  int free_out;
  int keepalive;
  uint32_t last;
};

static struct conn conns[MAXCONN];

/*
 * Document cache.
 */
struct centry
{
  char path[256];
  time_t mtime;
  off_t size;
  char *data;
  unsigned hits;
};
static struct centry cache[CACHE_ENTRIES];
static unsigned cache_hits, cache_misses;

static const char *
mime (const char *path)
{
  const char *e = strrchr (path, '.');
  if (e == NULL)
    return "application/octet-stream";
  if (!strcmp (e, ".html") || !strcmp (e, ".htm"))
    return "text/html";
  if (!strcmp (e, ".txt") || !strcmp (e, ".c") || !strcmp (e, ".h"))
    return "text/plain";
  if (!strcmp (e, ".css"))
    return "text/css";
  if (!strcmp (e, ".js"))
    return "application/javascript";
  if (!strcmp (e, ".png"))
    return "image/png";
  if (!strcmp (e, ".jpg") || !strcmp (e, ".jpeg"))
    return "image/jpeg";
  if (!strcmp (e, ".gif"))
    return "image/gif";
  return "application/octet-stream";
}

static char *
cache_get (const char *path, struct stat *st)
{
  struct centry *victim = &cache[0];

  for (int i = 0; i < CACHE_ENTRIES; i++)
    {
      struct centry *c = &cache[i];
      if (c->data && !strcmp (c->path, path))
	{
	  if (c->mtime == st->st_mtime && c->size == st->st_size)
	    {
	      c->hits++;
	      cache_hits++;
	      return c->data;
	    }
	  victim = c;
	  break;
	}
      if (c->data == NULL || c->hits < victim->hits)
	victim = c;
    }
  if (st->st_size > CACHE_MAXFILE)
    return NULL;
  cache_misses++;
  int fd = open (path, O_RDONLY);
  if (fd < 0)
    return NULL;
  char *buf = malloc (st->st_size + 1);
  if (buf == NULL)
    {
      close (fd);
      return NULL;
    }
  ssize_t got = 0, r;
  while (got < st->st_size && (r = read (fd, buf + got, st->st_size - got)) > 0)
    got += r;
  close (fd);
  if (got != st->st_size)
    {
      free (buf);
      return NULL;
    }
  free (victim->data);
  strlcpy (victim->path, path, sizeof (victim->path));
  victim->mtime = st->st_mtime;
  victim->size = st->st_size;
  victim->data = buf;
  victim->hits = 1;
  return buf;
}

/*
 * Responses.
 */
static void
respond (struct conn *c, int code, const char *status, const char *type,
	 const char *body, size_t blen, int copy_body)
{
  char hdr[512];
  char date[64];
  time_t now = time (NULL);
  strftime (date, sizeof (date), "%a, %d %b %Y %H:%M:%S GMT", gmtime (&now));
  int hl = snprintf (hdr, sizeof (hdr),
		     "HTTP/1.1 %d %s\r\nServer: ExOS-httpd/1.0 (Xok)\r\n"
		     "Date: %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
		     "Connection: %s\r\n\r\n", code, status, date, type,
		     (unsigned) blen, c->keepalive ? "keep-alive" : "close");
  c->out = malloc (hl + blen);
  if (c->out == NULL)
    {
      c->keepalive = 0;
      return;
    }
  memcpy (c->out, hdr, hl);
  if (body)
    memcpy (c->out + hl, body, blen);
  c->outlen = hl + blen;
  c->outoff = 0;
  c->free_out = 1;
  (void) copy_body;
}

static void
error_page (struct conn *c, int code, const char *status)
{
  char body[256];
  int n = snprintf (body, sizeof (body),
		    "<html><body><h1>%d %s</h1><hr><i>ExOS httpd on Xok"
		    "</i></body></html>\n", code, status);
  respond (c, code, status, "text/html", body, n, 1);
}

struct sbuf
{
  char *p;
  size_t len, cap;
};

static void
sb_printf (struct sbuf *b, const char *fmt, ...)
{
  va_list ap;
  char tmp[1024];
  va_start (ap, fmt);
  int n = vsnprintf (tmp, sizeof (tmp), fmt, ap);
  va_end (ap);
  if (n < 0)
    return;
  if ((size_t) n >= sizeof (tmp))
    n = sizeof (tmp) - 1;
  if (b->len + n + 1 > b->cap)
    {
      b->cap = (b->len + n + 1) * 2;
      b->p = realloc (b->p, b->cap);
    }
  memcpy (b->p + b->len, tmp, n);
  b->len += n;
  b->p[b->len] = 0;
}

/* The exokernel exposes everything: show it. */
static void
status_page (struct conn *c)
{
  struct sbuf b = { 0 };
  volatile struct sysinfo *si = sysinfo_page;
  unsigned st[5] = { 0 };
  uint64_t up = exos_time_ns () / 1000000000ULL;

  for (unsigned i = 0; i < si->si_nppages; i++)
    if (ppages_info[i].pp_state < 5)
      st[ppages_info[i].pp_state]++;
  sb_printf (&b, "<html><head><title>Xok/ExOS status</title></head><body>"
	     "<h1>Xok/ExOS status</h1><p>Uptime %llu s, %u CPUs, time slice "
	     "%u us. Served %ld requests (cache %u hits, %u misses).</p>",
	     up, si->si_ncpu, si->si_tick_nsec / 1000, served, cache_hits,
	     cache_misses);
  sb_printf (&b, "<h2>Physical memory (pages)</h2><table border=1><tr>"
	     "<th>total</th><th>free</th><th>kernel</th><th>user</th>"
	     "<th>buffer cache</th></tr><tr><td>%u</td><td>%u</td><td>%u</td>"
	     "<td>%u</td><td>%u</td></tr></table>", si->si_nppages,
	     st[PP_FREE], st[PP_KERNEL], st[PP_USER], st[PP_BC]);
  sb_printf (&b, "<h2>Environments</h2><table border=1><tr><th>env</th>"
	     "<th>pid</th><th>status</th><th>cpu</th><th>pages</th>"
	     "<th>slices</th><th>command</th></tr>");
  for (unsigned i = 1; i < NENV; i++)
    {
      volatile struct envinfo *ei = &((volatile struct envinfo *) UENVINFO)[i];
      if (ei->e_status == ENV_FREE)
	continue;
      volatile struct exos_proc *p = proc_of_env (ei->e_id);
      sb_printf (&b, "<tr><td>%x</td><td>%d</td><td>%u</td><td>%d</td>"
		 "<td>%u</td><td>%u</td><td>%s</td></tr>", ei->e_id,
		 p->magic == PROC_MAGIC ? p->pid : -1, ei->e_status,
		 ei->e_cpu, ei->e_npages, ei->e_nquanta,
		 p->magic == PROC_MAGIC ? (const char *) p->args : "");
    }
  sb_printf (&b, "</table><h2>Quantum vectors</h2>");
  for (unsigned cpu = 0; cpu < si->si_ncpu; cpu++)
    {
      sb_printf (&b, "<p>CPU %u (current slice %u): ", cpu, si->si_cpu[cpu].c_q);
      for (int q = 0; q < NQUANTA; q++)
	if (si->si_qvec[cpu][q].q_env)
	  sb_printf (&b, "[%d:%x] ", q, si->si_qvec[cpu][q].q_env);
      sb_printf (&b, "</p>");
    }
  unsigned bcn = 0, bcd = 0;
  for (int i = 0; i < XN_NBC; i++)
    if (xn_registry[i].bc_state & BC_USED)
      {
	bcn++;
	if (xn_registry[i].bc_state & BC_DIRTY)
	  bcd++;
      }
  sb_printf (&b, "<h2>XN buffer cache registry</h2><p>%u blocks cached, "
	     "%u dirty.</p><h2>Disks</h2><ul>", bcn, bcd);
  for (unsigned d = 0; d < si->si_ndisk; d++)
    sb_printf (&b, "<li>disk%u: %s, %u sectors%s</li>", d,
	       (const char *) si->si_disk[d].d_name, si->si_disk[d].d_nsectors,
	       si->si_disk[d].d_xn ? ", XN" : "");
  sb_printf (&b, "</ul><h2>Network</h2><ul>");
  for (unsigned n = 0; n < si->si_nnet; n++)
    sb_printf (&b, "<li>net%u: %s, rx %u, tx %u, dropped %u</li>", n,
	       (const char *) si->si_net[n].n_name, si->si_net[n].n_rxpkts,
	       si->si_net[n].n_txpkts, si->si_net[n].n_rxdrop);
  sb_printf (&b, "</ul><hr><i>ExOS httpd on the Xok exokernel</i></body>"
	     "</html>\n");
  respond (c, 200, "OK", "text/html", b.p, b.len, 1);
  free (b.p);
}

static void
dir_page (struct conn *c, const char *url, const char *path)
{
  struct sbuf b = { 0 };
  DIR *d = opendir (path);
  struct dirent *de;

  if (d == NULL)
    {
      error_page (c, 404, "Not Found");
      return;
    }
  sb_printf (&b, "<html><body><h1>Index of %s</h1><ul>", url);
  while ((de = readdir (d)) != NULL)
    sb_printf (&b, "<li><a href=\"%s%s%s\">%s</a></li>", url,
	       url[strlen (url) - 1] == '/' ? "" : "/", de->d_name,
	       de->d_name);
  closedir (d);
  sb_printf (&b, "</ul><hr><i>ExOS httpd</i></body></html>\n");
  respond (c, 200, "OK", "text/html", b.p, b.len, 1);
  free (b.p);
}

static void
handle_request (struct conn *c)
{
  char method[16], url[512], proto[16], path[600];
  struct stat st;

  if (sscanf (c->req, "%15s %511s %15s", method, url, proto) < 2)
    {
      c->keepalive = 0;
      error_page (c, 400, "Bad Request");
      return;
    }
  c->keepalive = !strcmp (proto, "HTTP/1.1")
    && !strstr (c->req, "Connection: close");
  if (strstr (c->req, "Connection: keep-alive"))
    c->keepalive = 1;
  served++;
  printf ("httpd: %s %s\n", method, url);
  if (strcmp (method, "GET") && strcmp (method, "HEAD"))
    {
      error_page (c, 501, "Not Implemented");
      return;
    }
  if (strstr (url, ".."))
    {
      error_page (c, 403, "Forbidden");
      return;
    }
  char *q = strchr (url, '?');
  if (q)
    *q = 0;
  if (!strcmp (url, "/status"))
    {
      status_page (c);
      return;
    }
  snprintf (path, sizeof (path), "%s%s", docroot, url);
  if (stat (path, &st) == 0 && S_ISDIR (st.st_mode))
    {
      char idx[620];
      snprintf (idx, sizeof (idx), "%s/index.html", path);
      if (stat (idx, &st) == 0)
	strcpy (path, idx);
      else
	{
	  dir_page (c, url, path);
	  return;
	}
    }
  if (stat (path, &st) < 0)
    {
      error_page (c, 404, "Not Found");
      return;
    }
  char *data = cache_get (path, &st);
  if (data == NULL)
    {
      error_page (c, 500, "Internal Server Error");
      return;
    }
  respond (c, 200, "OK", mime (path), strcmp (method, "HEAD") ? data : NULL,
	   strcmp (method, "HEAD") ? (size_t) st.st_size : 0, 0);
}

static void
conn_close (struct conn *c)
{
  close (c->fd);
  if (c->free_out)
    free (c->out);
  memset (c, 0, sizeof (*c));
  c->fd = -1;
}

int
main (int argc, char **argv)
{
  int port = 80, lfd, one = 1;
  struct sockaddr_in sin;

  for (int i = 1; i < argc; i++)
    if (!strcmp (argv[i], "-p") && i + 1 < argc)
      port = atoi (argv[++i]);
    else if (!strcmp (argv[i], "-r") && i + 1 < argc)
      docroot = argv[++i];
    else if (!strcmp (argv[i], "-n") && i + 1 < argc)
      max_requests = atol (argv[++i]);
  for (int i = 0; i < MAXCONN; i++)
    conns[i].fd = -1;

  lfd = socket (AF_INET, SOCK_STREAM, 0);
  if (lfd < 0)
    {
      perror ("httpd: socket");
      return 1;
    }
  setsockopt (lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
  memset (&sin, 0, sizeof (sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons (port);
  if (bind (lfd, (struct sockaddr *) &sin, sizeof (sin)) < 0
      || listen (lfd, 16) < 0)
    {
      perror ("httpd: bind/listen");
      return 1;
    }
  fcntl (lfd, F_SETFL, O_NONBLOCK);
  printf ("httpd: serving %s on port %d\n", docroot, port);

  while (max_requests < 0 || served < max_requests)
    {
      struct pollfd pf[MAXCONN + 1];
      int idx[MAXCONN + 1], n = 0;

      pf[n].fd = lfd;
      pf[n].events = POLLIN;
      idx[n++] = -1;
      for (int i = 0; i < MAXCONN; i++)
	if (conns[i].fd >= 0)
	  {
	    pf[n].fd = conns[i].fd;
	    pf[n].events = conns[i].out ? POLLOUT : POLLIN;
	    idx[n++] = i;
	  }
      if (poll (pf, n, 5000) < 0)
	continue;
      for (int k = 0; k < n; k++)
	{
	  if (!pf[k].revents)
	    continue;
	  if (idx[k] < 0)
	    {
	      int cfd;
	      while ((cfd = accept (lfd, NULL, NULL)) >= 0)
		{
		  int slot = -1;
		  for (int i = 0; i < MAXCONN; i++)
		    if (conns[i].fd < 0)
		      {
			slot = i;
			break;
		      }
		  if (slot < 0)
		    {
		      close (cfd);
		      continue;
		    }
		  fcntl (cfd, F_SETFL, O_NONBLOCK);
		  memset (&conns[slot], 0, sizeof (conns[slot]));
		  conns[slot].fd = cfd;
		}
	      continue;
	    }
	  struct conn *c = &conns[idx[k]];
	  if (c->out)
	    {
	      ssize_t w = write (c->fd, c->out + c->outoff,
				 c->outlen - c->outoff);
	      if (w < 0 && errno != EAGAIN)
		{
		  conn_close (c);
		  continue;
		}
	      if (w > 0)
		c->outoff += w;
	      if (c->outoff == c->outlen)
		{
		  if (c->free_out)
		    free (c->out);
		  c->out = NULL;
		  c->free_out = 0;
		  if (!c->keepalive)
		    conn_close (c);
		}
	      continue;
	    }
	  ssize_t r = read (c->fd, c->req + c->reqlen,
			    sizeof (c->req) - 1 - c->reqlen);
	  if (r <= 0)
	    {
	      if (r == 0 || errno != EAGAIN)
		conn_close (c);
	      continue;
	    }
	  c->reqlen += r;
	  c->req[c->reqlen] = 0;
	  char *end = strstr (c->req, "\r\n\r\n");
	  if (end == NULL)
	    {
	      if (c->reqlen >= sizeof (c->req) - 1)
		conn_close (c);
	      continue;
	    }
	  handle_request (c);
	  size_t used = end + 4 - c->req;
	  memmove (c->req, c->req + used, c->reqlen - used + 1);
	  c->reqlen -= used;
	  if (c->out == NULL)
	    conn_close (c);
	}
    }
  for (int i = 0; i < MAXCONN; i++)
    if (conns[i].fd >= 0)
      conn_close (&conns[i]);
  close (lfd);
  return 0;
}
