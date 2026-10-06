/*
 * netd: the ExOS network daemon.
 *
 * In ExOS every application runs its own network stack; netd only does
 * what has to be centralised: it configures the interface (DHCP, or a
 * static address), maintains the shared ARP cache and answers ARP
 * requests, ICMP echo requests and the UDP echo service.
 *
 * usage: netd [-s ip netmask gateway dns] [-c card]
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <exos/exos.h>
#include <exos/net.h>

static struct ring arp_ring, icmp_ring, dhcp_ring, echo_ring;

struct arp_pkt
{
  uint16_t htype, ptype;
  uint8_t hlen, plen;
  uint16_t op;
  uint8_t sha[6];
  uint32_t spa;
  uint8_t tha[6];
  uint32_t tpa;
} __attribute__ ((packed));

struct udp_hdr
{
  uint16_t sport, dport, len, sum;
} __attribute__ ((packed));

static int
install (struct ring *r, unsigned n, struct dpf_atom *a, int na)
{
  int f;

  if (ring_create (r, n) < 0)
    return -1;
  f = sys_dpf_insert (EXOS_CAP, a, na, r->id);
  if (f < 0)
    {
      printf ("netd: cannot install filter (%d)\n", f);
      return -1;
    }
  return f;
}

static void
handle_arp (uint8_t * frame, unsigned len)
{
  struct arp_pkt *a = (struct arp_pkt *) (frame + 14);

  if (len < 14 + sizeof (*a) || ntohs (a->ptype) != 0x0800 || a->hlen != 6
      || a->plen != 4)
    return;
  if (a->spa)
    arp_update (a->spa, a->sha);
  if (ntohs (a->op) == 1 && netcfg->configured && a->tpa == netcfg->ip)
    {
      struct arp_pkt r;
      r = *a;
      r.op = htons (2);
      memcpy (r.tha, a->sha, 6);
      r.tpa = a->spa;
      memcpy (r.sha, (const void *) netcfg->mac, 6);
      r.spa = netcfg->ip;
      net_send_eth (a->sha, 0x0806, &r, sizeof (r), NULL, 0);
    }
}

static void
handle_icmp (uint8_t * frame, unsigned len)
{
  struct ip_info ip;
  static uint8_t reply[1500];

  if (ip_parse (frame, len, &ip) < 0 || ip.proto != 1 || ip.plen < 8)
    return;
  if (ip.dst != netcfg->ip || ip.payload[0] != 8)
    return;
  if (in_cksum (ip.payload, ip.plen, 0) != 0 || ip.plen > sizeof (reply))
    return;
  memcpy (reply, ip.payload, ip.plen);
  reply[0] = 0;
  reply[2] = reply[3] = 0;
  uint16_t s = in_cksum (reply, ip.plen, 0);
  memcpy (reply + 2, &s, 2);
  ip_output (ip.src, 1, NULL, 0, reply, ip.plen);
}

static void
handle_echo (uint8_t * frame, unsigned len)
{
  struct ip_info ip;
  const struct udp_hdr *uh;
  struct udp_hdr r;
  uint32_t sum;
  struct
  {
    uint32_t src, dst;
    uint8_t zero, proto;
    uint16_t len;
  } __attribute__ ((packed)) ph;

  if (ip_parse (frame, len, &ip) < 0 || ip.proto != 17
      || ip.plen < sizeof (*uh))
    return;
  uh = (const struct udp_hdr *) ip.payload;
  unsigned dl = ntohs (uh->len);
  if (dl < sizeof (*uh) || dl > ip.plen)
    return;
  dl -= sizeof (*uh);
  r.sport = uh->dport;
  r.dport = uh->sport;
  r.len = htons (sizeof (r) + dl);
  r.sum = 0;
  ph.src = netcfg->ip;
  ph.dst = ip.src;
  ph.zero = 0;
  ph.proto = 17;
  ph.len = r.len;
  sum = in_cksum_add (&ph, sizeof (ph), 0);
  sum = in_cksum_add (&r, sizeof (r), sum);
  r.sum = in_cksum (uh + 1, dl, sum);
  if (r.sum == 0)
    r.sum = 0xffff;
  ip_output (ip.src, 17, &r, sizeof (r), uh + 1, dl);
}

/*
 * DHCP client.
 */
struct dhcp
{
  uint8_t op, htype, hlen, hops;
  uint32_t xid;
  uint16_t secs, flags;
  uint32_t ciaddr, yiaddr, siaddr, giaddr;
  uint8_t chaddr[16];
  uint8_t sname[64], file[128];
  uint32_t magic;
  uint8_t opts[312];
} __attribute__ ((packed));

