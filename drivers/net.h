#ifndef NET_H
#define NET_H
/* Minimal Ethernet/ARP/IPv4/UDP, hardware-agnostic over whichever NIC
   driver net_init finds a card for (drivers/rtl8139.c for real hardware,
   drivers/ne2k.c for v86/QEMU's NE2000-compatible emulation). ponytail: no
   routing table, assumes every destination is on the same link (true for
   QEMU's user-mode SLIRP network, a single /24), no fragmentation, no
   options. Enough to resolve a MAC and get one UDP datagram onto the wire,
   which is what a DNS lookup and a raw HTTP-over-TCP handshake both need
   as a foundation. */

/* Probes for a NIC (RTL8139 first, NE2000/RTL8029 as fallback, see net.c)
   and initializes it. Returns 1 on success, 0 if neither driver found a
   card. Replaces the old two-step "rtl8139_init() then net_init(ip)"
   pattern every call site used to need. */
int net_init(unsigned int our_ip);

/* Hardware-agnostic single raw-frame send and MAC accessor, for low-level
   diagnostics (kernel.c's "nettest"/"ifconfig") that used to call
   rtl8139_send/rtl8139_get_mac directly and so silently assumed RTL8139. */
int net_send_raw(const void *data, unsigned int len);
void net_get_mac(unsigned char mac_out[6]);

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

/* Opens one TCP connection to dest_ip:dest_port, sends request, reads the
   response into response (up to response_maxlen bytes), and closes. One
   connection at a time, no retransmission, no reassembly of out-of-order
   segments, exactly enough for a single request/response like a raw HTTP
   GET. Returns the number of response bytes read, or -1 on failure
   (ARP/handshake timeout, request too large). */
int tcp_get(unsigned int dest_ip, unsigned short dest_port,
            const void *request, unsigned int request_len,
            void *response, unsigned int response_maxlen);

/* Same as tcp_get with a caller-chosen reply deadline (ticks to wait for
   the first/next data after the request went out) instead of the default
   SLOW_REPLY budget sized for a local LLM. 0 means the default. A weather
   JSON one-liner that has not answered in 15 seconds is not going to. */
int tcp_get_timeout(unsigned int dest_ip, unsigned short dest_port,
                    const void *request, unsigned int request_len,
                    void *response, unsigned int response_maxlen,
                    unsigned int reply_timeout_ticks);

/* Why the last dns_resolve/tcp_get failed. Every failure path in net.c
   used to collapse into the same 0/-1, so a caller could not tell "no
   route" from "the server never answered" from "no such host", and the
   Weather window could only say "unavailable". Reset to NET_ERR_NONE at
   the start of each call. */
#define NET_ERR_NONE            0
#define NET_ERR_SEND            1  /* NIC refused the frame */
#define NET_ERR_ARP_TIMEOUT     2  /* gateway never answered ARP: no link/route */
#define NET_ERR_DNS_TIMEOUT     3  /* resolver never answered */
#define NET_ERR_DNS_NXDOMAIN    4  /* resolver answered: no such host / no A record */
#define NET_ERR_CONNECT_TIMEOUT 5  /* SYN never got its SYN-ACK */
#define NET_ERR_REPLY_TIMEOUT   6  /* connected, request sent, no data before the deadline */
int net_last_error(void);
const char *net_error_name(int err);

/* Passive open: waits for one inbound connection on port, discards
   whatever request it sends (there's only one thing being served),
   sends response (chunked and stop-and-wait ACKed if bigger than one
   segment), and closes. One connection, one page. Returns 1 on success,
   0 on timeout, a request that never arrived, or a chunk that was never
   ACKed. */
int tcp_serve_once(unsigned short port, const void *response, unsigned int response_len);

/* v30: real connect-scan primitive for one port, a short timeout of its
   own rather than tcp_get's WAN-scale one. Returns 1 open, 0 closed
   (a real RST came back), -1 filtered/unreachable (no answer in time). */
int tcp_probe_port(unsigned int dest_ip, unsigned short dest_port);

/* Regression test for the tcp_match bounds-check fix (see net.c): builds a
   real Ethernet+IP+TCP frame, then a second copy whose ip->total_length
   lies about carrying far more bytes than the frame actually has, exactly
   what a hostile or corrupt remote peer could send. Returns 1 if tcp_match
   accepts the honest frame and rejects the lying one, 0 if either check
   fails (the lying one being accepted is the real bug this guards against). */
int tcp_match_selftest(void);
#endif
