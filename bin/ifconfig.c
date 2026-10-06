/* ifconfig: the network configuration and the shared ARP cache. */
#include <stdio.h>
#include <arpa/inet.h>
#include <exos/exos.h>
#include <exos/net.h>

static const char *
ip (uint32_t a, char *b)
{
  return inet_ntop (AF_INET, &a, b, 16);
}

int
main (void)
{
  char b[4][16];
  if (netcfg->magic != NETCFG_MAGIC || !netcfg->configured)
    {
      printf ("network not configured (is netd running?)\n");
      return 1;
    }
  volatile struct netinfo *ni = &sysinfo_page->si_net[netcfg->card];
  printf ("net%u: %s %02x:%02x:%02x:%02x:%02x:%02x link %s\n", netcfg->card,
	  (const char *) ni->n_name, netcfg->mac[0], netcfg->mac[1],
	  netcfg->mac[2], netcfg->mac[3], netcfg->mac[4], netcfg->mac[5],
	  ni->n_link ? "up" : "down");
  printf ("  inet %s netmask %s gateway %s dns %s\n", ip (netcfg->ip, b[0]),
	  ip (netcfg->mask, b[1]), ip (netcfg->gw, b[2]), ip (netcfg->dns, b[3]));
  printf ("  rx %u packets, tx %u packets, %u dropped\n", ni->n_rxpkts,
	  ni->n_txpkts, ni->n_rxdrop);
  printf ("arp cache:\n");
  for (int i = 0; i < NARP; i++)
    if (netcfg->arp[i].ip)
      printf ("  %-15s %02x:%02x:%02x:%02x:%02x:%02x\n",
	      ip (netcfg->arp[i].ip, b[0]), netcfg->arp[i].mac[0],
	      netcfg->arp[i].mac[1], netcfg->arp[i].mac[2],
	      netcfg->arp[i].mac[3], netcfg->arp[i].mac[4],
	      netcfg->arp[i].mac[5]);
  return 0;
}