static void
dhcp_send (uint8_t type, uint32_t xid, uint32_t reqip, uint32_t server)
{
  static struct dhcp d;
  struct udp_hdr uh;
  int n = 0;
  uint32_t sum;
  struct
  {
    uint32_t src, dst;
    uint8_t zero, proto;
    uint16_t len;
  } __attribute__ ((packed)) ph;

  memset (&d, 0, sizeof (d));
  d.op = 1;
  d.htype = 1;
  d.hlen = 6;
  d.xid = xid;
  d.flags = htons (0x8000);	/* Broadcast replies. */
  memcpy (d.chaddr, (const void *) netcfg->mac, 6);
  d.magic = htonl (0x63825363);
  d.opts[n++] = 53, d.opts[n++] = 1, d.opts[n++] = type;
  if (reqip)
    {
      d.opts[n++] = 50, d.opts[n++] = 4;
      memcpy (d.opts + n, &reqip, 4);
      n += 4;
    }
  if (server)
    {
      d.opts[n++] = 54, d.opts[n++] = 4;
      memcpy (d.opts + n, &server, 4);
      n += 4;
    }
  d.opts[n++] = 55, d.opts[n++] = 3, d.opts[n++] = 1, d.opts[n++] = 3,
    d.opts[n++] = 6;
  d.opts[n++] = 255;
  uh.sport = htons (68);
  uh.dport = htons (67);
  uh.len = htons (sizeof (uh) + sizeof (d));
  uh.sum = 0;
  ph.src = 0;
  ph.dst = INADDR_BROADCAST;
  ph.zero = 0;
  ph.proto = 17;
  ph.len = uh.len;
  sum = in_cksum_add (&ph, sizeof (ph), 0);
  sum = in_cksum_add (&uh, sizeof (uh), sum);
  uh.sum = in_cksum (&d, sizeof (d), sum);
  ip_output (INADDR_BROADCAST, 17, &uh, sizeof (uh), &d, sizeof (d));
}

static int
dhcp_wait (uint32_t xid, uint8_t want, uint32_t * yi, uint32_t * server,
	   uint32_t * mask, uint32_t * gw, uint32_t * dns)
{
  uint32_t deadline = exos_time_ns () / 1000000 + 2000;

  while ((int32_t) (exos_time_ns () / 1000000 - deadline) < 0)
    {
      while (ring_pending (&dhcp_ring))
	{
	  uint8_t *frame = dhcp_ring.bufs + dhcp_ring.next * RING_BUFSZ;
	  unsigned len = dhcp_ring.flags[dhcp_ring.next];
	  struct ip_info ip;
	  int ok = 0;

	  if (ip_parse (frame, len, &ip) == 0 && ip.proto == 17
	      && ip.plen >= 8 + 240)
	    {
	      struct dhcp *d = (struct dhcp *) (ip.payload + 8);
	      if (d->op == 2 && d->xid == xid
		  && d->magic == htonl (0x63825363))
		{
		  uint8_t type = 0;
		  unsigned optlen = ip.plen - 8 - 240;
		  for (unsigned i = 0; i < optlen && d->opts[i] != 255;)
		    {
		      uint8_t o = d->opts[i];
		      if (o == 0)
			{
			  i++;
			  continue;
			}
		      uint8_t l = d->opts[i + 1];
		      const uint8_t *v = d->opts + i + 2;
		      if (o == 53)
			type = v[0];
		      else if (o == 1 && l >= 4)
			memcpy (mask, v, 4);
		      else if (o == 3 && l >= 4)
			memcpy (gw, v, 4);
		      else if (o == 6 && l >= 4)
			memcpy (dns, v, 4);
		      else if (o == 54 && l >= 4)
			memcpy (server, v, 4);
		      i += 2 + l;
		    }
		  if (type == want)
		    {
		      *yi = d->yiaddr;
		      ok = 1;
		    }
		}
	    }
	  dhcp_ring.flags[dhcp_ring.next] = 0;
	  dhcp_ring.next = (dhcp_ring.next + 1) % dhcp_ring.n;
	  if (ok)
	    return 0;
	}
      exos_sleep_until_mem (&dhcp_ring.flags[dhcp_ring.next], WK_NE, 0, 200);
    }
  return -1;
}

