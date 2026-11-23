#ifndef _PASW_VLAN_H
#define _PASW_VLAN_H

int pasw_vlan_port_set_bridged(struct pasw *sw, int port, unsigned brnum, u16 *pvid);
int pasw_vlan_port_set_standalone(struct pasw *sw, int port, u16 *pvid);

int pasw_vlan_setup(struct pasw *sw);

#endif	/* _PASW_VLAN_H */
