/* Temporary stubs for subsystems not yet implemented. */
#include "env.h"
#include "dev.h"

void net_init (void) {}
bool e1000_probe (struct pci_dev *d) { return false; }
bool rtl8139_probe (struct pci_dev *d) { return false; }
bool virtio_net_probe (struct pci_dev *d) { return false; }
bool ne2k_probe (struct pci_dev *d) { return false; }
void net_env_freed (struct env *e) {}
int sys_net_xmit (struct env *e, unsigned card, uaddr_t recs, unsigned n, uaddr_t notify) { return -E_NO_DEV; }
int sys_dpf_insert (struct env *e, unsigned k, uaddr_t atoms, unsigned n, unsigned ring) { return -E_NO_DEV; }
int sys_dpf_delete (struct env *e, unsigned k, unsigned fid) { return -E_NO_DEV; }
int sys_dpf_ref (struct env *e, unsigned k, unsigned fid, unsigned ke, envid_t id) { return -E_NO_DEV; }
int sys_dpf_pktring (struct env *e, unsigned k, unsigned fid, unsigned ring) { return -E_NO_DEV; }
int sys_pktring_setring (struct env *e, uaddr_t ents, unsigned n) { return -E_NO_DEV; }
int sys_pktring_modring (struct env *e, unsigned ring, unsigned idx, uaddr_t ent) { return -E_NO_DEV; }
int sys_pktring_delring (struct env *e, unsigned ring) { return -E_NO_DEV; }