static int
dhcp (void)
{
  struct dpf_atom a[8];
  int n = dpf_build_ip (a, 17, 0, htons (68), 0, 0);
  uint32_t xid = (uint32_t) exos_time_ns () ^ 0x5a5a1234;
  uint32_t yi = 0, server = 0, mask = 0, gw = 0, dns = 0;

  if (install (&dhcp_ring, 8, a, n) < 0)
    return -1;
  for (int tries = 0; tries < 5; tries++)
    {
      dhcp_send (1, xid, 0, 0);
      if (dhcp_wait (xid, 2, &yi, &server, &mask, &gw, &dns) < 0)
	continue;
      dhcp_send (3, xid, yi, server);
      if (dhcp_wait (xid, 5, &yi, &server, &mask, &gw, &dns) < 0)
	continue;
      netcfg->ip = yi;
      netcfg->mask = mask ? mask : htonl (0xffffff00);
      netcfg->gw = gw;
      netcfg->dns = dns;
      return 0;
    }
  return -1;
}

int
main (int argc, char **argv)
{
  struct dpf_atom a[8];
  int n, card = 0, stat = 0;
  char ipbuf[4][16];

  for (int i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "-c") && i + 1 < argc)
	card = atoi (argv[++i]);
      else if (!strcmp (argv[i], "-s") && i + 4 < argc)
	{
	  netcfg->ip = inet_addr (argv[i + 1]);
	  netcfg->mask = inet_addr (argv[i + 2]);
	  netcfg->gw = inet_addr (argv[i + 3]);
	  netcfg->dns = inet_addr (argv[i + 4]);
	  stat = 1;
	  i += 4;
	}
    }
  if (netcfg->magic != NETCFG_MAGIC)
    {
      printf ("netd: no network configuration page\n");
      return 1;
    }
  if ((unsigned) card >= sysinfo_page->si_nnet)
    {
      printf ("netd: no network interface\n");
      return 1;
    }
  netcfg->card = card;
  memcpy ((void *) netcfg->mac, (const void *) sysinfo_page->si_net[card].n_mac,
	  6);
  netcfg->netd = __envid;

  memset (a, 0, sizeof (a));
  a[0].a_op = DPF_EQ, a[0].a_size = 2, a[0].a_off = 12, a[0].a_mask = 0xffff,
    a[0].a_val = 0x0806;
  if (install (&arp_ring, 32, a, 1) < 0)
    return 1;

  if (!stat && dhcp () < 0)
    {
      printf ("netd: DHCP failed, using 10.0.2.15\n");
      netcfg->ip = inet_addr ("10.0.2.15");
      netcfg->mask = inet_addr ("255.255.255.0");
      netcfg->gw = inet_addr ("10.0.2.2");
      netcfg->dns = inet_addr ("10.0.2.3");
    }
  n = dpf_build_ip (a, 1, netcfg->ip, 0, 0, 0);
  if (install (&icmp_ring, 16, a, n) < 0)
    return 1;
  n = dpf_build_ip (a, 17, netcfg->ip, htons (7), 0, 0);
  if (install (&echo_ring, 16, a, n) < 0)
    return 1;
  netcfg->configured = 1;
  printf ("netd: %s: ip %s netmask %s gateway %s dns %s\n",
	  (const char *) sysinfo_page->si_net[card].n_name,
	  inet_ntop (AF_INET, (const void *) &netcfg->ip, ipbuf[0], 16),
	  inet_ntop (AF_INET, (const void *) &netcfg->mask, ipbuf[1], 16),
	  inet_ntop (AF_INET, (const void *) &netcfg->gw, ipbuf[2], 16),
	  inet_ntop (AF_INET, (const void *) &netcfg->dns, ipbuf[3], 16));

  for (;;)
    {
      struct ring *rings[3] = { &arp_ring, &icmp_ring, &echo_ring };
      void (*handlers[3]) (uint8_t *, unsigned) =
      {
      handle_arp, handle_icmp, handle_echo};
      struct wk_term t[5];

      for (int i = 0; i < 3; i++)
	while (ring_pending (rings[i]))
	  {
	    struct ring *r = rings[i];
	    unsigned len = r->flags[r->next];
	    handlers[i] (r->bufs + r->next * RING_BUFSZ,
			 len > RING_BUFSZ ? RING_BUFSZ : len);
	    r->flags[r->next] = 0;
	    r->next = (r->next + 1) % r->n;
	  }
      memset (t, 0, sizeof (t));
      for (int i = 0; i < 3; i++)
	{
	  t[2 * i].wk_op = WK_NE;
	  t[2 * i].wk_lkind = WK_MEM32;
	  t[2 * i].wk_lval = (uint32_t) & rings[i]->flags[rings[i]->next];
	  t[2 * i].wk_rkind = WK_CONST;
	  t[2 * i].wk_rval = 0;
	  if (i < 2)
	    t[2 * i + 1].wk_op = WK_OR;
	}
      sys_wkpred (t, 5);
    }
}
