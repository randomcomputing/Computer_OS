#include "arp.h"
#include "e1000.h"
#include "string.h"

// Ethernet + ARP wire formats are big-endian ("network byte order"); x86 is
// little-endian, so swap when putting multi-byte fields on the wire.
static unsigned short htons(unsigned short v) {
    return (unsigned short)((v << 8) | (v >> 8));
}

#define ETHERTYPE_ARP   0x0806
#define ARP_HTYPE_ETH   0x0001
#define ARP_PTYPE_IPV4  0x0800
#define ARP_OP_REQUEST  0x0001
#define ARP_OP_REPLY    0x0002

// Ethernet header (14 bytes): dst MAC, src MAC, ethertype.
typedef struct {
    unsigned char  dst[6];
    unsigned char  src[6];
    unsigned short ethertype;
} __attribute__((packed)) eth_hdr_t;

// ARP packet for IPv4 over Ethernet (28 bytes).
typedef struct {
    unsigned short htype;       // hardware type (1 = Ethernet)
    unsigned short ptype;       // protocol type (0x0800 = IPv4)
    unsigned char  hlen;        // hardware addr len (6)
    unsigned char  plen;        // protocol addr len (4)
    unsigned short oper;        // 1 = request, 2 = reply
    unsigned char  sha[6];      // sender hardware addr (MAC)
    unsigned char  spa[4];      // sender protocol addr (IP)
    unsigned char  tha[6];      // target hardware addr (MAC)
    unsigned char  tpa[4];      // target protocol addr (IP)
} __attribute__((packed)) arp_pkt_t;

// Our own IP under SLIRP's defaults (10.0.2.15). ARP doesn't strictly need a
// real one for the request to be answered, but we fill it in correctly.
#define OUR_IP 0x0A00020Fu   // 10.0.2.15

static void ip_to_bytes(unsigned int ip, unsigned char out[4]) {
    out[0] = (unsigned char)((ip >> 24) & 0xFF);
    out[1] = (unsigned char)((ip >> 16) & 0xFF);
    out[2] = (unsigned char)((ip >> 8)  & 0xFF);
    out[3] = (unsigned char)( ip        & 0xFF);
}

static unsigned int bytes_to_ip(const unsigned char in[4]) {
    return ((unsigned int)in[0] << 24) | ((unsigned int)in[1] << 16) |
           ((unsigned int)in[2] << 8)  |  (unsigned int)in[3];
}

int arp_request(unsigned int target_ip) {
    unsigned char mac[6];
    e1000_get_mac(mac);

    unsigned char frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t* eth = (eth_hdr_t*)frame;
    arp_pkt_t* arp = (arp_pkt_t*)(frame + sizeof(eth_hdr_t));

    // Ethernet: broadcast destination, our MAC source, ethertype ARP.
    for (int i = 0; i < 6; i++) eth->dst[i] = 0xFF;
    for (int i = 0; i < 6; i++) eth->src[i] = mac[i];
    eth->ethertype = htons(ETHERTYPE_ARP);

    // ARP request body.
    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(ARP_PTYPE_IPV4);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(ARP_OP_REQUEST);
    for (int i = 0; i < 6; i++) arp->sha[i] = mac[i];
    ip_to_bytes(OUR_IP, arp->spa);
    for (int i = 0; i < 6; i++) arp->tha[i] = 0x00;   // unknown — that's the ask
    ip_to_bytes(target_ip, arp->tpa);

    return e1000_send(frame, sizeof(frame));
}

int arp_poll_reply(unsigned char mac_out[6], unsigned int* ip_out) {
    unsigned char buf[1600];
    unsigned int len = e1000_receive_poll(buf, sizeof(buf));
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return 0;

    eth_hdr_t* eth = (eth_hdr_t*)buf;
    if (eth->ethertype != htons(ETHERTYPE_ARP)) return 0;

    arp_pkt_t* arp = (arp_pkt_t*)(buf + sizeof(eth_hdr_t));
    if (arp->oper != htons(ARP_OP_REPLY)) return 0;

    for (int i = 0; i < 6; i++) mac_out[i] = arp->sha[i];
    if (ip_out) *ip_out = bytes_to_ip(arp->spa);
    return 1;
}