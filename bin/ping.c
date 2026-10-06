/*
 * ping: ICMP echo, with the application's own packet filter.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <exos/exos.h>
#include <exos/net.h>

int
main (int argc, char **argv)
{
  struct ring ring;
  struct dpf_atom a[10];
  int n, count = 4, received = 0;
  uint16_t id = __envid & 0xffff;
  struct hostent *he;
  uint32_t dst;

  if (argc < 2)
    {
      fprintf (stderr, "usage: ping host [count]\n");
      return 2;
    }
  if (argc > 2)
    count = atoi (argv[2]);
  if (net_wait_config (5000) < 0)
    {
      fprintf (stderr, "ping: network down\n");
      return 1;
    }
  he = gethostbyname (argv[1]);
  if (he == NULL)
    {
      fprintf (stderr, "ping: unknown host %s\n", argv[1]);
      return 1;
    }
  memcpy (&dst, he->h_addr, 4);
  if (ring_create (&ring, 8) < 0)
    return 1;
  /* IP, ICMP, to us; then type 0 (echo reply) and our identifier. */
  n = dpf_build_ip (a, 1, netcfg->ip, 0, 0, 0);
  a[n].a_op = DPF_SHIFT, a[n].a_size = 1, a[n].a_off = 14, a[n].a_mask = 0x0f,
    a[n].a_val = 2, n++;
  a[n].a_op = DPF_EQ, a[n].a_size = 1, a[n].a_off = 14, a[n].a_mask = 0xff,
    a[n].a_val = 0, n++;
  a[n].a_op = DPF_EQ, a[n].a_size = 2, a[n].a_off = 18, a[n].a_mask = 0xffff,
    a[n].a_val = id, n++;
  if (sys_dpf_insert (EXOS_CAP, a, n, ring.id) < 0)
    {
      fprintf (stderr, "ping: cannot install packet filter "
	       "(needs root)\n");
      return 1;
    }
  printf ("PING %s (%s)\n", argv[1], inet_ntoa (*(struct in_addr *) &dst));
  for (int seq = 1; seq <= count; seq++)
    {
      uint8_t pkt[64];
      uint64_t t0 = exos_time_ns ();
      memset (pkt, 0, sizeof (pkt));
      pkt[0] = 8;
      pkt[4] = id >> 8, pkt[5] = id;
      pkt[6] = seq >> 8, pkt[7] = seq;
      for (int i = 8; i < 64; i++)
	pkt[i] = i;
      uint16_t s = in_cksum (pkt, sizeof (pkt), 0);
      memcpy (pkt + 2, &s, 2);
      if (ip_output (dst, 1, NULL, 0, pkt, sizeof (pkt)) < 0)
	{
	  printf ("ping: host unreachable\n");
	  continue;
	}
      while (exos_sleep_until_mem (&ring.flags[ring.next], WK_NE, 0, 1000) == 0)
	{
	  uint8_t *f = ring.bufs + ring.next * RING_BUFSZ;
	  struct ip_info ip;
	  int ok = ip_parse (f, ring.flags[ring.next], &ip) == 0
	    && ip.plen >= 8 && ((ip.payload[6] << 8) | ip.payload[7]) == seq;
	  ring.flags[ring.next] = 0;
	  ring.next = (ring.next + 1) % ring.n;
	  if (ok)
	    {
	      uint64_t us = (exos_time_ns () - t0) / 1000;
	      printf ("%u bytes from %s: icmp_seq=%d time=%llu.%03llu ms\n",
		      ip.plen, inet_ntoa (*(struct in_addr *) &ip.src), seq,
		      us / 1000, us % 1000);
	      received++;
	      break;
	    }
	}
      if (seq < count)
	usleep (200000);
    }
  printf ("%d packets transmitted, %d received\n", count, received);
  return received ? 0 : 1;
}
