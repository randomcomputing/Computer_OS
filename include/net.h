#ifndef NET_H
#define NET_H

// Minimal IPv4 + ICMP, just enough for ping.
//
// Built on the e1000 raw-frame driver and the ARP layer. ping() resolves the
// destination's link-layer MAC via ARP (going through the SLIRP gateway for
// non-local addresses), then sends ICMP echo requests wrapped in IPv4 and
// waits for echo replies, reporting round-trip time.
//
// IPv4 addresses are host-order 32-bit, e.g. 10.0.2.2 == 0x0A000202.

// Send `count` ICMP echo requests to `dst_ip` (host order), waiting up to
// `timeout_ms` for each reply. Prints one line per probe (reply + RTT, or
// timeout). Returns the number of replies received. Resolves the needed MAC
// via ARP first; returns -1 if ARP resolution fails.
int ping(unsigned int dst_ip, int count, unsigned int timeout_ms);

// Resolve a hostname to an IPv4 address via DNS (querying the SLIRP resolver
// at 10.0.2.3). On success writes the host-order IP into *out_ip and returns
// 1; returns 0 on failure (timeout, NXDOMAIN, parse error).
int dns_resolve(const char* hostname, unsigned int* out_ip);

// Our own IPv4 address (SLIRP default 10.0.2.15) and the gateway.
#define NET_OUR_IP   0x0A00020Fu   // 10.0.2.15
#define NET_GATEWAY  0x0A000202u   // 10.0.2.2
#define NET_DNS      0x0A000203u   // 10.0.2.3 (SLIRP DNS server)

#endif