/*
 * fetch: an HTTP/1.0 client.
 *   fetch [-o file] [-q] http://host[:port]/path
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int
main (int argc, char **argv)
{
  const char *out = NULL, *url = NULL;
  int quiet = 0;
  char host[128], path[512];
  int port = 80;

  for (int i = 1; i < argc; i++)
    if (!strcmp (argv[i], "-o") && i + 1 < argc)
      out = argv[++i];
    else if (!strcmp (argv[i], "-q"))
      quiet = 1;
    else
      url = argv[i];
  if (url == NULL || strncmp (url, "http://", 7))
    {
      fprintf (stderr, "usage: fetch [-o file] [-q] http://host[:port]/path\n");
      return 2;
    }
  url += 7;
  size_t hl = strcspn (url, ":/");
  if (hl >= sizeof (host))
    return 2;
  memcpy (host, url, hl);
  host[hl] = 0;
  url += hl;
  if (*url == ':')
    port = strtol (url + 1, (char **) &url, 10);
  snprintf (path, sizeof (path), "%s", *url ? url : "/");

  struct hostent *he = gethostbyname (host);
  if (he == NULL)
    {
      fprintf (stderr, "fetch: unknown host %s\n", host);
      return 1;
    }
  int fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      perror ("fetch: socket");
      return 1;
    }
  struct sockaddr_in sin = {.sin_family = AF_INET,.sin_port = htons (port) };
  memcpy (&sin.sin_addr, he->h_addr, 4);
  if (connect (fd, (struct sockaddr *) &sin, sizeof (sin)) < 0)
    {
      perror ("fetch: connect");
      return 1;
    }
  char req[700];
  int n = snprintf (req, sizeof (req), "GET %s HTTP/1.0\r\nHost: %s\r\n"
		    "User-Agent: ExOS-fetch/1.0\r\n\r\n", path, host);
  if (write (fd, req, n) != n)
    {
      perror ("fetch: write");
      return 1;
    }
  int ofd = 1;
  if (out && (ofd = open (out, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
    {
      perror (out);
      return 1;
    }
  static char buf[8192];
  int inhdr = 1, status = 0;
  size_t hlen = 0, total = 0;
  static char hdr[8192];
  ssize_t r;
  while ((r = read (fd, buf, sizeof (buf))) > 0)
    {
      char *p = buf;
      if (inhdr)
	{
	  size_t take = (size_t) r < sizeof (hdr) - hlen - 1
	    ? (size_t) r : sizeof (hdr) - hlen - 1;
	  memcpy (hdr + hlen, buf, take);
	  hlen += take;
	  hdr[hlen] = 0;
	  char *e = strstr (hdr, "\r\n\r\n");
	  if (e == NULL)
	    continue;
	  inhdr = 0;
	  sscanf (hdr, "HTTP/%*d.%*d %d", &status);
	  if (!quiet)
	    fprintf (stderr, "fetch: %.*s\n", (int) strcspn (hdr, "\r"), hdr);
	  size_t body = hlen - (e + 4 - hdr);
	  p = buf + (take - body);
	  r = body;
	}
      write (ofd, p, r);
      total += r;
    }
  close (fd);
  if (ofd != 1)
    close (ofd);
  if (!quiet)
    fprintf (stderr, "fetch: %u bytes\n", (unsigned) total);
  return status == 200 ? 0 : 1;
}
