#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

// Internal helpers shared between net.c and tcp.c. Not part of the public
// networking API (net.h) — these are the low-level framing primitives the
// transport layers build on. Kept minimal on purpose.

// Ethernet (14 bytes) and IPv4 (20 bytes) headers, network byte order.
typedef struct {
    unsigned char  dst[6];
    unsigned char  src[6];
    unsigned short ethertype;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    unsigned char  ver_ihl;
    unsigned char  tos;
    unsigned short total_len;
    unsigned short id;
    unsigned short flags_frag;
    unsigned char  ttl;
    unsigned char  protocol;
    unsigned short checksum;
    unsigned int   src_ip;      // network order
    unsigned int   dst_ip;      // network order
} __attribute__((packed)) ip_hdr_t;

#define ETHERTYPE_IPV4  0x0800
#define IP_PROTO_TCP    6

// 16-bit ones-complement checksum over a contiguous buffer.
unsigned short net_checksum16(const void* data, unsigned int len);

// Host-order IPv4 -> network-order (big-endian) 32-bit.
unsigned int net_ip_to_net(unsigned int ip);

// Resolve the SLIRP gateway's MAC via ARP. 1 on success, 0 on timeout.
int net_resolve_gateway_mac(unsigned char mac_out[6]);

#endif