#ifndef NET_H
#define NET_H
/* Minimal Ethernet/ARP/IPv4/UDP over rtl8139.c. ponytail: no routing table,
   assumes every destination is on the same link (true for QEMU's user-mode
   SLIRP network, a single /24), no fragmentation, no options. Enough to
   resolve a MAC and get one UDP datagram onto the wire, which is what a
   DNS lookup and a raw HTTP-over-TCP handshake both need as a foundation. */

void net_init(unsigned int our_ip);

/* Sends an ARP request for ip and waits (polling, with a timeout) for the
   reply. Returns 1 and fills mac_out on success, 0 on timeout. */
int arp_resolve(unsigned int ip, unsigned char mac_out[6]);

/* dest_ip/our_ip are host-byte-order IPv4 addresses (e.g. 10.0.2.2 as
   0x0A000202). Resolves the destination's MAC via ARP if not already known,
   then sends one UDP datagram. Returns 1 on success, 0 if ARP resolution
   or the underlying send failed. */
int udp_send(unsigned int dest_ip, unsigned short dest_port, unsigned short src_port,
             const void *data, unsigned int len);

/* Resolves hostname to an IPv4 address via a single-question A-record query
   to dns_server_ip (e.g. QEMU SLIRP's built-in resolver at 10.0.2.3), which
   forwards to whatever the host machine actually uses. Returns 1 and fills
   ip_out (host-byte-order, same form as dest_ip elsewhere) on success, 0 on
   timeout or no A record in the answer. */
int dns_resolve(const char *hostname, unsigned int dns_server_ip, unsigned int *ip_out);
#endif
