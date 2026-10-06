/*
 * ExOS application-level networking.
 *
 * Every process runs its own network stack (this library): it receives
 * the packets of its sockets through DPF filters and packet rings in its
 * own memory, and transmits frames directly with sys_net_xmit.  What
 * must be shared - the interface configuration and the ARP cache - is
 * in a shared page maintained by netd, a daemon that also answers ARP
 * and ICMP echo requests and runs DHCP.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef EXOS_NET_H
#define EXOS_NET_H

#include <stdint.h>
#include <xok/net.h>
#include <exos/fd.h>

#define NETCFG (USHARED + FILETAB_SIZE)
#define NARP 64

struct arp_entry
{
  uint32_t ip;			/* Network order; 0 = free. */
  uint8_t mac[6];
  uint16_t pad;
  uint32_t time;
};

struct netcfg_page
{
  uint32_t magic;
  volatile uint32_t configured;
  uint32_t card;
  uint32_t ip, mask, gw, dns;	/* Network byte order. */
  uint8_t mac[6];
  uint16_t pad;
  volatile uint32_t arp_gen;
  envid_t netd;
  volatile uint32_t lock;
  struct arp_entry arp[NARP];
};

#define NETCFG_MAGIC 0x4e455443
#define netcfg ((volatile struct netcfg_page *) NETCFG)

/* Packet rings in the process' memory. */
#define RING_BUFSZ 2048

struct ring
{
  int id;			/* Kernel ring id, or -1. */
  unsigned n, next;
  volatile uint32_t *flags;
  uint8_t *bufs;
  int refs;
};

int ring_create (struct ring *r, unsigned n);
void ring_destroy (struct ring *r);
static inline int
ring_pending (struct ring *r)
{
  return r->id >= 0 && r->flags[r->next] != 0;
}

/* DPF filter construction. */
int dpf_build_ip (struct dpf_atom *a, uint8_t proto, uint32_t dstip,
		  uint16_t dport, uint32_t srcip, uint16_t sport);

/* Link and network layers. */
int net_wait_config (int timeout_ms);
int net_send_eth (const uint8_t * dst, uint16_t type, const void *hdr,
		  unsigned hlen, const void *data, unsigned dlen);
int arp_lookup (uint32_t ip, uint8_t * mac);
int arp_resolve (uint32_t ip, uint8_t * mac);
void arp_update (uint32_t ip, const uint8_t * mac);
uint16_t in_cksum (const void *data, unsigned len, uint32_t sum);
uint32_t in_cksum_add (const void *data, unsigned len, uint32_t sum);
int ip_output (uint32_t dst, uint8_t proto, const void *hdr, unsigned hlen,
	       const void *data, unsigned dlen);

struct ip_info
{
  uint32_t src, dst;
  uint8_t proto;
  const uint8_t *payload;
  unsigned plen;
};
int ip_parse (const uint8_t * frame, unsigned len, struct ip_info *ip);

/* The socket layer drives the stack. */
void net_poll (void);
uint32_t net_next_timer (void);

#endif
