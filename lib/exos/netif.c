/*
 * ExOS networking: packet rings, Ethernet, ARP, IPv4.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <exos/net.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/*
 * Packet rings: buffers in our own memory, pinned by the kernel.  They
 * are not inherited across fork (the kernel keeps writing into these
 * physical pages).
 */
int
ring_create (struct ring *r, unsigned n)
{
  struct pktring_ent ents[PKTRING_MAXENTS];
  size_t bufsz = n * RING_BUFSZ, flagsz = PGROUNDUP (n * 4);
  uint8_t *mem;
  int id;

  if (n == 0 || n > PKTRING_MAXENTS)
    return -EINVAL;
  mem = exos_mmap_alloc_flags (bufsz + flagsz, PTE_W | PTE_NOFORK);
  if (mem == NULL)
    return -ENOMEM;
  r->bufs = mem;
  r->flags = (volatile uint32_t *) (mem + bufsz);
  for (unsigned i = 0; i < n; i++)
    {
      r->flags[i] = 0;
      ents[i].pr_flag = (uint32_t) & r->flags[i];
      ents[i].pr_buf = (uint32_t) (r->bufs + i * RING_BUFSZ);
      ents[i].pr_size = RING_BUFSZ;
    }
  id = sys_pktring_setring (ents, n);
  if (id < 0)
    {
      exos_mmap_free (mem, bufsz + flagsz);
      return -ENOMEM;
    }
  r->id = id;
  r->n = n;
  r->next = 0;
  r->refs = 1;
  return 0;
}

void
ring_destroy (struct ring *r)
{
  if (r->id < 0)
    return;
  sys_pktring_delring (r->id);
  exos_mmap_free (r->bufs, r->n * RING_BUFSZ + PGROUNDUP (r->n * 4));
  r->id = -1;
}

/*
 * DPF filter for IPv4: ethertype, protocol, destination address,
 * then (past the variable IP header) ports.  Zero means wildcard.
 */
int
dpf_build_ip (struct dpf_atom *a, uint8_t proto, uint32_t dstip,
	      uint16_t dport, uint32_t srcip, uint16_t sport)
{
  int n = 0;

  memset (a, 0, sizeof (*a) * 8);
  a[n].a_op = DPF_EQ, a[n].a_size = 2, a[n].a_off = 12, a[n].a_mask = 0xffff,
    a[n].a_val = 0x0800, n++;
  a[n].a_op = DPF_EQ, a[n].a_size = 1, a[n].a_off = 23, a[n].a_mask = 0xff,
    a[n].a_val = proto, n++;
  if (dstip)
    a[n].a_op = DPF_EQ, a[n].a_size = 4, a[n].a_off = 30,
      a[n].a_mask = 0xffffffff, a[n].a_val = ntohl (dstip), n++;
  if (srcip)
    a[n].a_op = DPF_EQ, a[n].a_size = 4, a[n].a_off = 26,
      a[n].a_mask = 0xffffffff, a[n].a_val = ntohl (srcip), n++;
  if (dport || sport)
    {
      /* base += IP header length */
      a[n].a_op = DPF_SHIFT, a[n].a_size = 1, a[n].a_off = 14,
	a[n].a_mask = 0x0f, a[n].a_val = 2, n++;
      if (dport)
	a[n].a_op = DPF_EQ, a[n].a_size = 2, a[n].a_off = 16,
	  a[n].a_mask = 0xffff, a[n].a_val = ntohs (dport), n++;
      if (sport)
	a[n].a_op = DPF_EQ, a[n].a_size = 2, a[n].a_off = 14,
	  a[n].a_mask = 0xffff, a[n].a_val = ntohs (sport), n++;
    }
  return n;
}

int
net_wait_config (int timeout_ms)
{
  if (netcfg->magic != NETCFG_MAGIC)
    return -ENETDOWN;
  if (netcfg->configured)
    return 0;
  exos_sleep_until_mem (&netcfg->configured, WK_NE, 0, timeout_ms);
  return netcfg->configured ? 0 : -ENETDOWN;
}

/*
 * Transmission: the kernel copies the frame from our buffers.
 */
int
net_send_eth (const uint8_t * dst, uint16_t type, const void *hdr,
	      unsigned hlen, const void *data, unsigned dlen)
{
  struct
  {
    uint8_t dst[6], src[6];
    uint16_t type;
  } __attribute__ ((packed)) eh;
  struct sendrec sr[3];
  int n = 0, r;

  memcpy (eh.dst, dst, 6);
  memcpy (eh.src, (const void *) netcfg->mac, 6);
  eh.type = htons (type);
  sr[n].sr_va = (uint32_t) & eh, sr[n++].sr_len = sizeof (eh);
  if (hlen)
    sr[n].sr_va = (uint32_t) hdr, sr[n++].sr_len = hlen;
  if (dlen)
    sr[n].sr_va = (uint32_t) data, sr[n++].sr_len = dlen;
  for (int tries = 0; tries < 1000; tries++)
    {
      r = sys_net_xmit (netcfg->card, sr, n, NULL);
      if (r != -E_FULL)
	break;
      sys_yield (-1);
    }
  return r < 0 ? -EIO : 0;
}

/*
 * ARP cache (shared, maintained by netd and by everybody).
 */
static void
cfg_lock (void)
{
  while (__atomic_exchange_n (&netcfg->lock, 1, __ATOMIC_ACQUIRE))
    sys_yield (-1);
}

static void
cfg_unlock (void)
{
  __atomic_store_n (&netcfg->lock, 0, __ATOMIC_RELEASE);
}

int
arp_lookup (uint32_t ip, uint8_t * mac)
{
  for (int i = 0; i < NARP; i++)
    if (netcfg->arp[i].ip == ip)
      {
	memcpy (mac, (const void *) netcfg->arp[i].mac, 6);
	return 0;
      }
  return -1;
}

void
arp_update (uint32_t ip, const uint8_t * mac)
{
  int slot = -1, oldest = 0;

  if (ip == 0)
    return;
  cfg_lock ();
  for (int i = 0; i < NARP; i++)
    {
      if (netcfg->arp[i].ip == ip)
	{
	  slot = i;
	  break;
	}
      if (slot < 0 && netcfg->arp[i].ip == 0)
	slot = i;
      if (netcfg->arp[i].time < netcfg->arp[oldest].time)
	oldest = i;
    }
  if (slot < 0)
    slot = oldest;
  netcfg->arp[slot].ip = ip;
  memcpy ((void *) netcfg->arp[slot].mac, mac, 6);
  netcfg->arp[slot].time = exos_time_ns () / 1000000;
  netcfg->arp_gen++;
  cfg_unlock ();
}

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

int
arp_resolve (uint32_t ip, uint8_t * mac)
{
  if (ip == INADDR_BROADCAST || ip == (netcfg->ip | ~netcfg->mask))
    {
      memcpy (mac, bcast, 6);
      return 0;
    }
  if (ip == netcfg->ip || (ntohl (ip) >> 24) == 127)
    {
      /* Ourselves: the kernel loops frames to our own address back. */
      memcpy (mac, (const void *) netcfg->mac, 6);
      return 0;
    }
  for (int tries = 0; tries < 4; tries++)
    {
      struct arp_pkt a;
      uint32_t gen = netcfg->arp_gen;

      if (arp_lookup (ip, mac) == 0)
	return 0;
      memset (&a, 0, sizeof (a));
      a.htype = htons (1);
      a.ptype = htons (0x0800);
      a.hlen = 6;
      a.plen = 4;
      a.op = htons (1);
      memcpy (a.sha, (const void *) netcfg->mac, 6);
      a.spa = netcfg->ip;
      a.tpa = ip;
      net_send_eth (bcast, 0x0806, &a, sizeof (a), NULL, 0);
      /* netd learns the reply and bumps arp_gen. */
      exos_sleep_until_mem (&netcfg->arp_gen, WK_NE, gen, 300);
    }
  return arp_lookup (ip, mac);
}

/*
 * IPv4.
 */
uint32_t
in_cksum_add (const void *data, unsigned len, uint32_t sum)
{
  const uint8_t *p = data;
  while (len > 1)
    {
      sum += (p[0] << 8) | p[1];
      p += 2;
      len -= 2;
    }
  if (len)
    sum += p[0] << 8;
  return sum;
}

uint16_t
in_cksum (const void *data, unsigned len, uint32_t sum)
{
  sum = in_cksum_add (data, len, sum);
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return htons (~sum & 0xffff);
}

struct ip_hdr
{
  uint8_t vhl, tos;
  uint16_t len, id, off;
  uint8_t ttl, proto;
  uint16_t sum;
  uint32_t src, dst;
} __attribute__ ((packed));

static uint16_t ip_id;

int
ip_output (uint32_t dst, uint8_t proto, const void *hdr, unsigned hlen,
	   const void *data, unsigned dlen)
{
  uint8_t buf[sizeof (struct ip_hdr) + 64];
  struct ip_hdr *ip = (struct ip_hdr *) buf;
  uint8_t mac[6];
  uint32_t nexthop;

  if (hlen > 64 || sizeof (*ip) + hlen + dlen > 1500)
    return -EMSGSIZE;
  if (dst == INADDR_BROADCAST || dst == (netcfg->ip | ~netcfg->mask)
      || (dst & netcfg->mask) == (netcfg->ip & netcfg->mask))
    nexthop = dst;
  else
    nexthop = netcfg->gw;
  if (arp_resolve (nexthop, mac) < 0)
    return -EHOSTUNREACH;
  memset (ip, 0, sizeof (*ip));
  ip->vhl = 0x45;
  ip->len = htons (sizeof (*ip) + hlen + dlen);
  ip->id = htons (__atomic_add_fetch (&ip_id, 1, __ATOMIC_RELAXED)
		  ^ (__envid << 6));
  ip->off = htons (0x4000);	/* Don't fragment. */
  ip->ttl = 64;
  ip->proto = proto;
  ip->src = netcfg->ip;
  ip->dst = dst;
  ip->sum = in_cksum (ip, sizeof (*ip), 0);
  memcpy (buf + sizeof (*ip), hdr, hlen);
  return net_send_eth (mac, 0x0800, buf, sizeof (*ip) + hlen, data, dlen);
}

int
ip_parse (const uint8_t * frame, unsigned len, struct ip_info *info)
{
  const struct ip_hdr *ip = (const struct ip_hdr *) (frame + 14);
  unsigned hl, tl;

  if (len < 14 + sizeof (*ip))
    return -1;
  if (frame[12] != 0x08 || frame[13] != 0x00)
    return -1;
  if ((ip->vhl >> 4) != 4)
    return -1;
  hl = (ip->vhl & 0x0f) * 4;
  tl = ntohs (ip->len);
  if (hl < 20 || tl < hl || 14 + tl > len)
    return -1;
  if (in_cksum (ip, hl, 0) != 0)
    return -1;
  if (ntohs (ip->off) & 0x3fff)
    return -1;			/* Fragments are not supported. */
  info->src = ip->src;
  info->dst = ip->dst;
  info->proto = ip->proto;
  info->payload = (const uint8_t *) ip + hl;
  info->plen = tl - hl;
  return 0;
}
